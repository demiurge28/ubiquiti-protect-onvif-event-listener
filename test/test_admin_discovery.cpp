// Copyright 2026 Ben
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
 * test_admin_discovery.cpp
 *
 * Exercises the admin page's ONVIF profile discovery code path
 * (AdminServer::DiscoverProfiles) against a MediaServiceEmulator.
 *
 * The emulator serves GetProfiles, GetStreamUri, and GetSnapshotUri
 * responses.  The test verifies that the returned JSON contains
 * the expected profile tokens, resolutions, encodings, and URIs.
 */

#include <iostream>
#include <string>

#include "admin_server.hpp"
#include "camera_emulators.hpp"

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

// Tiny helper: check that json contains a substring.
static bool has(const std::string& json, const std::string& sub) {
  return json.find(sub) != std::string::npos;
}

int main() {
  // Start a MediaServiceEmulator on an ephemeral port.
  MediaServiceEmulator emu;
  emu.start();

  const std::string addr = emu.local_address();  // "127.0.0.1:<port>"

  // ---- Basic discovery with no active token ----
  {
    const std::string json =
        onvif::AdminServer::DiscoverProfiles(addr, "admin", "pass123", "");

    check(json != "[]",
          "discovery returns non-empty result");
    check(has(json, "\"token\":\"MainStream\""),
          "MainStream profile token present");
    check(has(json, "\"token\":\"SubStream\""),
          "SubStream profile token present");

    // Resolution
    check(has(json, "\"width\":3840"),
          "MainStream width=3840");
    check(has(json, "\"height\":2160"),
          "MainStream height=2160");
    check(has(json, "\"width\":640"),
          "SubStream width=640");
    check(has(json, "\"height\":480"),
          "SubStream height=480");

    // Encoding
    check(has(json, "\"encoding\":\"H265\""),
          "MainStream encoding=H265");
    check(has(json, "\"encoding\":\"H264\""),
          "SubStream encoding=H264");

    // RTSP stream URIs (rewritten to 127.0.0.1:<port>)
    check(has(json, "\"rtsp_url\":\"rtsp://"),
          "rtsp_url present");
    check(has(json, "/stream/MainStream"),
          "MainStream RTSP path present");
    check(has(json, "/stream/SubStream"),
          "SubStream RTSP path present");

    // Snapshot URIs
    check(has(json, "\"snapshot_url\":\"http://"),
          "snapshot_url present");
    check(has(json, "profile=MainStream"),
          "MainStream snapshot URL present");
    check(has(json, "profile=SubStream"),
          "SubStream snapshot URL present");

    // No active profile
    check(!has(json, "\"active\":true"),
          "no profile marked active when active_token is empty");
  }

  // ---- Discovery with an active_token matching SubStream ----
  {
    const std::string json =
        onvif::AdminServer::DiscoverProfiles(addr, "admin", "pass123",
                                              "SubStream");

    check(has(json, "\"active\":true"),
          "SubStream is marked active");
    // MainStream should be active:false.
    // Count occurrences of "active":false vs true.
    size_t false_count = 0;
    size_t pos = 0;
    while ((pos = json.find("\"active\":false", pos)) != std::string::npos) {
      ++false_count;
      pos += 14;
    }
    check(false_count == 1,
          "exactly one profile is active:false (MainStream)");
  }

  // ---- Discovery against unreachable host returns empty array ----
  {
    const std::string json =
        onvif::AdminServer::DiscoverProfiles(
            "192.0.2.1:1", "admin", "pass", "");
    check(json == "[]",
          "unreachable host returns empty JSON array");
  }

  emu.stop();

  std::cerr << "PASS " << g_pass << " / FAIL " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
