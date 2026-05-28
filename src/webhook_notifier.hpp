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

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "absl/synchronization/mutex.h"

namespace onvif {

/**
 * WebhookNotifier
 *
 * Sends an HTTP POST to a user-configured URL on every new detection event.
 * The POST body is a JSON object describing the detection, suitable for
 * consumption by Home Assistant, ntfy.sh, Telegram bots, custom scripts, etc.
 *
 * Payload (application/json):
 * {
 *   "event":        "detection_start",
 *   "type":         "person",          // person | vehicle | animal | package
 *   "camera_ip":    "192.168.1.108",
 *   "camera_name":  "Front Door",      // Protect display name; empty if unknown
 *   "event_id":     "<UUID>",          // events table row id
 *   "timestamp_ms": 1748449200000,     // ms since Unix epoch (event start)
 *   "timestamp":    "2026-05-28T06:00:00Z"  // ISO-8601 UTC
 * }
 *
 * Behaviour:
 * - Webhooks are fired only for new events, not for detections coalesced
 *   into an existing event (same semantics as AlarmNotifier).
 * - The POST is fire-and-forget with a 5-second timeout; failures are logged
 *   but do not affect detection recording.
 * - TLS: self-signed certificates are accepted by default
 *   (webhook_tls_verify=false).  Set to true to enforce CA-chain validation.
 * - Thread-safe: notify() may be called concurrently from multiple camera
 *   threads.
 *
 * Usage:
 *   WebhookNotifier notifier;
 *   notifier.set_url("https://hooks.example.com/onvif");
 *   // On each new detection event:
 *   notifier.notify("person", "192.168.1.108", "Front Door", event_id, ts_ms);
 */
class WebhookNotifier {
 public:
  WebhookNotifier() = default;

  /// Set the target URL.  Empty string disables the notifier (no-op notify).
  /// Thread-safe; may be called at any time.
  void set_url(const std::string& url);

  /// When false (the default), HTTPS webhook endpoints accept self-signed
  /// certificates.  Set to true to enforce strict CA-chain validation.
  /// Thread-safe; may be called at any time.
  void set_tls_verify(bool verify);

  /// Register a mapping from camera IP to display name so the payload
  /// includes the human-readable Protect camera name.  Must be called
  /// before the listener starts (before notify() calls begin).
  void register_camera(const std::string& camera_ip,
                       const std::string& camera_name);

  /// Fire the webhook for a detection start event.
  ///   obj_type   -- "person", "vehicle", "animal", or "package"
  ///   camera_ip  -- camera IP address
  ///   event_id   -- UUID of the inserted events row
  ///   ts_ms      -- event start timestamp (ms since Unix epoch)
  /// No-op when no URL is configured.  Thread-safe.
  void notify(const std::string& obj_type,
              const std::string& camera_ip,
              const std::string& event_id,
              uint64_t ts_ms);

 private:
  static size_t discard_cb(char*, size_t s, size_t n, void*) {
    return s * n;
  }

  absl::Mutex mu_;
  std::string url_;          // protected by mu_
  bool        tls_verify_{false};  // protected by mu_
  // Camera IP -> Protect display name.  Written before run(); effectively
  // read-only after that, so notify() reads it without the lock after a
  // one-time snapshot under the lock at notify() entry.
  std::map<std::string, std::string> camera_names_;  // protected by mu_
};

}  // namespace onvif
