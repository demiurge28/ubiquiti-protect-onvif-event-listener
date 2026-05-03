// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "patch_watcher.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <utility>

#include "absl/log/log.h"

namespace protect_ui {

// inotify(7) requires the read buffer to be aligned for `struct
// inotify_event` and at least sizeof(struct inotify_event) + NAME_MAX +
// 1 bytes to guarantee one event fits.  8 KiB comfortably fits a few
// dozen events per read.
static constexpr size_t kInotifyBufSize = 8192;

PatchWatcher::PatchWatcher(std::vector<WatchSpec> specs,
                           Callback on_change,
                           std::chrono::milliseconds debounce)
    : specs_(std::move(specs)),
      on_change_(std::move(on_change)),
      debounce_(debounce) {}  // NOLINT(whitespace/indent_namespace)

PatchWatcher::~PatchWatcher() {
  stop();
}

bool PatchWatcher::start() {
  if (running_.exchange(true)) {
    LOG(WARNING) << "[patch_watcher] already running";
    return false;
  }

  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd_ < 0) {
    LOG(WARNING) << "[patch_watcher] inotify_init1: "
                 << std::strerror(errno);
    running_ = false;
    return false;
  }

  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    LOG(WARNING) << "[patch_watcher] eventfd: " << std::strerror(errno);
    close(inotify_fd_);
    inotify_fd_ = -1;
    running_ = false;
    return false;
  }

  thread_ = std::thread(&PatchWatcher::run, this);
  return true;
}

void PatchWatcher::stop() {
  if (!running_.exchange(false)) return;
  if (wake_fd_ >= 0) {
    uint64_t one = 1;
    ssize_t r = write(wake_fd_, &one, sizeof(one));
    (void)r;
  }
  if (thread_.joinable()) thread_.join();
  if (inotify_fd_ >= 0) {
    close(inotify_fd_);
    inotify_fd_ = -1;
  }
  if (wake_fd_ >= 0) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
  wd_to_spec_.clear();
}

void PatchWatcher::add_watches() {
  // We watch the directory (not individual files) because the upgrade
  // path on UniFi devices typically replaces files via rename -- the
  // file inode changes, so a per-file watch would silently expire.
  // Directory watches survive that and surface IN_MOVED_TO events.
  constexpr uint32_t kMask =
      IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
      IN_DELETE_SELF | IN_MOVE_SELF;
  for (size_t i = 0; i < specs_.size(); ++i) {
    int wd = inotify_add_watch(inotify_fd_, specs_[i].dir.c_str(), kMask);
    if (wd < 0) {
      // Path may not exist on a dev box, or before Protect is
      // installed.  Log and continue -- we'll retry on the next
      // rewatch trigger.
      LOG(INFO) << "[patch_watcher] inotify_add_watch(" << specs_[i].dir
                << "): " << std::strerror(errno);
      continue;
    }
    wd_to_spec_[wd] = i;
    LOG(INFO) << "[patch_watcher] watching " << specs_[i].dir
              << " (wd=" << wd << ")";
  }
}

bool PatchWatcher::name_matches(const WatchSpec& s, const char* name) {
  size_t len = std::strlen(name);
  bool ext_ok = (len >= 3 && std::strcmp(name + len - 3, ".js") == 0) ||
                (len >= 5 && std::strcmp(name + len - 5, ".conf") == 0);
  if (!ext_ok) return false;
  if (s.name_prefixes.empty()) return true;
  for (const auto& p : s.name_prefixes) {
    if (p.size() <= len &&
        std::strncmp(name, p.c_str(), p.size()) == 0) {
      return true;
    }
  }
  return false;
}

void PatchWatcher::run() {
  using clock = std::chrono::steady_clock;
  // sentinel: "no event pending"
  const clock::time_point kNoPending = clock::time_point::max();
  // Time at which the debounced callback should fire.
  clock::time_point pending_at = kNoPending;
  // Time at which to next retry inotify_add_watch for any specs that
  // currently have no watch (missing dir, dropped via IN_IGNORED, etc.).
  clock::time_point retry_at = clock::now();

  alignas(struct inotify_event) char buf[kInotifyBufSize];

  while (running_.load()) {
    // Block until the soonest of: pending fire, retry attempt, or
    // forever if neither is scheduled.
    int timeout_ms = -1;
    auto now = clock::now();
    clock::time_point soonest = kNoPending;
    if (pending_at != kNoPending) soonest = pending_at;
    if (retry_at != kNoPending && retry_at < soonest) soonest = retry_at;
    if (soonest != kNoPending) {
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              soonest - now).count();
      timeout_ms = remaining < 0 ? 0 : static_cast<int>(remaining);
    }

    struct pollfd pfds[2] = {
      {inotify_fd_, POLLIN, 0},
      {wake_fd_, POLLIN, 0},
    };
    int n = poll(pfds, 2, timeout_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG(WARNING) << "[patch_watcher] poll: " << std::strerror(errno);
      break;
    }

    if (pfds[1].revents & POLLIN) {
      uint64_t v;
      ssize_t r = read(wake_fd_, &v, sizeof(v));
      (void)r;
      break;
    }

    bool got_event = false;
    bool watch_dropped = false;

    if (pfds[0].revents & POLLIN) {
      ssize_t r;
      while ((r = read(inotify_fd_, buf, sizeof(buf))) > 0) {
        for (char* p = buf; p < buf + r;) {
          auto* ev = reinterpret_cast<struct inotify_event*>(p);
          if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
            // Watch went away (overlayfs rebase, dir replaced, etc.).
            wd_to_spec_.erase(ev->wd);
            watch_dropped = true;
          } else if (ev->len > 0) {
            auto it = wd_to_spec_.find(ev->wd);
            if (it != wd_to_spec_.end()) {
              const WatchSpec& s = specs_[it->second];
              if (name_matches(s, ev->name)) {
                LOG(INFO) << "[patch_watcher] fs change "
                          << s.dir << "/" << ev->name;
                got_event = true;
              }
            }
          }
          p += sizeof(struct inotify_event) + ev->len;
        }
      }
    }

    if (got_event) {
      pending_at = clock::now() + debounce_;
    }

    if (pending_at != kNoPending && clock::now() >= pending_at) {
      pending_at = kNoPending;
      if (on_change_) {
        on_change_();
        fire_count_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // Re-add any missing watches.  We retry on a 1s cadence whenever
    // any spec is unwatched -- the directory may have been deleted
    // (overlayfs rebase) and not yet recreated, or might never exist
    // on this device (dev box, missing Protect install).
    //
    // If we *recover* a watch we previously had to drop, we conclude
    // that we may have missed inotify events during the gap and
    // schedule a debounced fire.  This is what makes us self-heal
    // through e.g. a UniFi OS firmware-overlay rebase that swaps the
    // entire directory atomically.
    bool retry_due = retry_at != kNoPending && clock::now() >= retry_at;
    if (watch_dropped || retry_due) {
      size_t before = wd_to_spec_.size();
      add_watches();
      size_t after = wd_to_spec_.size();
      if (after > before && first_add_done_) {
        pending_at = clock::now() + debounce_;
      }
      first_add_done_ = true;
      if (after < specs_.size()) {
        retry_at = clock::now() + std::chrono::seconds(1);
      } else {
        retry_at = kNoPending;
      }
    }
  }
}

}  // namespace protect_ui
