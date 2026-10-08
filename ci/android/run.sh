#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# GitHub Actions host-side entrypoint.
#
# Runs the appropriate Android build inside `termux/termux-docker` (aarch64
# emulated via QEMU binfmt). The repo is mounted read-write so wheels land
# back in <repo>/dist-android/.
#
# Design notes:
#   * The termux-docker image's default entrypoint drops to a non-root user
#     (its package manager refuses to run as root), so we use a single
#     `docker run --rm` and let that entrypoint drop privileges for the whole
#     inner script.
#   * `--privileged` is required for reliable AArch64 emulation on x86 hosts
#     (see termux/termux-docker README).
#   * dist-android/ is pre-created world-writable so the container user can
#     write the built wheels back to the host checkout.
#
# Env inputs (set by the workflow): PYVER, TARGET, PYVER_FULL_3xx.
# ---------------------------------------------------------------------------
set -euo pipefail

PYVER="${PYVER:?PYVER required}"
TARGET="${TARGET:?TARGET required}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="termux/termux-docker:aarch64"

banner() { printf '\033[1;33m[run:%s@%s]\033[0m %s\n' "${PYVER}" "${TARGET}" "$*"; }

# Select the in-container build script.
case "${TARGET}" in
  termux)   INNER="termux.sh" ;;
  pydroid3) INNER="pydroid3.sh" ;;
  *) echo "unknown TARGET: ${TARGET}" >&2; exit 1 ;;
esac

# Ensure a world-writable output directory exists on the host checkout.
mkdir -p "${REPO_ROOT}/dist-android"
chmod 777 "${REPO_ROOT}/dist-android"

banner "pulling ${IMAGE}"
docker pull "${IMAGE}"

banner "running ${INNER} in ${IMAGE} (aarch64 via QEMU)"
docker run --rm --privileged \
  -e PYVER="${PYVER}" \
  -e TARGET="${TARGET}" \
  -e PYVER_FULL_310="${PYVER_FULL_310:-}" \
  -e PYVER_FULL_311="${PYVER_FULL_311:-}" \
  -e PYVER_FULL_312="${PYVER_FULL_312:-}" \
  -e PYVER_FULL_313="${PYVER_FULL_313:-}" \
  -v "${REPO_ROOT}:/repo" \
  -v "${CI_DIR}:/tmp/ci" \
  "${IMAGE}" \
  bash "/tmp/ci/${INNER}"

banner "done: ${REPO_ROOT}/dist-android"
ls -lah "${REPO_ROOT}/dist-android/"
