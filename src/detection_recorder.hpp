// Copyright 2026 Daniel W
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

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "object_detect.hpp"
#include "onvif_listener.hpp"

namespace onvif {

class AlarmNotifier;    // forward declaration; full definition in alarm_notifier.hpp
class MsrClient;        // forward declaration; full definition in msr_client.hpp
class WebhookNotifier;  // forward declaration; full definition in webhook_notifier.hpp

/**
 * DetectionRecorder
 *
 * Translates raw ONVIF events into human/vehicle detections and persists them
 * to a PostgreSQL database using a schema that mirrors the UniFi Protect event
 * tables, making third-party camera data structurally identical to data
 * produced by native Ubiquiti cameras.
 *
 * Supported ONVIF event formats
 * ------------------------------
 * AI events (suppress basic motion from the same camera once seen):
 *   tns1:RuleEngine/FieldDetector/ObjectsInside  (Hikvision IVS)
 *     source["Rule"]   = "Human" | "Vehicle"
 *     data["IsInside"] = "true"  | "false"
 *
 *   tns1:UserAlarm/IVA/HumanShapeDetect  (Hikvision knockoff / Dahua)
 *     data["State"]    = "true"  | "false"   (always maps to "person")
 *
 *   tns1:VehicleAlarm/IVB/VehicleDetect  (Hikvision knockoff / Dahua)
 *     data["State"]    = "true"  | "false"   (always maps to "vehicle")
 *
 *   tns1:RuleEngine/MyRuleDetector/PeopleDetect  (Reolink)
 *     data["State"]    = "true"  | "false"   (maps to "person")
 *
 *   tns1:RuleEngine/MyRuleDetector/VehicleDetect  (Reolink)
 *     data["State"]    = "true"  | "false"   (maps to "vehicle")
 *
 * Basic motion events (suppressed when camera emits AI events):
 *   tns1:RuleEngine/CellMotionDetector/Motion  (Amcrest, Lorex, UNVR etc.)
 *     data["IsMotion"] = "true"  | "false"   (maps to "person")
 *
 * Fallback motion (suppressed when camera emits CellMotionDetector or AI):
 *   tns1:VideoSource/MotionAlarm
 *     data["State"]    = "true"  | "false"   (maps to "person")
 *
 * Detection type mapping
 * ----------------------
 *   ONVIF "human"   -> smartDetectTypes ["person"],  smartDetectObjects.type "person"
 *   ONVIF "vehicle" -> smartDetectTypes ["vehicle"], smartDetectObjects.type "vehicle"
 *
 *   Generic motion events (CellMotionDetector, VideoSource/MotionAlarm) use the
 *   configured default_object_type (default "person") unless NanoDet-M is
 *   enabled (--detect / --detect_override), in which case the COCO class
 *   returned by the detector overrides the type (person / vehicle / animal).
 *   Per-camera overrides (set_camera_object_type) take priority over NCNN.
 *   Valid object types: person, vehicle, animal, package.
 *
 * Backend selection
 * -----------------
 *   conn is a libpq conninfo string
 *   (e.g. "host=localhost dbname=unifi user=protect")
 *
 * Database schema (mirrors UniFi Protect, tables must already exist)
 * -----------------------------------------------------------------------
 *
 *   events (
 *     id TEXT PK,                            -- UUID v4
 *     type TEXT,                             -- 'smartDetectZone'
 *     start INTEGER/BIGINT,                  -- ms since Unix epoch
 *     end INTEGER/BIGINT,                    -- ms since Unix epoch; NULL while active
 *     cameraId TEXT,                         -- camera IP address
 *     score INTEGER DEFAULT 0,
 *     smartDetectTypes TEXT DEFAULT '[]',    -- JSON array, e.g. '["person"]'
 *     metadata TEXT DEFAULT '{}',            -- JSON object
 *     locked INTEGER DEFAULT 0,
 *     thumbnailId TEXT,                      -- '<cameraIP>-<start_ms>'
 *     thumbnailFullfovId TEXT,
 *     packageThumbnailId TEXT,
 *     packageThumbnailFullfovId TEXT,
 *     deletedAt TEXT,
 *     deletionType TEXT,
 *     userId TEXT,
 *     partitionId TEXT,
 *     createdAt TEXT NOT NULL,               -- ISO-8601 UTC
 *     updatedAt TEXT NOT NULL                -- ISO-8601 UTC
 *   )
 *
 *   smartDetectObjects (
 *     id TEXT PK,                            -- UUID v4
 *     eventId TEXT NOT NULL,                 -- -> events.id
 *     thumbnailId TEXT,                      -- '<cameraIP>-<start_ms>'
 *     cameraId TEXT NOT NULL,                -- camera IP address
 *     type TEXT NOT NULL,                    -- 'person' or 'vehicle'
 *     attributes TEXT DEFAULT '{}',          -- JSON: {"confidence":0}
 *     smartDetectObjectGroupId TEXT,
 *     detectedAt INTEGER/BIGINT NOT NULL,    -- ms since Unix epoch
 *     metadata TEXT DEFAULT '{}',            -- JSON object
 *     createdAt TEXT NOT NULL,
 *     updatedAt TEXT NOT NULL
 *   )
 *
 * Thread safety
 * -------------
 * on_event() is fully thread-safe; the OnvifListener may call it from
 * multiple camera threads simultaneously.
 * set_snapshot() must be called before the listener starts.
 */
class DetectionRecorder {
 public:
  /// Factory: connects to PostgreSQL, verifies schema. Returns error on failure.
  static absl::StatusOr<std::unique_ptr<DetectionRecorder>> Create(
      const std::string& conn);

  ~DetectionRecorder();

  DetectionRecorder(const DetectionRecorder&)            = delete;
  DetectionRecorder& operator=(const DetectionRecorder&) = delete;
  DetectionRecorder(DetectionRecorder&&)                 = delete;
  DetectionRecorder& operator=(DetectionRecorder&&)      = delete;

  /// Register snapshot credentials for a camera. Must be called before run().
  void set_snapshot(const CameraConfig& cam);

  /// Set the directory where per-camera UBV thumbnail files are written.
  /// Each camera gets its own file: <dir>/<camera_ip>_thumbnails.ubv
  /// If not called, UBV files are not written (snapshots are still fetched
  /// for the thumbnailId reference if a snapshot URL is configured).
  /// Must be called before run().
  void set_ubv_dir(const std::string& dir);

  /// Set pre/post buffer padding applied to stored timestamps.
  /// start is stored as (detection_time - pre_sec*1000).
  /// end   is stored as (detection_end_time + post_sec*1000).
  /// Defaults: 2 s pre, 2 s post. Must be called before run().
  void set_buffer(uint32_t pre_sec, uint32_t post_sec);

  /// Merge consecutive detections from the same camera into one event if the
  /// new detection starts within @p sec seconds of the previous one ending.
  /// Coalescing re-uses the existing event row instead of creating a new one.
  /// Pass 0 to disable. Default: 30 s.
  void set_coalesce_window(uint32_t sec);

  /// Per-camera override for the coalesce window.  Useful for noisy cameras
  /// whose onboard tracker briefly loses sight and re-fires events as "new"
  /// tracks (issue #29) -- bump the window for that camera without affecting
  /// the others.  Pass @p sec = 0 to revert to the global window for the
  /// camera.  Thread-safe.
  void set_camera_coalesce_window(const std::string& camera_ip, uint32_t sec);

  /// Per-camera override for the snapshot URL path.  When set, the recorder
  /// fetches detection thumbnails from @c http://<camera_ip>@p path instead
  /// of the URL advertised by the camera's ONVIF service (which is wrong on
  /// some models -- common on Dahua, see issue #32).  Empty @p path removes
  /// any prior override.  Thread-safe.
  void set_camera_snapshot_url_path(const std::string& camera_ip,
                                    const std::string& path);

  /// Called (from an ONVIF listener camera thread) when GetSnapshotUri
  /// returns a snapshot URL for @p camera_ip.  Overrides the Protect-stored
  /// snapshot URL but defers to any explicit --camera_snapshot_urls path.
  /// Thread-safe; may be called at any time, including during on_event().
  void OnSnapshotUrlDiscovered(const std::string& camera_ip,
                               const std::string& snapshot_url);

  /// Called (from an ONVIF listener camera thread) when GetProfiles returns
  /// a VideoEncoderConfiguration/Resolution for @p camera_ip.  Overrides
  /// the 1920x1080 default but defers to any explicit --camera_resolutions
  /// entry.  Thread-safe; may be called at any time, including during
  /// on_event().
  void OnResolutionDiscovered(const std::string& camera_ip,
                              int width, int height);

  /// Set the pixel resolution for a specific camera.
  /// Used when computing the smartDetectObjectAreas bounding-box grid on
  /// Protect 7.1+.  When not set, 1920×1080 is assumed as the fallback.
  /// Must be called before run().
  void set_camera_resolution(const std::string& camera_ip, int width, int height);

  /// Drop new detection events from a camera once it has produced more than
  /// @p n events in the last rolling hour.  Pass 0 for unlimited. Default: 10.
  void set_max_events_per_hour(uint32_t n);

  /// Process one ONVIF event. Ignores events that are not human/vehicle
  /// detections. Thread-safe.
  void on_event(const OnvifEvent& ev);

  /// Scan the last @p days of ended events in the database and merge
  /// consecutive detections from the same camera with the same type if the
  /// gap between them is within the configured coalesce window.
  /// Returns the number of event rows deleted (merged away).
  /// Intended to be called once at startup, before run(). No-op if
  /// coalesce_window_sec is 0.
  int coalesce_history(int days = 30);

  /// Delete orphaned rows from all dependent tables (smartDetectRaws,
  /// thumbnails, smartDetectObjects, detectionLabels) in a single startup
  /// sweep.  A row is orphaned when its parent event no longer exists.
  /// smartDetectRaws uses a timestamp-range match (no eventId column);
  /// the others match directly on eventId.  thumbnails and smartDetectObjects
  /// are additionally filtered to third-party cameras; detectionLabels has no
  /// cameraId column so all orphaned rows are removed.
  /// Returns the total number of rows deleted across all tables.
  int purge_orphaned_rows();

  /// Delete stuck-open events (end IS NULL) from third-party cameras whose
  /// start timestamp is older than @p older_than_ms milliseconds.  These are
  /// left behind when the service is restarted mid-detection or when a camera
  /// fires rapid "started" events without matching "ended" events.  Dependent
  /// rows (smartDetectRaws, thumbnails, smartDetectObjects, detectionLabels)
  /// are removed first.  Returns the number of event rows deleted.
  int purge_stale_open_events(uint64_t older_than_ms = 300000);

  /// Set the object detector used to locate subjects for thumbnail cropping.
  /// If not called (or set to nullptr), falls back to the smart-crop heuristic.
  /// The detector must outlive the DetectionRecorder.
  void set_detector(const object_detect::ObjectDetector* detector);

  /// Set the alarm notifier used to trigger UniFi Protect security alarms on
  /// detection events.  If not called (or set to nullptr), alarms are not
  /// notified.  The notifier must outlive the DetectionRecorder.
  /// Must be called before run().
  void set_alarm_notifier(AlarmNotifier* notifier);

  /// Set the object type reported for generic motion events (CellMotionDetector,
  /// VideoSource/MotionAlarm) where the camera does not specify a type.
  /// Valid values: "person" (default), "vehicle", "animal", "package".
  void set_default_object_type(const std::string& type);

  /// Override the detection type for all events from a specific camera IP.
  /// The per-camera type takes precedence over the ONVIF-reported type and
  /// over the default_object_type.
  /// Valid values: "person", "vehicle", "animal", "package".
  void set_camera_object_type(const std::string& ip, const std::string& type);

  /// When override is true the detector is always run, ignoring any ONVIF
  /// bounding box provided by the camera. Has no effect if no detector is set.
  void set_detect_override(bool override);

  /// When enabled, thumbnail IDs use the MSR "{MAC}-{timestamp_ms}" format
  /// (length != 24) matching the native Protect convention, instead of the
  /// default 24-char hex format.  Both formats are always written to the DB
  /// thumbnails table and UBV files.  Must be called before run().
  void set_use_msr_thumbnail_ids(bool use_msr);

  /// Attach an MsrClient used to store third-party thumbnails via the MSR
  /// gRPC service (RecordingAPI.StoreSnapshots).  When set and the camera
  /// has a known MAC, the detection snapshot is forwarded to MSR, the
  /// returned native id replaces the local thumbnail id, and the
  /// DB `thumbnails` insert + UBV file write are skipped — the resulting
  /// thumbnail is indistinguishable from first-party Protect thumbnails.
  /// Must be called before run().  The client must outlive the recorder.
  void set_msr_client(MsrClient* msr);

  /// When true, an MSR StoreSnapshots failure causes the detection's
  /// thumbnail to be dropped entirely instead of falling back to a direct
  /// `thumbnails`-table INSERT.  Default false (preserves prior behaviour:
  /// fall back to DB write).  The fallback path is what compounds DB
  /// contention on a busy router — Protect's own writers are blocked on
  /// the same table — so production deployments should set this to true.
  void set_msr_drop_on_failure(bool drop);

  /// Register a camera name with the webhook notifier so detection payloads
  /// include the human-readable Protect camera name.  Must be called before
  /// the listener starts.
  void register_webhook_camera(const std::string& camera_ip,
                               const std::string& camera_name);

  /// Attach a WebhookNotifier that will be called for every new detection
  /// event (not for coalesced detections).  The notifier must outlive the
  /// recorder.  Must be called before run().  Thread-safe.
  void set_webhook_notifier(WebhookNotifier* notifier);

  /// When @p ms > 0 and a successful MSR StoreSnapshots id was returned
  /// for this camera within the last @p ms milliseconds, reuse that id
  /// instead of calling MSR again.  This is the queue-coalescing
  /// behaviour: bursts of detections for the same camera collapse onto
  /// a single MSR snapshot, so MSR's worker pool isn't saturated by
  /// near-duplicate snapshots.  Each event still gets its own DB rows
  /// (event/SDO/raw/track/labels), but they reference the cached MSR
  /// thumbnail.  Set to 0 to disable (default 0 — legacy behaviour:
  /// every event makes its own MSR call).
  void set_msr_burst_window_ms(uint64_t ms);

  /// When false (the default), HTTPS snapshot fetches skip server certificate
  /// verification, accepting self-signed certificates commonly used by IP
  /// cameras.  Set to true to enforce strict CA-chain validation — only
  /// useful when cameras have a certificate issued by a trusted CA accessible
  /// from the recorder host.  Thread-safe.
  void set_snapshot_tls_verify(bool verify);

  /// Test observability — number of times MSR was actually contacted
  /// vs. how many events triggered an MSR-eligible path.  Reset by the
  /// hourly aggregate emitter.
  uint64_t msr_calls_for_testing() const;
  uint64_t msr_burst_reuses_for_testing() const;

  // Defined in detection_recorder.cpp -- public so concrete backends in the
  // .cpp translation unit can inherit from it without friendship.
  struct IDbBackend {
    virtual ~IDbBackend() = default;

    virtual absl::Status create_schema() = 0;

    /// Register a camera's identifiers before the listener starts.
    /// Stores ip->id and ip->mac for later lookups.
    virtual void register_camera(const std::string& ip,
                                 const std::string& id,
                                 const std::string& mac) = 0;

    /// Enable MSR-format thumbnail IDs ("{MAC}-{ts_ms}", length != 24).
    virtual void set_use_msr_thumbnail_ids(bool /*use_msr*/) {}

    /// Compute the thumbnailId string for an event.
    virtual std::string make_thumbnail_id(const std::string& camera_ip,
                                          uint64_t           ts_ms) = 0;

    /// True when the backend needs a snapshot fetched on detection.
    virtual bool needs_snapshot() const = 0;

    /// Insert one row into events.  The trailing two params let callers
    /// supply a richer-than-default events.metadata JSON (used on Protect 7.1+
    /// where the UI consumes detectedAreas / detectedThumbnails / zonesStatus /
    /// weather) and a thumbnailFullfovId.  Both default to empty for the
    /// legacy path: empty metadata -> hardcoded {"source":"onvif-recorder"};
    /// empty thumb_fullfov_id -> NULL column.
    virtual void insert_event(const std::string& id,
                              uint64_t           ts_ms,
                              const std::string& camera_ip,
                              const std::string& sdt_json,
                              const std::string& thumb_id,
                              const std::string& now_str,
                              const std::string& metadata = "",
                              const std::string& thumb_fullfov_id = "") = 0;

    /// Insert one row into smartDetectObjectAreas.  Only called on Protect
    /// 7.1+ where the UI uses this table for the bbox overlay.  Default impl
    /// is a no-op for mocks / older-Protect backends.
    virtual void insert_smart_detect_object_area(
        const std::string& /*id*/,
        const std::string& /*sdo_id*/,
        int /*bbox_x1*/, int /*bbox_y1*/, int /*bbox_x2*/, int /*bbox_y2*/,
        uint64_t /*detected_at_ms*/,
        uint64_t /*last_seen_ms*/,
        const std::string& /*now_str*/) {}

    virtual void insert_sdo(const std::string& id,
                            const std::string& event_id,
                            const std::string& thumb_id,
                            const std::string& camera_ip,
                            const std::string& obj_type,
                            const std::string& attributes,
                            uint64_t           ts_ms,
                            const std::string& now_str) = 0;

    virtual void update_event_end(const std::string& event_id,
                                  uint64_t           end_ms,
                                  const std::string& now_str) = 0;

    /// Store a JPEG thumbnail: INSERT INTO thumbnails.
    virtual void write_thumbnail(const std::string&              thumb_id,
                                 const std::string&              event_id,
                                 const std::string&              camera_ip,
                                 uint64_t                        ts_ms,
                                 const std::string&              now_str,
                                 const std::vector<unsigned char>& jpeg) = 0;

    /// Insert one row into smartDetectRaws with a minimal payload JSON.
    virtual void insert_smart_detect_raw(const std::string& /*id*/,
                                         const std::string& /*camera_ip*/,
                                         uint64_t           /*ts_ms*/,
                                         const std::string& /*obj_type*/,
                                         const std::string& /*now_str*/) {}

    /// Insert one row into smartDetectTracks with a payload JSON
    /// describing the detection.  Native first-party events always
    /// have a row here; without it the iOS app's Find Anything filter
    /// skips the event.
    virtual void insert_smart_detect_track(const std::string& /*id*/,
                                            const std::string& /*event_id*/,
                                            const std::string& /*camera_ip*/,
                                            uint64_t           /*start_ms*/,
                                            uint64_t           /*end_ms*/,
                                            const std::string& /*obj_type*/,
                                            int                /*confidence*/,
                                            const std::string& /*now_str*/) {}

    /// Upsert label names into the `labels` table and return their serial lid
    /// values. Existing names are returned from cache to avoid redundant queries.
    /// Returns an empty vector on failure or if not implemented.
    virtual std::vector<int> upsert_labels(
        const std::vector<std::string>& /*names*/,
        const std::string& /*now_str*/) { return {}; }

    /// Insert one row into `detectionLabels`.
    /// Pass object_id empty to store NULL (event-level row).
    /// The event-level row (objectId IS NULL) is required for the INNER JOIN
    /// in Protect's find-anything / detection-search endpoint.
    virtual void insert_detection_label(const std::string&      /*event_id*/,
                                        const std::string&      /*object_id*/,
                                        const std::vector<int>& /*lids*/,
                                        const std::string&      /*now_str*/) {}

    /// One row returned by query_recent_events().
    struct EventSummary {
      std::string id;
      std::string camera_id;
      std::string sdt_json;   // smartDetectTypes text, e.g. '["person"]'
      uint64_t    start_ms{0};
      uint64_t    end_ms{0};
    };

    /// Fetch ended (non-NULL end) smartDetectZone events from the last @p days,
    /// sorted by (camera_id, sdt_json, start_ms). Used by coalesce_history().
    virtual std::vector<EventSummary> query_recent_events(int /*days*/) {
      return {};
    }

    /// Extend the surviving event's end to new_end_ms, then delete the merged
    /// event (from_id) and its dependent rows (smartDetectObjects, thumbnails,
    /// detectionLabels, smartDetectRaws).
    virtual void coalesce_events(const std::string& /*into_id*/,
                                  uint64_t           /*new_end_ms*/,
                                  const std::string& /*from_id*/,
                                  const std::string& /*now_str*/) {}

    /// Delete smartDetectRaws rows for third-party cameras whose timestamp
    /// does not fall within any existing smartDetectZone event for that camera.
    /// Returns the number of rows deleted.
    virtual int purge_orphaned_smart_detect_raws() { return 0; }

    /// Delete thumbnails rows for third-party cameras whose eventId no longer
    /// references an existing event. Returns the number of rows deleted.
    virtual int purge_orphaned_thumbnails() { return 0; }

    /// Delete smartDetectObjects rows for third-party cameras whose eventId no
    /// longer references an existing event. Returns the number of rows deleted.
    virtual int purge_orphaned_smart_detect_objects() { return 0; }

    /// Delete detectionLabels rows whose eventId no longer references an
    /// existing event. Returns the number of rows deleted.
    virtual int purge_orphaned_detection_labels() { return 0; }

    /// Delete all orphaned rows across smartDetectRaws, thumbnails,
    /// smartDetectObjects, and detectionLabels in a single transaction.
    /// Returns the total number of rows deleted.
    virtual int purge_all_orphaned_rows() { return 0; }

    /// Delete stuck-open events (end IS NULL) for third-party cameras whose
    /// start is older than older_than_ms, plus their dependent rows.
    /// All deletes run in a single transaction.
    /// Returns the number of event rows deleted.
    virtual int purge_stale_open_events(uint64_t /*older_than_ms*/) { return 0; }
  };

  /// Factory for testing: injects a custom backend (skips PostgreSQL connect).
  static absl::StatusOr<std::unique_ptr<DetectionRecorder>> CreateWithBackend(
      std::unique_ptr<IDbBackend> backend);

 private:
  DetectionRecorder();

  struct SnapshotInfo {
    std::string url;
    std::string user;
    std::string password;
  };

  std::unique_ptr<IDbBackend> db_;
  absl::Mutex mu_;

  uint64_t pre_buffer_ms_{2000};   // subtracted from event start timestamp
  uint64_t post_buffer_ms_{2000};  // added to event end timestamp

  // Snapshot info per camera IP -- written before run(), read-only after.
  std::map<std::string, SnapshotInfo> snapshot_info_;

  // Camera IP -> UniFi Protect UUID (from register_camera).
  // Written before run(); read-only after that.
  std::map<std::string, std::string> camera_ids_;

  // Camera IP -> MAC address (uppercase, no colons).
  // Written before run(); read-only after that.
  std::map<std::string, std::string> camera_macs_;

  // Base directory for UBV thumbnail files; empty = disabled.
  // When set, files are written using the native Protect path convention:
  //   {ubv_dir}/YYYY/MM/DD/{MAC}_0_thumbnails_{epoch_ms}.ubv
  std::string ubv_dir_;

  // Cache: MAC -> (date string "YYYY/MM/DD", resolved UBV file path).
  // Avoids directory scans on every thumbnail write.
  std::map<std::string, std::pair<std::string, std::string>> ubv_path_cache_;

  // Tracks the UUID of each open (not-yet-ended) event row in `events`.
  // Key: (camera_ip, detection_type)
  std::map<std::pair<std::string, std::string>, std::string> open_;

  // Cameras that have emitted at least one AI-level detection event
  // (FieldDetector or HumanShapeDetect).  CellMotionDetector events from
  // these cameras are suppressed to avoid PTZ-patrol false positives.
  std::set<std::string> ai_capable_cameras_;

  // Cameras that have emitted at least one CellMotionDetector event.
  // VideoSource/MotionAlarm events from these cameras are suppressed to
  // avoid double-counting (both topics fire simultaneously on most cameras).
  std::set<std::string> cell_motion_cameras_;

  // Optional object detector for thumbnail subject cropping.
  // Set before run(); read-only (non-owning pointer) after that.
  const object_detect::ObjectDetector* detector_{nullptr};

  // When true the detector is preferred over ONVIF-provided bounding boxes.
  bool detect_override_{false};

  // When true, thumbnail IDs use the MSR "{MAC}-{ts_ms}" format (len != 24).
  bool use_msr_thumb_ids_{false};

  // Optional alarm notifier. Set before run(); non-owning raw pointer.
  AlarmNotifier* alarm_notifier_{nullptr};

  // Optional webhook notifier. Set before run(); non-owning raw pointer.
  WebhookNotifier* webhook_notifier_{nullptr};

  // Optional MSR gRPC client. Set before run(); non-owning raw pointer.
  MsrClient* msr_client_{nullptr};

  // When true, on MSR failure, drop the snapshot entirely (no thumbnails
  // INSERT, no UBV write).  Default false (legacy fallback to DB write).
  bool msr_drop_on_failure_{false};

  // When > 0 and a recent MSR id is cached within this window for the
  // same camera, reuse it instead of calling MSR again.  Default 0 = off.
  uint64_t msr_burst_window_ms_{0};

  // Per-camera-mac most-recent successful MSR id and its wall-clock ms.
  struct MsrBurstCache {
    std::string id;          // last successful StoreSnapshots id
    uint64_t    ts_ms{0};    // wall-clock ms when stored
  };
  std::map<std::string, MsrBurstCache> msr_burst_cache_;

  // Test observability — counts updates as on_event flows.
  std::atomic<uint64_t> stats_msr_burst_reuses_{0};

  // Object type for generic motion events (CellMotionDetector, MotionAlarm).
  std::string default_object_type_{"person"};

  // Per-camera type overrides: all events from the keyed IP use this type.
  std::map<std::string, std::string> camera_object_types_;

  // Per-camera coalesce-window overrides (in ms).  Looked up under mu_ in
  // on_event(); absent entries fall back to coalesce_window_ms_.
  std::map<std::string, uint64_t> camera_coalesce_window_ms_;

  // Per-camera snapshot URL path overrides.  When present, on_event() rewrites
  // the snapshot URL to http://<camera_ip><path> instead of using the URL the
  // camera advertised via ONVIF.
  std::map<std::string, std::string> camera_snapshot_url_paths_;

  // Per-camera snapshot URLs discovered at runtime via ONVIF GetSnapshotUri.
  // Written from camera worker threads via OnSnapshotUrlDiscovered() (under
  // mu_); read in on_event() under the same lock.  Overrides the
  // Protect-stored snapshot_info_ URL but defers to camera_snapshot_url_paths_.
  std::map<std::string, std::string> discovered_snapshot_urls_;

  // Per-camera pixel resolution {width, height} for smartDetectObjectAreas
  // bounding-box grid computation on Protect 7.1+.
  // Written before run() via set_camera_resolution(); read-only after that.
  // Absent entries fall back to discovered_resolutions_ then 1920×1080.
  std::map<std::string, std::pair<int, int>> camera_image_sizes_;

  // Per-camera resolutions discovered at runtime via ONVIF GetProfiles
  // VideoEncoderConfiguration/Resolution.  Written from camera worker threads
  // via OnResolutionDiscovered() (under mu_); read in on_event() under the
  // same lock.  Overrides the 1920×1080 default but defers to the explicit
  // --camera_resolutions entries in camera_image_sizes_.
  std::map<std::string, std::pair<int, int>> discovered_resolutions_;

  // When false (the default), HTTPS snapshot fetches skip SSL peer / host
  // verification.  Most IP cameras use self-signed certificates; accepting
  // them without a full CA chain is the common-case behaviour.  Set to true
  // only when the camera presents a certificate signed by a trusted CA.
  bool snapshot_tls_verify_{false};

  // Coalescing: last completed event per (camera_ip, detection_type).
  // real_end_ms is the wall-clock time (ms) when the detection ended, without
  // the post-buffer offset.  0 means the event has been re-opened for coalescing.
  struct LastEventInfo {
    std::string event_id;
    uint64_t    real_end_ms{0};
  };
  std::map<std::pair<std::string, std::string>, LastEventInfo> last_event_;

  // Rate limiting: wall-clock creation timestamps (ms) of recent events per
  // camera IP.  Entries older than one hour are purged before each check.
  std::map<std::string, std::deque<uint64_t>> recent_event_times_;

  uint64_t coalesce_window_ms_{30000};  // --coalesce_window_sec * 1000
  uint32_t max_events_per_hour_{10};    // 0 = unlimited

  // Hourly aggregate counters (L2 in v1.4.8).  Incremented from on_event()
  // and the snapshot+MSR write paths; flushed to the journal once every
  // 3600s.  Reset to zero after each emit.
  std::atomic<uint64_t> stats_events_{0};
  std::atomic<uint64_t> stats_coalesced_{0};
  std::atomic<uint64_t> stats_rate_limited_{0};
  std::atomic<uint64_t> stats_snapshots_{0};
  std::atomic<uint64_t> stats_msr_ok_{0};
  std::atomic<uint64_t> stats_msr_fail_{0};
  std::atomic<uint64_t> stats_window_start_ms_{0};

  // Emit the hourly aggregate line if a window has elapsed.  Safe to call
  // from on_event() from any camera thread.
  void maybe_emit_hourly_stats();
};

}  // namespace onvif
