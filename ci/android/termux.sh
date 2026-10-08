#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Inside the Termux docker container (Termux target):
#   1. install build deps,
#   2. provision the requested CPython interpreter,
#   3. build the extension + assemble the wheel.
#
# Invoked by ci/android/run.sh via `docker exec`.
# ---------------------------------------------------------------------------
set -euo pipefail

banner() { printf '\033[1;36m[termux]\033[0m %s\n' "$*"; }

banner "installing build dependencies"
(pkg update >/dev/null 2>&1 || apt update >/dev/null 2>&1) || true
(pkg install -yq clang make binutils curl wget zip tar git patch python python-pip >/dev/null 2>&1 \
  || apt install -yq clang make binutils curl wget zip tar git patch python python-pip >/dev/null 2>&1) || true

python -m pip --version >/dev/null 2>&1 || python -m ensurepip >/dev/null 2>&1 || true

banner "provisioning CPython ${PYVER} (termux)"
bash /tmp/ci/build_python.sh

# shellcheck disable=SC1091
source "${TMPDIR:-/tmp}/pyenv.txt"
source /tmp/ci/common.sh

banner "building extension + wheel with ${PY_BIN}"
# NOTE: TARGET=termux -> common.sh uses `.cpython-3XX-aarch64-linux-android.so`.
build_and_package "${PY_BIN}" "${PY_TAG}" "${PY_ABI}" "${PY_PLAT}"

banner "artifacts:"
ls -lah /repo/dist-android/
