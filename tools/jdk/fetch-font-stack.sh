#!/usr/bin/env bash
# Fetch the font stack java.desktop needs, from the same Ubuntu focal ABI as the
# glibc pin.  Run once; it fills obj/jdk/stage/fclibs, which stage.sh copies to
# /lib/x86_64-linux-gnu in the guest.
#
# Why this exists: libfontmanager.so does NOT link fontconfig -- the JDK dlopens
# it -- and neither fontconfig nor its dependency chain was in the image.  Each
# missing library announced itself only as "Fontconfig head is null, check your
# fonts or fonts configuration" from lib/modules, which says nothing about which
# .so is missing.  fcprobe.c (tools/jdk/fcprobe.c) unwraps that one layer at a
# time; the chain turned out to be:
#
#   libfontconfig.so.1 (2.13.1) -> libexpat.so.1, libuuid.so.1, libfreetype.so.6
#   libfreetype.so.6 (2.10.1)   -> libz.so.1, libpng16.so.16
#
# freetype matters: the JDK ships its own libfreetype.so, but that build omits
# BDF support, so dlopen fails on "undefined symbol: FT_Get_BDF_Property".  The
# Ubuntu build has it.  All six land in /lib/x86_64-linux-gnu in the guest.
set -u
POOL=http://archive.ubuntu.com/ubuntu/pool/main
OUT="$(cd "$(dirname "$0")/../.." && pwd)/obj/jdk/stage/fclibs"
mkdir -p "$OUT"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

grab() { # pool-path, deb-name, member-glob, dest-name
  local p="$1" d="$2" m="$3" n="$4"
  ( cd "$work" && curl -sfO "$POOL/$p/$d" ) || { echo "FETCH FAILED $d"; return 1; }
  ( cd "$work" && ar x "$d" && tar xf data.tar.* ) || { echo "EXTRACT FAILED $d"; return 1; }
  local f; f=$(find "$work" -name "$m" | head -1)
  [ -n "$f" ] || { echo "MEMBER NOT FOUND $m in $d"; return 1; }
  cp "$f" "$OUT/$n" && echo "  $n  <- $d"
}

grab f/fontconfig      libfontconfig1_2.13.1-2ubuntu3_amd64.deb  'libfontconfig.so.1.12.0' libfontconfig.so.1
grab e/expat           libexpat1_2.2.9-1ubuntu0.8_amd64.deb      'libexpat.so.1.6.11'      libexpat.so.1
grab u/util-linux      libuuid1_2.34-0.1ubuntu9_amd64.deb        'libuuid.so.1.3.0'        libuuid.so.1
grab f/freetype        libfreetype6_2.10.1-2ubuntu0.4_amd64.deb   'libfreetype.so.6.17.1'   libfreetype.so.6
grab z/zlib            zlib1g_1.2.11.dfsg-2ubuntu9_amd64.deb     'libz.so.1.2.11'          libz.so.1
grab libp/libpng1.6    libpng16-16_1.6.37-2_amd64.deb            'libpng16.so.16.37.0'     libpng16.so.16

echo "staged into $OUT"
ls -l "$OUT"
