// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "cpu_profiler.hpp"

#include <signal.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace onvif {

CpuProfiler* CpuProfiler::g_instance_ = nullptr;

CpuProfiler& CpuProfiler::instance() {
  static CpuProfiler kInstance;
  g_instance_ = &kInstance;
  return kInstance;
}

namespace {
// Use a real-time signal that does NOT clash with anything libcurl,
// libpq, or libxml2 use.  SIGPROF is what gperftools picks but we
// want to avoid the (low) chance of stepping on a third-party lib's
// own profiler.  RTMIN+1 is reserved for application use.
inline int sample_signal() { return SIGRTMIN + 1; }
}  // namespace

// static
int CpuProfiler::capture_stack(Sample* out) {
  // __builtin_frame_address(0) returns the current frame pointer.
  // From there, walk fp[0] = saved-fp, fp[1] = return-address.
  // The recorder is built with -fno-omit-frame-pointer so this is
  // reliable for our own code; system libs may not have FP set, in
  // which case the chain truncates early — that's fine, partial
  // stacks are still useful.
  void** fp = static_cast<void**>(__builtin_frame_address(0));
  int depth = 0;
  while (fp && depth < kMaxFrames) {
    void* ret = fp[1];
    if (ret == nullptr) break;
    out->frames[depth++] = ret;
    void** next = static_cast<void**>(fp[0]);
    // Sanity: stack grows down, so next > fp.  Stop on
    // implausible / corrupt frame chain.
    if (next <= fp) break;
    fp = next;
  }
  out->depth = depth;
  return depth;
}

// static
void CpuProfiler::on_signal(int /*sig*/, siginfo_t* /*info*/,
                            void* /*ucontext*/) {
  CpuProfiler* self = g_instance_;
  if (!self || self->hz_.load(std::memory_order_relaxed) == 0) return;
  // Atomic increment claims a ring slot for this sample.  Wraps
  // naturally via the bitmask; older samples get overwritten.
  const uint64_t idx = self->head_.fetch_add(
      1, std::memory_order_relaxed) & (kRingSize - 1);
  capture_stack(&self->ring_[idx]);
}

void CpuProfiler::start(int hz) {
  // Always (re)install: sigaction is idempotent and stop() leaves
  // hz_ = 0 which makes the handler a no-op until restart.
  if (hz <= 0) {
    stop();
    return;
  }
  // Bind the singleton pointer — the handler runs in any thread
  // and needs a way back to the instance.
  (void)instance();

  struct sigaction sa{};
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sa.sa_sigaction = &CpuProfiler::on_signal;
  sigemptyset(&sa.sa_mask);
  if (sigaction(sample_signal(), &sa, nullptr) != 0) {
    LOG(ERROR) << "[cpu_profiler] sigaction failed: " << strerror(errno);
    return;
  }

  // Tear down any previous timer before rearming with the new rate.
  if (timer_id_ != 0) {
    timer_delete(reinterpret_cast<timer_t>(timer_id_));
    timer_id_ = 0;
  }

  // SIGEV_SIGNAL → kernel raises sample_signal() globally; the
  // signal is delivered to whichever thread is currently runnable
  // and not blocking it.  That gives us a uniformly-distributed
  // sample across all our threads (1 main + 4 cameras + admin +
  // log + supervisor).
  struct sigevent sev{};
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo  = sample_signal();
  timer_t tid;
  if (timer_create(CLOCK_MONOTONIC, &sev, &tid) != 0) {
    LOG(ERROR) << "[cpu_profiler] timer_create failed: "
               << strerror(errno);
    return;
  }
  timer_id_ = reinterpret_cast<uintptr_t>(tid);

  const long ns_per_tick = 1000000000L / hz;  // NOLINT(runtime/int)
  struct itimerspec its{};
  its.it_interval.tv_sec  = ns_per_tick / 1000000000L;
  its.it_interval.tv_nsec = ns_per_tick % 1000000000L;
  its.it_value = its.it_interval;
  if (timer_settime(tid, 0, &its, nullptr) != 0) {
    LOG(ERROR) << "[cpu_profiler] timer_settime failed: "
               << strerror(errno);
    timer_delete(tid);
    timer_id_ = 0;
    return;
  }

  hz_.store(hz, std::memory_order_release);
  LOG(INFO) << "[cpu_profiler] sampling at " << hz << " Hz";
}

void CpuProfiler::stop() {
  hz_.store(0, std::memory_order_release);
  if (timer_id_ != 0) {
    timer_delete(reinterpret_cast<timer_t>(timer_id_));
    timer_id_ = 0;
  }
  // Block the signal so any in-flight delivery is no-op'd by the
  // hz_ == 0 guard above.  We don't uninstall the sigaction; that
  // would race with a concurrent signal currently in flight.
  LOG(INFO) << "[cpu_profiler] stopped";
}

std::string CpuProfiler::snapshot() const {
  // Snapshot the ring head so we know how many samples to walk and
  // we don't read partially-written entries from the producer.
  // hz_ == 0 short-circuits to a status line so the endpoint
  // produces useful output even when sampling is disabled.
  if (hz_.load(std::memory_order_acquire) == 0) {
    return "cpu profiler disabled — set --cpu_profile_hz > 0 to enable\n";
  }

  // Aggregate by exact stack content.  Key is a packed string of
  // address bytes — fastest to build and equality-compare.
  std::map<std::string, uint64_t> counts;
  const uint64_t head_now = head_.load(std::memory_order_acquire);
  const uint64_t total = std::min<uint64_t>(head_now, kRingSize);
  for (uint64_t i = 0; i < total; ++i) {
    const Sample& s = ring_[i];
    if (s.depth <= 0) continue;
    std::string key(reinterpret_cast<const char*>(s.frames),
                    static_cast<size_t>(s.depth) * sizeof(void*));
    counts[key]++;
  }

  // Sort by count desc.
  std::vector<std::pair<std::string, uint64_t>> rows;
  rows.reserve(counts.size());
  for (auto& kv : counts) rows.emplace_back(std::move(kv.first), kv.second);
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  std::string out;
  out.reserve(8192);
  char hdr[256];
  std::snprintf(hdr, sizeof(hdr),
      "cpu profile: %llu samples in ring, %zu unique stacks\n"
      "format: <count>  pc=<addr0>  pc=<addr1> ...\n"
      "resolve via: addr2line -e /usr/bin/onvif-recorder <addr>\n\n",
      static_cast<unsigned long long>(total),  // NOLINT(runtime/int)
      rows.size());
  out += hdr;

  for (const auto& [key, cnt] : rows) {
    char line[64];
    std::snprintf(line, sizeof(line), "%6llu",
        static_cast<unsigned long long>(cnt));  // NOLINT(runtime/int)
    out += line;
    const void* const* frames =
        reinterpret_cast<const void* const*>(key.data());
    const size_t n = key.size() / sizeof(void*);
    for (size_t i = 0; i < n; ++i) {
      char addr[32];
      std::snprintf(addr, sizeof(addr), "  %p", frames[i]);
      out += addr;
    }
    out += '\n';
  }
  return out;
}

}  // namespace onvif
