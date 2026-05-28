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

#include "webhook_notifier.hpp"

#include <curl/curl.h>

#include <cinttypes>
#include <cstdio>
#include <map>
#include <string>

#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"
#include "util.hpp"

namespace onvif {

void WebhookNotifier::set_url(const std::string& url) {
  absl::MutexLock lk(&mu_);
  url_ = url;
}

void WebhookNotifier::set_tls_verify(bool verify) {
  absl::MutexLock lk(&mu_);
  tls_verify_ = verify;
}

void WebhookNotifier::register_camera(const std::string& camera_ip,
                                       const std::string& camera_name) {
  absl::MutexLock lk(&mu_);
  if (!camera_ip.empty())
    camera_names_[camera_ip] = camera_name;
}

void WebhookNotifier::notify(const std::string& obj_type,
                              const std::string& camera_ip,
                              const std::string& event_id,
                              uint64_t ts_ms) {
  // Snapshot config under lock; do HTTP I/O outside lock.
  std::string url;
  bool tls_verify;
  std::string camera_name;
  {
    absl::MutexLock lk(&mu_);
    url = url_;
    tls_verify = tls_verify_;
    auto it = camera_names_.find(camera_ip);
    if (it != camera_names_.end()) camera_name = it->second;
  }

  if (url.empty()) return;  // not configured

  // Build JSON payload.
  // {
  //   "event":        "detection_start",
  //   "type":         "person",
  //   "camera_ip":    "192.168.1.108",
  //   "camera_name":  "Front Door",
  //   "event_id":     "uuid-...",
  //   "timestamp_ms": 1748449200000,
  //   "timestamp":    "2026-05-28T06:00:00Z"
  // }
  using onvif::util::json_str;
  char ts_buf[24];
  std::snprintf(ts_buf, sizeof(ts_buf), "%" PRIu64, ts_ms);
  std::string body;
  body.reserve(256);
  body += "{\"event\":\"detection_start\"";
  body += ",\"type\":";      body += json_str(obj_type);
  body += ",\"camera_ip\":"; body += json_str(camera_ip);
  body += ",\"camera_name\":"; body += json_str(camera_name);
  body += ",\"event_id\":";  body += json_str(event_id);
  body += ",\"timestamp_ms\":"; body += ts_buf;
  body += ",\"timestamp\":"; body += json_str(util::utc_now_iso8601());
  body += "}";

  // POST the payload.
  CURL* curl = curl_easy_init();
  if (!curl) {
    LOG(ERROR) << "[webhook] curl_easy_init failed";
    return;
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: */*");

  curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);   // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);   // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  discard_cb);

  if (!tls_verify) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // NOLINT(runtime/int)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);  // NOLINT(runtime/int)
  }

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    LOG(WARNING) << "[webhook] POST " << url
                 << " failed: " << curl_easy_strerror(rc);
  } else {
    long http_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 200 && http_code < 300) {
      LOG(INFO) << "[webhook] POST " << url << " → " << http_code
                << " (" << obj_type << " on " << camera_ip << ")";
    } else {
      LOG(WARNING) << "[webhook] POST " << url << " HTTP " << http_code;
    }
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

}  // namespace onvif
