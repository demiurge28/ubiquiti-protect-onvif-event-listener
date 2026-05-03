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

/**
 * test_patch_watcher.cpp
 *
 * Verifies that PatchWatcher fires its callback on a debounced delay,
 * coalesces a burst of writes into a single fire, and re-establishes
 * watches after the watched directory is removed and recreated.
 */

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "patch_watcher.hpp"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

static std::string make_tmpdir(const char* suffix) {
  const char* base = std::getenv("TEST_TMPDIR");
  std::string dir = (base ? base : "/tmp");
  dir += "/test_patch_watcher_";
  dir += suffix;
  // Best-effort: rm + mkdir.  rmdir only works if empty -- use system().
  std::string rm = "rm -rf '" + dir + "'";
  (void)std::system(rm.c_str());  // NOLINT(runtime/int)
  ::mkdir(dir.c_str(), 0755);
  return dir;
}

static void touch(const std::string& path) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << "// fake js\n";
}

// Wait up to `timeout_ms` for `pred()` to return true; returns true if
// the predicate became true before the timeout.
template <typename Pred>
static bool wait_for(int timeout_ms, Pred pred) {
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  while (clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

// 1. A single matching write fires the callback exactly once.
static void test_single_fire() {
  std::string dir = make_tmpdir("single");
  std::atomic<int> fires{0};

  std::vector<protect_ui::WatchSpec> specs = {{dir, {"swai"}}};
  protect_ui::PatchWatcher w(std::move(specs),
                              [&fires]() { fires.fetch_add(1); },
                              std::chrono::milliseconds(150));
  check(w.start(), "single: start");

  // Give the watcher a beat to install the inotify watch before we
  // write -- otherwise we race and may miss the event.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  touch(dir + "/swai-7.0.57.js");

  bool fired = wait_for(2000, [&]() { return fires.load() == 1; });
  check(fired, "single: callback fired once within 2s");
  // Verify it stays at 1 for the remainder of the debounce window plus
  // a small safety margin -- no spurious extra fires.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  check(fires.load() == 1, "single: still 1 after settle");
  w.stop();
}

// 2. A burst of writes within the debounce window collapses to one fire.
static void test_debounce_coalesce() {
  std::string dir = make_tmpdir("burst");
  std::atomic<int> fires{0};

  std::vector<protect_ui::WatchSpec> specs = {{dir, {"swai", "vantage"}}};
  protect_ui::PatchWatcher w(std::move(specs),
                              [&fires]() { fires.fetch_add(1); },
                              std::chrono::milliseconds(200));
  check(w.start(), "burst: start");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Five writes in rapid succession, well within the 200ms debounce.
  touch(dir + "/swai.js");
  touch(dir + "/swai-7.0.57.js");
  touch(dir + "/vantage.js");
  touch(dir + "/vantage-7.0.57.js");
  touch(dir + "/swai.js");

  bool fired = wait_for(2000, [&]() { return fires.load() >= 1; });
  check(fired, "burst: at least one fire");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  check(fires.load() == 1, "burst: coalesced to exactly one fire");
  w.stop();
}

// 3. Filenames that don't match the prefix list don't fire.
static void test_prefix_filter() {
  std::string dir = make_tmpdir("filter");
  std::atomic<int> fires{0};

  std::vector<protect_ui::WatchSpec> specs = {{dir, {"swai"}}};
  protect_ui::PatchWatcher w(std::move(specs),
                              [&fires]() { fires.fetch_add(1); },
                              std::chrono::milliseconds(150));
  check(w.start(), "filter: start");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  touch(dir + "/something-else.js");   // wrong prefix
  touch(dir + "/swai.txt");            // wrong extension

  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  check(fires.load() == 0, "filter: no fire on non-matching writes");

  // Now a matching write -- should fire once.
  touch(dir + "/swai.js");
  bool fired = wait_for(1000, [&]() { return fires.load() == 1; });
  check(fired, "filter: matching write does fire");
  w.stop();
}

// 4. After the watched directory is rmdir'd and recreated, the watcher
//    re-establishes its inotify watch and continues to fire.
static void test_rewatch_after_rmdir() {
  std::string dir = make_tmpdir("rewatch");
  std::atomic<int> fires{0};

  std::vector<protect_ui::WatchSpec> specs = {{dir, {"swai"}}};
  protect_ui::PatchWatcher w(std::move(specs),
                              [&fires]() { fires.fetch_add(1); },
                              std::chrono::milliseconds(150));
  check(w.start(), "rewatch: start");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First fire on the original dir.
  touch(dir + "/swai.js");
  bool fired1 = wait_for(2000, [&]() { return fires.load() == 1; });
  check(fired1, "rewatch: first fire on original dir");

  // Simulate an overlayfs rebase: delete the dir (which kills the
  // watch) and recreate it.
  std::string rm = "rm -rf '" + dir + "'";
  (void)std::system(rm.c_str());  // NOLINT(runtime/int)
  // Give the watcher time to observe IN_DELETE_SELF / IN_IGNORED and
  // schedule a rewatch.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  ::mkdir(dir.c_str(), 0755);
  // The watcher only re-adds watches when it observes a fs event, so
  // we need to give it a chance to attempt re-add; the rewatch trigger
  // happens after the IN_IGNORED event, so it should already have
  // tried to re-add by now (and may have succeeded or failed depending
  // on timing relative to the mkdir).  Either way, a subsequent write
  // after the mkdir should produce another inotify event that drives
  // a fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  touch(dir + "/swai.js");
  bool fired2 = wait_for(3000, [&]() { return fires.load() >= 2; });
  check(fired2, "rewatch: fires again after dir is recreated");
  w.stop();
}

// 5. fire_count() reflects the number of fires.
static void test_fire_count() {
  std::string dir = make_tmpdir("count");
  std::atomic<int> fires{0};

  std::vector<protect_ui::WatchSpec> specs = {{dir, {"swai"}}};
  protect_ui::PatchWatcher w(std::move(specs),
                              [&fires]() { fires.fetch_add(1); },
                              std::chrono::milliseconds(120));
  check(w.start(), "count: start");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  touch(dir + "/swai.js");
  wait_for(1000, [&]() { return fires.load() == 1; });
  // Wait past the debounce + a margin so the watcher is idle, then
  // fire again.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  touch(dir + "/swai.js");
  wait_for(1000, [&]() { return fires.load() == 2; });

  check(w.fire_count() == 2, "count: fire_count() == 2");
  w.stop();
}

// 6. Watching a non-existent directory does not crash; the watcher
//    starts cleanly and simply never fires.
static void test_missing_dir() {
  std::atomic<int> fires{0};
  std::vector<protect_ui::WatchSpec> specs = {
    {"/nonexistent/path/that/does/not/exist", {"swai"}},
  };
  protect_ui::PatchWatcher w(std::move(specs),
                              [&fires]() { fires.fetch_add(1); },
                              std::chrono::milliseconds(100));
  check(w.start(), "missing: start does not fail");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  check(fires.load() == 0, "missing: no fire on missing dir");
  w.stop();
}

int main() {
  test_single_fire();
  test_debounce_coalesce();
  test_prefix_filter();
  test_rewatch_after_rmdir();
  test_fire_count();
  test_missing_dir();

  std::cout << "passed: " << g_pass << '\n';
  std::cout << "failed: " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
