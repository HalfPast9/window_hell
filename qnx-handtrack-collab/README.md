# QNX Raspberry Pi Hand-Gesture Hackathon

Space Invaders–style game controlled by two-hand webcam tracking, targeting **QNX on Raspberry Pi 4/5**.

This repo captures our shared work: a working **Python prototype** (dev machine), **QNX ports build fixes**, and **MediaPipe cross-compile progress** toward running live hand tracking on the Pi.

## Quick links

| Doc | Contents |
|-----|----------|
| [docs/SETUP.md](docs/SETUP.md) | Clone workspace, Docker, SDP, dependency build order |
| [docs/BUILD_STATUS.md](docs/BUILD_STATUS.md) | What works today, what is blocked |
| [docs/PI_DEPLOYMENT.md](docs/PI_DEPLOYMENT.md) | Flash QSTI, deploy binary, run on Pi |
| [prototype/virtual_joystick.py](prototype/virtual_joystick.py) | Working Python hand-control prototype |

## Architecture

```
PC (WSL2 + Docker [QNX])  →  cross-compile  →  scp to Pi  →  QNX runtime
```

- **Build host**: WSL2 Ubuntu + QNX Docker container + QNX SDP 8.0
- **Runtime**: Raspberry Pi with QSTI (`qnxpi.local`, `qnxuser` / `qnxuser`)
- **Input**: USB webcam via QNX Camera Framework (not `cv2.VideoCapture` on Pi)
- **Vision**: MediaPipe C++ graph `hand_tracking_cpu` (not Python Tasks API on Pi)

## Our custom patches

Apply on top of upstream [qnx-ports](https://github.com/qnx-ports) repos:

| Patch | Apply to | Purpose |
|-------|----------|---------|
| `patches/mediapipe.patch` | `mediapipe` @ `qnx-v0.10.26` | glog QNX fix, XNNPACK flag, python_qnx symlinks |
| `patches/com_github_glog_glog_qnx_fix.diff` | (included in mediapipe patch) | Fix host-build glog `__NR_gettid` error |
| `patches/build-files.patch` | `build-files` @ `main` | Python 3.13 / numpy 2.x paths for OpenCV & TFLite |

```bash
# Example: apply mediapipe patch
cd mediapipe && git checkout qnx-v0.10.26 && git apply ../patches/mediapipe.patch
```

## Build status (Jul 2026)

| Component | aarch64 (Pi) | Host (x86) |
|-----------|--------------|------------|
| muslflt | built | — |
| numpy | built | — |
| opencv | built (`nto-aarch64-le/build/lib/`) | — |
| tensorflow-lite | built | — |
| mediapipe `hand_tracking_tflite` | **not yet** | built (x86 smoke test) |
| mediapipe `hand_tracking_cpu` | **blocked** — needs `.configure.bazelrc` | — |

**Next blocker**: create `.configure.bazelrc` for QNX aarch64 Bazel cross-compile (`@local_config_cc//:cc-compiler-qnx_arm64`). Ask hackathon mentors if you do not have the template.

## Upstream repos & branches

```bash
./scripts/clone_workspace.sh   # clones all repos at pinned branches
```

| Repo | Branch |
|------|--------|
| build-files | main |
| mediapipe | qnx-v0.10.26 |
| opencv | qnx-4.9.0 |
| numpy | qnx_v1.25.0 |
| tensorflow | qnx_v2.16.1 |
| muslflt | (default) |

## License note

QNX SDP and QSTI images require a separate [QNX license](https://www.qnx.com/getqnx). Do not commit SDP files, license keys, or `.qnx/` credentials to this repo.
