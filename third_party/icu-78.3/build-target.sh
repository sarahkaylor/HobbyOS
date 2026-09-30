#!/usr/bin/env bash
# build-target.sh -- cross-build ICU 78.3 for the HobbyOS bare-metal targets
# (aarch64-none-elf / x86_64-none-elf) against the in-tree libc/libc++ sysroot
# (browser.md L6; lane browser/l6-icu-target).  Companion to build-host.sh;
# rationale, measured results and known gaps: ../../cross-notes.md.
#
#   bash build-target.sh --arch arm|intel [--outdir DIR] [--objdir DIR]
#                        [--host-root DIR] [--no-trim] [--no-probe] [-j N]
#
# Defaults (relative paths are taken from the invoking CWD):
#   --outdir     obj/<arch>/icu                   deliverable archives + MANIFEST
#   --objdir     obj/third_party/icu-78.3/<arch>  out-of-tree configure/build dir
#   --host-root  third_party/icu-78.3/build-host  host stage (build-host.sh)
#
# Two-stage ICU cross build (ICU's supported model):
#   1. host stage -- the vendored build-host.sh builds ICU for this workstation
#      WITH TOOLS; its build tree (build-host/obj) is the "--with-cross-build
#      cross buildroot" whose bin/ (genrb, pkgdata, icupkg, genbrk, makeconv,
#      gencnval, gencfu, icuexportdata, escapesrc, ...) the target data step
#      invokes.  Built on demand if build-host/obj/bin/genrb is missing.
#   2. target stage (this script) -- configure --host=<triple> against the
#      in-tree sysroot, then make.  Only the libraries are built: the tools
#      subdir is disabled for the target (--disable-tools) because target
#      executables cannot link or run here and are not needed -- the data step
#      takes every tool from the stage-1 cross buildroot.
#
# Data: trimmed, source-built, static (the F0 decision recorded in README.md).
# ICU_DATA_FILTER_FILE=data-filter-en.json (root+en locales, no brkitr
# dictionaries) is consumed by configure; the release tarball's prebuilt
# data/in/icudt78l.dat is parked first because ICU's make silently prefers it
# and then ignores the filter (build-host-trimmed.sh convention).  --no-trim
# keeps the prebuilt full-data archive instead.
#
# Output: <outdir>/libicuuc.a libicui18n.a libicudata.a + MANIFEST.txt.
# Verification: nm spot-checks on every archive (expected renamed core
# symbols), a target link probe against crt0+libc+libcxx.a (symbol-closure
# test; see --probe-strict) and sha256 in the MANIFEST.
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${VENDOR_DIR}/../.." && pwd)"
SRC="${VENDOR_DIR}/src"
LIBCXX_VENDOR="${REPO_ROOT}/third_party/libcxx-21.1.8"
LIBCXX_INCLUDE="${LIBCXX_VENDOR}/src/llvm-project-21.1.8.src/libcxx/include"
FILTER="${VENDOR_DIR}/data-filter-en.json"

ARCH=""
OUTDIR=""
OBJDIR=""
HOST_ROOT=""
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
TRIM=1
PROBE=1
PROBE_STRICT=0

log()  { printf '[icu-target] %s\n' "$*"; }
die()  { printf '[icu-target] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --arch)        ARCH="$2"; shift 2 ;;
    --outdir)      OUTDIR="$2"; shift 2 ;;
    --objdir)      OBJDIR="$2"; shift 2 ;;
    --host-root)   HOST_ROOT="$2"; shift 2 ;;
    --no-trim)     TRIM=0; shift ;;
    --no-probe)    PROBE=0; shift ;;
    --probe-strict) PROBE=1; PROBE_STRICT=1; shift ;;
    -j)            JOBS="$2"; shift 2 ;;
    *) die "usage: $0 --arch arm|intel [--outdir DIR] [--objdir DIR] [--host-root DIR] [--no-trim] [--no-probe] [--probe-strict] [-j N]" ;;
  esac
done

case "$ARCH" in
  arm|intel) ;;
  *) die "--arch must be arm|intel (got '${ARCH:-}')" ;;
esac
[ -n "$OUTDIR" ]    || OUTDIR="${REPO_ROOT}/obj/${ARCH}/icu"
[ -n "$OBJDIR" ]    || OBJDIR="${REPO_ROOT}/obj/third_party/icu-78.3/${ARCH}"
[ -n "$HOST_ROOT" ] || HOST_ROOT="${VENDOR_DIR}/build-host"
OUTDIR="$(mkdir -p "$OUTDIR" && cd "$OUTDIR" && pwd)"
OBJDIR="$(mkdir -p "$OBJDIR" && cd "$OBJDIR" && pwd)"
CROSS_ROOT="${HOST_ROOT}/obj"

# --- toolchain (mirrors the Makefile's USER_CFLAGS and libcxx build-target.sh)
case "$ARCH" in
  arm)
    TRIPLE="aarch64-none-elf"
    # No -mgeneral-regs-only: userland may use FP/SIMD (F1.5 enables FPEN and
    # the kernel saves the FPSIMD state per process); libcxx.a is built the
    # same way.  Keep in sync with Makefile USER_CFLAGS (ARCH=arm).
    TARGET_FLAGS="--target=${TRIPLE} -ffreestanding -mcpu=cortex-a53"
    EXTRA_LDFLAGS=""
    ;;
  intel)
    TRIPLE="x86_64-none-elf"
    # -Wl,-no-pie: for x86_64-none-elf clang delegates links to /usr/bin/gcc,
    # which defaults to PIE -> lld emits ET_DYN and every absolute relocation
    # from crt0.o/libc.a is rejected (R_X86_64_32S ... recompile with -fPIC).
    # The driver-form -no-pie is swallowed by clang; only -Wl,-no-pie reaches
    # gcc.  See cross-notes.md "why each flag".
    TARGET_FLAGS="--target=${TRIPLE} -ffreestanding -mno-red-zone"
    EXTRA_LDFLAGS="-Wl,-no-pie"
    ;;
esac

CXX="${CXX:-clang}"
CLANG_RESOURCE_INCLUDE="$("${CXX}" -print-resource-dir 2>/dev/null)/include"
[ -d "$CLANG_RESOURCE_INCLUDE" ] || die "cannot locate the clang resource include dir"
# -nostdinc + explicit resource-dir -isystem: hermetic search path (compiler
# freestanding headers reachable, host glibc never).  For x86_64-none-elf the
# driver would otherwise append the workstation's glibc dirs (arch match);
# see the Makefile's P3 comment.
STDINC="-nostdinc -isystem ${CLANG_RESOURCE_INCLUDE}"
# -fuse-ld=lld: Ubuntu's default link driver is x86-64 binutils ld.bfd
# ("unrecognised emulation mode: aarch64elf").  -nostdlib: without it the
# driver adds its own startfiles and -lgcc/-lc, none of which exist for these
# triples; configure's link tests are then literally the in-tree user-program
# recipe (crt0.o + libc.a from LIBS).
LINK_DRIVER_FLAGS="-fuse-ld=lld -nostdlib"

CC_BIN="${CC:-clang}"
CC="${CC_BIN} ${TARGET_FLAGS} ${STDINC} ${LINK_DRIVER_FLAGS}"
# -nostdinc++ + the vendored libc++ headers: ICU 78 needs a C++17 std subset
# (string_view, mutex, atomic, ...).  -x c++ because this box has no clang++
# binary (house pattern -- the Makefile compiles C++ the same way).
CXX="${CXX} -x c++ ${TARGET_FLAGS} ${STDINC} ${LINK_DRIVER_FLAGS} -nostdinc++ -I${LIBCXX_INCLUDE}"

# -fno-exceptions: ICU never throws (its error model is UErrorCode; every
# "throw" hit in the sources is inside a comment).  RTTI is required and
# stays ON: common/normalizer2.cpp, common/serv.cpp and i18n/alphaindex.cpp
# use dynamic_cast, common/rbbi.cpp uses typeid -- a link-time dependency on
# libc++abi's cast machinery, see cross-notes.md "gaps".
CFLAGS="-O2 -g -std=c11 -I${REPO_ROOT}/src/libc/include -I${REPO_ROOT}/src/include -I${REPO_ROOT}/src/user_include"
CXXFLAGS="-O2 -g -std=c++17 -fno-exceptions -frtti -I${LIBCXX_INCLUDE} -I${REPO_ROOT}/src/libc/include -I${REPO_ROOT}/src/include -I${REPO_ROOT}/src/user_include"
LDFLAGS="-T ${REPO_ROOT}/src/user/linker.ld ${EXTRA_LDFLAGS}"
# LIBS: configure's link tests are the in-tree user-program recipe (crt0 +
# libc.a where HobbyOS pthreads live).  No -ldl/-lpthread/-lgcc: the target
# has none of them.
LIBS="${REPO_ROOT}/obj/${ARCH}/crt0.o ${REPO_ROOT}/obj/${ARCH}/libc.a"

# --- 0. sysroot pieces the configure link tests need -----------------------
if [ ! -f "${REPO_ROOT}/obj/${ARCH}/crt0.o" ] || [ ! -f "${REPO_ROOT}/obj/${ARCH}/libc.a" ]; then
  log "building the target sysroot (obj/${ARCH}/libc.a + crt0.o)"
  make -C "${REPO_ROOT}" "ARCH=${ARCH}" "obj/${ARCH}/libc.a"
fi
[ -f "${REPO_ROOT}/obj/${ARCH}/crt0.o" ] || die "obj/${ARCH}/crt0.o missing"
[ -f "${REPO_ROOT}/obj/${ARCH}/libc.a" ]   || die "obj/${ARCH}/libc.a missing"

# --- 1. vendored tree present ----------------------------------------------
if [ ! -f "${SRC}/source/configure" ]; then
  log "ICU sources not extracted; running fetch.sh"
  bash "${VENDOR_DIR}/fetch.sh"
fi
[ -f "${SRC}/source/configure" ] || die "src/source/configure missing after fetch.sh"

# --- 2. host stage (tools for the cross buildroot) -------------------------
if [ ! -x "${CROSS_ROOT}/bin/genrb" ]; then
  log "host tools missing in ${CROSS_ROOT}; running build-host.sh (JOBS=${JOBS})"
  JOBS="${JOBS}" bash "${VENDOR_DIR}/build-host.sh"
fi
[ -x "${CROSS_ROOT}/bin/genrb" ]          || die "cross buildroot tool ${CROSS_ROOT}/bin/genrb missing"
[ -f "${CROSS_ROOT}/config/icucross.mk" ] || die "cross buildroot ${CROSS_ROOT}/config/icucross.mk missing (run build-host.sh)"

# --- 3. data strategy ------------------------------------------------------
if [ "$TRIM" = 1 ]; then
  if [ ! -f "${SRC}/source/data/locales/root.txt" ]; then
    log "ICU data sources missing; running fetch.sh --data-src"
    bash "${VENDOR_DIR}/fetch.sh" --data-src
  fi
  [ -f "${SRC}/source/data/locales/root.txt" ] || die "data sources missing after fetch.sh --data-src"
  [ -f "$FILTER" ] || die "filter file missing: $FILTER"
  # A present prebuilt archive makes ICU silently ignore ICU_DATA_FILTER_FILE
  # (byte-identical output -- measured in build-host-trimmed.sh).
  if [ -f "${SRC}/source/data/in/icudt78l.dat" ]; then
    log "parking prebuilt data/in/icudt78l.dat -> icudt78l.dat.parked-prebuilt"
    mv "${SRC}/source/data/in/icudt78l.dat" "${SRC}/source/data/in/icudt78l.dat.parked-prebuilt"
  fi
else
  if [ ! -f "${SRC}/source/data/in/icudt78l.dat" ] && [ -f "${SRC}/source/data/in/icudt78l.dat.parked-prebuilt" ]; then
    log "restoring parked prebuilt data/in/icudt78l.dat (--no-trim)"
    mv "${SRC}/source/data/in/icudt78l.dat.parked-prebuilt" "${SRC}/source/data/in/icudt78l.dat"
  fi
fi

# --- 4. overlay the committed bare-metal platform fragment -----------------
# configure maps *-none-elf to mh-unknown, which upstream ships as an error
# stub; the committed config/mh-unknown (an mh-linux copy, see its header)
# replaces it.  Always re-copy: the committed file is the source of truth.
cp "${VENDOR_DIR}/config/mh-unknown" "${SRC}/source/config/mh-unknown"
grep -q "HobbyOS bare-metal overlay" "${SRC}/source/config/mh-unknown" \
  || die "mh-unknown overlay did not land"

# --- 5. configure + build --------------------------------------------------
cd "$OBJDIR"
CONFIGURE_ARGS=(
  "--host=${TRIPLE}" --build=x86_64-pc-linux-gnu
  "--with-cross-build=${CROSS_ROOT}"
  "--prefix=${OBJDIR}/prefix"
  --disable-shared --enable-static
  --disable-samples --disable-tests --disable-extras --disable-icuio
  --disable-tools
  --with-data-packaging=static
  "CC=${CC}" "CXX=${CXX}" "CFLAGS=${CFLAGS}" "CXXFLAGS=${CXXFLAGS}"
  "LDFLAGS=${LDFLAGS}" "LIBS=${LIBS}"
)
if [ ! -f config.status ]; then
  log "configure (${ARCH}; trimmed=${TRIM})"
  t0=$(date +%s)
  if [ "$TRIM" = 1 ]; then
    ICU_DATA_FILTER_FILE="$FILTER" "${SRC}/source/configure" "${CONFIGURE_ARGS[@]}" 2>&1 | tee -a "${OBJDIR}/target-build.log"
  else
    "${SRC}/source/configure" "${CONFIGURE_ARGS[@]}" 2>&1 | tee -a "${OBJDIR}/target-build.log"
  fi
  CONFIGURE_SECS="$(( $(date +%s) - t0 ))s"
  log "configure done in ${CONFIGURE_SECS}"
else
  CONFIGURE_SECS="cached"
  log "configure already done in ${OBJDIR} (rm -rf it for a fresh one)"
fi

log "make -j${JOBS}"
t0=$(date +%s)
make -j"${JOBS}" 2>&1 | tee -a "${OBJDIR}/target-build.log"
MAKE_SECS="$(( $(date +%s) - t0 ))s"
log "make done in ${MAKE_SECS}"

# --- 6. collect the deliverables -------------------------------------------
for lib in libicuuc.a libicui18n.a libicudata.a; do
  [ -f "${OBJDIR}/lib/${lib}" ] || die "expected archive missing: ${OBJDIR}/lib/${lib}"
done
cp -f "${OBJDIR}/lib/libicuuc.a" "${OBJDIR}/lib/libicui18n.a" "${OBJDIR}/lib/libicudata.a" "${OUTDIR}/"

# --- 7. nm spot-check (renaming is ON: versioned _78 suffixes) -------------
NM="${NM:-}"
if [ -z "${NM}" ]; then
  for cand in llvm-nm llvm-nm-21 nm; do
    if command -v "$cand" >/dev/null 2>&1; then NM="$cand"; break; fi
  done
fi
[ -n "${NM}" ] || die "no nm found"
log "nm spot-check via ${NM}"
# NOTE: match against a FILE, never `nm | grep -q`: under `set -o pipefail` a
# `grep -q` that matches early closes the pipe, nm dies of SIGPIPE and the
# pipeline reports failure -- a false negative (bit this script once on the
# first symbols of each archive).
NM_TMP="$(mktemp "${TMPDIR:-/tmp}/icu-nm-XXXXXX")"
trap 'rm -f "$NM_TMP"' EXIT
PROBE_FAIL=0
SPOT_DONE=0
check_archive() { # <archive> <symbol>...
  local ar="$1"; shift
  "${NM}" --defined-only "$ar" 2>/dev/null | awk '{print $NF}' > "$NM_TMP"
  local s
  SPOT_DONE=$(( SPOT_DONE + $# ))
  for s in "$@"; do
    if grep -qxF "$s" "$NM_TMP"; then
      printf '  PASS %-48s %s\n' "$(basename "$ar")" "$s"
    else
      printf '  FAIL %-48s %s\n' "$(basename "$ar")" "$s"
      PROBE_FAIL=1
    fi
  done
}
# Symbol lists: renaming stays at ICU's default (versioned _78 suffixes), so
# match the renamed names.  Note ubrk_open lives in *common* in ICU 78
# (ubrk.cpp moved from i18n to common), hence the uc check includes it.
check_archive "${OUTDIR}/libicuuc.a"   u_init_78 u_charType_78 u_strToUpper_78 u_strToLower_78 u_strFromUTF8_78 u_strToUTF8_78 ubrk_open_78
check_archive "${OUTDIR}/libicui18n.a" ucol_open_78 ucal_open_78 udat_open_78 unum_formatDouble_78 ucsdet_open_78
check_archive "${OUTDIR}/libicudata.a" icudt78_dat
[ "$PROBE_FAIL" = 0 ] || die "nm spot-check failed (symbols above)"

# --- 8. link probe: symbol closure against crt0 + libc + libcxx ------------
# Links a tiny C program that calls the JSC-class C API surface against the
# three archives, the house libcxx.a, and libc.a.  Static-link closure is the
# point: any symbol ICU pulls in that the sysroot cannot provide shows up here
# by name.  Informational by default (RTTI cast machinery is a known upstream
# exclusion in libcxx.a); --probe-strict escalates a failure to exit 1.
PROBE_STATUS="skipped (--no-probe)"
if [ "$PROBE" = 1 ]; then
  LIBCXX_A="${REPO_ROOT}/obj/${ARCH}/libcxx.a"
  if [ ! -f "$LIBCXX_A" ]; then
    if [ -f "${LIBCXX_VENDOR}/build-target.sh" ]; then
      log "libcxx.a absent; building it (house script) for the link probe"
      bash "${LIBCXX_VENDOR}/build-target.sh" --arch "$ARCH" --objdir "${REPO_ROOT}/obj/${ARCH}/libcxx" --out "$LIBCXX_A"
    else
      log "libcxx build-target.sh missing; probe will be skipped"
    fi
  fi
  if [ -f "$LIBCXX_A" ]; then
    LD_BIN="${LD:-}"
    if [ -z "$LD_BIN" ]; then
      for cand in ld.lld ld.lld-21; do
        if command -v "$cand" >/dev/null 2>&1; then LD_BIN="$cand"; break; fi
      done
    fi
    [ -n "$LD_BIN" ] || die "no ld.lld found for the link probe"
    PROBE_DIR="${OBJDIR}/probe"; mkdir -p "$PROBE_DIR"
    # Compiled as C++ (the JSC-class consumption mode): ICU's ptypes.h only
    # pulls the C11 <uchar.h> in C mode, and the HobbyOS sysroot has no
    # uchar.h yet (documented gap; C consumers can define U_HAVE_CHAR16_T=0).
    if ${CXX} ${CXXFLAGS} -I"${SRC}/source/common" -I"${SRC}/source/i18n" \
         -c "${VENDOR_DIR}/probe/icu_target_probe.cpp" -o "${PROBE_DIR}/icu_target_probe.o" \
         > "${PROBE_DIR}/icu_probe.compile.log" 2>&1; then
      log "link probe: ${PROBE_DIR}/icu_probe.elf"
      if "${LD_BIN}" -T "${REPO_ROOT}/src/user/linker.ld" -e _start -o "${PROBE_DIR}/icu_probe.elf" \
           "${PROBE_DIR}/icu_target_probe.o" \
           "${OUTDIR}/libicui18n.a" "${OUTDIR}/libicuuc.a" "${OUTDIR}/libicudata.a" \
           "$LIBCXX_A" "${REPO_ROOT}/obj/${ARCH}/libc.a" \
           > "${PROBE_DIR}/icu_probe.link.log" 2>&1; then
        PROBE_STATUS="link OK (libicui18n.a libicuuc.a libicudata.a libcxx.a libc.a)"
        log "probe: ${PROBE_STATUS}"
      else
        PROBE_STATUS="link GAPS (informational; see ${PROBE_DIR}/icu_probe.link.log)"
        warn_syms="$(grep -o "undefined symbol: [A-Za-z0-9_.@]*" "${PROBE_DIR}/icu_probe.link.log" | sort -u | paste -sd' ' - || true)"
        log "probe: link reported undefined symbols: ${warn_syms:-<see log>}"
        log "probe: ${PROBE_STATUS}"
        if [ "$PROBE_STRICT" = 1 ]; then
          die "link probe failed (--probe-strict)"
        fi
      fi
    else
      PROBE_STATUS="compile FAILED (informational; see ${PROBE_DIR}/icu_probe.compile.log)"
      log "probe: ${PROBE_STATUS}"
      if [ "$PROBE_STRICT" = 1 ]; then
        die "probe compile failed (--probe-strict)"
      fi
    fi
  else
    PROBE_STATUS="skipped (libcxx.a unavailable)"
    log "probe: ${PROBE_STATUS}"
  fi
fi

# --- 9. manifest -----------------------------------------------------------
MANIFEST="${OUTDIR}/MANIFEST.txt"
{
  echo "ICU 78.3 target artifacts (HobbyOS, arch=${ARCH}, ${TRIPLE})"
  echo "built    : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "git      : $(git -C "${REPO_ROOT}" rev-parse HEAD 2>/dev/null || echo n/a)"
  echo "data     : $([ "$TRIM" = 1 ] && echo "trimmed (ICU_DATA_FILTER_FILE=$(basename "$FILTER")), source-built, static" || echo "prebuilt full archive, static")"
  echo "configure: $(printf '%q ' "${SRC}/source/configure" "${CONFIGURE_ARGS[@]}")"
  echo "make     : make -j${JOBS}"
  echo "times    : configure ${CONFIGURE_SECS}  make ${MAKE_SECS}"
  echo "ldflags  : ${LDFLAGS}"
  echo "spot     : ${SPOT_DONE} symbols PASS (versioned _78 names)"
  echo "probe    : ${PROBE_STATUS}"
  echo "archives :"
  for lib in libicuuc.a libicui18n.a libicudata.a; do
    printf '  %-14s %12d B  %4d members  sha256 %s\n' "$lib" \
      "$(stat -c%s "${OUTDIR}/${lib}")" "$(ar t "${OUTDIR}/${lib}" | wc -l)" \
      "$(sha256sum "${OUTDIR}/${lib}" | cut -d' ' -f1)"
  done
  if command -v file >/dev/null 2>&1; then
    sample="$(ar t "${OUTDIR}/libicuuc.a" | head -1)"
    echo "sample   : ${sample}: $(ar p "${OUTDIR}/libicuuc.a" "${sample}" | file -b -)"
  fi
} > "$MANIFEST"
log "manifest: ${MANIFEST}"
cat "$MANIFEST"
log "OK: ${OUTDIR}"
