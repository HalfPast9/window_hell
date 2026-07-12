# Raspberry Pi deployment

## Flash QSTI

1. Install in QNX Software Center:
   - Pi 4: `com.qnx.qnx800.quickstart.rpi4`
   - Pi 5: `com.qnx.qnx800.quickstart.rpi5`
2. Flash the `.img` to a 32GB+ SD card (Pi Imager / Balena Etcher)
3. Boot, connect Ethernet or Wi-Fi
4. SSH: `ssh qnxuser@qnxpi.local` (password: `qnxuser`)

## Deploy (once aarch64 binary exists)

```bash
PI=qnxuser@qnxpi.local
MP=~/qnx_workspace/mediapipe

ssh $PI 'mkdir -p /data/home/qnxuser/handtrack/{bin,libs,models,graphs}'

scp $MP/bazel-bin/mediapipe/examples/qnx/hand_tracking/hand_tracking_cpu \
    $PI:/data/home/qnxuser/handtrack/bin/

scp $MP/mediapipe/modules/hand_landmark/hand_landmark_full.tflite \
    $MP/mediapipe/modules/palm_detection/palm_detection_full.tflite \
    $PI:/data/home/qnxuser/handtrack/models/

scp $MP/mediapipe/graphs/hand_tracking/hand_tracking_desktop_live.pbtxt \
    $PI:/data/home/qnxuser/handtrack/graphs/

scp /tmp/staging/aarch64le/usr/local/lib/libopencv*.so* \
    $PI:/data/home/qnxuser/handtrack/libs/

scp ~/qnx_workspace/build-files/ports/tensorflow/nto-aarch64-le/build/libtensorflow-lite.so \
    $PI:/data/home/qnxuser/handtrack/libs/
```

On Pi, resolve remaining deps:

```bash
export LD_LIBRARY_PATH=/data/home/qnxuser/handtrack/libs:$LD_LIBRARY_PATH
ldd /data/home/qnxuser/handtrack/bin/hand_tracking_cpu
```

## Run live demo

```bash
cd /data/home/qnxuser/handtrack
export LD_LIBRARY_PATH=/data/home/qnxuser/handtrack/libs:$LD_LIBRARY_PATH

./bin/hand_tracking_cpu \
  --calculator_graph_config_file=graphs/hand_tracking_desktop_live.pbtxt \
  --camera_unit=0
```

Try `--camera_unit=1` etc. if the webcam is not unit 0. Plug in a **USB UVC webcam** for simplest setup.

## Game control integration (TODO)

The stock demo only renders landmark overlays. Port logic from `prototype/virtual_joystick.py`:

- 8s calibration, hands open
- Sort hands by screen X (not MediaPipe left/right labels)
- Left palm → `move_x`, `move_y` (deadzone 0.05)
- Right palm vector → `aim_angle` 0–360°
- Right fist (landmark 12 near wrist) → `shoot`

Wire output to Space Invaders via shared struct or QNX IPC between vision and game threads.
