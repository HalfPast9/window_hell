#!/usr/bin/env python3
"""handtrack_fake_sender.py — stdlib-only UDP test harness for handtrack.c.

Sends synthetic HtPacket datagrams so the game's hand-tracking input path
(src/handtrack.c) can be exercised and verified end-to-end before any camera
or MediaPipe/TFLite work exists. Wire format must match src/handtrack.h
exactly (16 bytes, little-endian):

    uint32 magic; uint8 version; uint8 buttons; uint16 seq;
    int16 move_x_q; int16 move_y_q; uint16 aim_q; uint8 flags; uint8 reserved;

Usage:
    python3 tools/handtrack_fake_sender.py <target-ip> [--port 47800] --selftest

--selftest walks the ship in a circle (sweeping move_x/move_y through all
four compass directions), spins the aim angle through a full turn, and
pulses shoot on/off every second — enough to eyeball-verify axis signs,
8-way quantization, the aim_q convention, and the 250ms staleness timeout
(kill this script and confirm keyboard control resumes).
"""
import argparse
import math
import socket
import struct
import time

HT_MAGIC = 0x31525448  # "HTR1"
HT_VERSION = 1
HT_PORT_DEFAULT = 47800

HT_BTN_SHOOT = 0x01
HT_BTN_FOCUS = 0x02
HT_FLAG_MOVE_VALID = 0x01
HT_FLAG_AIM_VALID = 0x02

PACKET_FMT = "<IBBHhhHBB"
assert struct.calcsize(PACKET_FMT) == 16


def build_packet(seq, move_x, move_y, aim_turns, buttons, flags):
    move_x_q = max(-32767, min(32767, int(move_x * 32767)))
    move_y_q = max(-32767, min(32767, int(move_y * 32767)))
    aim_q = int((aim_turns % 1.0) * 65536) & 0xFFFF
    return struct.pack(
        PACKET_FMT,
        HT_MAGIC, HT_VERSION, buttons, seq & 0xFFFF,
        move_x_q, move_y_q, aim_q, flags, 0,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="game host IP (127.0.0.1 for same-machine testing)")
    ap.add_argument("--port", type=int, default=HT_PORT_DEFAULT)
    ap.add_argument("--rate", type=float, default=30.0, help="packets/sec")
    ap.add_argument("--selftest", action="store_true",
                     help="sweep move/aim/shoot instead of reading a real tracker")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dt = 1.0 / args.rate
    seq = 0
    t0 = time.monotonic()

    print(f"handtrack_fake_sender: sending to {args.target}:{args.port} @ {args.rate}Hz "
          f"(selftest={'on' if args.selftest else 'off'}), Ctrl-C to stop")

    try:
        while True:
            t = time.monotonic() - t0

            if args.selftest:
                # Compass sweep: move vector walks a slow circle so every
                # 8-way direction (and the hysteresis band between them)
                # gets exercised.
                move_x = math.sin(t * 0.5)
                move_y = math.cos(t * 0.5)
                aim_turns = (t * 0.15) % 1.0  # slow full-circle sweep
                shoot = (int(t) % 2) == 0  # 1s on / 1s off
                buttons = HT_BTN_SHOOT if shoot else 0
                flags = HT_FLAG_MOVE_VALID | HT_FLAG_AIM_VALID
            else:
                # Real tracker integration point: replace this branch with
                # actual hand-landmark-derived move/aim/buttons.
                move_x = move_y = 0.0
                aim_turns = 0.0
                buttons = 0
                flags = 0

            pkt = build_packet(seq, move_x, move_y, aim_turns, buttons, flags)
            sock.sendto(pkt, (args.target, args.port))
            seq += 1
            time.sleep(dt)
    except KeyboardInterrupt:
        print("\nhandtrack_fake_sender: stopped")


if __name__ == "__main__":
    main()
