#!/usr/bin/env bash
# tools/cp/x11/build.sh — build the REAL X11 client libraries (libX11, libxcb
# + every xcb extension, Xext, Xi, Xrandr, Xrender, Xfixes, Xcursor, Xinerama,
# xshmfence) as x86_64 shared objects for HobbyOS's Linux-ABI compat class,
# against the PINNED glibc 2.31 sysroot (docs/cyberpunk.md AD-2, RimWorld
# AD-11: never against host or HobbyOS headers).  Docs: docs/cp-x11.md.
#
#   compiler  clang --sysroot=<glibc 2.31 sysroot>   (tools/cp/build-wine.sh
#             explains why clang and not gcc, and why focal's gcc-10 libgcc)
#   prefix    /CP/X11  (compiled-in paths: share/X11/locale, XErrorDB, icons)
#   install   $WORK/destdir/CP/X11   full dev tree (headers, .pc, .so links)
#   pkgconfig $WORK/pc               the dev tree's .pc files with /CP/X11
#                                     rewritten to the destdir (use this as
#                                     PKG_CONFIG_LIBDIR to build against it)
#   stage     $WORK/stage/CP/X11     the runtime tree for the HobbyOS disk:
#             lib/<SONAME> (real files, no symlinks — FAT has none), DT_RPATH
#             $ORIGIN on every library so a library loaded from /CP/X11/lib
#             finds its siblings there BEFORE LD_LIBRARY_PATH (=/LIB64, where
#             the RimWorld shim libX11.so.6 may live), + share/X11/{locale,
#             XErrorDB,Xcms.txt}.
#
# Usage:  bash tools/cp/x11/build.sh [build|stage|audit|smoke|all]   (default all)
# Env:    CP_X11_WORK  (default $HOME/cp/x11-build)
#         CP_SYSROOT   (default $HOME/cp/wine-build/sysroot-glibc231 if it has
#                       .ok — lane C2-wine's, used READ-ONLY — else one is
#                       assembled in $CP_X11_WORK/sysroot from the
#                       tools/cp/fetch-runtime.sh debs)
#         CP_RUNTIME   (default <repo>/third_party/cp-runtime)
#         JOBS         (default 6)
#         CP_X11_FORCE=1  rebuild every package
# Host tools: clang, make, python3, patchelf, objdump/readelf/nm, xz, dpkg-deb.
# Exit:   0 ok, 1 build/audit failure, 2 tooling error.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RT="${CP_RUNTIME:-$ROOT/third_party/cp-runtime}"
DL="$RT/x11/dl"
WORK="${CP_X11_WORK:-$HOME/cp/x11-build}"
JOBS="${JOBS:-6}"
STEP="${1:-all}"
PREFIX=/CP/X11
DEST="$WORK/destdir"
DEV="$DEST$PREFIX"
PCDIR="$WORK/pc"
STAGE="$WORK/stage$PREFIX"
LOGS="$WORK/logs"
GLIBC_MAX_ALLOWED=2.31
# The glibc 2.31 runtime sonames a compat process gets from /LIB64.
GLIBC_SONAMES="libc.so.6 libdl.so.2 libpthread.so.0 libm.so.6 librt.so.1 ld-linux-x86-64.so.2 libgcc_s.so.1"

say() { printf 'cp-x11-build: %s\n' "$*"; }
die() { say "ERROR: $*"; exit "${2:-1}"; }

for t in clang make python3 patchelf objdump readelf nm strip tar dpkg-deb; do
  command -v "$t" >/dev/null || die "missing host tool: $t (apt install ${t})" 2
done

# Build order = dependency order.  name|tarball|extra configure args
PKGS="xorgproto|xorgproto-2026.1.tar.xz|--disable-legacy
xcb-proto|xcb-proto-1.17.0.tar.xz|
libpthread-stubs|libpthread-stubs-0.5.tar.xz|
libXau|libXau-1.0.12.tar.xz|
libXdmcp|libXdmcp-1.1.5.tar.xz|
libxcb|libxcb-1.17.0.tar.xz|--enable-composite --enable-damage --enable-dri2 --enable-dri3 --enable-present --enable-randr --enable-render --enable-shape --enable-shm --enable-sync --enable-xfixes --enable-xinerama --enable-xinput --enable-xkb --enable-xtest --enable-dpms --enable-glx --enable-resource --enable-screensaver --enable-xv --disable-selinux --without-doxygen --disable-devel-docs
xtrans|xtrans-1.6.0.tar.xz|
libX11|libX11-1.8.14.tar.xz|--enable-xthreads --enable-xkb --enable-xlocale --disable-specs --without-xmlto --without-fop --without-xsltproc --disable-lint-library
libXext|libXext-1.3.7.tar.xz|--disable-specs --without-xmlto --without-fop --without-xsltproc
libXrender|libXrender-0.9.13.tar.xz|
libXfixes|libXfixes-6.0.2.tar.xz|
libXcomposite|libXcomposite-0.4.7.tar.xz|
libXi|libXi-1.8.4.tar.xz|--disable-docs --disable-specs --without-xmlto --without-fop --without-xsltproc --without-asciidoc
libXrandr|libXrandr-1.5.5.tar.xz|
libXcursor|libXcursor-1.2.3.tar.xz|
libXinerama|libXinerama-1.1.6.tar.xz|
libxshmfence|libxshmfence-1.3.3.tar.xz|
freetype|freetype-2.14.3.tar.xz|--with-zlib=no --with-bzip2=no --with-png=no --with-harfbuzz=no --with-brotli=no --with-librsvg=no --enable-freetype-config"

# Packages a build may go without when their tarball is absent (reported as
# SKIPPED, never silently): libXdmcp (libxcb then has no XDM auth),
# libxshmfence (only Mesa's DRI3 path uses it; Wine does not) and libXi
# (without it Wine configures with no XInput2: no raw mouse, no ClipCursor --
# tools/cp/build-wine.sh's X variant refuses that unless told otherwise).
OPTIONAL_PKGS=" libXdmcp libxshmfence libXi "
# FreeType (lane C4-x2, for Wine's --with-freetype): built from the tarball
# vendored in third_party/ with its own bundled zlib/gzip code and no
# png/bzip2/brotli/harfbuzz, so libfreetype.so.6 needs only libc.  It is
# staged in /CP/X11/lib with the X libraries: that directory is first on a
# Wine session's LD_LIBRARY_PATH (docs/cp-x11.md §2.3) and win32u dlopens
# libfreetype.so.6 by soname.

# --- sysroot ------------------------------------------------------------------
pick_sysroot() {
  if [ -n "${CP_SYSROOT:-}" ]; then SYS="$CP_SYSROOT"
  elif [ -f "$HOME/cp/wine-build/sysroot-glibc231/.ok" ]; then SYS="$HOME/cp/wine-build/sysroot-glibc231"
  else
    SYS="$WORK/sysroot"
    if [ ! -f "$SYS/.ok" ]; then
      bash "$ROOT/tools/cp/fetch-runtime.sh" >/dev/null || die "fetch-runtime.sh failed" 2
      rm -rf "$SYS"; mkdir -p "$SYS"
      local d l t
      for d in libc6_2.31-0ubuntu9.18_amd64.deb libc6-dev_2.31-0ubuntu9.18_amd64.deb \
               linux-libc-dev_5.4.0-218.238_amd64.deb libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb \
               libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb; do
        dpkg-deb -x "$RT/dl/$d" "$SYS"
      done
      while IFS= read -r l; do t=$(readlink "$l"); ln -sfn "$SYS$t" "$l"; done \
        < <(find "$SYS" -type l -lname '/*')
      touch "$SYS/.ok"
    fi
  fi
  [ -f "$SYS/usr/include/stdio.h" ] && [ -f "$SYS/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o" ] ||
    die "sysroot $SYS is not a glibc 2.31 + gcc-10 sysroot" 2
  grep -q '__GLIBC_MINOR__	31' "$SYS/usr/include/features.h" || die "sysroot $SYS is not glibc 2.31" 2
}

# --- tarball lookup -----------------------------------------------------------------
# $DL (fetch.sh's verified cache) first.  Then the tarballs the repo already
# holds, each checked against its committed sha256: the native X stack's
# third_party/x11/dl (13 of fetch.sh's 16 pins are byte-identical to its
# pins, docs/cp-x11.md §1.1, plus libXcomposite) and third_party/<tarball>
# (FreeType, vendored).  Prints the path; returns 1 when there is none.
tb_path() {
  local tb="$1" c sum
  [ -f "$DL/$tb" ] && { printf '%s\n' "$DL/$tb"; return 0; }
  sum="$ROOT/third_party/x11/sums/$tb.sha256"
  for c in "$ROOT/third_party/x11/dl/$tb" "$ROOT/third_party/$tb"; do
    [ -f "$c" ] && [ -f "$sum" ] || continue
    [ "$(sha256sum "$c" | cut -d' ' -f1)" = "$(cut -d' ' -f1 "$sum")" ] ||
      die "$c does not match $sum -- not using it" 1
    printf '%s\n' "$c"; return 0
  done
  return 1
}

# Which optional packages are present changes what later packages link
# (libxcb + libXdmcp), so it is part of every cache key.
built_set() {
  local p out=""
  for p in xdmcp xshmfence; do [ -f "$DEV/lib/pkgconfig/$p.pc" ] && out="$out$p,"; done
  printf '%s' "$out"
}

# --- one autotools package ----------------------------------------------------------
pc_refresh() {
  # The dev tree's .pc files, with /CP/X11 -> the destdir, so later packages
  # (and C5-vk's Mesa) can build against the dev tree with plain pkg-config.
  mkdir -p "$PCDIR"
  local f
  for f in "$DEV"/lib/pkgconfig/*.pc "$DEV"/share/pkgconfig/*.pc; do
    [ -f "$f" ] || continue
    sed "s|$PREFIX|$DEV|g" "$f" > "$PCDIR/$(basename "$f")"
  done
  # libtool archives would hard-code /CP/X11/lib into later links.
  rm -f "$DEV"/lib/*.la
}

build_pkg() {
  local name="$1" tb="$2" extra="$3"
  local src="$WORK/src" obj="$WORK/obj/$name" log="$LOGS/$name.log" stamp="$WORK/.stamps/$name"
  local tbp; tbp=$(tb_path "$tb") || {
    case "$OPTIONAL_PKGS" in *" $name "*) say "$name: SKIPPED (optional; $tb not in $DL or the native stack)"; return 0 ;; esac
    die "$tb missing: run tools/cp/x11/fetch.sh" 2; }
  local key; key=$(printf '%s|%s|%s|%s|%s|r1' "$(sha256sum "$tbp" | cut -c1-16)" "$extra" "$SYS" "$CFLAGS_X" "$(built_set)" | sha256sum | cut -c1-16)
  if [ "${CP_X11_FORCE:-0}" != 1 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key" ]; then
    say "$name: cached"; return 0
  fi
  local dir; dir=$(tar -tf "$tbp" | awk -F/ 'NR==1{print $1}')
  rm -rf "${src:?}/$dir" "$obj"; mkdir -p "$src" "$obj" "$LOGS" "$WORK/.stamps"
  tar -C "$src" -xf "$tbp"
  say "$name: configure + make -j$JOBS (log $log)"
  local t0=$SECONDS
  # shellcheck disable=SC2086  # $extra is a word list
  ( cd "$obj" &&
    PKG_CONFIG_LIBDIR="$PCDIR" PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR= \
    CC="clang --sysroot=$SYS" CPP="clang --sysroot=$SYS -E" \
    CFLAGS="$CFLAGS_X" LDFLAGS="-Wl,-rpath-link,$DEV/lib" \
    "$src/$dir/configure" --prefix="$PREFIX" --host=x86_64-linux-gnu --build=x86_64-linux-gnu \
      --disable-static --enable-shared --enable-silent-rules $extra &&
    make -j"$JOBS" && make install DESTDIR="$DEST" ) > "$log" 2>&1 || {
      tail -25 "$log"; die "$name failed (see $log)"; }
  pc_refresh
  printf '%s\n' "$key" > "$stamp"
  say "$name: ok ($((SECONDS - t0)) s)"
}

do_build() {
  pick_sysroot
  say "sysroot: $SYS ($(awk '$2=="__GLIBC_MINOR__"{print "glibc 2."$3}' "$SYS/usr/include/features.h"))"
  say "compiler: clang --sysroot=$SYS ($(clang --version | awk 'NR==1'))"
  CFLAGS_X="-O2 -g -fno-plt"
  # Never let configure see the host's own X11 .pc files.
  mkdir -p "$PCDIR"
  local name tb extra
  while IFS='|' read -r name tb extra; do
    [ -n "$name" ] || continue
    build_pkg "$name" "$tb" "$extra"
  done <<EOF
$PKGS
EOF
  # What the shared libraries' configure runs actually enabled (raw rows).
  say "libxcb extensions built: $(cd "$DEV/lib" && ls libxcb-*.so.? 2>/dev/null | sed 's/\.so\..*//; s/^libxcb-//' | tr '\n' ' ')"
}

# --- stage the runtime tree ----------------------------------------------------------
do_stage() {
  [ -d "$DEV/lib" ] || die "nothing built yet (run build)"
  rm -rf "$STAGE"; mkdir -p "$STAGE/lib" "$STAGE/share/X11"
  local f so n=0
  for f in "$DEV"/lib/*.so.*; do
    [ -L "$f" ] && continue                      # real file only
    so=$(readelf -dW "$f" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
    [ -n "$so" ] || die "$f has no SONAME"
    cp "$f" "$STAGE/lib/$so"
    strip --strip-debug "$STAGE/lib/$so"
    patchelf --force-rpath --set-rpath '$ORIGIN' "$STAGE/lib/$so"
    n=$((n+1))
  done
  cp -r "$DEV/share/X11/locale" "$STAGE/share/X11/"
  cp "$DEV/share/X11/XErrorDB" "$DEV/share/X11/Xcms.txt" "$STAGE/share/X11/" 2>/dev/null || true
  ( cd "$STAGE" && find . -type f | LC_ALL=C sort | xargs sha256sum ) > "$WORK/stage-manifest.sha256"
  say "stage: $n shared objects -> $STAGE/lib ($(du -sh "$STAGE/lib" | cut -f1)); tree $(du -sh "$STAGE" | cut -f1), $(find "$STAGE" -type f | wc -l) files"
  say "stage: manifest $WORK/stage-manifest.sha256"
}

# --- audit: GLIBC_ versions, DT_NEEDED closure, undefined-symbol resolution ----------
do_audit() {
  # sort/comm must agree on one collation: under en_US.UTF-8 (strix's
  # default) sort ignores '_' and comm rejects the input, which turned every
  # symbol into "unresolved".
  export LC_ALL=C
  pick_sysroot
  [ -d "$STAGE/lib" ] || die "nothing staged (run stage)"
  local report="$WORK/audit.txt" bad=0 max=0 f b v needed rp n
  local glibc_dir="$SYS/lib/x86_64-linux-gnu"
  : > "$report"
  # Defined dynamic symbols of the whole closure (staged libs + glibc 2.31).
  local defs="$WORK/audit-defs.txt"
  { for f in "$STAGE"/lib/*; do nm -D --defined-only "$f" | awk '{print $NF}'; done
    for b in libc.so.6 libdl.so.2 libpthread.so.0 libm.so.6 librt.so.1 ld-linux-x86-64.so.2; do
      nm -D --defined-only "$glibc_dir/$b" | awk '{print $NF}'; done
    nm -D --defined-only "$SYS/lib/x86_64-linux-gnu/libgcc_s.so.1" | awk '{print $NF}'
  } | sed 's/@.*//' | sort -u > "$defs"
  for f in "$STAGE"/lib/*; do
    b=$(basename "$f")
    v=$(objdump -T "$f" | awk '/\*UND\*/' | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
    needed=$(readelf -dW "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    rp=$(readelf -dW "$f" | sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\1=\2/p')
    printf '%-26s GLIBC_max=%-5s %-14s needed=%s\n' "$b" "${v:-none}" "$rp" "$needed" >> "$report"
    if [ -n "$v" ]; then
      [ "$(printf '%s\n%s\n' "$v" "$GLIBC_MAX_ALLOWED" | sort -V | tail -1)" = "$GLIBC_MAX_ALLOWED" ] ||
        { say "AUDIT FAIL: $b needs GLIBC_$v > $GLIBC_MAX_ALLOWED"; bad=1; }
      max=$(printf '%s\n%s\n' "$max" "$v" | sort -V | tail -1)
    fi
    for n in $needed; do
      if [ -f "$STAGE/lib/$n" ]; then continue; fi
      case " $GLIBC_SONAMES " in *" $n "*) continue ;; esac
      say "AUDIT FAIL: $b DT_NEEDED $n is neither staged nor glibc 2.31"; bad=1
    done
    # Every non-weak undefined symbol must be defined somewhere in the closure.
    local und; und=$(nm -D --undefined-only "$f" | awk '$1=="U"{print $2}' | sed 's/@.*//' | sort -u | comm -23 - "$defs" || true)
    if [ -n "$und" ]; then say "AUDIT FAIL: $b unresolved: $(echo $und)"; bad=1; fi
  done
  cat "$report"
  say "audit: $(ls "$STAGE/lib" | wc -l) shared objects; max GLIBC_ required = GLIBC_$max (allowed <= $GLIBC_MAX_ALLOWED)"
  say "audit: DT_NEEDED union: $(sed 's/.*needed=//' "$report" | tr ' ' '\n' | grep . | sort -u | tr '\n' ' ')"
  say "audit: every NEEDED resolves inside {stage} + {glibc 2.31}; every strong undefined symbol is defined in that closure: $([ $bad = 0 ] && echo yes || echo NO)"
  [ "$bad" = 0 ] || die "audit failed"
}

# --- the in-OS smoke client (tools/cp/x11/xsmoke.c) -----------------------------------
# An ordinary glibc PIE: built against the dev tree, DT_RPATH /CP/X11/lib (old
# style on purpose: DT_RPATH is searched BEFORE LD_LIBRARY_PATH=/LIB64, and it
# applies to the whole dependency tree, so the RimWorld shim can never win).
do_smoke() {
  pick_sysroot
  local out="$WORK/xsmoke/xsmoke.elf" libs
  if [ ! -f "$DEV/lib/pkgconfig/xshmfence.pc" ]; then
    say "smoke: SKIPPED (xsmoke links libxshmfence, which this build does not have)"; return 0
  fi
  mkdir -p "$WORK/xsmoke"
  libs="-lX11 -lX11-xcb -lxcb -lxcb-present -lxcb-dri3 -lxcb-composite -lxcb-damage -lxcb-shm -lxcb-sync -lXext -lXi -lXrandr -lXrender -lXfixes -lXcursor -lXinerama -lxshmfence -ldl"
  # shellcheck disable=SC2086
  clang --sysroot="$SYS" -O2 -g -std=gnu11 -Wall -Wno-unused-parameter -fPIE -pie \
    -I"$DEV/include" -o "$out" "$ROOT/tools/cp/x11/xsmoke.c" -L"$DEV/lib" $libs \
    -Wl,-rpath-link,"$DEV/lib" -Wl,--disable-new-dtags -Wl,-rpath,$PREFIX/lib \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 || die "xsmoke build failed"
  local v
  v=$(objdump -T "$out" | awk '/\*UND\*/' | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)
  say "smoke: $out ($(stat -c %s "$out") B) GLIBC_max=$v"
  say "smoke: $(readelf -dW "$out" | sed -n 's/.*(RPATH).*\[\(.*\)\]/DT_RPATH=\1/p')  interp=$(readelf -lW "$out" | sed -n 's/.*interpreter: \(.*\)]/\1/p')"
  say "smoke: DT_NEEDED: $(readelf -dW "$out" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
}

case "$STEP" in
  build) do_build ;;
  stage) do_stage ;;
  audit) do_audit ;;
  smoke) do_smoke ;;
  all)   do_build; do_stage; do_audit; do_smoke ;;
  *) die "unknown step $STEP" 2 ;;
esac
