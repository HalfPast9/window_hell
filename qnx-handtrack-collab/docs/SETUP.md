# Workspace setup

## Prerequisites

- WSL2 Ubuntu (or Linux host)
- Docker
- QNX SDP 8.0 installed at `~/qnx800` (not included in this repo)
- ~32 GB RAM recommended for parallel builds

## 1. Clone this repo and upstream projects

```bash
mkdir -p ~/qnx_workspace && cd ~/qnx_workspace
git clone <THIS_REPO_URL> qnx-handtrack-collab
./qnx-handtrack-collab/scripts/clone_workspace.sh
```

## 2. Apply our patches

```bash
cd ~/qnx_workspace/mediapipe
git apply ../qnx-handtrack-collab/patches/mediapipe.patch
cp ../qnx-handtrack-collab/patches/com_github_glog_glog_qnx_fix.diff third_party/

cd ~/qnx_workspace/build-files
git apply ../qnx-handtrack-collab/patches/build-files.patch
```

## 3. Start Docker build environment

```bash
cd ~/qnx_workspace/build-files/docker
./docker-build-qnx-image.sh    # first time only
./docker-create-container.sh
```

Inside the `[QNX]` container:

```bash
source ~/qnx800/qnxsdp-env.sh
```

## 4. Build dependency stack (aarch64 for Pi)

```bash
cd ~/qnx_workspace
STAGING=/tmp/staging/aarch64le

# muslflt
make -C build-files/ports/muslflt \
  INSTALL_ROOT_nto=$STAGING USE_INSTALL_ROOT=true install \
  QNX_PROJECT_ROOT="$(pwd)/muslflt" -j4

# numpy (needs python3.11 venv on host — see build-files/ports/numpy/README.md)
make -C build-files/ports/numpy install -j4

# opencv
make -C build-files/ports/opencv \
  INSTALL_ROOT_nto=$STAGING USE_INSTALL_ROOT=true install \
  QNX_PROJECT_ROOT="$(pwd)/opencv" -j4

# tensorflow-lite (needs flatc-native-build first — see scripts/clone_workspace.sh)
QNX_PROJECT_ROOT="$(pwd)/tensorflow" \
  TFLITE_HOST_TOOLS_DIR="$(pwd)/flatc-native-build/flatbuffers-flatc/bin/" \
  make -C build-files/ports/tensorflow install JLEVEL=4
```

## 5. Build MediaPipe (host smoke test)

See `scripts/build_mediapipe_host.sh`.

Host build validates the graph compiles. For Pi, use `scripts/build_mediapipe_pi.sh` once `.configure.bazelrc` exists.

## Docker extras installed during our session

```bash
apt install -y libopencv-dev gcc-12 g++-12
```

Set for Bazel host actions:

```bash
export CC=/usr/bin/gcc-12
export CXX=/usr/bin/g++-12
```
