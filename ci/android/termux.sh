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
# Strict install: if the package manager fails, we MUST know - otherwise the
# CPython source build below fails silently (curl/tar/configure need these
# tools) and the log shows nothing but "failed to provision".
if command -v pkg >/dev/null 2>&1; then
  pkg update || { echo "pkg update FAILED" >&2; exit 1; }
  pkg install -yq clang make binutils curl wget zip tar git patch python python-pip \
    || { echo "pkg install FAILED" >&2; exit 1; }
else
  apt update || { echo "apt update FAILED" >&2; exit 1; }
  apt install -yq clang make binutils curl wget zip tar git patch python python-pip \
    || { echo "apt install FAILED" >&2; exit 1; }
fi

# Self-check: the tools the build depends on must actually be on PATH.
for tool in clang make curl tar zip git patch; do
  command -v "${tool}" >/dev/null 2>&1 \
    || { echo "MISSING build tool: ${tool}" >&2; exit 1; }
done

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
