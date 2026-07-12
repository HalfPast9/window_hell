#!/bin/sh
# deploy.sh — scp the QNX binary to the Pi target and run it.
# Usage: TARGET_IP=192.168.1.42 ./deploy.sh [-- extra args passed to the game]
# Target: QNX 8.0 self-hosted developer desktop (quickstart image), qnxuser
# login, no root SSH. Default sshd MAC negotiation fails against this image
# ("Corrupted MAC on input") — every ssh/scp call pins -o MACs=hmac-sha2-256.
# Remote home (/data/home/qnxuser) is the deploy target, not /tmp: root "/"
# is the read-only IFS image and reports 100% full.
#
# Multiplayer (two Pis over the direct ethernet link, see BRINGUP_LOG.md):
#   TARGET_IP=<pi-A-ip> NO_RUN=1 ./deploy.sh          # copy only, don't launch
#   TARGET_IP=<pi-A-ip> ./deploy.sh -- --mp-host
#   TARGET_IP=<pi-B-ip> ./deploy.sh -- --mp-join 192.168.100.1
# NO_RUN=1 copies the binary and exits without launching — useful when both
# Pis need the binary staged before either is started (avoid one side's
# WAITING_ROOM handshake retries running out before the other side is ready;
# in practice this rarely matters since retries are indefinite, but it keeps
# the two launches decoupled from scp timing).
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

# Everything after `--` is passed straight through to the remote binary
# (e.g. `--mp-host`, `--mp-join <ip>`).
GAME_ARGS=""
while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then shift; GAME_ARGS="$*"; break; fi
    shift
done

scp $SSH_OPTS "$BIN" "${TARGET_USER}@${TARGET_IP}:${REMOTE_DIR}/"

if [ "${NO_RUN:-}" = "1" ]; then
    echo "deploy: copied only (NO_RUN=1); not launching."
    exit 0
fi

ssh $SSH_OPTS "${TARGET_USER}@${TARGET_IP}" \
    "slay -f windowed-hell-qnx 2>/dev/null; chmod +x ${REMOTE_DIR}/windowed-hell-qnx; ${REMOTE_DIR}/windowed-hell-qnx ${GAME_ARGS}"
