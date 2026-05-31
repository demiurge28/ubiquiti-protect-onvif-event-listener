# ONVIF Event Recorder

Bridges **third-party ONVIF cameras** into UniFi Protect. It listens to ONVIF
WS-PullPoint event streams and writes detection events (person, vehicle, animal,
package) to the UniFi Protect PostgreSQL database so they appear natively in the
Protect UI — including timeline clips, thumbnails, smart detection search, and
security alarms.

It can also add smart detection to **first-party UniFi cameras that lack native
AI** (e.g. G3 Instant). See [First-party camera support](#first-party-camera-support).

---

## Fork additions

This fork extends the original [demiurge28/ubiquiti-protect-onvif-event-listener](https://github.com/demiurge28/ubiquiti-protect-onvif-event-listener)
with the following improvements.

### Per-profile ONVIF snapshot selection and resolution discovery

Multi-channel cameras — fisheye models, PTZ cameras with e-PTZ channels, and
multi-sensor units — expose multiple **ONVIF media profiles**, each with its own
snapshot endpoint and sensor resolution. UniFi Protect typically stores only
one profile's URL and defaults to 1920×1080 for all cameras.

This fork adds automatic per-profile discovery in a **single `GetProfiles` SOAP
exchange** per (re)connect:

- `GetServices` detects the ONVIF Media service.
- `GetProfiles` returns all available profile tokens plus each profile's
  embedded `VideoEncoderConfiguration/Resolution`.
- `GetSnapshotUri(ProfileToken)` retrieves the per-channel snapshot URL for the
  selected profile.
- The discovered URL **and** resolution override the Protect-stored values for
  all subsequent thumbnail fetches and bbox grid computations — without touching
  the database.
- A new `--camera_snapshot_profiles` flag (and admin UI **Cameras** card entry)
  lets you pin a specific profile token per camera.
- Failures at any step fall back silently to the Protect-stored URL and the
  1920×1080 default.

**Snapshot URL priority** (highest wins):
1. `--camera_snapshot_urls` explicit path override
2. ONVIF-discovered URL from `GetSnapshotUri`
3. Protect-stored `thirdPartyCameraInfo.snapshotUrl`

**Resolution priority** (highest wins):
1. `--camera_resolutions` explicit override
2. ONVIF-discovered from `GetProfiles` VideoEncoderConfiguration
3. 1920×1080 fallback

See [FLAGS.md § Per-profile snapshot selection](FLAGS.md#per-profile-snapshot-selection)
for the full reference, vendor token examples, and limitations.

### HTTPS snapshot support

Detection thumbnail fetches now work against `https://` camera snapshot URLs.
Previously any `https://` URL would fail at TLS certificate verification because
no SSL options were set on the curl handle.

- By default (`--snapshot_tls_verify=false`) the recorder accepts **self-signed
  certificates**, which is the common case for IP cameras.
- Set `--snapshot_tls_verify=true` in the admin UI or `/etc/default/onvif-recorder.local`
  to enforce strict CA-chain validation for cameras that present a CA-signed certificate.
- Snapshot timeout extended 5 s → 10 s to accommodate TLS handshake latency.

### Bug fixes

- **Diagnostic dump tarball race** — the admin UI's *Download diagnostic dump*
  button previously wrote all concurrent requests to the same
  `/tmp/onvif-dump.tar.gz`, corrupting output when two sessions triggered a
  dump simultaneously. Each request now uses a unique path derived from its
  own `mkdtemp()` working directory.

### Build improvements

**ARM64 native test config** (`--config=arm64_native`) — enables the full
Bazel test suite to run on **Apple Silicon Macs** via `docker + colima`
without needing an x86-64 build host:

```bash
./build-in-docker.sh --test   # auto-detects ARM64 and uses arm64_native config
```

Key changes:
- `build-in-docker.sh`: architecture-aware Bazelisk download (was hardcoded
  to `linux-amd64`, breaking on ARM64 Docker containers).
- New `--config=arm64_native` in `.bazelrc`: uses `//third_party` from-source
  builds + `openssl_arm64_native`/`libcurl_arm64_native` instead of the
  x86→ARM64 cross-compile sysroot.
- `test/Makefile`: missing `camera_emulators.cpp` source added to the
  detection recorder build.

**All 20 tests pass** on both x86-64 Linux and ARM64 Apple Silicon.

---

## Installation on Dream Router / Dream Machine

### Windows installer (no terminal required)

Download the latest `OnvifRecorderInstaller-v*.exe` from
[Releases](https://github.com/demiurge28/ubiquiti-protect-onvif-event-listener/releases)
and run it. The GUI:

1. Walks you through enabling SSH on your router
   (**UniFi OS → System → Advanced → SSH**).
2. Tests the connection with the root password you set.
3. Adds the apt repository + installs the package, streaming progress live.
4. Saves the credentials (password encrypted with Windows DPAPI) in
   `HKCU\Software\OnvifRecorderInstaller` so subsequent launches offer
   one-click upgrade, re-install, or uninstall.

The installer is unsigned, so Windows SmartScreen will prompt the first
time — click **More info → Run anyway**.

### Install via .deb (recommended for macOS/Linux users)

Download the latest `.deb` from
[Releases](https://github.com/demiurge28/ubiquiti-protect-onvif-event-listener/releases),
copy it to your UniFi device, and install:

```bash
# On your Mac/Linux machine — substitute the actual version number:
DEV=root@<router-ip>
VER=1.7.0
curl -fsSL -O \
  https://github.com/demiurge28/ubiquiti-protect-onvif-event-listener/releases/download/v${VER}/onvif-recorder_${VER}_arm64.deb
scp onvif-recorder_${VER}_arm64.deb ${DEV}:/tmp/
ssh ${DEV} "dpkg -i /tmp/onvif-recorder_${VER}_arm64.deb"
```

Or directly on the router over SSH:

```bash
VER=1.7.0
curl -fsSL -O \
  https://github.com/demiurge28/ubiquiti-protect-onvif-event-listener/releases/download/v${VER}/onvif-recorder_${VER}_arm64.deb
dpkg -i onvif-recorder_${VER}_arm64.deb
```

`dpkg -i` auto-enables the systemd service and two daily timers (channel sync,
auto-update). Detections will appear in UniFi Protect within seconds of the
first motion event. Service logs:

```bash
journalctl -u onvif-recorder -f
```

**What happens on first start:**

1. The Protect user ID is **auto-discovered** from the unifi-core database
   and cached to `/var/lib/onvif-recorder/protect-user-id`.
2. The Protect UI is **live-patched** so third-party cameras appear in the
   alarm creation picker.
3. nginx is patched to expose the admin UI at `https://<device>/onvif/admin/`.
4. Third-party ONVIF cameras are loaded from the Protect database and smart
   detection flags are enabled for them.

No flags are required for the default setup.

### Upgrading

Repeat the same `dpkg -i` command with the new version number. The package
migrates configuration automatically; no flags or database changes are needed.

### Admin UI

After install, manage the package without SSH at
`https://<device>/onvif/admin/`:

- Force an update check
- Enable / disable auto-updates and change the schedule
- Edit configuration flags
- Uninstall the package

### Uninstall

```bash
apt-get remove onvif-recorder          # stops service, rolls back DB changes
apt-get purge  onvif-recorder          # also removes /etc/onvif-recorder and state
```

### Legacy install (deprecated)

Prior releases shipped as a raw binary at `/root/onvif_recorder`. The `.deb`
postinst migrates an existing manual install automatically on first upgrade —
no action required.

---

## Configuration

For day-to-day changes — toggling NanoDet override, picking which first-party
cameras get smart-detect, raising the per-camera rate limit — use the admin
UI: open `https://<router-ip>/onvif/admin/` while logged in to UniFi OS.
Save & restart applies the change live.

The form covers detection, rate limiting, first-party cameras (tickbox list),
RTSP audio, and verbose logging.  Anything not set in the UI falls back to
the command-line flag.  See [`FLAGS.md`](FLAGS.md) for the full reference and
the more advanced flags that aren't in the UI (database paths, change log,
rollback, etc.).

### Example: enable verbose logging only

```
Admin UI -> Logging -> verbose -> true -> Save & restart
```

equivalent to:

```bash
echo 'ONVIF_RECORDER_FLAGS="--verbose"' \
    > /etc/default/onvif-recorder.local
systemctl restart onvif-recorder
```

### Example: route detections from a wildlife camera as `animal`

```
Admin UI -> Detection -> camera_object_types
  -> 192.168.1.108=animal -> Save & restart
```

### Example: select a specific ONVIF media profile for snapshots

Multi-channel cameras (fisheye, e-PTZ) can expose several ONVIF media profiles,
each with its own snapshot URL. By default the recorder auto-discovers the first
available profile via `GetProfiles` + `GetSnapshotUri`. To pin a specific profile:

```
Admin UI -> Cameras -> camera_snapshot_profiles
  -> 192.168.1.108=MainStream -> Save & restart
```

Leave the field empty to keep auto-discovery. Use `--raw_log` to inspect the
profile tokens your camera advertises:

```bash
/usr/bin/onvif-recorder --verbose --raw_log=/tmp/onvif-raw.jsonl
# grep GetProfiles in /tmp/onvif-raw.jsonl to see available profile tokens
```

### Example: cameras with HTTPS snapshot URLs

If your camera's snapshot URL uses `https://` (e.g. as advertised by `GetSnapshotUri`
or set via `--camera_snapshot_urls`), the recorder accepts self-signed certificates
by default — no configuration needed. To enforce strict CA-chain validation instead:

```
Admin UI -> Cameras -> snapshot_tls_verify -> true -> Save & restart
```

equivalent to:

```bash
echo 'ONVIF_RECORDER_FLAGS="--snapshot_tls_verify=true"' \
    >> /etc/default/onvif-recorder.local
systemctl restart onvif-recorder
```

Leave at `false` (the default) if your cameras use self-signed or no certificate.

---

## Troubleshooting

**Check the log for errors:**
```bash
journalctl -u onvif-recorder -f
```

**Enable verbose output** to see per-camera lifecycle events:
```bash
systemctl stop onvif-recorder
/usr/bin/onvif-recorder --verbose
```

**Camera not working?** Capture a raw diagnostic log and open a GitHub issue:
```bash
/usr/bin/onvif-recorder --verbose --raw_log=/tmp/onvif-raw.jsonl
# Let it run 60+ seconds, then Ctrl+C
# Attach /tmp/onvif-raw.jsonl to your issue
```

**Camera went offline and did not reconnect?** The recorder automatically retries.
After 3 consecutive failures it pauses for up to 1 hour, then resumes. If a camera
reboot takes longer than expected, restart the service:
```bash
systemctl restart onvif-recorder
```

---

## Building from source

### Prerequisites

- **Ubuntu 24.04** x86_64 build host (Ubuntu 22.04 also works)
- [Bazelisk](https://github.com/bazelbuild/bazelisk) installed at `~/.local/bin/bazel`
- Git with submodule support

### 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/demiurge28/ubiquiti-protect-onvif-event-listener.git
cd ubiquiti-protect-onvif-event-listener
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

### 2. Install Bazelisk

```bash
mkdir -p ~/.local/bin
curl -fsSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o ~/.local/bin/bazel
chmod +x ~/.local/bin/bazel
```

### 3. Install apt dependencies

All C libraries are built from source via Bazel. Only build tools are needed:

```bash
sudo apt-get install -y \
  build-essential clang cmake ninja-build perl \
  python3-cpplint
```

### 4. Build

```bash
# x86_64 native binary
scripts/bz build --config=x86 //:onvif_recorder

# ARM64 release binary (PGO + ThinLTO, for Dream Router / Dream Machine)
scripts/bz build --config=arm64_release //:onvif_recorder

# All tests
scripts/bz test --config=x86 //test:all
```

The ARM64 build downloads its own sysroot automatically — no manual cross-toolchain
setup is required.

### Runtime dependencies (ARM64)

The ARM64 binary is almost entirely statically linked:

```
libm.so.6  libc.so.6  ld-linux-aarch64.so.1  libgcc_s.so.1
```

### Runtime dependencies (x86_64)

```
libm.so.6  libc.so.6  ld-linux-x86-64.so.1  libgcc_s.so.1
libldap.so.2  liblber.so.2
```

---

## Project structure

```
main.cpp                      — Binary entry point
onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
detection_recorder.hpp/.cpp   — Detection -> PostgreSQL recorder
alarm_notifier.hpp/.cpp       — Protect automation trigger + history
motion_poller.hpp/.cpp        — First-party camera motion -> smart detect poller
camera_change_log.hpp/.cpp    — Cameras-table change log and rollback
protect_ui_patch.hpp/.cpp     — Live-patch Protect UI alarm picker
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode
jpeg_crop.hpp/.cpp            — JPEG decode/crop/re-encode via libjpeg
object_detect.hpp/.cpp        — NanoDet-M object detection via NCNN
unifi_camera_config.hpp/.cpp  — Load camera credentials from Protect DB
```
