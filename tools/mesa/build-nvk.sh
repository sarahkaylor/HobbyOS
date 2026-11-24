#!/usr/bin/env bash
# tools/mesa/build-nvk.sh -- Mesa's NVK Vulkan ICD for HobbyOS's Linux-ABI
# compat class.  THIS IS THE SUPPORTED PATH: tools/nv/nvk/build.sh is a thin
# driver of it, and tools/mesa/patches/0014-nvk-hobbyos-node-and-named-refusals.patch
# is the only change to Mesa's source.
#
# WHAT IT PRODUCES
#   $WORK/build/src/nouveau/vulkan/libvulkan_nouveau.so
#     SONAME   libvulkan_nouveau.so
#     NEEDED   libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 ld-linux-x86-64.so.2
#   libdrm, libelf, zlib and libc++ are linked in *statically*, so the compat
#   class stages no library beyond the glibc 2.31 set it already has.  `audit`
#   re-checks that closure, the SONAME and the two ICD entry points.
#
# WHY A SEPARATE ARTIFACT FROM build-hobbyos.sh (in this same directory)
#   build-hobbyos.sh builds the GL 3.3 DSO for x86_64-none-elf by replaying the
#   host's compile lines, and it deliberately configures no Vulkan driver
#   (`-Dvulkan-drivers=`): a Vulkan ICD is a different artifact with a
#   different dependency closure -- a glibc DSO that a glibc client dlopen()s.
#   That is why the tree's Mesa configuration has two entry points:
#     build-hobbyos.sh  GL   26.2.6  none-elf headers, 0 DT_NEEDED
#     build-nvk.sh      NVK  25.0.7  glibc 2.31 sysroot, glibc-only closure
#   The target here IS x86_64-linux-gnu, so meson's configure probes, Rust
#   bindgen and the code generators all run on the host; only the headers and
#   libraries are the 2.31 pin.  Same technique as tools/cp/vk/build.sh (which
#   builds lavapipe for the same class), and the glibc pin set below is that
#   script's pin set: docs/cp-pins.md is the table both must match.
#
# WHY MESA 25.0.7 AND NOT THE TREE'S 26.2.6 PIN (GL) / tools/cp/vk (lavapipe)
#   MEASURED: `meson setup -Dllvm=disabled -Dvulkan-drivers=nouveau` on 26.2.6
#   stops at
#     src/mesa-26.2.6/meson.build:977:3:
#       ERROR: Feature llvm cannot be disabled: CLC requires LLVM
#   -- that series compiles OpenCL kernels (CLC) for nouveau, so the ICD would
#   carry a static LLVM (the lavapipe build pays that cost; this one exists to
#   stay small).  25.0.7 is the last series where `-Dllvm=disabled` is allowed
#   with vulkan-drivers=nouveau.  Both series need the Rust NAK for nouveau
#   (rustc + bindgen), so Rust is NOT what separates them.
#
# USAGE
#   tools/mesa/build-nvk.sh [sysroot|zlib|libelf|libcxx|libdrm|mesa|probe|audit|all]
#
#   Env: NVK_WORK   (default <repo>/obj/mesa-nvk)  work dir
#        NVK_DL     (default $HOME/.cache/hobbyos-mesa/dl)  pin cache
#        NVK_LIBCXX (default $NVK_WORK/libcxx)  libc++ install (include/c++/v1,
#                   lib/libc++.a); the libcxx step builds it if absent
#        NVK_LIBCXX_TARBALL / LIBCXX_SRC   where the pinned LLVM container /
#                   extracted tree live (default: third_party/libcxx-21.1.8)
#        NVK_MESA_TARBALL   pre-fetched mesa-25.0.7.tar.xz
#        NVK_VKH    Vulkan headers for the probe (default: Mesa's own
#                   vendored include/, so no separate download)
#        NVK_FORCE_MESA=1   rebuild the ICD even if it is present
#
#   Prereqs (host): clang lld llvm-ar meson ninja cmake patch dpkg-deb readelf
#   curl sha256sum rustc bindgen cbindgen (the Rust NAK; bindgen/cbindgen ship
#   in ~/.cargo/bin, which this script puts on PATH), python3-mako python3-yaml,
#   and /usr/lib/x86_64-linux-gnu/libelf.a + headers (apt: libelf-dev).
#
# LICENSE: Mesa is MIT; this script is part of the HobbyOS tree (GPL-2.0).
#   Mesa, libdrm, zlib and LLVM's runtimes are fetched, never committed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="${NVK_WORK:-$ROOT/obj/mesa-nvk}"
DL="${NVK_DL:-$HOME/.cache/hobbyos-mesa/dl}"
SYS="$W/sysroot-glibc231"
LCXX="${NVK_LIBCXX:-$W/libcxx}"
STEP="${1:-all}"
# bindgen/cbindgen are Rust cargo installs; rustc must be reachable for the
# NAK's proc-macro crate.  Put them on PATH before the tool check below.
PATH="${CARGO_BIN:-$HOME/.cargo/bin}:$PATH"
export PATH
# Mesa's configure runs its own generators through `python3` from PATH and
# requires mako + yaml.  A virtualenv earlier on PATH (a dev/Hermes shell) can
# shadow /usr/bin/python3 and meson then stops with "Python (3.x) mako module
# >= 0.8.0 required to build mesa" -- measured here, and not something a clean
# checkout should have to guess at.  Pick the first python3 that has them.
if ! python3 -c 'import mako, yaml' >/dev/null 2>&1; then
  for d in /usr/bin /usr/local/bin; do
    if [ -x "$d/python3" ] && "$d/python3" -c 'import mako, yaml' >/dev/null 2>&1; then
      printf 'nvk-build: python3: %s has mako+yaml; prepending %s to PATH\n' "$d/python3" "$d"
      PATH="$d:$PATH"; export PATH; break
    fi
  done
fi

MESA_VER=25.0.7
# sha256 of the release container as fetched 2026-11-24 (archive.mesa3d.org
# publishes a detached .sig, not a checksum file; this pin is what makes a
# swapped tarball fail loudly instead of silently changing the ICD).
MESA_SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
MESA_URL=https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz
LIBDRM_VER=2.4.131
LIBDRM_URL=https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VER.tar.xz
LLVM_VER=21.1.8
SRC="$W/src/mesa-$MESA_VER"
B="$W/build"
PATCH="$HERE/patches/0014-nvk-hobbyos-node-and-named-refusals.patch"

say()  { printf 'nvk-build: %s\n' "$*"; }
note() { printf 'nvk-build:   %s\n' "$*"; }
die()  { say "ERROR: $*"; exit "${2:-1}"; }
need() { for t in "$@"; do command -v "$t" >/dev/null || die "missing host tool: $t"; done; }
sha_ok() { echo "$2  $1" | sha256sum -c --quiet - >/dev/null 2>&1; }

# The configured vulkan-drivers of a build dir, read from meson's own option
# table rather than from our command line -- the point is that an empty or
# missing driver list must fail loudly.  No early `exit` inside the awk: meson
# is Python, closing its stdout early makes its final flush fail (exit 120),
# and `pipefail` then turns an innocent probe into a script exit (measured).
meson_vulkan_drivers() {
  meson configure "$1" 2>/dev/null | awk '
     $1=="vulkan-drivers" && !seen { v=$2; gsub(/[][ ]/,"",v); seen=1 }
     END { if (seen) print v }' || true
}

mkdir -p "$W/src" "$W/log" "$DL"

# --- 1. glibc 2.31 sysroot ---------------------------------------------------
# The compat class runs glibc 2.31 (docs/rimworld-plan.md §2, docs/cp-pins.md).
# Compiling against the host's headers names symbols 2.31 does not have
# (__isoc23_*), which is a load-time failure in the guest, so every header and
# every link input comes from this pin.  Identical table to tools/cp/vk/build.sh
# (lavapipe) -- the compat class has one libc, so the pins must agree.
do_sysroot() {
  [ -f "$SYS/.ok" ] && { say "sysroot: $SYS (cached)"; return; }
  local pins="
libc6_2.31-0ubuntu9.18_amd64.deb|1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb
libc6-dev_2.31-0ubuntu9.18_amd64.deb|11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb
linux-libc-dev_5.4.0-218.238_amd64.deb|0aa210458df2ab7bc4536e8edf0f9423a7018e74e2eac0b5777fd2a36f170c2f|http://archive.ubuntu.com/ubuntu/pool/main/l/linux/linux-libc-dev_5.4.0-218.238_amd64.deb
libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb|4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb
libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb|ebe5e967fca32ea5811946180f7822ee749857861eadaa29514e65857ce3b897|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb"
  rm -rf "$SYS"; mkdir -p "$SYS"
  local n sha url
  while IFS='|' read -r n sha url; do
    [ -n "$n" ] || continue
    if [ ! -f "$DL/$n" ]; then
      curl -fsSL --retry 3 -o "$DL/$n.part" "$url" || die "cannot fetch $url"
      mv -f "$DL/$n.part" "$DL/$n"
    fi
    sha_ok "$DL/$n" "$sha" || die "sha256 mismatch: $n (pin drifted?)"
    dpkg-deb -x "$DL/$n" "$SYS"
  done <<< "$pins"
  local l t cnt=0
  while IFS= read -r l; do t=$(readlink "$l"); ln -sfn "$SYS$t" "$l"; cnt=$((cnt+1)); done \
    < <(find "$SYS" -type l -lname '/*')
  say "sysroot: $SYS ($cnt absolute symlinks re-pointed into it)"
  touch "$SYS/.ok"
}

# --- 2. zlib, static and PIC --------------------------------------------------
# Without this the ICD carries DT_NEEDED libz.so.1 (Mesa's disk cache
# compresses) and the compat class stages no libz.  A static PIC libz.a inside
# the sysroot removes the dependency instead of adding a guest library.
do_zlib() {
  [ -f "$SYS/usr/lib/libz.a" ] && { say "zlib: static libz.a in sysroot (cached)"; return; }
  local tb="$ROOT/third_party/zlib-1.3.1/zlib-1.3.1.tar.gz"
  [ -f "$tb" ] || die "third_party/zlib-1.3.1/zlib-1.3.1.tar.gz missing"
  [ -d "$W/src/zlib-1.3.1" ] || tar -xf "$tb" -C "$W/src"
  ( cd "$W/src/zlib-1.3.1" &&
    CC="clang --sysroot=$SYS" CFLAGS="-O2 -fPIC" ./configure --static >/dev/null &&
    make -j"$(nproc)" libz.a >/dev/null ) || die "zlib build failed"
  cp "$W/src/zlib-1.3.1/libz.a" "$SYS/usr/lib/libz.a"
  cp "$W/src/zlib-1.3.1/zlib.h" "$W/src/zlib-1.3.1/zconf.h" "$SYS/usr/include/"
  say "zlib: static PIC libz.a installed into the sysroot"
}

# --- 3. libelf (NVK requires it) ----------------------------------------------
# Mesa's nir/cubin loader uses libelf and nvk refuses to configure without it
# ("NVK requires libelf").  This one input comes from the host (apt: libelf-dev)
# because the 2.31 set has no libelf; the `audit` step proves that linking it
# pulls no symbol newer than the pin.
do_libelf() {
  [ -f "$SYS/usr/lib/libelf.a" ] && { say "libelf: present in sysroot (cached)"; return; }
  [ -f /usr/lib/x86_64-linux-gnu/libelf.a ] || die "no /usr/lib/x86_64-linux-gnu/libelf.a (apt: libelf-dev)"
  cp /usr/include/libelf.h /usr/include/gelf.h "$SYS/usr/include/"
  cp /usr/lib/x86_64-linux-gnu/libelf.a "$SYS/usr/lib/"
  mkdir -p "$SYS/usr/lib/pkgconfig"
  printf 'prefix=%s/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: libelf\nDescription: ELF access library\nVersion: 0.194\nLibs: -L${libdir} -lelf\nCflags: -I${includedir}\n' "$SYS" \
    > "$SYS/usr/lib/pkgconfig/libelf.pc"
  say "libelf: static libelf.a + headers installed into the sysroot"
}

# --- 4. libc++, static and hermetic (Mesa has C++ TUs) -------------------------
# util/u_qsort.cpp and the ASTC texture code are C++.  The 2.31 set has no C++
# runtime and the compat class stages none, so libc++/libc++abi/libunwind are
# built from the tree's pinned LLVM container as PIC static archives with
# hidden visibility, and linked INTO the ICD: no libstdc++/libc++ DT_NEEDED and
# no C++ symbols exported into the client process.
# Recipe and flags are tools/cp/vk/build.sh's do_runtimes (lavapipe needs the
# same thing); the container sha256 is third_party/libcxx-21.1.8's pin.
do_libcxx() {
  [ -f "$LCXX/lib/libc++.a" ] && { say "libcxx: $LCXX (cached)"; return; }
  local tb="${NVK_LIBCXX_TARBALL:-}"
  if [ -z "$tb" ] || [ ! -f "$tb" ]; then
    for c in "$ROOT/third_party/llvm-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz" \
             "$ROOT/third_party/libcxx-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz" \
             "$W/dl/llvm-project-$LLVM_VER.src.tar.xz"; do
      [ -f "$c" ] && { tb="$c"; break; }
    done
  fi
  if [ -z "$tb" ] || [ ! -f "$tb" ]; then
    # The container is 150 MB and is fetched, never committed; the tree's own
    # fetcher downloads it with the same sha256 pin (third_party/libcxx-…/
    # container.sha256).  build-hobbyos.sh takes the same route for its C++ half.
    local fetch="$ROOT/third_party/libcxx-$LLVM_VER/fetch.sh"
    [ -f "$fetch" ] || die "LLVM container not found and $fetch is missing"
    say "libcxx: container missing; running third_party/libcxx-$LLVM_VER/fetch.sh"
    bash "$fetch" > "$W/log/fetch-libcxx.log" 2>&1 \
      || { tail -20 "$W/log/fetch-libcxx.log"; die "third_party/libcxx-$LLVM_VER/fetch.sh failed"; }
    tb="$ROOT/third_party/libcxx-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"
  fi
  [ -f "$tb" ] || die "LLVM container still missing after fetch ($tb)"
  local want got
  want="$(cut -d' ' -f1 "$ROOT/third_party/libcxx-$LLVM_VER/container.sha256")"
  got="$(sha256sum "$tb" | cut -d' ' -f1)"
  [ "$got" = "$want" ] || die "LLVM container sha256 mismatch (want $want, got $got)"
  local src="${LIBCXX_SRC:-$W/src/llvm-project-$LLVM_VER.src}"
  if [ ! -f "$src/runtimes/CMakeLists.txt" ]; then
    rm -rf "$src"; mkdir -p "$src"
    tar -xf "$tb" -C "$src" --strip-components=1
  fi
  rm -rf "$W/build-libcxx" "$LCXX"
  say "libcxx: configure (static, hermetic, --sysroot=$SYS)"
  cmake -G Ninja -S "$src/runtimes" -B "$W/build-libcxx" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LCXX" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
    -DCMAKE_C_FLAGS="--sysroot=$SYS -O2 -g1 -fPIC" \
    -DCMAKE_CXX_FLAGS="--sysroot=$SYS -O2 -g1 -fPIC" \
    -DCMAKE_ASM_FLAGS="--sysroot=$SYS" \
    -DCMAKE_EXE_LINKER_FLAGS="--sysroot=$SYS -fuse-ld=lld -nostdlib++" \
    -DCMAKE_SHARED_LINKER_FLAGS="--sysroot=$SYS -fuse-ld=lld -nostdlib++" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
    -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON -DLIBCXXABI_ENABLE_STATIC=ON -DLIBUNWIND_ENABLE_STATIC=ON \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
    -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
    -DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=ON \
    -DLIBCXX_HERMETIC_STATIC_LIBRARY=ON -DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON \
    -DLIBUNWIND_HERMETIC_STATIC_LIBRARY=ON \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_INCLUDE_TESTS=OFF -DLIBCXX_ENABLE_LOCALIZATION=ON \
    > "$W/log/libcxx.log" 2>&1 || { tail -20 "$W/log/libcxx.log"; die "libcxx configure failed"; }
  ninja -C "$W/build-libcxx" -j"$(nproc)" install-cxx install-cxxabi install-unwind \
    >> "$W/log/libcxx.log" 2>&1 || { tail -30 "$W/log/libcxx.log"; die "libcxx build failed"; }
  [ -f "$LCXX/lib/libc++.a" ] && [ -f "$LCXX/include/c++/v1/vector" ] || die "libc++ install incomplete"
  say "libcxx: $(stat -c%s "$LCXX/lib/libc++.a") B libc++.a (+ abi + unwinder merged in)"
}

# --- 5. libdrm (nouveau), static ----------------------------------------------
do_libdrm() {
  [ -f "$W/libdrm/lib/libdrm_nouveau.a" ] && { say "libdrm: $W/libdrm (cached)"; return; }
  [ -f "$DL/libdrm-$LIBDRM_VER.tar.xz" ] || \
    curl -fsSL --retry 3 -o "$DL/libdrm-$LIBDRM_VER.tar.xz" "$LIBDRM_URL" \
    || die "cannot fetch libdrm $LIBDRM_VER"
  [ -d "$W/src/libdrm-$LIBDRM_VER" ] || tar -xf "$DL/libdrm-$LIBDRM_VER.tar.xz" -C "$W/src"
  cat > "$W/libdrm-cross.ini" <<EOF
[binaries]
c = ['clang', '--sysroot=$SYS']
ar = 'llvm-ar'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-O2', '-g1', '-fPIC', '-w']
c_link_args = ['-fuse-ld=lld', '--sysroot=$SYS']
default_library = 'static'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
  rm -rf "$W/build-libdrm"
  meson setup "$W/build-libdrm" "$W/src/libdrm-$LIBDRM_VER" \
    --cross-file "$W/libdrm-cross.ini" --prefix="$W/libdrm" \
    -Dnouveau=enabled -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
    -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
    -Dtests=false -Dman-pages=disabled -Dvalgrind=disabled \
    > "$W/log/libdrm-setup.log" 2>&1 || { tail -20 "$W/log/libdrm-setup.log"; die "libdrm configure failed"; }
  ninja -C "$W/build-libdrm" install > "$W/log/libdrm-build.log" 2>&1 \
    || { tail -20 "$W/log/libdrm-build.log"; die "libdrm build failed"; }
  say "libdrm: $(ls "$W/libdrm/lib"/libdrm*.a | tr '\n' ' ')"
}

# --- 6. the ICD ---------------------------------------------------------------
do_mesa() {
  local icd="$B/src/nouveau/vulkan/libvulkan_nouveau.so"
  if [ -f "$icd" ] && [ "${NVK_FORCE_MESA:-0}" != 1 ]; then
    say "mesa: $icd (cached; NVK_FORCE_MESA=1 to rebuild)"; return
  fi
  [ -f "$DL/mesa-$MESA_VER.tar.xz" ] || {
    local c="${NVK_MESA_TARBALL:-$HOME/.cache/hobbyos-mesa/mesa-$MESA_VER.tar.xz}"
    if [ -f "$c" ] && [ "$c" != "$DL/mesa-$MESA_VER.tar.xz" ]; then
      cp "$c" "$DL/mesa-$MESA_VER.tar.xz"
    else
      curl -fsSL --retry 3 -o "$DL/mesa-$MESA_VER.tar.xz" "$MESA_URL" \
        || die "cannot fetch mesa $MESA_VER"
    fi
  }
  sha_ok "$DL/mesa-$MESA_VER.tar.xz" "$MESA_SHA256" \
    || die "mesa tarball sha256 mismatch (want $MESA_SHA256)"
  [ -d "$SRC" ] || tar -xf "$DL/mesa-$MESA_VER.tar.xz" -C "$W/src"
  # The one HobbyOS patch (node discovery + named refusals).  It lives with the
  # other Mesa patches; `patch` is used rather than git so this works on the
  # released tarball, which has no .git.
  [ -f "$PATCH" ] || die "patch missing: $PATCH"
  if ! grep -q NVK_HOBBYOS_NODE "$SRC/src/nouveau/vulkan/nvk_instance.c"; then
    ( cd "$SRC" && patch -p1 --batch --forward < "$PATCH" ) \
      || die "patch did not apply: $PATCH"
    say "mesa: applied $(basename "$PATCH")"
  else
    say "mesa: $(basename "$PATCH") already applied"
  fi
  [ -f "$LCXX/lib/libc++.a" ] || die "no $LCXX/lib/libc++.a (run: $0 libcxx)"
  cat > "$W/mesa-cross.ini" <<EOF
[binaries]
c = ['clang', '--sysroot=$SYS']
cpp = ['clang++', '--sysroot=$SYS', '-stdlib=libc++', '-nostdinc++', '-isystem', '$LCXX/include/c++/v1']
rust = 'rustc'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
bindgen = '$(command -v bindgen || echo bindgen)'
cbindgen = '$(command -v cbindgen || echo cbindgen)'

[built-in options]
c_args = ['-O2', '-g1', '-fPIC', '-w', '-mtune=generic']
cpp_args = ['-O2', '-g1', '-fPIC', '-w', '-mtune=generic']
c_link_args = ['-fuse-ld=lld', '--sysroot=$SYS', '-Wl,--build-id=sha1']
cpp_link_args = ['-fuse-ld=lld', '--sysroot=$SYS', '-Wl,--build-id=sha1', '-stdlib=libc++', '-L$LCXX/lib', '-static-libgcc', '-Wl,--as-needed', '-lpthread']
buildtype = 'release'
b_ndebug = 'true'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
  export PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$W/libdrm/lib/pkgconfig"
  export PKG_CONFIG_PATH=
  export BINDGEN_EXTRA_CLANG_ARGS="--sysroot=$SYS -isystem $SYS/usr/include -isystem $SYS/usr/include/x86_64-linux-gnu"
  # Vulkan drivers only, and NO empty list: this configuration's whole point is
  # -Dvulkan-drivers=nouveau (build-hobbyos.sh is the one that configures none).
  # No gallium, no GL, no EGL, no WSI platforms (HobbyOS presents through its
  # own X stack), no LLVM, no zstd, no shader cache (its zlib would be a
  # DT_NEEDED), no libelf-less fallback.
  rm -rf "$B"
  meson setup "$B" "$SRC" \
    --cross-file "$W/mesa-cross.ini" --prefix="$W/install" \
    -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
    -Dgbm=disabled -Dglvnd=disabled -Dllvm=disabled -Dvideo-codecs= \
    -Dzstd=disabled -Dshader-cache=disabled -Dxmlconfig=disabled \
    -Dbuild-tests=false -Dtools= -Dhtml-docs=disabled \
    > "$W/log/mesa-setup.log" 2>&1 || { tail -20 "$W/log/mesa-setup.log"; die "mesa configure failed"; }
  # Assert the configuration actually names a Vulkan driver, so a regression to
  # an empty -Dvulkan-drivers= can never be silent again.
  local vd
  vd=$(meson_vulkan_drivers "$B")
  [ "$vd" = "nouveau" ] || die "configured vulkan-drivers='$vd', expected nouveau"
  say "mesa: configured vulkan-drivers=$vd (llvm=disabled, platforms=none)"
  ninja -C "$B" -j"$(nproc)" \
    src/nouveau/vulkan/libvulkan_nouveau.so src/nouveau/vulkan/nouveau_icd.x86_64.json \
    > "$W/log/mesa-build.log" 2>&1 || { tail -30 "$W/log/mesa-build.log"; die "mesa build failed"; }
  say "mesa: $(stat -c%s "$icd") B $icd"
}

# --- 7. the probe -------------------------------------------------------------
# The smallest loader-equivalent probe (calls the ICD ABI directly, no loader,
# no ICD JSON).  Vulkan headers come from Mesa's own vendored include/ so a
# clean checkout needs no separate Vulkan-Headers download.
do_probe() {
  local icd="$B/src/nouveau/vulkan/libvulkan_nouveau.so"
  [ -f "$icd" ] || die "build the ICD first"
  local vkh="${NVK_VKH:-$SRC/include}"
  [ -f "$vkh/vulkan/vulkan.h" ] || die "no Vulkan headers at $vkh (NVK_VKH=...)"
  clang --sysroot="$SYS" -O2 -fPIE -pie -Wall -I"$vkh" \
    -o "$W/nvk_probe" "$ROOT/src/user/nvk_probe.c" "$icd" \
    -Wl,-rpath,/CP/VK/lib -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    || die "probe link failed"
  say "probe: $W/nvk_probe"
}

# --- 8. audit -----------------------------------------------------------------
# Every claim this build makes about the DSO, checked here so it cannot rot.
do_audit() {
  local icd="$B/src/nouveau/vulkan/libvulkan_nouveau.so" bad=0
  [ -f "$icd" ] || die "no ICD to audit"
  local soname vd
  soname=$(readelf -dW "$icd" | awk '/SONAME/{gsub(/[][ ]/,"",$NF); print $NF}')
  say "SONAME: ${soname:-<none>}"
  [ "$soname" = libvulkan_nouveau.so ] || { say "  FAIL: SONAME is '${soname}'"; bad=1; }
  vd=$(meson_vulkan_drivers "$B")
  say "vulkan-drivers: ${vd:-<not configured>}"
  [ "$vd" = nouveau ] || { say "  FAIL: configured vulkan-drivers='$vd'"; bad=1; }
  local needed
  needed=$(readelf -dW "$icd" | awk '/NEEDED/{gsub(/[][ ]/,"",$NF); print $NF}' | sort | tr '\n' ' ')
  say "DT_NEEDED: $needed"
  for n in $needed; do
    case "$n" in
      libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|ld-linux-x86-64.so.2) ;;
      *) say "  FAIL: $n is outside the glibc 2.31 set the compat class stages"; bad=1 ;;
    esac
  done
  local ex; ex=$(nm -D --defined-only "$icd" | awk '{print $NF}')
  for s in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion; do
    echo "$ex" | grep -qx "$s" || { say "  FAIL: $s not exported"; bad=1; }
  done
  # Undefined symbols must be: the 2.31 runtime, ld.so's TLS helper, or weak.
  local syms sym vers unacct
  nm -D --undefined-only "$icd" | awk '{print $NF}' | sed 's/@.*//' | sort -u > "$W/undef.txt"
  nm -D --defined-only \
     "$SYS/lib/x86_64-linux-gnu/libc.so.6" \
     "$SYS/lib/x86_64-linux-gnu/libm.so.6" \
     "$SYS/lib/x86_64-linux-gnu/libdl.so.2" \
     "$SYS/lib/x86_64-linux-gnu/libpthread.so.0" \
     "$SYS/lib/x86_64-linux-gnu/librt.so.1" \
     "$SYS/lib/x86_64-linux-gnu/ld-2.31.so" 2>/dev/null \
     | awk '{print $NF}' | sed 's/@.*//' | sort -u > "$W/sysdef.txt"
  unacct=$(comm -23 "$W/undef.txt" "$W/sysdef.txt" | tr '\n' ' ')
  say "undefined not in the 2.31 runtime/ld.so: ${unacct:-none}"
  for sym in $unacct; do
    vers=$(readelf -sW --dyn-syms "$icd" | awk -v s="$sym" '$8==s && $5=="WEAK"{print "weak"}' | head -1)
    [ "$vers" = weak ] || { say "  FAIL: hard reference to $sym"; bad=1; }
  done
  [ "$bad" = 0 ] || die "audit FAILED"
  say "audit: PASS ($(stat -c%s "$icd") B, $(echo "$ex" | wc -l) exports)"
}

case "$STEP" in
  sysroot) do_sysroot ;;
  zlib)    do_zlib ;;
  libelf)  do_libelf ;;
  libcxx)  do_libcxx ;;
  libdrm)  do_libdrm ;;
  mesa)    do_mesa ;;
  probe)   do_probe ;;
  audit)   do_audit ;;
  all)
    need clang clang++ llvm-ar meson ninja cmake dpkg-deb readelf curl sha256sum patch
    need python3 rustc bindgen cbindgen
    do_sysroot; do_zlib; do_libelf; do_libcxx; do_libdrm; do_mesa; do_probe; do_audit ;;
  *) die "unknown step '$STEP' (sysroot|zlib|libelf|libcxx|libdrm|mesa|probe|audit|all)" 2 ;;
esac
