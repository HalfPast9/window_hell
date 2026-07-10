#!/bin/sh
# deploy.sh — scp the QNX binary to the Pi 4B target and run it.
# Usage: TARGET_IP=192.168.1.42 ./deploy.sh
set -eu

TARGET_IP="${TARGET_IP:-192.168.x.x}"

if [ "$TARGET_IP" = "192.168.x.x" ]; then
    echo "warning: TARGET_IP not set, using placeholder ($TARGET_IP)." >&2
    echo "  usage: TARGET_IP=<pi-ip> ./deploy.sh" >&2
fi

BIN="bin/windowed-hell-qnx"
if [ ! -f "$BIN" ]; then
    echo "error: $BIN not found. Run 'make qnx' first." >&2
    exit 1
fi

scp "$BIN" "root@${TARGET_IP}:/tmp/"
ssh "root@${TARGET_IP}" "slay windowed-hell-qnx; /tmp/windowed-hell-qnx"
