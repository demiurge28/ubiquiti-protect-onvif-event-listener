#!/usr/bin/env bash
# build-deb.sh — assemble a Debian package from a pre-built onvif_recorder binary.
#
# Prerequisites:
#   * Bazel binary already built. Paths we look in, in order:
#       --binary=<path>
#       ~/.cache/bazel/arm64_release/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder   (default for --arch=arm64)
#       ~/.cache/bazel/x86/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder              (default for --arch=amd64)
#   * debian/ staging tree in project root.
#   * dpkg-deb, fakeroot, xz-utils installed.
#
# Usage:
#   scripts/build-deb.sh --arch=arm64
#   scripts/build-deb.sh --arch=arm64 --version=1.5.0
#   scripts/build-deb.sh --arch=arm64 --binary=/path/to/onvif_recorder
#
# Output:
#   dist/onvif-recorder_<version>_<arch>.deb
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

ARCH=""
VERSION=""
BINARY=""

for arg in "$@"; do
    case "$arg" in
        --arch=*)    ARCH="${arg#--arch=}" ;;
        --version=*) VERSION="${arg#--version=}" ;;
        --binary=*)  BINARY="${arg#--binary=}" ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

case "$ARCH" in
    arm64|amd64) ;;
    "") echo "--arch=arm64|amd64 is required" >&2; exit 1 ;;
    *)  echo "Unsupported --arch=$ARCH (arm64|amd64)" >&2; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# Version: from git describe --tags --dirty, stripping leading "v".
# Debian versions must start with a digit; strip a leading "v" if present.
# ---------------------------------------------------------------------------
if [ -z "$VERSION" ]; then
    VERSION=$(git describe --tags --dirty 2>/dev/null || echo "0.0.0-dev")
    VERSION="${VERSION#v}"
    # dpkg-deb dislikes '+' and '/' but tolerates '~', '.', '-'. Replace '/' -> '.'.
    VERSION="${VERSION//\//.}"
fi

# ---------------------------------------------------------------------------
# Locate binary
# ---------------------------------------------------------------------------
if [ -z "$BINARY" ]; then
    case "$ARCH" in
        arm64)
            BINARY="$HOME/.cache/bazel/arm64_release/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder"
            ;;
        amd64)
            BINARY="$HOME/.cache/bazel/x86/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder"
            ;;
    esac
fi

if [ ! -f "$BINARY" ]; then
    echo "Binary not found at $BINARY" >&2
    echo "Build it first, or pass --binary=<path>." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Model files — optional; shipped if found next to the repo root.
# Downloaded by Bazel http_file into the runfiles tree; we accept either
# the repo root (manual curl) or a user-specified path.
# ---------------------------------------------------------------------------
MODEL_PARAM=""
MODEL_BIN=""
for candidate in "$SCRIPT_DIR/nanodet-plus-m_416.param" "$HOME/models/nanodet-plus-m_416.param" \
                 "$SCRIPT_DIR/nanodet_m.param" "$HOME/models/nanodet_m.param"; do
    if [ -f "$candidate" ]; then MODEL_PARAM="$candidate"; break; fi
done
for candidate in "$SCRIPT_DIR/nanodet-plus-m_416.bin" "$HOME/models/nanodet-plus-m_416.bin" \
                 "$SCRIPT_DIR/nanodet_m.bin" "$HOME/models/nanodet_m.bin"; do
    if [ -f "$candidate" ]; then MODEL_BIN="$candidate"; break; fi
done

# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------
STAGE="$SCRIPT_DIR/dist/deb-staging/$ARCH"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
mkdir -p "$STAGE/usr/bin"
mkdir -p "$STAGE/usr/libexec/onvif-recorder"
mkdir -p "$STAGE/usr/share/doc/onvif-recorder"
mkdir -p "$STAGE/usr/share/onvif-recorder/models"
mkdir -p "$STAGE/lib/systemd/system"
mkdir -p "$STAGE/etc/default"
mkdir -p "$STAGE/etc/onvif-recorder"

# Binary — strip to satisfy Debian policy (Bazel's release config leaves
# the binary unstripped because it keeps symbols useful for in-the-wild
# crash reports, but Debian lintian insists).  Pick the right strip tool
# for the target architecture.
install -m 0755 "$BINARY" "$STAGE/usr/bin/onvif-recorder"
case "$ARCH" in
    arm64)
        STRIP="$(command -v aarch64-linux-gnu-strip || true)"
        [ -z "$STRIP" ] && STRIP="$(command -v llvm-strip || true)"
        [ -z "$STRIP" ] && STRIP="strip"
        ;;
    amd64)
        STRIP="strip"
        ;;
esac
if ! "$STRIP" --strip-unneeded "$STAGE/usr/bin/onvif-recorder"; then
    echo "ERROR: strip failed. Install $STRIP (e.g. binutils-aarch64-linux-gnu for --arch=arm64)." >&2
    exit 1
fi

# Helper scripts
install -m 0755 debian/detect-channel.sh      "$STAGE/usr/libexec/onvif-recorder/detect-channel.sh"
install -m 0755 debian/install-apt-source.sh  "$STAGE/usr/libexec/onvif-recorder/install-apt-source.sh"

# systemd units
install -m 0644 debian/onvif-recorder.service              "$STAGE/lib/systemd/system/onvif-recorder.service"
install -m 0644 debian/onvif-recorder-channel.service      "$STAGE/lib/systemd/system/onvif-recorder-channel.service"
install -m 0644 debian/onvif-recorder-channel.timer        "$STAGE/lib/systemd/system/onvif-recorder-channel.timer"
install -m 0644 debian/onvif-recorder-autoupdate.service   "$STAGE/lib/systemd/system/onvif-recorder-autoupdate.service"
install -m 0644 debian/onvif-recorder-autoupdate.timer     "$STAGE/lib/systemd/system/onvif-recorder-autoupdate.timer"

# Default (EnvironmentFile) — conffile
install -m 0644 debian/default/onvif-recorder "$STAGE/etc/default/onvif-recorder"

# Default channel — conffile; postinst will not overwrite if present.
echo "stable" > "$STAGE/etc/onvif-recorder/channel"
chmod 0644 "$STAGE/etc/onvif-recorder/channel"

# Model files (optional)
if [ -n "$MODEL_PARAM" ] && [ -n "$MODEL_BIN" ]; then
    install -m 0644 "$MODEL_PARAM" "$STAGE/usr/share/onvif-recorder/models/nanodet-plus-m_416.param"
    install -m 0644 "$MODEL_BIN"   "$STAGE/usr/share/onvif-recorder/models/nanodet-plus-m_416.bin"
else
    echo "warning: NanoDet-Plus-m model files not found; package will not ship models." >&2
    echo "  Models will be auto-downloaded on first run." >&2
fi

# Docs
if [ -f README.md ]; then
    install -m 0644 README.md "$STAGE/usr/share/doc/onvif-recorder/README.md"
fi

# Debian-format copyright (points at the system-installed Apache-2.0 text
# instead of embedding the full licence verbatim, which lintian flags).
cat > "$STAGE/usr/share/doc/onvif-recorder/copyright" <<'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: onvif-recorder
Upstream-Contact: Daniel Williams <danielwoz@gmail.com>
Source: https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener

Files: *
Copyright: 2025-2026 Daniel Williams <danielwoz@gmail.com>
License: Apache-2.0
 On Debian systems, the full text of the Apache-2.0 license can be found
 in /usr/share/common-licenses/Apache-2.0.
COPYRIGHT
chmod 0644 "$STAGE/usr/share/doc/onvif-recorder/copyright"

# Debian changelog (compressed).  Not maintained per-release — we point at
# the upstream GitHub releases instead.
cat > "$STAGE/usr/share/doc/onvif-recorder/changelog.Debian" <<CHANGELOG
onvif-recorder ($VERSION) unstable; urgency=medium

  * See https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases
    for the full release history.

 -- Daniel Williams <danielwoz@gmail.com>  $(LC_ALL=C date -u -R)
CHANGELOG
gzip -n9 "$STAGE/usr/share/doc/onvif-recorder/changelog.Debian"
chmod 0644 "$STAGE/usr/share/doc/onvif-recorder/changelog.Debian.gz"

# lintian overrides: embedded static libs are intentional (Bazel statically
# links curl / libjpeg / libxml2 / libssh / gmp / idn2 / zlib so the binary
# runs on any UniFi OS version); shelling out to dpkg-query / systemctl is
# how the admin UI works; /usr/bin/onvif-recorder in prerm is our own binary.
mkdir -p "$STAGE/usr/share/lintian/overrides"
cat > "$STAGE/usr/share/lintian/overrides/onvif-recorder" <<'OVERRIDES'
embedded-library * [usr/bin/onvif-recorder]
uses-dpkg-database-directly [usr/bin/onvif-recorder]
maintainer-script-calls-systemctl *
command-with-path-in-maintainer-script *
no-manual-page *
OVERRIDES
chmod 0644 "$STAGE/usr/share/lintian/overrides/onvif-recorder"

# ---------------------------------------------------------------------------
# DEBIAN control + maintainer scripts
# ---------------------------------------------------------------------------
sed -e "s|@VERSION@|$VERSION|g" -e "s|@ARCH@|$ARCH|g" \
    debian/control.in > "$STAGE/DEBIAN/control"
install -m 0644 debian/conffiles "$STAGE/DEBIAN/conffiles"
install -m 0755 debian/preinst   "$STAGE/DEBIAN/preinst"
install -m 0755 debian/postinst  "$STAGE/DEBIAN/postinst"
install -m 0755 debian/prerm     "$STAGE/DEBIAN/prerm"
install -m 0755 debian/postrm    "$STAGE/DEBIAN/postrm"
install -m 0644 debian/triggers  "$STAGE/DEBIAN/triggers"

# ---------------------------------------------------------------------------
# Build .deb
# ---------------------------------------------------------------------------
OUT="$SCRIPT_DIR/dist/onvif-recorder_${VERSION}_${ARCH}.deb"
mkdir -p "$(dirname "$OUT")"

if command -v fakeroot >/dev/null 2>&1; then
    fakeroot dpkg-deb -Zxz --build "$STAGE" "$OUT"
else
    dpkg-deb -Zxz --build "$STAGE" "$OUT"
fi

echo "==> $OUT"
dpkg-deb --info "$OUT"
