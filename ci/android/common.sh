#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Source-able library: builds the `private_attribute` C++ extension for the
# Android (bionic / aarch64) target and assembles a wheel by hand.
#
# The extension is compiled directly with clang++ against the provisioned
# interpreter's include path (no pip/setuptools dependency, so it works for
# bare source-built CPython too).
#
# Callers (termux.sh / pydroid3.sh) must:
#     source /tmp/ci/common.sh
#     build_and_package "${PY_BIN}" "${PY_TAG}" "${PY_ABI}" "${PY_PLAT}"
#
# The extension suffix is FORCED per-target, because the two Android runtimes
# genuinely use different extension-module naming (both are aarch64 + bionic):
#
#   * termux   (native bionic build, __ANDROID__ defined)
#       -> .cpython-3XX[-t]-aarch64-linux-android.so   (WITH arch segment)
#   * pydroid3 (Chaquopy NDK cross-build + platform-tag fixing)
#       -> .cpython-3XX[-t].so                          (NO arch segment)
#
# Why forced, not probed? A termux-container source build would always report
# the termux-style (arch-ful) suffix, which is WRONG for the pydroid3 target.
# Chaquopy's real build tool (build-wheel.py) drops the platform segment; to
# match the actual Pydroid3 runtime we compute the pydroid3 suffix here.
# The wheel platform tag (linux_aarch64) is what actually distinguishes the
# target ABI for pip's platform matching.
# ---------------------------------------------------------------------------

PYVER="${PYVER:?PYVER is required (e.g. 3.13 or 3.13t)}"

PYVER_SHORT="${PYVER%t}"
PYVER_FREETHREADED=0
[[ "${PYVER}" == *t ]] && PYVER_FREETHREADED=1
PYVER_MINOR="${PYVER_SHORT#3.}"

# ABI tag ('t' for free-threaded) — computed deterministically.
PYABI_TAG=""
[[ "${PYVER_FREETHREADED}" == "1" ]] && PYABI_TAG="t"

# Target-aware extension suffix (see header comment above).
case "${TARGET:-}" in
  termux)
    EXT_SUFFIX=".cpython-3${PYVER_MINOR}${PYABI_TAG}-aarch64-linux-android.so"
    ;;
  pydroid3)
    EXT_SUFFIX=".cpython-3${PYVER_MINOR}${PYABI_TAG}.so"
    ;;
  *)
    # Default: pydroid3-style (no arch), safe fallback.
    EXT_SUFFIX=".cpython-3${PYVER_MINOR}${PYABI_TAG}.so"
    ;;
esac

# Default Android aarch64 (bionic) platform tag — matches Termux and Pydroid3.
DEFAULT_PLATFORM_TAG="linux_aarch64"

# Inside the container the repo is mounted at /repo and the CI scripts at
# /tmp/ci, so the repo root cannot be derived from this file's own path.
REPO_ROOT="${REPO_ROOT:-/repo}"
OUT_DIR="${REPO_ROOT}/dist-android"
mkdir -p "${OUT_DIR}" 2>/dev/null || OUT_DIR="${HOME}/dist-android"
mkdir -p "${OUT_DIR}" || { echo "cannot create ${OUT_DIR}" >&2; exit 1; }

# Package version, read from setup.py (single source of truth).
VERSION="$(sed -n "s/^ *version=['\"]\([^'\"]*\)['\"].*/\1/p" "${REPO_ROOT}/setup.py" | head -n 1)"
[[ -n "${VERSION}" ]] || VERSION="0.0.0"

info()  { printf '\033[1;34m[android:%s]\033[0m %s\n' "${PYVER}" "$*"; }
warn()  { printf '\033[1;33m[android:%s]\033[0m %s\n' "${PYVER}" "$*" >&2; }
fail()  { printf '\033[1;31m[android:%s]\033[0m %s\n' "${PYVER}" "$*" >&2; exit 1; }

sha256_b64() { python3 -c 'import hashlib,base64,sys;print("sha256="+base64.urlsafe_b64encode(hashlib.sha256(open(sys.argv[1],"rb").read()).digest()).rstrip(b"=").decode())' "$1"; }

# ---------------------------------------------------------------------------
# Probe the interpreter for its include dirs (header location only).
# Sets globals PROBED_INCLUDE and PROBED_INTERNAL.
# ---------------------------------------------------------------------------
probe_interpreter() {
  local python_bin="$1"
  local out
  out="$("${python_bin}" - <<'PY'
import sysconfig, os
inc = sysconfig.get_paths()["include"]
internal = os.path.join(inc, "internal")
print(inc)
print(internal if os.path.isdir(internal) else "")
PY
)"
  PROBED_INCLUDE="$(echo "${out}" | sed -n '1p')"
  PROBED_INTERNAL="$(echo "${out}" | sed -n '2p')"
  info "interpreter: include=${PROBED_INCLUDE} internal=${PROBED_INTERNAL:-<none>}"
}

# ---------------------------------------------------------------------------
build_and_package() {
  local python_bin="${1:-python}"
  local python_tag="${2:-cp3${PYVER_MINOR}${PYABI_TAG}}"
  local abi_tag="${3:-none}"
  local plat_tag="${4:-${DEFAULT_PLATFORM_TAG}}"

  probe_interpreter "${python_bin}"

  # Deterministic Android suffix (no arch segment), see header comment.
  local ext_suffix="${EXT_SUFFIX}"
  [[ -n "${ext_suffix}" ]] || fail "EXT_SUFFIX not set"

  local include_dir="${PROBED_INCLUDE}"
  [[ -d "${include_dir}" ]] || fail "include dir not found: ${include_dir}"

  local inc_flags=(-I"${include_dir}")
  if [[ -n "${PROBED_INTERNAL:-}" ]]; then
    inc_flags+=(-I"${PROBED_INTERNAL}")
  fi

  info "compiling extension with clang++ (EXT_SUFFIX=${ext_suffix})"

  # Match setup.py flags + -fPIC -shared -O2 for a release Android .so.
  # (-fno-exceptions -fno-rtti are used on Linux by setup.py already.)
  local out_so="${OUT_DIR}/private_attribute${ext_suffix}"
  clang++ -std=c++17 -fno-exceptions -fno-rtti -g0 -O2 -fPIC -shared \
    "${inc_flags[@]}" \
    -I"${REPO_ROOT}" \
    "${REPO_ROOT}/private_attribute.cpp" \
    -o "${out_so}"

  [[ -f "${out_so}" ]] || fail "compilation produced no .so"

  info "built extension: ${out_so}"

  # --- assemble wheel by hand -------------------------------------------
  # Layout (top level = site-packages):
  #   private_attribute<ext_suffix>
  #   private_attribute.pyi
  #   private_attribute_cpp-<VERSION>.dist-info/{METADATA,WHEEL,RECORD}
  local dist_info="${OUT_DIR}/private_attribute_cpp-${VERSION}.dist-info"
  local pkg_dir="${OUT_DIR}/wheelroot"
  rm -rf "${pkg_dir}" "${dist_info}"
  mkdir -p "${pkg_dir}" "${dist_info}"

  cp "${out_so}" "${pkg_dir}/private_attribute${ext_suffix}"
  cp "${REPO_ROOT}/private_attribute.pyi" "${pkg_dir}/private_attribute.pyi"

  local wheel_name="private_attribute_cpp-${VERSION}-${python_tag}-${abi_tag}-${plat_tag}.whl"
  local wheel_path="${OUT_DIR}/${wheel_name}"
  local record_file="${dist_info}/RECORD"

  {
    echo "Wheel-Version: 1.0"
    echo "Generator: private-attribute-cpp-android-ci"
    echo "Root-Is-Purelib: false"
    echo "Tag: ${python_tag}-${abi_tag}-${plat_tag}"
  } > "${dist_info}/WHEEL"

  {
    echo "Metadata-Version: 2.1"
    echo "Name: private-attribute-cpp"
    echo "Version: ${VERSION}"
    echo "Summary: Define private attributes with a C++ implementation."
    echo "License: MIT"
    echo "Requires-Python: >=3.10"
  } > "${dist_info}/METADATA"

  : > "${record_file}"

  local f rel h sz p
  for f in "${pkg_dir}/private_attribute${ext_suffix}" "${pkg_dir}/private_attribute.pyi"; do
    rel="${f#${pkg_dir}/}"
    h="$(sha256_b64 "${f}")"
    sz="$(stat -c %s "${f}")"
    printf '%s,%s,%s\n' "${rel}" "${h}" "${sz}" >> "${record_file}"
  done
  for f in WHEEL METADATA; do
    p="${dist_info}/${f}"
    rel="private_attribute_cpp-${VERSION}.dist-info/${f}"
    h="$(sha256_b64 "${p}")"
    sz="$(stat -c %s "${p}")"
    printf '%s,%s,%s\n' "${rel}" "${h}" "${sz}" >> "${record_file}"
  done

  # RECORD must end with itself (empty hash/size).
  printf '%s,,\n' "private_attribute_cpp-${VERSION}.dist-info/RECORD" >> "${record_file}"

  # --- zip the wheel (files at top level) ------------------------------
  if command -v zip >/dev/null 2>&1; then
    ( cd "${pkg_dir}" && rm -f "${wheel_path}" && \
      zip -q -r "${wheel_path}" . \
      && cd "${OUT_DIR}" && \
      zip -q -r "${wheel_path}" "private_attribute_cpp-${VERSION}.dist-info" )
  else
    rm -f "${wheel_path}"
    python3 - "${wheel_path}" "${pkg_dir}" "${dist_info}" <<'PY'
import zipfile, os, sys
wheel, pkg_dir, dist_info = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(wheel, 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(pkg_dir):
        for fn in files:
            p = os.path.join(root, fn)
            z.write(p, os.path.relpath(p, pkg_dir))
    for root, _, files in os.walk(dist_info):
        for fn in files:
            p = os.path.join(root, fn)
            z.write(p, os.path.relpath(p, os.path.dirname(dist_info)))
print('wrote', wheel)
PY
  fi

  # Clean staging: keep only the .so and the wheel.
  rm -rf "${pkg_dir}" "${dist_info}"

  info "wheel written: ${wheel_path}"
  echo "${wheel_path}"
}
