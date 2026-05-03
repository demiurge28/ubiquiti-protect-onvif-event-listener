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

#ifndef SRC_PATCH_WATCHER_HPP_
#define SRC_PATCH_WATCHER_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace protect_ui {

// One directory we want to watch, plus the filename prefixes that should
// trigger the on_change callback.  An empty `name_prefixes` matches every
// `.js` or `.conf` file in the directory.
struct WatchSpec {
  std::string dir;
  std::vector<std::string> name_prefixes;
};

// inotify-driven watcher that re-runs idempotent patch logic when the
// Protect UI files or nginx site file change underneath us (e.g. after
// a UniFi OS firmware-overlay update that bypasses apt -- our dpkg
// trigger only covers apt-driven upgrades).
//
// Owns one background thread; the destructor stops cleanly via an
// eventfd wake.  All public methods are thread-safe with respect to
// each other but `start` / `stop` should not be called concurrently.
class PatchWatcher {
 public:
  using Callback = std::function<void()>;

  PatchWatcher(std::vector<WatchSpec> specs,
               Callback on_change,
               std::chrono::milliseconds debounce =
                   std::chrono::seconds(5));
  ~PatchWatcher();

  PatchWatcher(const PatchWatcher&) = delete;
  PatchWatcher& operator=(const PatchWatcher&) = delete;

  bool start();
  void stop();

  // Visible for testing: how many times the callback has fired.
  uint64_t fire_count() const { return fire_count_.load(); }

 private:
  void run();
  void add_watches();
  bool name_matches(const WatchSpec& s, const char* name);

  std::vector<WatchSpec> specs_;
  Callback on_change_;
  std::chrono::milliseconds debounce_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> fire_count_{0};
  int inotify_fd_ = -1;
  int wake_fd_ = -1;
  std::unordered_map<int, size_t> wd_to_spec_;
  // Set to true after the first add_watches() pass.  Used to suppress
  // the spurious "regained a watch" fire on the very first iteration.
  bool first_add_done_ = false;
};

}  // namespace protect_ui

#endif  // SRC_PATCH_WATCHER_HPP_
