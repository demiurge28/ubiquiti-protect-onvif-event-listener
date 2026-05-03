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

#include "listener_status.hpp"

#include <string>
#include <unordered_set>

#include "absl/synchronization/mutex.h"

namespace onvif {

namespace {

absl::Mutex& mu() {
  static absl::Mutex m;
  return m;
}

std::unordered_set<std::string>& needs_admin() {
  static std::unordered_set<std::string> s;
  return s;
}

}  // namespace

void mark_camera_needs_onvif_admin(const std::string& host) {
  absl::MutexLock lk(&mu());
  needs_admin().insert(host);
}

void clear_camera_needs_onvif_admin(const std::string& host) {
  absl::MutexLock lk(&mu());
  needs_admin().erase(host);
}

bool camera_needs_onvif_admin(const std::string& host) {
  absl::MutexLock lk(&mu());
  return needs_admin().count(host) > 0;
}

}  // namespace onvif
