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

#ifndef SRC_LISTENER_STATUS_HPP_
#define SRC_LISTENER_STATUS_HPP_

#include <string>

namespace onvif {

// Tiny process-global registry of "needs operator attention" hints
// surfaced from camera-thread code.  Read by the admin Camera Health
// card to render an actionable badge per camera.
//
// Today there is exactly one hint -- "ONVIF Administrator user
// required" -- emitted when an OnvifListener observes ter:NotAuthorized
// on CreatePullPointSubscription (issue #20 / Hikvision firmware,
// where ONVIF event subscriptions need a separate ONVIF user with
// Administrator privileges).  More hints can be added here as we get
// confident handling them.
//
// All functions are thread-safe.  The underlying registry is a static
// inside listener_status.cpp; no construction/teardown ceremony.

// Mark a camera (keyed by host/IP, matching cfg.ip) as needing an
// ONVIF Administrator user.
void mark_camera_needs_onvif_admin(const std::string& host);

// Clear the flag for a camera.  Call this from successful-path code
// paths (e.g. after a healthy subscription event) so transient auth
// failures don't stick forever.
void clear_camera_needs_onvif_admin(const std::string& host);

// Read the current flag.  Used by the admin Camera Health builder.
bool camera_needs_onvif_admin(const std::string& host);

}  // namespace onvif

#endif  // SRC_LISTENER_STATUS_HPP_
