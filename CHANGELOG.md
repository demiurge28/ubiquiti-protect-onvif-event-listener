# Changelog

All notable changes to ONVIF Event Recorder will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Per-profile ONVIF snapshot selection** (`--camera_snapshot_profiles`): on each
  camera (re)connect the recorder now calls `GetServices`, detects a media service,
  calls `GetProfiles`, and calls `GetSnapshotUri(ProfileToken)` to obtain the
  per-channel snapshot URL. The discovered URL overrides the Protect-stored snapshot
  URL for all subsequent thumbnail fetches, with transparent fallback to the
  Protect-stored URL when discovery fails at any step. Resolves incorrect snapshot
  URLs on multi-profile cameras (fisheye, e-PTZ, multi-sensor units).
  - `--camera_snapshot_profiles` flag and admin UI **Cameras** card entry: accepts
    comma-separated `ip=token` pairs to pin a specific profile token, bypassing
    `GetProfiles` auto-discovery.
  - `CameraConfig::snapshot_profile` field; `SnapshotUrlCallback` type and
    `OnvifListener::set_snapshot_url_callback()` for decoupled URL update delivery.
  - `DetectionRecorder::OnSnapshotUrlDiscovered()`: thread-safe in-memory URL
    override that slots between the Protect-stored URL and any explicit
    `--camera_snapshot_urls` path override.
  - `xmlns:trt` (`http://www.onvif.org/ver10/media/wsdl`) namespace added to every
    SOAP envelope and XPath registry for Media service call support.

- **Automatic camera resolution discovery**: the `GetProfiles` response already
  called for snapshot profile selection also carries each profile's
  `VideoEncoderConfiguration/Resolution`. The recorder now extracts width×height
  from the selected profile in the same SOAP exchange — no additional round-trip.
  Discovered resolutions feed the Protect 7.1+ `smartDetectObjectAreas` bbox grid
  so the detection overlay aligns correctly on 4K and non-1080p cameras without
  manual `--camera_resolutions` entries.
  - Resolution priority: `--camera_resolutions` explicit override >
    ONVIF-discovered > 1920×1080 fallback. Explicit overrides are never clobbered.
  - `ResolutionCallback` type and `OnvifListener::set_resolution_callback()` for
    decoupled delivery to `DetectionRecorder`.
  - `DetectionRecorder::OnResolutionDiscovered()`: thread-safe in-memory update;
    `discovered_resolutions_` map slots between `camera_image_sizes_` and the
    1920×1080 default in the Protect 7.1+ enrichment path.

### Changed

### Fixed

- **Diagnostic dump tarball race** (`admin_server.cpp`): the
  `/api/diagnostic_dump` handler passed a hardcoded `/tmp/onvif-dump.tar.gz`
  path to `build_diagnostic_dump()`, so two concurrent dump requests from
  the admin UI would overwrite each other's output. The tarball path is now
  derived from the unique temp directory created by `mkdtemp()` (e.g.
  `/tmp/onvif-dump.ABC123.tar.gz`), making each request fully independent.
  `build_diagnostic_dump()` return type changed from `absl::Status` to
  `absl::StatusOr<std::string>` to surface the path to the caller.

### Removed

### Dependencies

### Migration Notes

- No action required for existing deployments. The feature is opt-in: cameras
  that do not advertise an ONVIF media service in `GetServices` are unaffected.
  Auto-discovery is enabled automatically for cameras that do advertise one.

<!-- [Unreleased]: https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/compare/vX.Y.Z...HEAD -->
