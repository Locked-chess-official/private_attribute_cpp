#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Provision a CPython interpreter for the Android build (runs INSIDE the
# Termux docker container, bionic libc / aarch64).
#
# Inputs (env):  PYVER            e.g. 3.13 or 3.13t
#                TARGET           termux | pydroid3
#                PYVER_FULL_3xx   pinned stable tarball versions (optional)
#
# Outputs (written to $SCRATCH/pyenv.txt, sourced by callers):
#   PY_BIN   -> path to the python binary to use
#   PY_TAG   -> pip python tag, e.g. cp313 or cp313t
#   PY_ABI   -> abi tag ("none")
#   PY_PLAT  -> wheel platform tag ("linux_aarch64")
#
# Fallback rule (per user requirement): try a prebuilt interpreter first;
# if it is not found, download CPython source and build it.
# ---------------------------------------------------------------------------
set -euo pipefail

PYVER="${PYVER:?PYVER required}"
TARGET="${TARGET:?TARGET required}"

PYVER_SHORT="${PYVER%t}"
FREETHREADED=0
[[ "${PYVER}" == *t ]] && FREETHREADED=1
MINOR="${PYVER_SHORT#3.}"

banner() { printf '\033[1;32m[provision]\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------------------
# Scratch directory. /tmp inside termux-docker is root-owned and the
# container runs as uid 1000 ('system'), so hard-coded /tmp paths fail with
# EACCES. Use $TMPDIR instead (= $PREFIX/tmp, owned by the container user);
# falling back to $HOME keeps this working outside the container too.
# ---------------------------------------------------------------------------
SCRATCH="${TMPDIR:-${HOME}/.ci-tmp}"
mkdir -p "${SCRATCH}"

# ---------------------------------------------------------------------------
# Build CPython from source inside termux (bionic). Returns the install dir.
# ---------------------------------------------------------------------------
build_cpython_from_source() {
  local ver="$1"                 # 3.10 .. 3.15
  local ft="$2"                  # 0/1
  local ft_suffix=""
  [[ "$ft" == "1" ]] && ft_suffix="t"
  # Install under $HOME so a non-root termux user can write it.
  local install_dir="${HOME}/.pyenv/py-${ver}${ft_suffix}"

  if [[ -x "${install_dir}/bin/python${ver}${ft_suffix}" ]]; then
    echo "${install_dir}"
    return 0
  fi

  local srcdir="${SCRATCH}/cpython-src-${ver}${ft_suffix}"
  rm -rf "${srcdir}"
  mkdir -p "${srcdir}"

  # Resolve source: pinned tarball for stable versions, git branch for 3.14+.
  local tarball=""
  case "${ver}" in
    3.10) tarball="Python-${PYVER_FULL_310:-3.10.16}.tgz" ;;
    3.11) tarball="Python-${PYVER_FULL_311:-3.11.11}.tgz" ;;
    3.12) tarball="Python-${PYVER_FULL_312:-3.12.8}.tgz"  ;;
    3.13) tarball="Python-${PYVER_FULL_313:-3.13.13}.tgz" ;;
  esac

  if [[ -n "${tarball}" ]]; then
    local dirver="${tarball#Python-}"; dirver="${dirver%.tgz}"
    local url="https://www.python.org/ftp/python/${dirver}/${tarball}"
    banner "downloading ${url}"
    if ! curl -fsSL "${url}" -o "${SCRATCH}/${tarball}"; then
      echo "download FAILED: ${url}" >&2
      return 1
    fi
    tar -xzf "${SCRATCH}/${tarball}" -C "${srcdir}" --strip-components=1
  else
    banner "cloning CPython ${ver} (no stable tarball pinned)"
    (pkg install -yq git >/dev/null 2>&1 || apt install -yq git >/dev/null 2>&1) || true
    if ! git clone --depth 1 --branch "${ver}" https://github.com/python/cpython.git "${srcdir}"; then
      echo "git clone FAILED for CPython ${ver}" >&2
      return 1
    fi
  fi

  local conf_args=(
    --prefix="${install_dir}"
    --disable-shared
    --without-ensurepip
  )
  [[ "$ft" == "1" ]] && conf_args+=(--disable-gil)

  banner "configuring+building CPython ${ver}${ft_suffix} (this may take several minutes)"
  if (
    cd "${srcdir}"
    ./configure "${conf_args[@]}" >"${SCRATCH}/pyconf.log" 2>&1 \
      && make -j"$(nproc)" >"${SCRATCH}/pymake.log" 2>&1 \
      && make install >"${SCRATCH}/pyinstall.log" 2>&1
  ); then
    :
  else
    echo "CPython ${ver}${ft_suffix} build FAILED (see ${SCRATCH}/pyconf.log, ${SCRATCH}/pymake.log, ${SCRATCH}/pyinstall.log)" >&2
    return 1
  fi

  echo "${install_dir}"
}

# ---------------------------------------------------------------------------
# Interpreter binary name for the requested version.
# ---------------------------------------------------------------------------
bin_name() {
  case "${PYVER}" in
    3.10)  echo "python3.10" ;;
    3.11)  echo "python3.11" ;;
    3.12)  echo "python3.12" ;;
    3.13)  echo "python3.13" ;;
    3.13t) echo "python3.13t" ;;
    3.14)  echo "python3.14" ;;
    3.14t) echo "python3.14t" ;;
    3.15)  echo "python3.15" ;;
    3.15t) echo "python3.15t" ;;
    *)     echo "python${PYVER_SHORT}" ;;
  esac
}

PY_BIN=""
PY_INSTALL_DIR=""

# ---------------------------------------------------------------------------
# Termux: prefer the apt-packaged interpreter, else build from source.
# ---------------------------------------------------------------------------
if [[ "${TARGET}" == "termux" ]]; then
  name="$(bin_name)"
  banner "Termux target: looking for prebuilt interpreter '${name}'"

  # Termux uses `apt` (wrapped by `pkg`); try both spellings for safety.
  (apt install -yq python >/dev/null 2>&1 || pkg install -yq python >/dev/null 2>&1) || true

  if command -v "${name}" >/dev/null 2>&1 && [[ "${FREETHREADED}" != "1" ]]; then
    PY_BIN="$(command -v "${name}")"
    banner "using prebuilt interpreter: ${PY_BIN}"
  else
    banner "prebuilt '${name}' not available (or free-threaded) -> building from source"
    PY_INSTALL_DIR="$(build_cpython_from_source "${PYVER_SHORT}" "${FREETHREADED}")" \
      || { echo "building CPython ${PYVER} FAILED (see ${SCRATCH}/py*.log)" >&2; exit 1; }
    PY_BIN="${PY_INSTALL_DIR}/bin/$(bin_name)"
  fi

# ---------------------------------------------------------------------------
# Pydroid3: no prebuilt CI interpreter -> always build CPython from source.
# The wheel platform tag (linux_aarch64) matches Chaquopy's runtime ABI.
# ---------------------------------------------------------------------------
elif [[ "${TARGET}" == "pydroid3" ]]; then
  banner "Pydroid3 target: building CPython from source (Chaquopy-compatible ABI)"
  PY_INSTALL_DIR="$(build_cpython_from_source "${PYVER_SHORT}" "${FREETHREADED}")" \
    || { echo "building CPython ${PYVER} FAILED (see ${SCRATCH}/py*.log)" >&2; exit 1; }
  PY_BIN="${PY_INSTALL_DIR}/bin/$(bin_name)"
else
  echo "unknown TARGET: ${TARGET}" >&2
  exit 1
fi

[[ -x "${PY_BIN}" ]] || { echo "failed to provision python for ${PYVER}@${TARGET}" >&2; exit 1; }

# Compute the ABI tag deterministically (avoid ${VAR:+t} on a 0/1 flag,
# since a value of 0 is non-empty and would wrongly emit 't').
ABITAG=""
[[ "${FREETHREADED}" == "1" ]] && ABITAG="t"

{
  echo "PY_BIN=${PY_BIN}"
  echo "PY_TAG=cp3${MINOR}${ABITAG}"
  echo "PY_ABI=none"
  echo "PY_PLAT=linux_aarch64"
} > "${SCRATCH}/pyenv.txt"

banner "provisioned: ${PY_BIN}"
"${PY_BIN}" -c 'import sys, sysconfig; print("  ", sys.version.split()[0], "| platform =", sysconfig.get_platform(), "| SOABI =", sysconfig.get_config_var("SOABI"))' >&2
