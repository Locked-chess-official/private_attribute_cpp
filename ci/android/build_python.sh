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

  # Resolve source: pinned tarball for every supported minor (3.10..3.15).
  # A failed tarball download falls back to a git branch clone below, so a
  # freshly-released minor (e.g. 3.15.0) still builds even if its tarball is
  # not yet mirrored on python.org.
  local tarball=""
  case "${ver}" in
    3.10) tarball="Python-${PYVER_FULL_310:-3.10.22}.tgz" ;;
    3.11) tarball="Python-${PYVER_FULL_311:-3.11.17}.tgz" ;;
    3.12) tarball="Python-${PYVER_FULL_312:-3.12.15}.tgz" ;;
    3.13) tarball="Python-${PYVER_FULL_313:-3.13.16}.tgz" ;;
    3.14) tarball="Python-${PYVER_FULL_314:-3.14.8}.tgz"  ;;
    3.15) tarball="Python-${PYVER_FULL_315:-3.15.0}.tgz"  ;;
  esac

  local downloaded=0
  if [[ -n "${tarball}" ]]; then
    local dirver="${tarball#Python-}"; dirver="${dirver%.tgz}"
    local url="https://www.python.org/ftp/python/${dirver}/${tarball}"
    banner "downloading ${url}"
    if curl -fsSL "${url}" -o "${SCRATCH}/${tarball}"; then
      tar -xzf "${SCRATCH}/${tarball}" -C "${srcdir}" --strip-components=1
      downloaded=1
    else
      banner "tarball download FAILED (${url}); falling back to git branch ${ver}"
    fi
  fi

  if [[ "${downloaded}" != "1" ]]; then
    banner "cloning CPython ${ver} (git branch)"
    (pkg install -yq git >/dev/null 2>&1 || apt install -yq git >/dev/null 2>&1) || true
    if ! git clone --depth 1 --branch "${ver}" https://github.com/python/cpython.git "${srcdir}"; then
      echo "git clone FAILED for CPython ${ver}" >&2
      return 1
    fi
  fi

  # Configure args mirror termux-packages/packages/python/build.sh - CPython
  # on bionic (Android) needs explicit ac_cv_* cache vars for functions that
  # exist in glibc but NOT in bionic, otherwise configure says "yes" and make
  # dies (e.g. sem_clockwait -> Python/parking_lot.c, issue python/cpython
  # #143640). This is the authoritative set used by the official Termux build.
  local conf_args=(
    --prefix="${install_dir}"
    --without-ensurepip
    # NOTE: deliberately NO --with-system-ffi / --with-system-expat /
    # --enable-loadable-sqlite-extensions / --disable-static. Those options
    # only matter for _ctypes/pyexpat/sqlite3 (all irrelevant to our single
    # C++ extension), and some are not even valid in CPython 3.13+ configure
    # (e.g. --with-system-ffi and --disable-static only emit "unrecognized
    # options" warnings). Bundled libffi/expat/sqlite build fine on bionic.
    # bionic is missing several glibc functions; force 'no' so configure
    # doesn't detect them and make doesn't fail on implicit declarations.
    ac_cv_file__dev_ptmx=yes
    ac_cv_file__dev_ptc=no
    ac_cv_func_wcsftime=no
    ac_cv_func_ftime=no
    ac_cv_func_faccessat=no
    ac_cv_func_link=no
    ac_cv_func_linkat=no
    ac_cv_func_fexecve=no
    ac_cv_func_getlogin_r=no
    ac_cv_func_getloadavg=no
    ac_cv_func_sem_clockwait=no
    ac_cv_func_preadv2=no
    ac_cv_func_pwritev2=no
    ac_cv_func_close_range=no
    ac_cv_func_copy_file_range=no
    ac_cv_buggy_getaddrinfo=no
    ac_cv_little_endian_double=yes
    ac_cv_working_tzset=yes
    ac_cv_header_sys_xattr_h=no
    ac_cv_func_getgrent=yes
    # POSIX semaphores / shared memory: enable via cache vars (bionic needs
    # libandroid-posix-semaphore; configure's link check may fail otherwise).
    ac_cv_posix_semaphores_enabled=yes
    ac_cv_func_sem_open=yes
    ac_cv_func_sem_timedwait=yes
    ac_cv_func_sem_getvalue=yes
    ac_cv_func_sem_unlink=yes
    ac_cv_func_shm_open=yes
    ac_cv_func_shm_unlink=yes
  )
  [[ "$ft" == "1" ]] && conf_args+=(--disable-gil)

  # In termux-docker we build natively for aarch64 (QEMU emulates the CPU but
  # uname -m is aarch64), so --build is the android tuple, NOT the x86_64 host.
  local machine
  machine="$(uname -m)"   # aarch64 inside termux-docker
  banner "building for machine=${machine} (uname), TARGET=${TARGET}, CC=$(command -v clang || echo missing)"

  # Shared vs static differs per target: pydroid3's real interpreter is a
  # SHARED build (Py_ENABLE_SHARED=1), termux's is static. We reproduce only
  # the RESULT that matters for the extension: shared libpython + flat SOABI
  # for pydroid3 (the flat-vs-arch EXT_SUFFIX split is forced in common.sh,
  # not by these flags). We build NATIVELY (aarch64 under QEMU), so there is
  # no --host/--build cross tuple — Chaquopy's `--host=... --build=i686-linux`
  # only applied because IT compiled on an x86 host with the NDK.
  if [[ "${TARGET}" == "pydroid3" ]]; then
    conf_args+=(--enable-shared --enable-ipv6)
  else
    conf_args+=(--disable-shared)
  fi
  conf_args+=(--build="${machine}-linux-android")

  # DO NOT pass --with-build-python here. It is only needed when CROSS-
  # compiling (host != build), and configure hard-requires it to be the SAME
  # minor version as the target -- passing termux's `python` (3.14) while
  # building 3.13 aborts with "incompatible version ... (expected: 3.13)".
  # We build NATIVELY (aarch64 under QEMU), so CPython uses the just-built
  # ./python for its freeze/regen step automatically; no build python needed.

  # Use termux's native clang AS-IS: NO --target and NO -D__ANDROID_API__.
  # termux's clang already defaults to the Android target at termux's own API
  # level and searches termux's real headers ($PREFIX/include) + libc
  # ($PREFIX/lib). Forcing --target=aarch64-linux-android21 (or any old API)
  # gates out <signal.h> decls like sigwaitinfo/sigtimedwait (API 26+) and
  # <spawn.h> (API 28+), which then fail in Modules/signalmodule.c
  # ("implicit declaration") and Modules/posixmodule.c ("spawn.h file not
  # found"). termux-packages' own python/build.sh builds with bare clang +
  # ac_cv_* cache vars (no --target) — the proven path we follow.
  export CC="clang"
  export CXX="clang++"
  banner "using native termux clang ($(clang --version 2>/dev/null | head -n1 || echo clang))"

  # Link against libandroid-posix-semaphore (provides the POSIX sem_* symbols
  # that bionic itself does not ship). Same as termux-packages build.sh.
  export LDFLAGS="${LDFLAGS:-} -landroid-posix-semaphore"

  # ---------------------------------------------------------------------
  # termux-docker has NO /bin/sh. Its Dockerfile literally does:
  #   ENV PATH=${TERMUX__PREFIX}/bin
  #   SHELL ["sh", "-c"]   # comment: "Docker uses /bin/sh by default,
  #                         #  but we don't have it."
  # CPython's autoconf `configure` starts with "#!/bin/sh" and the generated
  # Makefiles hardcode "SHELL = /bin/sh", so `./configure` dies with
  # "bad interpreter: /bin/sh: No such file or directory" and `make` cannot
  # run recipe lines. Fix: point autoconf + make at termux's real shell.
  # ---------------------------------------------------------------------
  local real_sh="${PREFIX:-/data/data/com.termux/files/usr}/bin/sh"
  export CONFIG_SHELL="${real_sh}"
  export SHELL="${real_sh}"

  # Belt-and-suspenders: rewrite the shipped #!/bin/sh shebangs too, so any
  # nested re-exec of these helpers works (termux-fix-shebang is the official
  # helper when present; sed fallback otherwise).
  if command -v termux-fix-shebang >/dev/null 2>&1; then
    find "${srcdir}" -type f \( -name configure -o -name config.guess -o -name config.sub -o -name install-sh \) \
      -exec termux-fix-shebang {} +
  else
    find "${srcdir}" -type f \( -name configure -o -name config.guess -o -name config.sub -o -name install-sh \) \
      -exec sed -i "1s|^#!.*/bin/sh.*|#!${real_sh}|" {} +
  fi

  banner "configuring+building CPython ${ver}${ft_suffix} (this may take several minutes)"
  # -j2 under QEMU: nproc reports host cores but QEMU translates each
  # instruction; high -j causes thrashing/OOM. 2 is a safe sweet spot.
  #
  # Logs are `tee`d to stdout so they stream into the CI runner log; a plain
  # redirect-to-file would hide every failure from the Actions UI (the file
  # lives in the container's /tmp, which is not mounted back to the host).
  local make_jobs="2"
  if (
    cd "${srcdir}"
    "${real_sh}" ./configure "${conf_args[@]}" 2>&1 | tee "${SCRATCH}/pyconf.log" \
      && make -j"${make_jobs}" SHELL="${real_sh}" 2>&1 | tee "${SCRATCH}/pymake.log" \
      && make install SHELL="${real_sh}" 2>&1 | tee "${SCRATCH}/pyinstall.log"
  ); then
    :
  else
    echo "CPython ${ver}${ft_suffix} build FAILED (see ${SCRATCH}/pyconf.log, ${SCRATCH}/pymake.log, ${SCRATCH}/pyinstall.log)" >&2
    echo "--- tail ${SCRATCH}/pyconf.log ---" >&2
    tail -n 50 "${SCRATCH}/pyconf.log" >&2 || true
    echo "--- tail ${SCRATCH}/pymake.log ---" >&2
    tail -n 50 "${SCRATCH}/pymake.log" >&2 || true
    echo "--- tail ${SCRATCH}/pyinstall.log ---" >&2
    tail -n 50 "${SCRATCH}/pyinstall.log" >&2 || true
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
