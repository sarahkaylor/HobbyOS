#!/usr/bin/env bash
# tools/cp/build-wine.sh — build the pinned upstream Wine (docs/cp-pins.md) for
# HobbyOS's Linux-ABI compat class, 64-bit only, console-only (Phase 1, P2).
#
#   Unix side (x86_64-linux-gnu):  clang 21 against the PINNED glibc 2.31 sysroot
#                                  (AD-2; RimWorld AD-11 — never host headers)
#   PE side   (x86_64-windows):    clang 21 --target=x86_64-windows + lld-link
#                                  (Wine's own headers/CRT; no MinGW needed)
#   Result:                        $WORK/stage/CP/WINE/{bin,lib/wine,share/wine}
#                                  (layout proposal F4: prefix /CP/WINE)
#
# WHY A REAL SYSROOT AND CLANG (measured 2026-11-14 on linuxci, gcc 15.2):
#   `gcc --sysroot=S -print-search-dirs` still lists the HOST's
#   /usr/lib/gcc/x86_64-linux-gnu/15/../../../x86_64-linux-gnu (= host
#   /usr/lib/x86_64-linux-gnu) BEFORE the sysroot, so -lc/-lpthread would bind
#   to host glibc 2.4x.  clang --sysroot finds a GCC installation *inside* the
#   sysroot and then keeps every -L there.
#
# WHY GCC 10's crt/libgcc (libgcc-10-dev, focal), NOT THE HOST GCC 15's:
#   measured: with gcc 15's libgcc_eh.a in the sysroot, configure's
#   `-static-pie` probe fails with "undefined reference to `_dl_find_object'"
#   (a GLIBC_2.35 symbol) from unwind-dw2-fde-dip.o — gcc 15's static unwinder
#   is newer than the 2.31 pin, and configure then silently falls back to a
#   fixed-address `-static -Wl,-Ttext=0x7d7d00000000` preloader.  The focal
#   libgcc-10-dev matches the pin's era (and the libgcc-s1 10.5 runtime).
#
# Sysroot = the RimWorld pin set (docs/rimworld-plan.md §2, tools/mesa/
# build-hobbyos.sh:86-99, tools/gate/m1_glibc_hello.sh): libc6 + libc6-dev
# 2.31-0ubuntu9.18, linux-libc-dev 5.4.0-218.238 (UAPI headers), libgcc-s1
# 10.5.0, plus libgcc-10-dev 10.5.0 (above).  The debs' ABSOLUTE symlinks
# (lib64/ld-linux-x86-64.so.2 -> /lib/x86_64-linux-gnu/ld-2.31.so,
# usr/lib/.../libdl.so -> /lib/...) are re-pointed into the sysroot, else ld
# follows them onto the host's glibc.
#
# PRELOADER FORM: CP_WINE_PRELOADER=pie (default: whatever configure picks with
# this sysroot — static-pie) or =fixed (forces Wine's own fallback, a non-PIE
# static ET_EXEC at -Ttext=0x7d7d00000000; configure.ac:1046-1051).
#
# VARIANTS (CP_WINE_VARIANT):
#   console (default)  the P2 build above: every optional library off.
#   x                  lane C4-x2 (P4/P5): X11 + XInput2, RandR, Render, Fixes,
#                      Cursor, Xinerama, Composite, Shape, XShm, FreeType and
#                      Vulkan on, against C4-x's /CP/X11 dev tree
#                      (tools/cp/x11/build.sh -> $CP_X11_WORK/destdir/CP/X11,
#                      FreeType 2.14.3 included).  Its own work dir (default
#                      $HOME/cp/wine-build-x): the console build's tree is
#                      never touched.  winevulkan dlopens libvulkan.so.1, so
#                      configure only has to see a library with that soname:
#                      with no CP_VULKAN_LIB_DIR (a dir holding a real
#                      libvulkan.so) a 1-symbol stub is linked in
#                      $WORK/vkstub and is NEVER staged.  configure's result
#                      is checked feature by feature (config.h) and printed;
#                      a missing feature fails the step unless it is listed
#                      in CP_WINE_ALLOW_MISSING (space-separated names).
#
# Usage:  bash tools/cp/build-wine.sh [configure|build|install|audit|all]
# Env:    CP_WINE_VARIANT (console | x; default console)
#         CP_WINE_WORK  (default $HOME/cp/wine-build, x: $HOME/cp/wine-build-x)
#         CP_WINE_SYSROOT  use this glibc 2.31 + gcc-10 sysroot read-only
#                       instead of assembling $WORK/sysroot-glibc231
#         CP_X11_WORK   (x variant; default $HOME/cp/x11-build)
#         CP_VULKAN_LIB_DIR, CP_WINE_ALLOW_MISSING (x variant, above)
#         CP_RUNTIME    (default <repo>/third_party/cp-runtime)
#         JOBS          (default 12)
# Exit:   0 ok, 1 build/audit failure, 2 tooling error.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RT="${CP_RUNTIME:-$ROOT/third_party/cp-runtime}"
DL="$RT/dl"
VARIANT="${CP_WINE_VARIANT:-console}"
case "$VARIANT" in
  console) WORK="${CP_WINE_WORK:-$HOME/cp/wine-build}" ;;
  x)       WORK="${CP_WINE_WORK:-$HOME/cp/wine-build-x}"
           X11W="${CP_X11_WORK:-$HOME/cp/x11-build}"
           X11DEV="$X11W/destdir/CP/X11"; X11PC="$X11W/pc"; X11STAGE="$X11W/stage/CP/X11" ;;
  *) printf 'build-wine: ERROR: CP_WINE_VARIANT must be console or x\n'; exit 2 ;;
esac
JOBS="${JOBS:-12}"
STEP="${1:-all}"

WINE_VER=11.0
PREFIX=/CP/WINE
SYS="${CP_WINE_SYSROOT:-$WORK/sysroot-glibc231}"
SRC="$WORK/src/wine-$WINE_VER"
BLD="$WORK/build-x86_64"
STAGE="$WORK/stage"
GLIBC_MAX_ALLOWED=2.31

say() { printf 'build-wine: %s\n' "$*"; }
die() { say "ERROR: $*"; exit "${2:-1}"; }

for t in clang lld-link dpkg-deb flex bison make objdump readelf; do
  command -v "$t" >/dev/null || die "missing host tool: $t" 2
done

# --- 0. pins --------------------------------------------------------------------
bash "$ROOT/tools/cp/fetch-runtime.sh" >/dev/null || die "fetch-runtime.sh failed (run it directly for detail)" 2
say "pins verified (tools/cp/fetch-runtime.sh)"

# --- 1. glibc 2.31 sysroot --------------------------------------------------------
make_sysroot() {
  if [ -n "${CP_WINE_SYSROOT:-}" ]; then
    [ -f "$SYS/usr/include/stdio.h" ] && [ -f "$SYS/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o" ] &&
      grep -q '__GLIBC_MINOR__	31' "$SYS/usr/include/features.h" ||
      die "CP_WINE_SYSROOT=$SYS is not a glibc 2.31 + gcc-10 sysroot" 2
    say "sysroot: $SYS (CP_WINE_SYSROOT, read-only)"; return
  fi
  [ -f "$SYS/.ok" ] && { say "sysroot: $SYS (cached)"; return; }
  rm -rf "$SYS"; mkdir -p "$SYS"
  # The last deb is the GCC "installation" clang looks for inside --sysroot.
  for d in libc6_2.31-0ubuntu9.18_amd64.deb libc6-dev_2.31-0ubuntu9.18_amd64.deb \
           linux-libc-dev_5.4.0-218.238_amd64.deb libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb \
           libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb; do
    dpkg-deb -x "$DL/$d" "$SYS"
  done
  local n=0 l t
  while IFS= read -r l; do
    t=$(readlink "$l"); ln -sfn "$SYS$t" "$l"; n=$((n+1))
  done < <(find "$SYS" -type l -lname '/*')
  say "sysroot: re-pointed $n absolute symlinks into $SYS"
  for l in lib64/ld-linux-x86-64.so.2 usr/lib/x86_64-linux-gnu/libpthread.so \
           usr/lib/x86_64-linux-gnu/libdl.so usr/lib/x86_64-linux-gnu/libm.so; do
    [ -e "$SYS/$l" ] || die "sysroot symlink $l dangles"
  done
  [ -f "$SYS/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o" ] || die "libgcc-10-dev did not provide crtbeginS.o"
  say "sysroot: GCC 10.5 crt/libgcc from libgcc-10-dev (focal)"
  touch "$SYS/.ok"
}

# --- 2. source ----------------------------------------------------------------------
extract_src() {
  [ -f "$SRC/.ok" ] && { say "source: $SRC (cached)"; return; }
  rm -rf "$SRC"; mkdir -p "$WORK/src"
  tar -C "$WORK/src" -xf "$DL/wine-$WINE_VER.tar.xz"
  touch "$SRC/.ok"
  say "source: extracted wine-$WINE_VER"
}

# --- 3. configure ------------------------------------------------------------------
# Console-only phase 1: every optional host library is switched off explicitly
# so configure cannot pick up a HOST library by accident (the sysroot has none
# anyway; this makes the intent auditable in config.log).
WITHOUT=(alsa capi coreaudio cups dbus ffmpeg fontconfig freetype gettext gphoto
         gnutls gssapi gstreamer hwloc inotify krb5 netapi opencl opengl oss pcap
         pcsclite pulse sane sdl udev unwind usb v4l2 vulkan wayland x
         xcomposite xcursor xfixes xinerama xinput xinput2 xrandr xrender xshape
         xshm xxf86vm)

# The x variant's features: name | configure --with name(s) | config.h macro
# that proves it.  Everything else stays in WITHOUT.
X_FEATURES="x|x|SONAME_LIBX11
xext|x|SONAME_LIBXEXT
xinput|xinput|SONAME_LIBXI
xinput2|xinput2|HAVE_X11_EXTENSIONS_XINPUT2_H
xrandr|xrandr|SONAME_LIBXRANDR
xrender|xrender|SONAME_LIBXRENDER
xfixes|xfixes|SONAME_LIBXFIXES
xcursor|xcursor|SONAME_LIBXCURSOR
xinerama|xinerama|SONAME_LIBXINERAMA
xcomposite|xcomposite|SONAME_LIBXCOMPOSITE
xshape|xshape|HAVE_X11_EXTENSIONS_SHAPE_H
xshm|xshm|HAVE_LIBXXSHM
freetype|freetype|SONAME_LIBFREETYPE
vulkan|vulkan|SONAME_LIBVULKAN"

# A library with libvulkan.so.1's soname and vkGetInstanceProcAddr, for
# configure's WINE_CHECK_SONAME only (winevulkan dlopens the real loader at
# run time; C5-vk2 stages it in /CP/VK).  Never installed or staged.
vk_stub() {
  local d="$WORK/vkstub"
  mkdir -p "$d"
  printf 'void *vkGetInstanceProcAddr(void *i, const char *n) { (void)i; (void)n; return 0; }\n' > "$d/vkstub.c"
  clang --sysroot="$SYS" -shared -fPIC -O2 -Wl,-soname,libvulkan.so.1 -o "$d/libvulkan.so.1" "$d/vkstub.c" ||
    die "vulkan configure stub failed" 2
  ln -sfn libvulkan.so.1 "$d/libvulkan.so"
  printf '%s\n' "$d"
}

do_configure() {
  make_sysroot; extract_src
  rm -rf "$BLD"; mkdir -p "$BLD"
  local wo=() p
  local without=("${WITHOUT[@]}")
  local pcdir="$SYS/usr/lib/x86_64-linux-gnu/pkgconfig" xargs=() ldflags=""
  if [ "$VARIANT" = x ]; then
    [ -f "$X11DEV/include/X11/Xlib.h" ] && [ -f "$X11PC/x11.pc" ] ||
      die "no X11 dev tree at $X11DEV (run tools/cp/x11/build.sh with CP_X11_WORK=$X11W)" 2
    [ -f "$X11PC/freetype2.pc" ] || die "no freetype2.pc in $X11PC (tools/cp/x11/build.sh builds FreeType)" 2
    local on=" " name w m
    while IFS='|' read -r name w m; do on="$on$w "; done <<<"$X_FEATURES"
    without=()
    for p in "${WITHOUT[@]}"; do case "$on" in *" $p "*) ;; *) without+=("$p") ;; esac; done
    local vkdir="${CP_VULKAN_LIB_DIR:-}"
    [ -n "$vkdir" ] || vkdir=$(vk_stub)
    pcdir="$pcdir:$X11PC"
    ldflags="-Wl,-rpath-link,$X11DEV/lib -L$vkdir"
    xargs=(--with-x --x-includes="$X11DEV/include" --x-libraries="$X11DEV/lib")
    say "configure: variant x: X11 + FreeType from $X11DEV, libvulkan for configure from $vkdir"
  fi
  for p in "${without[@]}"; do wo+=("--without-$p"); done
  local pre=()
  # Force Wine's fixed-address fallback by failing configure's -static-pie probe.
  [ "${CP_WINE_PRELOADER:-pie}" = fixed ] && pre=(ac_cv_cflags__static_pie=no)
  say "configure: CC='clang --sysroot=$SYS', PE: clang --target=x86_64-windows"
  ( cd "$BLD" &&
    PKG_CONFIG_LIBDIR="$pcdir" PKG_CONFIG_PATH= \
    CC="clang --sysroot=$SYS" \
    CFLAGS="-O2 -g" \
    x86_64_CC=clang \
    env ${ldflags:+"LDFLAGS=$ldflags"} \
    "$SRC/configure" --prefix="$PREFIX" --enable-archs=x86_64 --disable-tests \
      "${xargs[@]}" "${wo[@]}" "${pre[@]}" ) > "$WORK/configure.log" 2>&1 || {
    tail -30 "$WORK/configure.log"; die "configure failed (see $WORK/configure.log)"; }
  [ "$VARIANT" = x ] && check_x_features
  grep -E '^configure: (WARNING|error)|x86_64_CC|CROSSCC' "$WORK/configure.log" | head -40 || true
  say "configure: PE compiler = $(grep -E '^x86_64_CC=' "$BLD/config.log" | head -1)"
  say "configure: preloader LDFLAGS = $(grep -E '^WINEPRELOADER_LDFLAGS=' "$BLD/config.log" | head -1)"
  say "configure: loader LDFLAGS    = $(grep -E '^WINELOADER_LDFLAGS=' "$BLD/config.log" | head -1)"
}

# Feature-by-feature proof from config.h (raw #define lines), plus the
# sonames winex11/win32u/winevulkan will dlopen.
check_x_features() {
  local h="$BLD/include/config.h" name w m line bad=0 sum="$WORK/configure-summary.txt"
  [ -f "$h" ] || die "no $h"
  : > "$sum"
  while IFS='|' read -r name w m; do
    line=$(grep -E "^#define $m( |$)" "$h" | head -1 || true)
    if [ -n "$line" ]; then printf 'build-wine: feature %-10s ON   %s\n' "$name" "$line" | tee -a "$sum"
    else
      printf 'build-wine: feature %-10s OFF  (no #define %s)\n' "$name" "$m" | tee -a "$sum"
      case " ${CP_WINE_ALLOW_MISSING:-} " in *" $name "*) ;; *) bad=1 ;; esac
    fi
  done <<<"$X_FEATURES"
  grep -E '^configure: (WARNING|error)' "$WORK/configure.log" | sed 's/^/build-wine: configure says: /' | tee -a "$sum" || true
  [ "$bad" = 0 ] || die "variant x: a required feature is off (see above; CP_WINE_ALLOW_MISSING to accept)"
}

do_build() {
  [ -f "$BLD/Makefile" ] || do_configure
  say "build: make -j$JOBS (log $WORK/build.log)"
  local t0=$SECONDS
  make -C "$BLD" -j"$JOBS" > "$WORK/build.log" 2>&1 || {
    grep -nE 'error:|Error [0-9]' "$WORK/build.log" | head -30; die "make failed (see $WORK/build.log)"; }
  say "build: done in $((SECONDS - t0)) s"
}

do_install() {
  rm -rf "$STAGE"
  # install-lib = the runtime only (no widl/wrc/winegcc/headers/import libs).
  make -C "$BLD" install-lib DESTDIR="$STAGE" > "$WORK/install.log" 2>&1 || die "make install-lib failed"
  rm -rf "$STAGE$PREFIX/share/man" "$STAGE$PREFIX/share/applications"
  if [ "${CP_WINE_STRIP:-1}" = 1 ]; then
    # Debug info stays in $BLD (unstripped objects); the staged tree is what
    # HobbyOS has to hold, so strip DWARF from both halves.
    find "$STAGE$PREFIX/lib/wine/x86_64-windows" -type f \( -name '*.dll' -o -name '*.exe' \
         -o -name '*.drv' -o -name '*.sys' -o -name '*.ocx' -o -name '*.cpl' -o -name '*.acm' \
         -o -name '*.ax' -o -name '*.tlb' -o -name '*.ds' -o -name '*.msstyles' -o -name '*.com' \) \
         -exec llvm-strip --strip-debug {} + 2>/dev/null || true
    find "$STAGE$PREFIX" -type f \( -name '*.so' -o -perm -u+x \) -exec sh -c \
      'for f; do [ "$(head -c4 "$f" | od -An -c | tr -d " ")" = 177ELF ] && strip --strip-debug "$f"; done' sh {} +
  fi
  say "install: $(du -sh "$STAGE$PREFIX" | cut -f1) staged at $STAGE$PREFIX (strip-debug=${CP_WINE_STRIP:-1})"
  ( cd "$STAGE$PREFIX" && find . -type f | LC_ALL=C sort | xargs sha256sum ) > "$WORK/stage-manifest.sha256"
  say "install: manifest $WORK/stage-manifest.sha256 ($(wc -l < "$WORK/stage-manifest.sha256") files)"
}

# --- 4. audit: GLIBC_ symbol versions + DT_NEEDED closure -------------------------
do_audit() {
  local max="" bad=0 f v needed
  local report="$WORK/audit.txt"
  : > "$report"
  while IFS= read -r f; do
    [ "$(head -c4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ] || continue
    v=$(objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
    needed=$(readelf -dW "$f" | awk '/NEEDED/{gsub(/[\[\]]/,"",$NF); printf "%s ", $NF}' || true)
    interp=$(readelf -lW "$f" 2>/dev/null | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p' || true)
    printf '%-70s GLIBC_max=%-6s interp=%s needed=%s\n' "${f#"$STAGE"}" "${v:-none}" "${interp:--}" "$needed" >> "$report"
    if [ -n "$v" ]; then
      if [ "$(printf '%s\n%s\n' "$v" "$GLIBC_MAX_ALLOWED" | sort -V | tail -1)" != "$GLIBC_MAX_ALLOWED" ]; then
        say "AUDIT FAIL: ${f#"$STAGE"} needs GLIBC_$v > $GLIBC_MAX_ALLOWED"; bad=1
      fi
      max=$(printf '%s\n%s\n' "${max:-0}" "$v" | sort -V | tail -1)
    fi
  done < <(find "$STAGE$PREFIX" -type f \( -perm -u+x -o -name '*.so' \))
  local nelf; nelf=$(wc -l < "$report")
  say "audit: $nelf ELF objects; max GLIBC_ symbol version required = GLIBC_${max:-none} (report $report)"
  say "audit: DT_NEEDED union: $(awk -F'needed=' '{print $2}' "$report" | tr ' ' '\n' | grep . | sort -u | tr '\n' ' ')"
  [ "$bad" = 0 ] || die "binaries need glibc newer than the $GLIBC_MAX_ALLOWED pin"
  [ "$VARIANT" = x ] && audit_x_closure "$report"
  return 0
}

# x variant: every DT_NEEDED must resolve inside {this Wine stage} + {the
# /CP/X11 stage} + {glibc 2.31}, and every soname Wine dlopens (config.h
# SONAME_*) must be in /CP/X11 -- except libvulkan.so.1, which is /CP/VK's
# (lane C5-vk2).
audit_x_closure() {
  local report="$1" n bad=0 so m
  local glibc=" libc.so.6 libdl.so.2 libpthread.so.0 libm.so.6 librt.so.1 ld-linux-x86-64.so.2 libgcc_s.so.1 libresolv.so.2 libutil.so.1 "
  [ -d "$X11STAGE/lib" ] || die "no /CP/X11 stage at $X11STAGE (tools/cp/x11/build.sh stage)"
  local own; own=" $(find "$STAGE$PREFIX" -name '*.so*' -printf '%f ') "
  for n in $(awk -F'needed=' '{print $2}' "$report" | tr ' ' '\n' | grep . | sort -u); do
    case "$glibc$own" in *" $n "*) continue ;; esac
    [ -f "$X11STAGE/lib/$n" ] && continue
    say "AUDIT FAIL: DT_NEEDED $n is in neither the Wine stage, /CP/X11 nor glibc 2.31"; bad=1
  done
  while read -r m so; do
    so=${so//\"/}
    if [ "$so" = libvulkan.so.1 ]; then say "audit: dlopen $m=$so -> /CP/VK (C5-vk2's loader; not in this stage)"; continue; fi
    # configure defines SONAME_LIBODBC even with no unixODBC; odbc32 only
    # reports "no driver manager" when it is absent.
    if [ "$so" = libodbc.so ]; then say "audit: dlopen $m=$so -> absent on purpose (odbc32 fails soft)"; continue; fi
    if [ -f "$X11STAGE/lib/$so" ]; then say "audit: dlopen $m=$so -> /CP/X11/lib/$so"
    else say "AUDIT FAIL: Wine dlopens $so ($m) and /CP/X11 lacks it"; bad=1; fi
  done < <(grep -E '^#define SONAME_' "$BLD/include/config.h" | awk '{print $2, $3}')
  say "audit: x closure: every DT_NEEDED and every dlopen soname resolves in {Wine stage} + {/CP/X11} + {glibc 2.31} (+ /CP/VK for vulkan): $([ $bad = 0 ] && echo yes || echo NO)"
  [ "$bad" = 0 ] || die "x closure audit failed"
}

case "$STEP" in
  configure) do_configure ;;
  build)     do_build ;;
  install)   do_install ;;
  audit)     do_audit ;;
  all)       do_configure; do_build; do_install; do_audit ;;
  *) die "unknown step $STEP" 2 ;;
esac
