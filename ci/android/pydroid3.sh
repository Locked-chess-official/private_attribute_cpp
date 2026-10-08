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
# Strict install: if the package manager fails, we MUST know - otherwise the
# CPython source build below fails silently (curl/tar/configure need these
# tools) and the log shows nothing but "failed to provision".
if command -v pkg >/dev/null 2>&1; then
  pkg update || { echo "pkg update FAILED" >&2; exit 1; }
  pkg install -yq clang make binutils curl wget zip tar git patch pkg-config libandroid-posix-semaphore libffi python python-pip \
    || { echo "pkg install FAILED" >&2; exit 1; }
else
  apt update || { echo "apt update FAILED" >&2; exit 1; }
  apt install -yq clang make binutils curl wget zip tar git patch pkg-config libandroid-posix-semaphore libffi python python-pip \
    || { echo "apt install FAILED" >&2; exit 1; }
fi

# Self-check: the tools the build depends on must actually be on PATH.
for tool in clang make curl tar zip git patch pkg-config; do
  command -v "${tool}" >/dev/null 2>&1 \
    || { echo "MISSING build tool: ${tool}" >&2; exit 1; }
done

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
