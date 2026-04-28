// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <signal.h>  // siginfo_t

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace onvif {

// Async-signal-safe periodic CPU sampler.
//
// When start(hz) is called with hz > 0, a POSIX timer fires
// SIGRTMIN+1 at the requested rate; the signal handler captures
// up to kMaxFrames return addresses from the interrupted thread by
// walking frame pointers (the binary is built with
// gperftools_enable_frame_pointers=ON, so this is reliable).  Each
// sample is written into a fixed-size lock-free ring; snapshot()
// aggregates ring contents into a stack-hash → count table and
// renders a sorted text dump for /api/cpuz.
//
// hz = 0 disables sampling entirely; the handler is uninstalled and
// the ring goes idle.  All work happens in the handler — no
// background thread, no malloc, no mutex acquires.
class CpuProfiler {
 public:
  static CpuProfiler& instance();

  // Start sampling at @p hz times per second.  Replaces any prior
  // timer.  hz <= 0 stops sampling.
  void start(int hz);

  // Stop sampling.  Idempotent.
  void stop();

  // Whether sampling is currently active.
  bool running() const { return hz_.load() > 0; }

  // Render the current ring contents as text.  Format: one block per
  // unique stack, sorted by sample count descending.
  std::string snapshot() const;

  // Per-frame and per-ring sizes — public so the test can size buffers.
  static constexpr int kMaxFrames = 32;
  static constexpr int kRingSize  = 4096;

 private:
  CpuProfiler() = default;

  struct Sample {
    int   depth;
    void* frames[kMaxFrames];
  };

  // Signal handler.  Async-signal-safe: only walks frame pointers
  // and writes to the lock-free ring.
  static void on_signal(int sig, siginfo_t* info, void* ucontext);

  // Capture the calling thread's stack into @p out.  Pure
  // pointer-chasing, no library calls.
  static int capture_stack(Sample* out);

  std::atomic<int>      hz_{0};
  std::atomic<uint64_t> head_{0};
  Sample                ring_[kRingSize];
  // Opaque timer handle stored as uintptr_t to avoid leaking
  // <signal.h> / <time.h> types into headers that include this one.
  uintptr_t             timer_id_{0};

  static CpuProfiler* g_instance_;
};

}  // namespace onvif
