#!/bin/sh
# deploy.sh — scp the QNX binary to the Pi target and run it.
# Usage: TARGET_IP=192.168.1.42 ./deploy.sh
# Target: QNX 8.0 self-hosted developer desktop (quickstart image), qnxuser
# login, no root SSH. Default sshd MAC negotiation fails against this image
# ("Corrupted MAC on input") — every ssh/scp call pins -o MACs=hmac-sha2-256.
# Remote home (/data/home/qnxuser) is the deploy target, not /tmp: root "/"
# is the read-only IFS image and reports 100% full.
set -eu

TARGET_IP="${TARGET_IP:-192.168.x.x}"
TARGET_USER="${TARGET_USER:-qnxuser}"
SSH_OPTS="-o MACs=hmac-sha2-256"
REMOTE_DIR="/data/home/qnxuser"

if [ "$TARGET_IP" = "192.168.x.x" ]; then
    echo "warning: TARGET_IP not set, using placeholder ($TARGET_IP)." >&2
    echo "  usage: TARGET_IP=<pi-ip> ./deploy.sh" >&2
fi

BIN="bin/windowed-hell-qnx"
if [ ! -f "$BIN" ]; then
    echo "error: $BIN not found. Run 'make qnx' first." >&2
    exit 1
fi

scp $SSH_OPTS "$BIN" "${TARGET_USER}@${TARGET_IP}:${REMOTE_DIR}/"
ssh $SSH_OPTS "${TARGET_USER}@${TARGET_IP}" \
    "slay -f windowed-hell-qnx 2>/dev/null; chmod +x ${REMOTE_DIR}/windowed-hell-qnx; ${REMOTE_DIR}/windowed-hell-qnx"
