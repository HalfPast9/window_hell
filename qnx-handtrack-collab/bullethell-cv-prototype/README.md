# Bullet Hell CV Prototype

Two-hand webcam joystick controller using MediaPipe Hand Landmarker (Python Tasks API).
Sends `HtPacket` UDP datagrams to [windowed-hell](https://github.com/tan-pixel/qnx-handtrack-collab) on the Pi (`:47800`).

## Setup

```bash
python -m venv .venv
.venv\Scripts\activate   # Windows
pip install opencv-python mediapipe
```

Download `hand_landmarker.task` from [MediaPipe Hand Landmarker](https://developers.google.com/mediapipe/solutions/vision/hand_landmarker) and place it in this directory.

## Run

```bash
# send controls to the game on the Pi (same Wi-Fi)
python joystick_controller.py <pi-ip>

# optional
python joystick_controller.py <pi-ip> --port 47800 --gain 4.0
```

The Pi game must be running with handtrack enabled (`WH_HANDTRACK=1` or `--handtrack`).

- **Left hand**: movement (calibrated center, deadzone)
- **Right hand**: aim direction; close fist to shoot
- Press **Esc** to quit
