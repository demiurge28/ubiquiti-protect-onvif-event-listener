#!/bin/sh
# install.sh — one-line installer for onvif-recorder.
#
#   curl -fsSL https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/install.sh | sh
#
# Detects the user's UniFi Protect release channel (Stable / Release Candidate
# / Early Access) and configures APT to pull matching onvif-recorder builds.
# Safe to re-run; it just (re)writes the keyring + sources list.
set -e

REPO_URL="https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener"
KEYRING=/usr/share/keyrings/onvif-recorder-archive-keyring.gpg
SRC_FILE=/etc/apt/sources.list.d/onvif-recorder.list
CHANNEL_FILE=/etc/onvif-recorder/channel

if [ "$(id -u)" -ne 0 ]; then
    echo "This installer must run as root." >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get not found — this installer only supports Debian-based UniFi OS." >&2
    exit 1
fi

DEB_ARCH=$(dpkg --print-architecture)
case "$DEB_ARCH" in
    arm64) ;;
    *)
        echo "Unsupported architecture: $DEB_ARCH (only arm64 is published)." >&2
        exit 1
        ;;
esac

# 1. GPG keyring.
echo "==> Fetching signing key ..."
mkdir -p /usr/share/keyrings
curl -fsSL "$REPO_URL/onvif-recorder.gpg" -o /tmp/onvif-recorder.gpg
gpg --dearmor < /tmp/onvif-recorder.gpg > "$KEYRING"
chmod 0644 "$KEYRING"
rm -f /tmp/onvif-recorder.gpg

# 2. Channel detection (best effort; falls back to stable).
mkdir -p /etc/onvif-recorder
if [ ! -f "$CHANNEL_FILE" ]; then
    echo "stable" > "$CHANNEL_FILE"
fi
CHANNEL=$(tr -d '[:space:]' < "$CHANNEL_FILE")
case "$CHANNEL" in
    stable|rc|early-access) ;;
    *) CHANNEL=stable; echo "stable" > "$CHANNEL_FILE" ;;
esac
echo "==> Using channel: $CHANNEL"

# 3. APT source.
echo "deb [arch=$DEB_ARCH signed-by=$KEYRING] $REPO_URL $CHANNEL main" \
    > "$SRC_FILE"

# 4. Install.
echo "==> apt-get update ..."
apt-get update \
    -o Dir::Etc::sourcelist="$SRC_FILE" \
    -o Dir::Etc::sourceparts=- \
    -o APT::Get::List-Cleanup=0

echo "==> Installing onvif-recorder ..."
apt-get install -y onvif-recorder

echo ""
echo "Done.  Manage it from https://<this-device>/onvif/admin/"
echo "Channel can be changed by editing $CHANNEL_FILE or via the admin UI."
