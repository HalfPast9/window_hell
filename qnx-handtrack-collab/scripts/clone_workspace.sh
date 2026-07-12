#!/bin/bash
# Clone upstream qnx-ports repos at pinned branches into ~/qnx_workspace
set -euo pipefail

ROOT="${QNX_WORKSPACE:-$HOME/qnx_workspace}"
mkdir -p "$ROOT"
cd "$ROOT"

clone_if_missing() {
  local dir="$1" url="$2" branch="$3"
  if [[ -d "$dir/.git" ]]; then
    echo "==> $dir already exists, skipping clone"
    return
  fi
  echo "==> Cloning $dir ($branch)"
  git clone --branch "$branch" --depth 1 "$url" "$dir"
}

clone_if_missing build-files https://github.com/qnx-ports/build-files.git main
clone_if_missing mediapipe   https://github.com/qnx-ports/mediapipe.git qnx-v0.10.26
clone_if_missing opencv      https://github.com/qnx-ports/opencv.git qnx-4.9.0
clone_if_missing numpy       https://github.com/qnx-ports/numpy.git qnx_v1.25.0
clone_if_missing tensorflow  https://github.com/qnx-ports/tensorflow.git qnx_v2.16.1
clone_if_missing muslflt     https://github.com/qnx-ports/muslflt.git main

if [[ ! -d flatc-native-build/flatbuffers-flatc/bin ]]; then
  echo "==> Building host flatc"
  mkdir -p flatc-native-build && cd flatc-native-build
  cmake ../tensorflow/tensorflow/lite/tools/cmake/native_tools/flatbuffers
  cmake --build .
  cd "$ROOT"
fi

echo ""
echo "Done. Next: apply patches from qnx-handtrack-collab/patches/ (see docs/SETUP.md)"
