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
 * test_listener_status.cpp
 *
 * Mark / clear / read of the per-camera ONVIF-admin hint flag.
 */

#include <iostream>
#include <string>

#include "listener_status.hpp"

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

int main() {
  using onvif::camera_needs_onvif_admin;
  using onvif::clear_camera_needs_onvif_admin;
  using onvif::mark_camera_needs_onvif_admin;

  // Initially nothing flagged.
  check(!camera_needs_onvif_admin("192.0.2.1"), "unset host: false");

  // Mark, then read.
  mark_camera_needs_onvif_admin("192.0.2.1");
  check(camera_needs_onvif_admin("192.0.2.1"), "marked host: true");
  check(!camera_needs_onvif_admin("192.0.2.2"), "other host unaffected: false");

  // Marking twice is idempotent.
  mark_camera_needs_onvif_admin("192.0.2.1");
  check(camera_needs_onvif_admin("192.0.2.1"), "double mark: still true");

  // Clear.
  clear_camera_needs_onvif_admin("192.0.2.1");
  check(!camera_needs_onvif_admin("192.0.2.1"), "cleared host: false");

  // Clearing an unmarked host is a no-op.
  clear_camera_needs_onvif_admin("192.0.2.99");
  check(!camera_needs_onvif_admin("192.0.2.99"), "clear of unset: still false");

  // Multiple hosts independent.
  mark_camera_needs_onvif_admin("a");
  mark_camera_needs_onvif_admin("b");
  check(camera_needs_onvif_admin("a") && camera_needs_onvif_admin("b"),
        "two hosts marked");
  clear_camera_needs_onvif_admin("a");
  check(!camera_needs_onvif_admin("a") && camera_needs_onvif_admin("b"),
        "clearing one leaves the other");
  clear_camera_needs_onvif_admin("b");

  std::cerr << "PASS " << g_pass << " / FAIL " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
