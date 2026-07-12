#!/usr/bin/env python3
"""
Virtual joystick prototype — tested on dev machine (Linux + webcam).

Controls:
  - Left hand palm  → move_x, move_y
  - Right hand palm → aim_angle (0-360°)
  - Right fist      → shoot

Requires: opencv-python, mediapipe, hand_landmarker.task model file.
"""

import cv2
import mediapipe as mp
import math
import time

BaseOptions = mp.tasks.BaseOptions
HandLandmarker = mp.tasks.vision.HandLandmarker
HandLandmarkerOptions = mp.tasks.vision.HandLandmarkerOptions
VisionRunningMode = mp.tasks.vision.RunningMode

options = HandLandmarkerOptions(
    base_options=BaseOptions(model_asset_path='hand_landmarker.task'),
    running_mode=VisionRunningMode.IMAGE,
    num_hands=2,
    min_hand_detection_confidence=0.5)

landmarker = HandLandmarker.create_from_options(options)

centers = {"Left": None, "Right": None}
DEADZONE = 0.05
CALIBRATION_DURATION = 8.0
calibration_start_time = None

cap = cv2.VideoCapture(0)
print("Starting camera... Please look for the new OpenCV window on your taskbar!")

while cap.isOpened():
    success, frame = cap.read()
    if not success:
        continue

    if calibration_start_time is None:
        calibration_start_time = time.time()

    frame = cv2.flip(frame, 1)

    mp_image = mp.Image(
        image_format=mp.ImageFormat.SRGB,
        data=cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
    results = landmarker.detect(mp_image)

    controls = {"move_x": 0.0, "move_y": 0.0, "aim_angle": 0.0, "shoot": 0}
    current_positions = {"Left": None, "Right": None}
    current_landmarks = {"Left": None, "Right": None}

    time_elapsed = time.time() - calibration_start_time

    if results.hand_landmarks:
        sorted_hands = sorted(results.hand_landmarks, key=lambda lm: lm[0].x)

        if len(sorted_hands) == 2:
            current_landmarks["Left"] = sorted_hands[0]
            current_landmarks["Right"] = sorted_hands[1]
        elif len(sorted_hands) == 1:
            if sorted_hands[0][0].x < 0.5:
                current_landmarks["Left"] = sorted_hands[0]
            else:
                current_landmarks["Right"] = sorted_hands[0]

        for side in ["Left", "Right"]:
            lms = current_landmarks[side]
            if lms:
                wrist = lms[0]
                middle_knuckle = lms[9]
                palm_x = (wrist.x + middle_knuckle.x) / 2.0
                palm_y = (wrist.y + middle_knuckle.y) / 2.0
                current_positions[side] = (palm_x, palm_y)

                if side == "Right" and time_elapsed > CALIBRATION_DURATION:
                    middle_tip = lms[12]
                    fist_dist = math.hypot(
                        middle_tip.x - wrist.x, middle_tip.y - wrist.y)
                    if fist_dist < 0.20:
                        controls["shoot"] = 1

    if time_elapsed <= CALIBRATION_DURATION:
        time_left = int(CALIBRATION_DURATION - time_elapsed) + 1
        cv2.putText(
            frame,
            f"CALIBRATING: Keep hands open & steady! ({time_left}s)",
            (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
        if current_positions["Left"]:
            centers["Left"] = current_positions["Left"]
        if current_positions["Right"]:
            centers["Right"] = current_positions["Right"]
    else:
        cv2.putText(
            frame,
            "ACTIVE: Move hands | Close right fist to shoot",
            (20, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)

        if centers["Left"] and current_positions["Left"]:
            dx = current_positions["Left"][0] - centers["Left"][0]
            dy = current_positions["Left"][1] - centers["Left"][1]
            if abs(dx) > DEADZONE:
                controls["move_x"] = dx
            if abs(dy) > DEADZONE:
                controls["move_y"] = dy
            cv2.line(
                frame,
                (int(centers["Left"][0] * frame.shape[1]),
                 int(centers["Left"][1] * frame.shape[0])),
                (int(current_positions["Left"][0] * frame.shape[1]),
                 int(current_positions["Left"][1] * frame.shape[0])),
                (255, 0, 0), 2)

        if centers["Right"] and current_positions["Right"]:
            dx = current_positions["Right"][0] - centers["Right"][0]
            dy = current_positions["Right"][1] - centers["Right"][1]
            if math.hypot(dx, dy) > DEADZONE:
                angle_rad = math.atan2(dy, dx)
                angle_deg = math.degrees(angle_rad)
                controls["aim_angle"] = (
                    angle_deg if angle_deg >= 0 else angle_deg + 360)
            cv2.line(
                frame,
                (int(centers["Right"][0] * frame.shape[1]),
                 int(centers["Right"][1] * frame.shape[0])),
                (int(current_positions["Right"][0] * frame.shape[1]),
                 int(current_positions["Right"][1] * frame.shape[0])),
                (0, 0, 255), 2)

        print(
            f"Move: ({controls['move_x']:.2f}, {controls['move_y']:.2f}) | "
            f"Aim: {controls['aim_angle']:.0f}° | Shoot: {controls['shoot']}")

    if centers["Left"]:
        cv2.circle(
            frame,
            (int(centers["Left"][0] * frame.shape[1]),
             int(centers["Left"][1] * frame.shape[0])),
            30, (255, 0, 0), 2)
    if centers["Right"]:
        cv2.circle(
            frame,
            (int(centers["Right"][0] * frame.shape[1]),
             int(centers["Right"][1] * frame.shape[0])),
            30, (0, 0, 255), 2)

    cv2.imshow("Virtual Joystick Sandbox", frame)
    if cv2.waitKey(1) & 0xFF == 27:
        break

cap.release()
cv2.destroyAllWindows()
