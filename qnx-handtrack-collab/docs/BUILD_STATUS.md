# Build status

## What we proved

- Full MediaPipe Bazel graph compiles on the **host** (x86-64 Linux) after fixes:
  - glog QNX/host split (`com_github_glog_glog_qnx_fix.diff`)
  - `--define=xnn_enable_avxvnniint8=false` (GCC 11 lacks that ISA flag)
  - GCC 12 for XNNPACK (`-mavx512fp16`)
  - Host OpenCV headers (`libopencv-dev`)
  - Fixed `python_qnx` symlinks in `build_qnx_examples.sh`

- QNX ports for Pi target:
  - `libtensorflow-lite.so` at `build-files/ports/tensorflow/nto-aarch64-le/build/`
  - OpenCV aarch64 libs at `build-files/ports/opencv/nto-aarch64-le/build/lib/`

## What we have NOT done yet

1. **Cross-compile MediaPipe for aarch64 QNX** — last binary was x86-64 Linux ELF
2. **Create `.configure.bazelrc`** — required for `@local_config_cc//:cc-compiler-qnx_arm64`
3. **Deploy and run `hand_tracking_cpu` on Pi** with USB webcam
4. **Port Python control logic** (calibration, move, aim, shoot) to C++

## Why the host build happened

Bazel defaults to host platform (`k8`) when:
- `.configure.bazelrc` is missing
- `--platforms=//mediapipe:qnx_arm64` / `--cpu=qnx_arm64` not passed
- QNX `local_config_cc` compilers were never generated

The host build is useful as a compile test only. **The Pi needs `hand_tracking_cpu` as QNX ARM64.**

## Correct Pi target

```
//mediapipe/examples/qnx/hand_tracking:hand_tracking_cpu
```

Not `hand_tracking_tflite` (processes a video file, no live camera).

## python_qnx symlink fix (manual, if build script already ran)

If `python_qnx/bin` is a symlink to `/usr/bin`, fix before Bazel:

```bash
cd ~/qnx_workspace/mediapipe
rm -rf python_qnx/bin python_qnx/python
mkdir -p python_qnx/bin
ln -sf /usr/bin/python3.11 python_qnx/bin/python3   # host python for Bazel tooling
ln -sf python_qnx/bin/python3 python_qnx/python
# Point lib/ includes at QNX SDP python 3.13 for aarch64 target
ln -sf $QNX_TARGET/aarch64le/usr/lib/libpython3.13.so python_qnx/lib/libpython3.so
```

Adjust paths to match your Docker container's Python layout.
