#!/usr/bin/env bash
# cross-intel-wk2-libs.sh — WK-4b (D-7) intel staging: curl/mbedTLS for the
# x86_64-none-elf WebKit NetworkProcess backend (WE-2 lane).
#
# The fork's ARM recipe (HobbyOS/continuation/wk4b/wk4b-libs-cross.sh) is
# ARM-only; the canonical WebKitBuild/HobbyOS-intel build dir predates the
# merged USE_CURL wiring, so a fresh ARCH=intel configure fails at
# find_package(CURL) — no intel curl/mbedTLS staging exists.  This script is
# the missing intel half, mirroring wk4b-libs-cross.sh one-for-one (same
# layout, same patches, same closure) with the intel spec delta:
#
#   triple              x86_64-none-elf
#   arch CFLAGS         -mno-red-zone -mcmodel=large   (matches the OS
#                       USER_CFLAGS + toolchain-hobbyos.cmake intel row)
#   freestanding        -nostdinc -isystem <clang-resource>/include
#   non-PIE             -Wl,-no-pie (x86_64 none-elf defaults PIE; the
#                       non-PIC sysroot closure is not PIC)
#   setjmp              obj/intel/setjmp.o  (the OS keeps setjmp/longjmp
#                       OUT of libc.a on x86_64; the ARM naked defs in the
#                       fork's gaps.c are compiled out there)
#
# It cross-builds, into the SAME prefix layout as the ARM staging
# (/home/sarah/webkit-hobbyos-wk2/<arch>/prefix {include/, lib/}):
#
#   mbedTLS 3.6.7   -> include/mbedtls (+ include/psa), lib/libmbed{tls,x509,crypto}.a
#   libcurl 8.22.0  -> include/curl/curl.h, lib/libcurl.a  (mbedTLS backend)
#
# and compiles the fork's BSD/POSIX net-compat closure objects used by BOTH
# the curl autoconf probes and (via the fork toolchain) the WebKit link
# closure.  Finally it runs a link smoke: a tiny freestanding executable
# referencing curl_easy_init() + mbedtls_ssl_init() links against the prefix
# archives + sysroot closure (the same shape as the NetworkProcess link).
#
#   bash tools/cross-intel-wk2-libs.sh [--only mbedtls,curl] [--prefix DIR]
#                                      [--srcroot DIR] [-j N] [--skip-smoke]
#
# tarballs come from the OS repo's committed third_party (mbedtls-3.6.7.tar.bz2,
# curl-8.22.0.tar.xz); FORK (read-only) supplies the net-compat sources +
# shim headers.  The fork is NEVER modified; build artifacts land only in
# PREFIX / SRCROOT.
set -euo pipefail

export PATH="$HOME/.local/bin:/usr/lib/llvm-21/bin:$PATH"

HDYOS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # OS repo root
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FORK="${FORK:-/home/sarah/webkit-hobbyos}"                  # canonical fork (READ-ONLY)
TRIPLE=x86_64-none-elf
SPEC="-mno-red-zone -mcmodel=large"
ARCH=intel
PREFIX="${HOBBYOS_WK2_PREFIX:-/home/sarah/webkit-hobbyos-wk2/${ARCH}/prefix}"
SRCROOT="${WK4B_SRCROOT:-/home/sarah/webkit-hobbyos-wk2/${ARCH}/src}"
DL="${DL_DIR:-${HDYOS}/third_party}"                        # OS repo committed tarballs
OBJROOT="${SRCROOT}/wk4b-obj"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
ONLY=""
SKIP_SMOKE=0

usage() { echo "usage: $0 [--only mbedtls,curl] [--prefix DIR] [--srcroot DIR] [-j N] [--skip-smoke]"; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --only) ONLY="$2"; shift 2;;
    --prefix) PREFIX="$2"; shift 2;;
    --srcroot) SRCROOT="$2"; shift 2;;
    -j) JOBS="$2"; shift 2;;
    --skip-smoke) SKIP_SMOKE=1; shift;;
    *) usage;;
  esac
done
want() { [ -z "$ONLY" ] && return 0; case ",${ONLY}," in *",$1,"*) return 0;; *) return 1;; esac; }

PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
SRCROOT="$(mkdir -p "$SRCROOT" "$OBJROOT" && cd "$SRCROOT" && pwd)"
LOGS="${SRCROOT}/wk4b-logs"
mkdir -p "$LOGS"

[ -d "$FORK/HobbyOS/lib" ] || { echo "FATAL: FORK=$FORK missing HobbyOS/lib (read-only fork path)"; exit 1; }

CLANG_RES="$(clang -print-resource-dir)/include"
STDINC="-nostdinc -isystem ${CLANG_RES}"
SYSINCS="-I${HDYOS}/src/libc/include -I${HDYOS}/src/include -I${HDYOS}/src/user_include"
FORK_SHIM="-I${FORK}/HobbyOS/include"
TARGET_FLAGS="--target=${TRIPLE} -ffreestanding -nostdlib"
LSCRIPT="${HDYOS}/src/user/linker.ld"
CRT="${HDYOS}/obj/${ARCH}/crt0.o"
LIBC="${HDYOS}/obj/${ARCH}/libc.a"
SETJMP="${HDYOS}/obj/${ARCH}/setjmp.o"
NETCOMPAT_O="${OBJROOT}/netcompat.o"
RESOLV_O="${OBJROOT}/resolv.o"
MBEDTLS_ALT_O="${OBJROOT}/mbedtls_alt.o"
GAPS_O="${OBJROOT}/gaps.o"
GAPS_VARC_O="${OBJROOT}/gaps_varc.o"

log() { printf '[cross-intel-wk2:%s] %s\n' "$ARCH" "$*"; }

stale() {
  local out="$1"; shift
  [ -f "$out" ] || return 0
  local s
  for s in "$@"; do [ "$s" -nt "$out" ] && return 0; done
  return 1
}

# ---------------------------------------------------------- net-compat closure
# Same closure objects the fork toolchain builds into every WebKit process
# (netcompat.c weak POSIX/BSD defs, OS resolv, mbedtls platform glue, gap
# fillers).  intel: gaps.c/gaps_varc.S/-mtune and gaps_varc.S use
# -mno-red-zone -mcmodel=large (the ARM recipe's -mcpu=generic/-mcpu=cortex-a53
# are ARM-only; the fork toolchain's intel fork-lib CFLAGS are these).
build_closure() {
  local SHIMS="${FORK}/HobbyOS/include"
  if stale "$NETCOMPAT_O" "${FORK}/HobbyOS/lib/netcompat.c" "${SHIMS}/netdb.h" "${SHIMS}/sys/socket.h" "${SHIMS}/errno.h" "${SHIMS}/netinet/in.h" "${SHIMS}/unistd.h" "${SHIMS}/fcntl.h"; then
    clang ${TARGET_FLAGS} ${SPEC} -O2 -g ${STDINC} ${FORK_SHIM} ${SYSINCS} \
      -c "${FORK}/HobbyOS/lib/netcompat.c" -o "$NETCOMPAT_O"
  fi
  if stale "$RESOLV_O" "${HDYOS}/src/user/resolv.c"; then
    clang ${TARGET_FLAGS} ${SPEC} -O2 -g ${STDINC} ${FORK_SHIM} ${SYSINCS} \
      -c "${HDYOS}/src/user/resolv.c" -o "$RESOLV_O"
  fi
  if stale "$MBEDTLS_ALT_O" "${FORK}/HobbyOS/lib/mbedtls_alt.c"; then
    clang ${TARGET_FLAGS} ${SPEC} -O2 -g ${STDINC} ${FORK_SHIM} ${SYSINCS} \
      -c "${FORK}/HobbyOS/lib/mbedtls_alt.c" -o "$MBEDTLS_ALT_O"
  fi
  if stale "$GAPS_O" "${FORK}/HobbyOS/lib/gaps.c"; then
    clang ${TARGET_FLAGS} ${SPEC} -O2 -g ${STDINC} ${SYSINCS} \
      -c "${FORK}/HobbyOS/lib/gaps.c" -o "$GAPS_O"
  fi
  if stale "$GAPS_VARC_O" "${FORK}/HobbyOS/lib/gaps_varc.S"; then
    clang ${TARGET_FLAGS} ${SPEC} -O2 -g ${STDINC} ${SYSINCS} \
      -c "${FORK}/HobbyOS/lib/gaps_varc.S" -o "$GAPS_VARC_O"
  fi
  log "closure objects ok (netcompat $(stat -c%s "$NETCOMPAT_O") B / resolv $(stat -c%s "$RESOLV_O") B / mbedtls_alt $(stat -c%s "$MBEDTLS_ALT_O") B / gaps $(stat -c%s "$GAPS_O") B / gaps_varc $(stat -c%s "$GAPS_VARC_O") B)"
}

# ------------------------------------------------------------------- mbedTLS
build_mbedtls() {
  want mbedtls || return 0
  if [ -f "${PREFIX}/lib/libmbedtls.a" ] && [ -f "${PREFIX}/lib/libmbedx509.a" ] && [ -f "${PREFIX}/lib/libmbedcrypto.a" ]; then
    log "mbedtls: cached"; return 0
  fi
  log "mbedtls: configure-free make build (static libs, no programs/tests)"
  local d="${SRCROOT}/mbedtls"
  [ -f "$d/library/Makefile" ] || { mkdir -p "$d"; tar -C "$d" --strip-components=1 -xjf "${DL}/mbedtls-3.6.7.tar.bz2"; }
  ( cd "$d"
    make -C library -j"${JOBS}" \
      libmbedcrypto.a libmbedx509.a libmbedtls.a \
      AR=llvm-ar \
      CC="clang ${TARGET_FLAGS} ${SPEC}" \
      CFLAGS="-O2 -g ${STDINC} ${FORK_SHIM} ${SYSINCS} -I${TOOLS_DIR} -DMBEDTLS_USER_CONFIG_FILE='\"mbedtls-user-config.h\"'" \
      >"${LOGS}/mbedtls-make.log" 2>&1 )
  mkdir -p "${PREFIX}/lib" "${PREFIX}/include"
  cp -f "$d/library/libmbedtls.a" "$d/library/libmbedx509.a" "$d/library/libmbedcrypto.a" "${PREFIX}/lib/"
  cp -rf "$d/include/mbedtls" "$d/include/psa" "${PREFIX}/include/"
  for a in libmbedtls.a libmbedx509.a libmbedcrypto.a; do
    [ -f "${PREFIX}/lib/$a" ] || { log "mbedtls FAILED ($a)"; return 1; }
  done
  log "mbedtls ok: $(grep -m1 MBEDTLS_VERSION_STRING ${PREFIX}/include/mbedtls/build_info.h | tr -s ' ') ($(stat -c%s "${PREFIX}/lib/libmbedtls.a") + $(stat -c%s "${PREFIX}/lib/libmbedx509.a") + $(stat -c%s "${PREFIX}/lib/libmbedcrypto.a") B)"
}

# ------------------------------------------------------------------- libcurl
# HobbyOS isn't on curl's hard-coded 'systems that need <sys/select.h>'
# list (include/curl/curl.h), so fd_set is undeclared in <curl/multi.h>'s
# curl_multi_fdset — breaking both the build AND every consumer TU of the
# installed header (WebKit's CurlContext.h includes <curl/curl.h>).  The
# port keeps sys/select.h first-class (fork shim), so include it for all
# non-Windows targets.  Idempotent + documented (applied to the source tree,
# so the patched header also installs into the prefix).
patch_curl_select() {
  local f="$1/include/curl/curl.h"
  grep -q "WK-4b" "$f" && { log "curl: select patch cached"; return 0; }
  python3 - "$f" <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "    _POSIX_C_SOURCE >= 200112L)\n#include <sys/select.h>\n#endif\n"
addition = anchor + """
/* HobbyOS WK-4b (wk4b-libs-cross.sh): the port keeps <sys/select.h>
   first-class (fork shim; fd_set is needed by <curl/multi.h>'s
   curl_multi_fdset and by the curl backend), and it is not on the list
   above -> include it for every non-Windows target. */
#if !defined(_WIN32)
#include <sys/select.h>
#endif
"""
if anchor not in src:
    sys.exit("anchor not found in " + path)
open(path, "w").write(src.replace(anchor, addition, 1))
print("patched", path)
PYEOF
}

patch_curl_wait() {
  local f="$1/lib/curlx/wait.c"
  grep -q "WK-4b" "$f" && { log "curl: wait.c poll patch cached"; return 0; }
  python3 - "$f" <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path).read()

inc_old = """#include "curl_setup.h"

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#elif defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif
"""
inc_new = inc_old + """
/* HobbyOS WK-4b (wk4b-libs-cross.sh): Curl_wait_ms() sleeps via
   poll(NULL, 0, ms) — needs the real <poll.h> (HAVE_POLL_H). */
#ifdef HAVE_POLL_H
#include <poll.h>
#elif defined(HAVE_SYS_POLL_H)
#include <sys/poll.h>
#endif
"""
if inc_old not in src:
    sys.exit("include anchor not found in " + path)

sel_old = """#else
  /* avoid using poll() for this since it behaves incorrectly with no sockets
     on Apple operating systems */
  {
    struct timeval pending_tv;
    r = select(0, NULL, NULL, NULL, curlx_mstotv(&pending_tv, timeout_ms));
  }
#endif /* _WIN32 */
"""
sel_new = """#else
  /* HobbyOS WK-4b (wk4b-libs-cross.sh): the libc select() is ms-based, not
     POSIX timeval (a pointer here would be passed as the ms arg).  This is a
     pure sleep with no fds, so poll(NULL, 0, ms) — a first-class sysroot
     call — has identical semantics; all real fd waiting already goes through
     curl's poll-based engine (HAVE_POLL). */
  {
    r = poll(NULL, 0, (int)timeout_ms);
  }
#endif /* _WIN32 */
"""
if sel_old not in src:
    sys.exit("select anchor not found in " + path)

src = src.replace(inc_old, inc_new, 1).replace(sel_old, sel_new, 1)
open(path, "w").write(src)
print("patched", path)
PYEOF
}

build_curl() {
  want curl || return 0
  [ -f "${PREFIX}/lib/libcurl.a" ] && { log "curl: cached"; return 0; }
  log "curl: configure+build (static, mbedTLS backend)"
  local d="${SRCROOT}/curl"
  [ -f "$d/configure" ] || { mkdir -p "$d"; tar -C "$d" --strip-components=1 -xJf "${DL}/curl-8.22.0.tar.xz"; }
  patch_curl_select "$d"
  patch_curl_wait "$d"

  # Closure for the autoconf probes: sysroot closure + the fork objects
  # (net-compat, mbedTLS platform glue, gap fillers).  intel: setjmp/longjmp
  # ride in via obj/intel/setjmp.o (the OS keeps them out of libc.a on
  # x86_64; the fork's gaps.c naked defs are ARM-only).  (Stripped out of the
  # generated Makefiles right after configure — archives need no closure and
  # libtool chokes on non-libtool objects.)
  export CC="clang ${TARGET_FLAGS} ${SPEC} ${STDINC}"
  export CFLAGS="-O2 -g ${STDINC} ${FORK_SHIM} ${SYSINCS}"
  export CPPFLAGS="${STDINC} ${FORK_SHIM} ${SYSINCS} -I${PREFIX}/include"
  export LDFLAGS="-T ${LSCRIPT} -fuse-ld=lld -Wl,-no-pie -L${PREFIX}/lib ${CRT} ${LIBC} ${SETJMP} ${GAPS_O} ${GAPS_VARC_O} ${MBEDTLS_ALT_O} ${NETCOMPAT_O} ${RESOLV_O}"
  export LIBS=""
  export AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm
  export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"
  export CPATH=""

  ( cd "$d"
    ./configure --host="${TRIPLE}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX}" \
      --disable-shared --enable-static \
      --with-mbedtls="${PREFIX}" \
      --without-openssl --without-gnutls --without-wolfssl \
      --without-libpsl --without-zlib --without-brotli --without-zstd \
      --without-libidn2 --without-librtmp --without-nghttp2 \
      --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
      --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
      --disable-smtp --disable-ftp --disable-gopher --disable-mqtt --disable-manual \
      --disable-threaded-resolver --disable-ares \
      --enable-unix-sockets --disable-ipv6 \
      >"${LOGS}/curl-configure.log" 2>&1 \
      || { log "curl configure FAILED — see ${LOGS}/curl-configure.log"; tail -40 "${LOGS}/curl-configure.log"; return 1; }

    find . \( -name Makefile -o -name '*.mk' \) -exec sed -i \
      "s#${CRT}# #g; s#${LIBC}# #g; s#${SETJMP}# #g; s#${GAPS_O}# #g; s#${GAPS_VARC_O}# #g; s#${MBEDTLS_ALT_O}# #g; s#${NETCOMPAT_O}# #g; s#${RESOLV_O}# #g" {} +

    make -C lib -j"${JOBS}" >"${LOGS}/curl-make.log" 2>&1 \
      || { log "curl make FAILED — see ${LOGS}/curl-make.log"; tail -40 "${LOGS}/curl-make.log"; return 1; }
    mkdir -p "${PREFIX}/lib" "${PREFIX}/include"
    cp -f lib/.libs/libcurl.a "${PREFIX}/lib/"
    cp -rf include/curl "${PREFIX}/include/"
    mkdir -p "${PREFIX}/bin" && cp -f curl-config "${PREFIX}/bin/" 2>/dev/null || true )
  [ -f "${PREFIX}/lib/libcurl.a" ] || { log "curl FAILED (archive missing)"; return 1; }
  [ -f "${PREFIX}/include/curl/curl.h" ] || { log "curl FAILED (headers missing)"; return 1; }
  log "curl ok: $(grep -m1 'LIBCURL_VERSION ' "${PREFIX}/include/curl/curlver.h" | tr -s ' ') ($(stat -c%s "${PREFIX}/lib/libcurl.a") B)"
}

# ---------------------------------------------------------------- link smoke
# A tiny freestanding intel executable referencing curl_easy_init() +
# mbedtls_ssl_init(), linked against the staged archives + the full sysroot
# closure — the same link shape (and same closure members) as the WebKit
# NetworkProcess link.
build_smoke() {
  want smoke || return 0
  local c="${OBJROOT}/intel-link-smoke.c" o="${OBJROOT}/intel-link-smoke.o" bin="${OBJROOT}/intel-link-smoke"
  [ -f "${PREFIX}/lib/libcurl.a" ] || { log "smoke: skip (no libcurl.a)"; return 1; }
  if stale "$bin" "$c" "$o" \
      "${PREFIX}/lib/libcurl.a" "${PREFIX}/lib/libmbedtls.a" \
      "${PREFIX}/lib/libmbedx509.a" "${PREFIX}/lib/libmbedcrypto.a" \
      "$CRT" "$LIBC" "$SETJMP" "$NETCOMPAT_O" "$MBEDTLS_ALT_O" "$GAPS_O" "$GAPS_VARC_O"; then
    cat > "$c" <<'CEOF'
/* WE-2 link smoke: reference the two staged backends, never call them.
   volatile globals -> the compiler cannot elide the function-pointer
   initializers, so both relocations survive to the link. */
#include <curl/curl.h>
#include <mbedtls/ssl.h>
void *(*volatile wk_curl_init)(void) = (void *)curl_easy_init;
void (*volatile wk_mbedtls_ssl_init)(mbedtls_ssl_context *) = mbedtls_ssl_init;
int main(void) {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    return (wk_curl_init && wk_mbedtls_ssl_init) ? 0 : 1;
}
CEOF
    clang ${TARGET_FLAGS} ${SPEC} -O2 ${STDINC} -I${PREFIX}/include ${FORK_SHIM} ${SYSINCS} \
      -c "$c" -o "$o"
    clang ${TARGET_FLAGS} ${SPEC} -O2 -fuse-ld=lld -nostdlib -nostartfiles \
      -T ${LSCRIPT} -Wl,-no-pie -Wl,-e,_start -o "$bin" \
      -L${PREFIX}/lib \
      "$CRT" "$o" \
      -Wl,--start-group \
      "$LIBC" "$SETJMP" "$NETCOMPAT_O" "$MBEDTLS_ALT_O" "$RESOLV_O" "$GAPS_O" "$GAPS_VARC_O" \
      -lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
      -Wl,--end-group \
      >"${LOGS}/smoke-link.log" 2>&1 \
      || { log "smoke LINK FAILED — see ${LOGS}/smoke-link.log"; tail -40 "${LOGS}/smoke-link.log"; return 1; }
  fi
  local n
  # t/T = defined (lld may resolve the archive's global to a local def in the
  # final binary under -ffunction-sections/--gc-sections), w/W = weak import (no).
  n="$(nm "$bin" | grep -cE " [tTwW] (curl_easy_init|mbedtls_ssl_init)$")"
  log "smoke: ${n} of 2 backend symbols resolved to concrete defs"
  [ "$n" = "2" ] || { log "smoke FAILED: backend symbols not defined"; return 1; }
  log "smoke ok: $(stat -c%s "$bin") B — curl_easy_init + mbedtls_ssl_init linked"
}

build_closure
build_mbedtls
build_curl
if [ "$SKIP_SMOKE" = "0" ]; then build_smoke; fi
log "ALL DONE: ${PREFIX}"
ls -la "${PREFIX}/lib/"*.a | awk '{print $5, $9}'
