#!/usr/bin/env python3
"""MediaPipe webcam joystick → windowed-hell HtPacket UDP sender.

Sends 16-byte HtPacket datagrams matching src/handtrack.h so a laptop
webcam can drive the game on a QNX Pi (or a local --handtrack build).

Coordinate convention (after cv2.flip selfie-mirror — do NOT negate X):
  move +x = screen right, move +y = screen down (OpenCV / game match)
  aim = atan2(dy, dx) as fractional turns → aim_q (same as sim.c)

Usage:
    python joystick_controller.py <pi-ip> [--port 47800] [--gain 4.0]
    python joystick_controller.py 127.0.0.1   # local game with --handtrack

Requires hand_landmarker.task in the working directory (or pass --model).
Press Esc to quit.
"""
from __future__ import annotations

import argparse
import math
import socket
import struct
import time

import cv2
import mediapipe as mp

# --- HtPacket wire format (must match window_hell/src/handtrack.h) ---
HT_MAGIC = 0x31525448  # "HTR1"
HT_VERSION = 1
HT_PORT_DEFAULT = 47800
HT_BTN_SHOOT = 0x01
HT_FLAG_MOVE_VALID = 0x01
HT_FLAG_AIM_VALID = 0x02
PACKET_FMT = "<IBBHhhHBB"
assert struct.calcsize(PACKET_FMT) == 16

DEADZONE = 0.05
CALIBRATION_DURATION = 8.0

BaseOptions = mp.tasks.BaseOptions
HandLandmarker = mp.tasks.vision.HandLandmarker
HandLandmarkerOptions = mp.tasks.vision.HandLandmarkerOptions
VisionRunningMode = mp.tasks.vision.RunningMode


def build_packet(seq, move_x, move_y, aim_turns, buttons, flags):
    move_x_q = max(-32767, min(32767, int(move_x * 32767)))
    move_y_q = max(-32767, min(32767, int(move_y * 32767)))
    aim_q = int((aim_turns % 1.0) * 65536) & 0xFFFF
    return struct.pack(
        PACKET_FMT,
        HT_MAGIC, HT_VERSION, buttons, seq & 0xFFFF,
        move_x_q, move_y_q, aim_q, flags, 0,
    )


def clamp1(v: float) -> float:
    return max(-1.0, min(1.0, v))


def parse_args():
    ap = argparse.ArgumentParser(description="Webcam hand tracker → HtPacket UDP")
    ap.add_argument("target", help="game host IP (Pi LAN IP, or 127.0.0.1 for local)")
    ap.add_argument("--port", type=int, default=HT_PORT_DEFAULT)
    ap.add_argument("--gain", type=float, default=4.0,
                    help="multiply raw palm deltas before clamp to [-1,1] "
                         "(game press threshold is 0.35)")
    ap.add_argument("--model", default="hand_landmarker.task",
                    help="path to MediaPipe hand_landmarker.task")
    ap.add_argument("--camera", type=int, default=0)
    return ap.parse_args()


def main():
    args = parse_args()

    options = HandLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=args.model),
        running_mode=VisionRunningMode.IMAGE,
        num_hands=2,
        min_hand_detection_confidence=0.5,
    )
    landmarker = HandLandmarker.create_from_options(options)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.target, args.port)
    seq = 0

    centers = {"Left": None, "Right": None}
    calibration_start_time = None

    cap = cv2.VideoCapture(args.camera)
    if not cap.isOpened():
        raise SystemExit(f"failed to open camera index {args.camera}")

    print(f"Starting camera → sending HtPackets to {args.target}:{args.port} "
          f"(gain={args.gain}). Esc to quit.")

    while cap.isOpened():
        success, frame = cap.read()
        if not success:
            continue

        if calibration_start_time is None:
            calibration_start_time = time.time()

        frame = cv2.flip(frame, 1)

        mp_image = mp.Image(
            image_format=mp.ImageFormat.SRGB,
            data=cv2.cvtColor(frame, cv2.COLOR_BGR2RGB),
        )
        results = landmarker.detect(mp_image)

        controls = {
            "move_x": 0.0,
            "move_y": 0.0,
            "aim_angle": 0.0,
            "aim_valid": False,
            "move_valid": False,
            "shoot": 0,
        }
        current_positions = {"Left": None, "Right": None}
        current_landmarks = {"Left": None, "Right": None}

        time_elapsed = time.time() - calibration_start_time
        active = time_elapsed > CALIBRATION_DURATION

        if results.hand_landmarks:
            # Sort by X — mirrored frame: left side of image = player's left hand
            sorted_hands = sorted(results.hand_landmarks, key=lambda lm: lm[0].x)

            if len(sorted_hands) == 2:
                current_landmarks["Left"] = sorted_hands[0]
                current_landmarks["Right"] = sorted_hands[1]
            elif len(sorted_hands) == 1:
                if sorted_hands[0][0].x < 0.5:
                    current_landmarks["Left"] = sorted_hands[0]
                else:
                    current_landmarks["Right"] = sorted_hands[0]

            for side in ("Left", "Right"):
                lms = current_landmarks[side]
                if not lms:
                    continue
                wrist = lms[0]
                middle_knuckle = lms[9]
                palm_x = (wrist.x + middle_knuckle.x) / 2.0
                palm_y = (wrist.y + middle_knuckle.y) / 2.0
                current_positions[side] = (palm_x, palm_y)

                if side == "Right" and active:
                    middle_tip = lms[12]
                    fist_dist = math.hypot(
                        middle_tip.x - wrist.x, middle_tip.y - wrist.y
                    )
                    if fist_dist < 0.20:
                        controls["shoot"] = 1

        if not active:
            time_left = int(CALIBRATION_DURATION - time_elapsed) + 1
            cv2.putText(
                frame,
                f"CALIBRATING: Keep hands open & steady! ({time_left}s)",
                (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2,
            )
            if current_positions["Left"]:
                centers["Left"] = current_positions["Left"]
            if current_positions["Right"]:
                centers["Right"] = current_positions["Right"]
            # Keepalive with flags=0 so the game's 250ms timeout does not flap
            buttons = 0
            flags = 0
            move_x = move_y = 0.0
            aim_turns = 0.0
        else:
            cv2.putText(
                frame,
                "ACTIVE: Move hands | Close right fist to shoot",
                (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2,
            )

            if centers["Left"] and current_positions["Left"]:
                dx = current_positions["Left"][0] - centers["Left"][0]
                dy = current_positions["Left"][1] - centers["Left"][1]
                if abs(dx) > DEADZONE:
                    controls["move_x"] = dx
                if abs(dy) > DEADZONE:
                    controls["move_y"] = dy
                controls["move_valid"] = True
                cv2.line(
                    frame,
                    (int(centers["Left"][0] * frame.shape[1]),
                     int(centers["Left"][1] * frame.shape[0])),
                    (int(current_positions["Left"][0] * frame.shape[1]),
                     int(current_positions["Left"][1] * frame.shape[0])),
                    (255, 0, 0), 2,
                )

            if centers["Right"] and current_positions["Right"]:
                dx = current_positions["Right"][0] - centers["Right"][0]
                dy = current_positions["Right"][1] - centers["Right"][1]
                if math.hypot(dx, dy) > DEADZONE:
                    angle_rad = math.atan2(dy, dx)
                    angle_deg = math.degrees(angle_rad)
                    if angle_deg < 0:
                        angle_deg += 360.0
                    controls["aim_angle"] = angle_deg
                    controls["aim_valid"] = True
                cv2.line(
                    frame,
                    (int(centers["Right"][0] * frame.shape[1]),
                     int(centers["Right"][1] * frame.shape[0])),
                    (int(current_positions["Right"][0] * frame.shape[1]),
                     int(current_positions["Right"][1] * frame.shape[0])),
                    (0, 0, 255), 2,
                )

            # Scale raw palm deltas so typical throws clear HT_PRESS_T (0.35)
            move_x = clamp1(controls["move_x"] * args.gain)
            move_y = clamp1(controls["move_y"] * args.gain)
            aim_turns = (controls["aim_angle"] % 360.0) / 360.0
            buttons = HT_BTN_SHOOT if controls["shoot"] else 0
            flags = 0
            if controls["move_valid"]:
                flags |= HT_FLAG_MOVE_VALID
            if controls["aim_valid"]:
                flags |= HT_FLAG_AIM_VALID

            print(
                f"Move: ({move_x:.2f}, {move_y:.2f}) | "
                f"Aim: {controls['aim_angle']:.0f}° | Shoot: {controls['shoot']} | "
                f"seq={seq}"
            )

        pkt = build_packet(seq, move_x, move_y, aim_turns, buttons, flags)
        sock.sendto(pkt, dest)
        seq = (seq + 1) & 0xFFFF

        cv2.putText(
            frame,
            f"SENDING -> {args.target}:{args.port}",
            (20, frame.shape[0] - 20),
            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (200, 200, 200), 1,
        )

        if centers["Left"]:
            cv2.circle(
                frame,
                (int(centers["Left"][0] * frame.shape[1]),
                 int(centers["Left"][1] * frame.shape[0])),
                30, (255, 0, 0), 2,
            )
        if centers["Right"]:
            cv2.circle(
                frame,
                (int(centers["Right"][0] * frame.shape[1]),
                 int(centers["Right"][1] * frame.shape[0])),
                30, (0, 0, 255), 2,
            )

        cv2.imshow("Virtual Joystick -> HtPacket", frame)
        if cv2.waitKey(1) & 0xFF == 27:
            break

    cap.release()
    cv2.destroyAllWindows()
    sock.close()


if __name__ == "__main__":
    main()
