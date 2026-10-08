#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Inside the Termux docker container (Pydroid3 target):
#   1. install build deps,
#   2. provision the requested CPython interpreter,
#   3. build the extension + assemble the wheel.
#
# Pydroid3 is Chaquopy-based (NDK cross-compiled). Its extension modules use
# a flat `.cpython-3XX.so` / `.cpython-3XXt.so` suffix (no arch segment),
# unlike Termux's `.cpython-3XX-aarch64-linux-android.so`. The target-aware
# suffix is computed inside common.sh from $TARGET.
#
# Invoked by ci/android/run.sh via `docker run`.
# ---------------------------------------------------------------------------
set -euo pipefail

banner() { printf '\033[1;35m[pydroid3]\033[0m %s\n' "$*"; }

banner "installing build dependencies"
(pkg update >/dev/null 2>&1 || apt update >/dev/null 2>&1) || true
(pkg install -yq clang make binutils curl wget zip tar git patch python python-pip >/dev/null 2>&1 \
  || apt install -yq clang make binutils curl wget zip tar git patch python python-pip >/dev/null 2>&1) || true

python -m pip --version >/dev/null 2>&1 || python -m ensurepip >/dev/null 2>&1 || true

banner "provisioning CPython ${PYVER} (pydroid3)"
bash /tmp/ci/build_python.sh

# shellcheck disable=SC1091
source "${TMPDIR:-/tmp}/pyenv.txt"
source /tmp/ci/common.sh

# NOTE: TARGET=pydroid3 -> common.sh uses the flat `.cpython-3XX.so` suffix.

banner "building extension + wheel with ${PY_BIN}"
build_and_package "${PY_BIN}" "${PY_TAG}" "${PY_ABI}" "${PY_PLAT}"

banner "artifacts:"
ls -lah /repo/dist-android/
