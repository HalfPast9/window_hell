#!/bin/bash
# Cross-compile for Raspberry Pi (QNX aarch64). Requires .configure.bazelrc.
set -euo pipefail

if [[ ! -f ~/qnx_workspace/mediapipe/.configure.bazelrc ]]; then
  echo "ERROR: ~/qnx_workspace/mediapipe/.configure.bazelrc is missing."
  echo "Get the QNX hackathon template or create one — see docs/BUILD_STATUS.md"
  exit 1
fi

source ~/qnx800/qnxsdp-env.sh
cd ~/qnx_workspace/mediapipe

export BAZEL_JOBS="${BAZEL_JOBS:-4}"
export CC=/usr/bin/gcc-12
export CXX=/usr/bin/g++-12
export PATH="$HOME/qnx_workspace/.bin:$PATH"

bazel build --config=qnx_arm64 -c opt \
  --define MEDIAPIPE_DISABLE_GPU=1 \
  --define=xnn_enable_avxvnniint8=false \
  --jobs="$BAZEL_JOBS" \
  --action_env=CC --action_env=CXX \
  --action_env=PYTHON_BIN_PATH=/usr/bin/python3 \
  --action_env=QNX_HOST --action_env=QNX_TARGET \
  --repo_env=BAZEL_CXXOPTS=-std=c++17 \
  --override_repository=python="$(pwd)/python" \
  --override_repository=python_qnx="$(pwd)/python_qnx" \
  --extra_toolchains=@python_qnx//:qnx_py_toolchain \
  --extra_toolchains=@python_qnx//:qnx_py_cc_toolchain \
  --extra_toolchains=//cpp_qnx:qnx_cc_toolchain_arm64 \
  //mediapipe/examples/qnx/hand_tracking:hand_tracking_cpu

file bazel-bin/mediapipe/examples/qnx/hand_tracking/hand_tracking_cpu
