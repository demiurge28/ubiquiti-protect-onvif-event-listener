#!/bin/sh
# install-apt-source.sh — (re)writes /etc/apt/sources.list.d/onvif-recorder.list
# based on the contents of /etc/onvif-recorder/channel.
set -e

CHANNEL_FILE=/etc/onvif-recorder/channel
SRC_FILE=/etc/apt/sources.list.d/onvif-recorder.list
KEYRING=/usr/share/keyrings/onvif-recorder-archive-keyring.gpg
REPO_URL=${ONVIF_REPO_URL:-https://demiurge28.github.io/ubiquiti-protect-onvif-event-listener}

CHANNEL=$(cat "$CHANNEL_FILE" 2>/dev/null | tr -d '[:space:]')
[ -n "$CHANNEL" ] || CHANNEL=stable

case "$CHANNEL" in
    stable|rc|early-access) ;;
    *)
        logger -t onvif-recorder-apt "unknown channel '$CHANNEL', using stable"
        CHANNEL=stable
        ;;
esac

DEB_ARCH=$(dpkg --print-architecture)

NEW="deb [arch=$DEB_ARCH signed-by=$KEYRING] $REPO_URL $CHANNEL main
"

if [ "$(cat "$SRC_FILE" 2>/dev/null)" != "$NEW" ]; then
    printf '%s' "$NEW" > "$SRC_FILE"
    logger -t onvif-recorder-apt "wrote $SRC_FILE ($CHANNEL)"
fi

exit 0
