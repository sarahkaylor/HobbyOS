#!/usr/bin/env bash
# tools/cp/vk/build.sh — Vulkan on the CPU for HobbyOS's Linux-ABI compat class
# (docs/cyberpunk.md P5, lane C5-vk; docs/cp-vk.md).
#
#   Mesa 26.2.6 lavapipe (-Dvulkan-drivers=swrast) + the pinned Vulkan-Loader
#   (vulkan-sdk-1.4.363.0, docs/cp-pins.md) + the probes in src/user/cp/vk*,
#   all built with clang 21 against the PINNED glibc 2.31 sysroot (AD-2;
#   RimWorld AD-11 — never host headers, never host libraries).
#
#   Result (proposal for F4 — prefix /CP/VK, self-contained):
#     $WORK/stage/CP/VK/lib/libvulkan.so.1          the loader (winevulkan's soname)
#     $WORK/stage/CP/VK/lib/libvulkan_lvp.so        lavapipe, LLVM + libc++ linked in
#     $WORK/stage/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json
#     $WORK/stage/CP/VK/bin/{vkenum,vkcube}         the probes
#
# WHY A REAL (NATIVE) MESON BUILD, NOT tools/mesa/build-hobbyos.sh's REPLAY:
#   build-hobbyos.sh targets x86_64-none-elf with HobbyOS headers and so cannot
#   run configure probes.  Here the target IS x86_64-linux-gnu: a binary linked
#   against the glibc 2.31 sysroot runs on the (newer-glibc) build host, so
#   meson/cmake's cc.run() probes, LLVM's tablegen and llvm-config all work
#   unmodified.  The sysroot only has to stop HOST headers/libraries leaking in
#   (clang --sysroot keeps every -L inside it; gcc does not — build-wine.sh).
#
# WHY libc++ STATIC AND HERMETIC (not libstdc++):
#   LLVM and Mesa's gallivm are C++.  The 2.31 sysroot set has no libstdc++,
#   and the compat class stages none.  The tree already pins LLVM 21.1.8
#   (third_party/llvm-21.1.8, same tarball as third_party/libcxx-21.1.8), so
#   libc++/libc++abi/libunwind are built from it as PIC static archives with
#   hidden visibility (HERMETIC) and linked INTO libvulkan_lvp.so.  Result:
#   no libstdc++/libc++ DT_NEEDED, no C++ symbols exported to the host process
#   (Wine, the game) — the DSO's closure is glibc only.
#
# LLVM: X86 target only, static libraries, no tools except llvm-config (which
#   meson queries), RTTI/EH off (LLVM's defaults; Mesa then builds cpp_rtti=false).
#   linuxci shares 32 GB with QEMU boots: LLVM_PARALLEL_LINK_JOBS=1, JOBS=6.
#
# Usage:  bash tools/cp/vk/build.sh [sysroot|runtimes|llvm|loader|mesa|probes|stage|audit|all]
# Env:    CP_VK_WORK   (default $HOME/cp/vk-build)  out-of-tree work dir
#         CP_RUNTIME   (default <repo>/third_party/cp-runtime)
#         LLVM_TARBALL (default: search the tree's llvm/libcxx vendor dirs, then
#                       $WORK/dl; sha256-checked against third_party/llvm-21.1.8)
#         MESA_TARBALL (default: $WORK/dl, ~/.cache/hobbyos-mesa; sha256-checked)
#         JOBS         (default 16)
#         CP_VK_WSI    none (default: headless, docs/cp-vk.md §1-6) | x11: the
#                      loader with BUILD_WSI_XLIB/XCB_SUPPORT and Mesa
#                      -Dplatforms=x11 against C4-x's libraries
#                      (tools/cp/x11/build.sh; docs/cp-x11.md §5), staged in
#                      $WORK/stage-x11 + the X11 probe vkcubex.  LLVM, libc++
#                      and the headers are shared between the variants.
#         CP_X11_WORK  (x11 only; default $HOME/cp/x11-build) C4-x's work dir:
#                      destdir/CP/X11 (headers, libs) and pc/ (pkg-config)
#         GLSLANG      glslangValidator >= 12.2 for the mesa step (PATH lookup)
# Exit:   0 ok, 1 build/audit failure, 2 tooling error.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RT="${CP_RUNTIME:-$ROOT/third_party/cp-runtime}"
DL="$RT/dl"
WORK="${CP_VK_WORK:-$HOME/cp/vk-build}"
JOBS="${JOBS:-16}"
STEP="${1:-all}"

PREFIX=/CP/VK
SYS="$WORK/sysroot-glibc231"
LCXX="$WORK/libcxx"          # libc++ install (include/c++/v1, lib/libc++.a)
LLVMI="$WORK/llvm"           # LLVM install (static libs + llvm-config)
VKH="$WORK/vulkan-headers"   # Vulkan-Headers install
GLIBC_MAX_ALLOWED=2.31

LLVM_VER=21.1.8
LLVM_SHA256=4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142
MESA_VER=26.2.6
MESA_SHA256=b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92
MESA_URL=https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz
VK_TAG=vulkan-sdk-1.4.363.0

say() { printf 'cp-vk: %s\n' "$*"; }
die() { say "ERROR: $*"; exit "${2:-1}"; }

WSI="${CP_VK_WSI:-none}"
case "$WSI" in
  none) SFX= ;;
  x11)  SFX=-x11 ;;
  *) die "CP_VK_WSI=$WSI: expected none or x11" 2 ;;
esac
STAGE="$WORK/stage$SFX"
X11W="${CP_X11_WORK:-$HOME/cp/x11-build}"
X11DEV="$X11W/destdir/CP/X11"   # C4-x dev tree (include/, lib/)
X11PC="$X11W/pc"                # its .pc files, paths rewritten to the destdir
X11_RPATH=/CP/X11/lib           # where the guest stages the X libraries
SYSPC="$SYS/usr/lib/x86_64-linux-gnu/pkgconfig"
PCLIBDIR="$SYSPC"
if [ "$WSI" = x11 ]; then
  [ -f "$X11PC/xcb.pc" ] && [ -f "$X11DEV/include/xcb/xcb.h" ] || \
    die "CP_VK_WSI=x11 needs C4-x's libraries at $X11W (CP_X11_WORK=$X11W bash tools/cp/x11/build.sh all)" 2
  PCLIBDIR="$X11PC:$SYSPC"
fi

for t in clang clang++ ld.lld llvm-ar cmake ninja meson dpkg-deb readelf python3 sha256sum; do
  command -v "$t" >/dev/null || die "missing host tool: $t (apt: clang lld llvm cmake ninja-build meson dpkg readelf python3-mako python3-yaml)" 2
done
python3 -c 'import mako, yaml' 2>/dev/null || die "python3 needs mako + yaml (apt install python3-mako python3-yaml)" 2
# glslangValidator: only the mesa step needs it (>= 12.2: Mesa's BVH shaders
# pass '-P' preamble options, meson.build:735-760).  The probe shaders are
# committed as SPIR-V headers (src/user/cp/vkcube_*_spv.h) so `probes` needs
# no glslang; CP_VK_REGEN_SPV=1 regenerates them with $GLSLANG.
GLSLANG="${GLSLANG:-$(command -v glslangValidator || true)}"
mkdir -p "$WORK/dl" "$WORK/src" "$WORK/log"

# ---------------------------------------------------------------------------
# Common flags.  Every C/C++ compile and link goes through clang --sysroot.
# ---------------------------------------------------------------------------
CFLAGS_COMMON="--sysroot=$SYS -O2 -g1 -fPIC"
CXXFLAGS_COMMON="$CFLAGS_COMMON -stdlib=libc++ -nostdinc++ -isystem $LCXX/include/c++/v1"
LDFLAGS_COMMON="--sysroot=$SYS -fuse-ld=lld -Wl,--build-id=sha1"
# -static-libgcc: clang++ otherwise adds a hard (not --as-needed) -lgcc_s, a
#   DT_NEEDED on libgcc_s.so.1 for nothing — the unwinder is libc++.a's.
# --exclude-libs,<C++ runtime archives>: nothing from libc++/libc++abi/
#   libunwind is exported, so a DSO never interposes on the host process's own
#   C++ runtime or unwinder (measured: libunwind's HERMETIC option alone still
#   exports _Unwind_*).  NOT --exclude-libs,ALL: Mesa links lavapipe from its
#   own static archives, and ALL hid vk_icdGetInstanceProcAddr too (measured:
#   an 8.8 MB DSO with zero exports after --gc-sections).  Mesa's lavapipe
#   target has NO version script, so LLVM's archives are named explicitly in
#   the Mesa link (do_mesa: measured 29,787 exports without it).
XLIBS="-Wl,--exclude-libs,libc++.a:libc++abi.a:libunwind.a"
CXX_LDFLAGS="$LDFLAGS_COMMON -stdlib=libc++ -L$LCXX/lib -static-libgcc -Wl,--as-needed $XLIBS -lpthread"

sha_ok() { echo "$2  $1" | sha256sum -c --quiet - >/dev/null 2>&1; }

# --- 0. pins (OFFLINE: this script never downloads) ------------------------------
# The pinned files are verified by sha256 where they already are; a missing
# file is an error naming its URL (fetch it with tools/cp/fetch-runtime.sh or
# by hand, after approval).  The table is the subset of docs/cp-pins.md /
# tools/cp/fetch-runtime.sh this build reads; a row changes in all three.
VK_PINS="Vulkan-Loader-$VK_TAG.tar.gz|941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7|https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/$VK_TAG.tar.gz
Vulkan-Headers-$VK_TAG.tar.gz|4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e|https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$VK_TAG.tar.gz
libc6_2.31-0ubuntu9.18_amd64.deb|1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb
libc6-dev_2.31-0ubuntu9.18_amd64.deb|11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb
linux-libc-dev_5.4.0-218.238_amd64.deb|0aa210458df2ab7bc4536e8edf0f9423a7018e74e2eac0b5777fd2a36f170c2f|http://archive.ubuntu.com/ubuntu/pool/main/l/linux/linux-libc-dev_5.4.0-218.238_amd64.deb
libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb|4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb
libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb|ebe5e967fca32ea5811946180f7822ee749857861eadaa29514e65857ce3b897|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb"
pins() {
  local n sha url c bad=0
  while IFS='|' read -r n sha url; do
    if [ ! -f "$DL/$n" ]; then say "missing $DL/$n (pinned $sha, $url)"; bad=1
    elif ! sha_ok "$DL/$n" "$sha"; then say "MISMATCH $DL/$n (pinned $sha)"; bad=1; fi
  done <<< "$VK_PINS"
  [ "$bad" = 0 ] || die "pinned runtime files missing or wrong in $DL (CP_RUNTIME=$RT)" 2
  say "pins verified offline in $DL: Vulkan-Loader/Headers $VK_TAG, glibc 2.31 set"
  if [ ! -f "$WORK/dl/llvm-project-$LLVM_VER.src.tar.xz" ]; then
    for c in "${LLVM_TARBALL:-}" "$ROOT/third_party/llvm-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz" \
             "$ROOT/third_party/libcxx-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"; do
      [ -n "$c" ] && [ -f "$c" ] && { cp "$c" "$WORK/dl/"; break; }
    done
  fi
  [ -f "$WORK/dl/llvm-project-$LLVM_VER.src.tar.xz" ] || \
    die "LLVM tarball not found (third_party/llvm-$LLVM_VER/fetch.sh fetches it; sha256 $LLVM_SHA256)" 2
  sha_ok "$WORK/dl/llvm-project-$LLVM_VER.src.tar.xz" "$LLVM_SHA256" || die "LLVM tarball sha256 mismatch"
  grep -q "$LLVM_SHA256" "$ROOT/third_party/llvm-$LLVM_VER/container.sha256" || die "LLVM pin drifted from third_party/llvm-$LLVM_VER/container.sha256"
  if [ ! -f "$WORK/dl/mesa-$MESA_VER.tar.xz" ]; then
    for c in "${MESA_TARBALL:-}" "$HOME/.cache/hobbyos-mesa/mesa-$MESA_VER.tar.xz"; do
      [ -n "$c" ] && [ -f "$c" ] && { cp "$c" "$WORK/dl/"; break; }
    done
  fi
  [ -f "$WORK/dl/mesa-$MESA_VER.tar.xz" ] || die "Mesa tarball not found ($MESA_URL, sha256 $MESA_SHA256)" 2
  sha_ok "$WORK/dl/mesa-$MESA_VER.tar.xz" "$MESA_SHA256" || die "Mesa tarball sha256 mismatch"
  say "pins verified: llvm-project $LLVM_VER ($LLVM_SHA256), mesa $MESA_VER ($MESA_SHA256)"
}

# --- 1. glibc 2.31 sysroot (same recipe as tools/cp/build-wine.sh) ---------------
do_sysroot() {
  [ -f "$SYS/.ok" ] && { say "sysroot: $SYS (cached)"; return; }
  rm -rf "$SYS"; mkdir -p "$SYS"
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
  [ -f "$SYS/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o" ] || die "libgcc-10-dev did not provide crtbeginS.o"
  touch "$SYS/.ok"
}

src_extract() { # $1 tarball  $2 dir-name
  [ -f "$WORK/src/$2/.ok" ] && return
  rm -rf "${WORK:?}/src/$2"; tar -C "$WORK/src" -xf "$1"; touch "$WORK/src/$2/.ok"
  say "source: extracted $2"
}

# --- 2. libc++ / libc++abi / libunwind (static, PIC, hermetic) --------------------
do_runtimes() {
  [ -f "$LCXX/.ok" ] && { say "runtimes: $LCXX (cached)"; return; }
  src_extract "$WORK/dl/llvm-project-$LLVM_VER.src.tar.xz" "llvm-project-$LLVM_VER.src"
  local B="$WORK/build-runtimes"; rm -rf "$B" "$LCXX"
  # -nostdlib++: the sysroot has no C++ library yet (that is what this step
  # builds), so CMake's compiler sanity link must not ask for -lstdc++.
  say "runtimes: configure (log $WORK/log/runtimes.log)"
  cmake -G Ninja -S "$WORK/src/llvm-project-$LLVM_VER.src/runtimes" -B "$B" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LCXX" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
    -DCMAKE_C_FLAGS="$CFLAGS_COMMON" -DCMAKE_CXX_FLAGS="$CFLAGS_COMMON" -DCMAKE_ASM_FLAGS="--sysroot=$SYS" \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS_COMMON -nostdlib++" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_COMMON -nostdlib++" \
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
    > "$WORK/log/runtimes.log" 2>&1 || { tail -30 "$WORK/log/runtimes.log"; die "runtimes configure failed"; }
  ninja -C "$B" -j"$JOBS" install-cxx install-cxxabi install-unwind >> "$WORK/log/runtimes.log" 2>&1 \
    || { tail -30 "$WORK/log/runtimes.log"; die "runtimes build failed"; }
  # Only the static archives may be found by -lc++ (there is no .so to find).
  ls "$LCXX/lib"
  [ -f "$LCXX/lib/libc++.a" ] && [ -f "$LCXX/include/c++/v1/vector" ] || die "libc++ install incomplete"
  touch "$LCXX/.ok"
  say "runtimes: libc++ $(du -h "$LCXX/lib/libc++.a" | cut -f1) static (abi + unwinder merged in)"
}

# --- 3. LLVM (static, X86 only) -------------------------------------------------
do_llvm() {
  [ -f "$LLVMI/.ok" ] && { say "llvm: $LLVMI (cached)"; return; }
  do_runtimes
  local B="$WORK/build-llvm"
  say "llvm: configure (log $WORK/log/llvm.log), -j$JOBS, link jobs 1"
  [ -f "$B/build.ninja" ] || cmake -G Ninja -S "$WORK/src/llvm-project-$LLVM_VER.src/llvm" -B "$B" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LLVMI" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$CFLAGS_COMMON" -DCMAKE_CXX_FLAGS="$CXXFLAGS_COMMON" \
    -DCMAKE_EXE_LINKER_FLAGS="$CXX_LDFLAGS" -DCMAKE_SHARED_LINKER_FLAGS="$CXX_LDFLAGS" \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS= \
    -DLLVM_HOST_TRIPLE=x86_64-pc-linux-gnu \
    -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF -DLLVM_ENABLE_PIC=ON \
    -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_LIBCXX=ON \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF \
    -DLLVM_ENABLE_FFI=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_PARALLEL_LINK_JOBS=1 -DLLVM_PARALLEL_COMPILE_JOBS="$JOBS" \
    > "$WORK/log/llvm.log" 2>&1 || { tail -30 "$WORK/log/llvm.log"; die "llvm configure failed"; }
  local t0=$SECONDS
  # `install` installs the libraries + headers + cmake files; llvm-config is a
  # tool (LLVM_BUILD_TOOLS=OFF excludes it from `all`) so it is built and
  # copied explicitly — meson's config-tool lookup needs it.
  ninja -C "$B" -j"$JOBS" install llvm-config >> "$WORK/log/llvm.log" 2>&1 \
    || { tail -40 "$WORK/log/llvm.log"; die "llvm build failed (I/O error on linuxci? re-run)"; }
  install -D -m755 "$B/bin/llvm-config" "$LLVMI/bin/llvm-config"
  "$LLVMI/bin/llvm-config" --version --has-rtti --shared-mode --targets-built >/dev/null
  touch "$LLVMI/.ok"
  say "llvm: built in $((SECONDS-t0)) s; $("$LLVMI/bin/llvm-config" --version) rtti=$("$LLVMI/bin/llvm-config" --has-rtti) mode=$("$LLVMI/bin/llvm-config" --shared-mode) targets=$("$LLVMI/bin/llvm-config" --targets-built)"
}

# --- 4. Vulkan-Headers + Vulkan-Loader --------------------------------------------
do_loader() {
  do_sysroot
  src_extract "$DL/Vulkan-Headers-$VK_TAG.tar.gz" "Vulkan-Headers-$VK_TAG"
  src_extract "$DL/Vulkan-Loader-$VK_TAG.tar.gz" "Vulkan-Loader-$VK_TAG"
  if [ ! -f "$VKH/.ok" ]; then
    cmake -G Ninja -S "$WORK/src/Vulkan-Headers-$VK_TAG" -B "$WORK/build-vkh" \
      -DCMAKE_INSTALL_PREFIX="$VKH" > "$WORK/log/vkh.log" 2>&1
    cmake --install "$WORK/build-vkh" >> "$WORK/log/vkh.log" 2>&1
    touch "$VKH/.ok"
  fi
  local B="$WORK/build-loader$SFX" LG="$WORK/log/loader$SFX.log"; rm -rf "$B"
  local wsi=OFF lcf="$CFLAGS_COMMON"
  # x11: the loader only needs the xlib/xcb HEADERS (it forwards
  # vkCreateXlib/XcbSurfaceKHR to the ICD and never links an X library).
  [ "$WSI" = x11 ] && { wsi=ON; lcf="$lcf -I$X11DEV/include"; }
  say "loader: configure + build, WSI xlib/xcb $wsi (log $LG)"
  # FALLBACK_DATA_DIRS: with XDG_DATA_DIRS unset (a wave boot, or a Wine
  # process that never sets it) the loader still finds
  # $PREFIX/share/vulkan/icd.d/lvp_icd.x86_64.json — no env needed.  An
  # explicit VK_DRIVER_FILES / VK_ICD_FILENAMES still overrides it.
  # The loader REQUIREs pkg-config on Linux; point it at the sysroot so no
  # host .pc file can leak in (the WSI ones are all switched off anyway).
  PKG_CONFIG_LIBDIR="$PCLIBDIR" PKG_CONFIG_PATH= \
  cmake -G Ninja -S "$WORK/src/Vulkan-Loader-$VK_TAG" -B "$B" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
    -DCMAKE_C_FLAGS="$lcf" -DCMAKE_ASM_FLAGS="--sysroot=$SYS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_COMMON" -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS_COMMON" \
    -DCMAKE_PREFIX_PATH="$VKH" -DVULKAN_HEADERS_INSTALL_DIR="$VKH" \
    -DBUILD_WSI_XCB_SUPPORT=$wsi -DBUILD_WSI_XLIB_SUPPORT=$wsi -DBUILD_WSI_WAYLAND_SUPPORT=OFF \
    -DBUILD_WSI_DIRECTFB_SUPPORT=OFF -DBUILD_TESTS=OFF \
    -DFALLBACK_DATA_DIRS="$PREFIX/share:/usr/local/share:/usr/share" \
    > "$LG" 2>&1 || { tail -30 "$LG"; die "loader configure failed"; }
  grep -E 'XCB|X11|Xlib' "$LG" | head -8 || true
  ninja -C "$B" -j"$JOBS" >> "$LG" 2>&1 || { tail -30 "$LG"; die "loader build failed"; }
  DESTDIR="$STAGE" cmake --install "$B" >> "$LG" 2>&1
  say "loader: $(readelf -dW "$STAGE$PREFIX/lib/libvulkan.so.1" | awk '/SONAME/{print $NF}') installed under $STAGE$PREFIX/lib"
}

# --- 5. Mesa lavapipe -------------------------------------------------------------
do_mesa() {
  do_llvm
  src_extract "$WORK/dl/mesa-$MESA_VER.tar.xz" "mesa-$MESA_VER"
  local B="$WORK/build-mesa$SFX" NF="$WORK/meson-native$SFX.ini"
  local LC="$WORK/log/mesa-configure$SFX.log" LB="$WORK/log/mesa-build$SFX.log"
  local plat= xlink=
  # x11: Mesa's X11 WSI links libxcb + xcb-{dri3,present,sync,xfixes,randr,shm},
  # libX11-xcb and libxshmfence.  DT_RPATH (not RUNPATH: --disable-new-dtags)
  # /CP/X11/lib so the guest's copies win over XSESSION's LD_LIBRARY_PATH=/LIB64
  # (docs/cp-x11.md §5); -rpath-link resolves libxcb's libXau/libXdmcp.
  if [ "$WSI" = x11 ]; then
    plat=x11
    xlink=", '-L$X11DEV/lib', '-Wl,-rpath-link,$X11DEV/lib', '-Wl,--disable-new-dtags', '-Wl,-rpath,$X11_RPATH'"
  fi
  # Arrays in the native file: meson passes each element as one argv entry.
  local cxxinc="'-stdlib=libc++', '-nostdinc++', '-isystem', '$LCXX/include/c++/v1'"
  # Hide every static archive that is not Mesa's own: libc++/abi/unwind,
  # the static libdrm subproject, and LLVM.
  local xl="libc++.a:libc++abi.a:libunwind.a:libdrm.a" a
  for a in "$LLVMI"/lib/libLLVM*.a; do xl="$xl:$(basename "$a")"; done
  local MXLIBS="-Wl,--exclude-libs,$xl"
  cat > "$NF" <<EOF
[binaries]
c = ['clang', '--sysroot=$SYS']
cpp = ['clang++', '--sysroot=$SYS', $cxxinc]
ar = 'llvm-ar'
strip = 'llvm-strip'
llvm-config = '$LLVMI/bin/llvm-config'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-g1']
cpp_args = ['-g1']
c_link_args = ['-fuse-ld=lld', '-Wl,--build-id=sha1'$xlink]
cpp_link_args = ['-fuse-ld=lld', '-Wl,--build-id=sha1', '-stdlib=libc++', '-L$LCXX/lib', '-static-libgcc', '-Wl,--as-needed', '$MXLIBS', '-lpthread'$xlink]

[properties]
pkg_config_libdir = '$PCLIBDIR'
EOF
  # libdrm: on Linux every Mesa Vulkan driver build requires it
  # (meson.build: with_dri2 = with_any_vk and dri platform 'drm').  Mesa's own
  # tarball pins it in subprojects/libdrm.wrap (2.4.133 + sha256); we fetch
  # exactly that file from its official URL, verify the wrap's sha256, and
  # let meson build it as a STATIC subproject (--exclude-libs hides it), so
  # lavapipe gains no libdrm.so.2 DT_NEEDED.  --wrap-mode=nodownload forbids
  # meson from downloading anything itself.
  local wrap="$WORK/src/mesa-$MESA_VER/subprojects/libdrm.wrap"
  local drm_file drm_url drm_sha
  drm_file=$(sed -n 's/^source_filename = //p' "$wrap")
  drm_url=$(sed -n 's/^source_url = //p' "$wrap")
  drm_sha=$(sed -n 's/^source_hash = //p' "$wrap")
  mkdir -p "$WORK/src/mesa-$MESA_VER/subprojects/packagecache"
  local pc="$WORK/src/mesa-$MESA_VER/subprojects/packagecache/$drm_file"
  if ! sha_ok "$pc" "$drm_sha"; then
    [ -f "$WORK/dl/$drm_file" ] || { [ -n "${LIBDRM_TARBALL:-}" ] && cp "$LIBDRM_TARBALL" "$WORK/dl/$drm_file"; } || true
    [ -f "$WORK/dl/$drm_file" ] || die "missing $WORK/dl/$drm_file ($drm_url, sha256 $drm_sha; or set LIBDRM_TARBALL)" 2
    sha_ok "$WORK/dl/$drm_file" "$drm_sha" || die "$drm_file sha256 != Mesa's libdrm.wrap ($drm_sha)"
    cp "$WORK/dl/$drm_file" "$pc"
  fi
  say "mesa: libdrm subproject $drm_file sha256 $drm_sha (= Mesa $MESA_VER subprojects/libdrm.wrap)"
  if [ ! -f "$B/build.ninja" ]; then
    [ -n "$GLSLANG" ] && [ -x "$GLSLANG" ] || die "the mesa step needs glslangValidator >= 12.2 (GLSLANG=...)" 2
    say "mesa: configure, platforms='$plat', glslang $("$GLSLANG" --version | sed -n 's/^Glslang Version: [0-9]*://p') (log $LC)"
    PATH="$(dirname "$GLSLANG"):$PATH" PKG_CONFIG_LIBDIR="$PCLIBDIR" PKG_CONFIG_PATH= \
    meson setup "$B" "$WORK/src/mesa-$MESA_VER" --native-file "$NF" \
      --prefix="$PREFIX" --libdir=lib --buildtype=release -Db_ndebug=true \
      --wrap-mode=nodownload -Dallow-fallback-for=libdrm -Dlibdrm:default_library=static \
      -Dcpp_rtti=false \
      -Dplatforms=$plat -Dvulkan-drivers=swrast -Dgallium-drivers= \
      -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
      -Dllvm=enabled -Dshared-llvm=disabled -Ddraw-use-llvm=true \
      -Dshader-cache=disabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dspirv-tools=disabled \
      -Dbuild-tests=false -Dvideo-codecs= -Dtools= -Dvulkan-layers= \
      -Dandroid-libbacktrace=disabled \
      > "$LC" 2>&1 || { tail -40 "$LC"; die "mesa configure failed"; }
    grep -A40 -E '^  (Vulkan|LLVM|Platforms)' "$LC" | head -40 || true
  fi
  say "mesa: build (log $LB)"
  local t0=$SECONDS
  PATH="$(dirname "${GLSLANG:-/nonexistent/x}"):$PATH" ninja -C "$B" -j"$JOBS" >> "$LB" 2>&1 || { tail -40 "$LB"; die "mesa build failed"; }
  DESTDIR="$STAGE" meson install -C "$B" --no-rebuild --skip-subprojects >> "$LB" 2>&1
  say "mesa: built in $((SECONDS-t0)) s"
}

# --- 6. probes (src/user/cp/vk*) ---------------------------------------------------
do_probes() {
  do_sysroot
  local P="$ROOT/src/user/cp" O="$WORK/probes"; mkdir -p "$O" "$STAGE$PREFIX/bin"
  # The SPIR-V is committed (src/user/cp/vkcube_{vert,frag}_spv.h, generated
  # by `CP_VK_REGEN_SPV=1 bash tools/cp/vk/build.sh probes` with glslang
  # 16.2.0) so a build host needs no glslang.  Regeneration rewrites the
  # committed headers; `git diff` then shows whether the bytes changed.
  if [ "${CP_VK_REGEN_SPV:-0}" = 1 ]; then
    [ -n "$GLSLANG" ] && [ -x "$GLSLANG" ] || die "CP_VK_REGEN_SPV=1 needs glslangValidator (GLSLANG=...)" 2
    local gv; gv=$("$GLSLANG" --version | sed -n 's/^Glslang Version: [0-9]*://p')
    for s in vkcube.vert vkcube.frag; do
      "$GLSLANG" -V --quiet -o "$O/$s.spv" "$P/$s"
      python3 - "$O/$s.spv" "$P/${s/./_}_spv.h" "${s/./_}_spv" "$s" "$gv" <<'EOF2'
import sys, struct, hashlib
data = open(sys.argv[1], 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
with open(sys.argv[2], 'w') as f:
    f.write('/* GENERATED -- do not edit.  SPIR-V of src/user/cp/%s (%d bytes, sha256 %s),\n'
            % (sys.argv[4], len(data), hashlib.sha256(data).hexdigest()))
    f.write(' * made by glslangValidator %s: `glslangValidator -V --quiet -o <out>.spv %s`\n'
            % (sys.argv[5], sys.argv[4]))
    f.write(' * (CP_VK_REGEN_SPV=1 bash tools/cp/vk/build.sh probes). */\n')
    f.write('static const unsigned int %s[] = {\n' % sys.argv[3])
    for i in range(0, len(words), 8):
        f.write('  ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',\n')
    f.write('};\n')
EOF2
    done
    say "probes: regenerated SPIR-V headers with glslang $gv"
  fi
  for s in vkcube_vert_spv.h vkcube_frag_spv.h; do
    [ -f "$P/$s" ] || die "missing committed $P/$s (CP_VK_REGEN_SPV=1 regenerates it)"
  done
  # Plain glibc PIEs: libdl + libc only.  They dlopen("libvulkan.so.1") the way
  # winevulkan does; RUNPATH names the staged lib dir so no LD_LIBRARY_PATH is
  # needed (a wave boot has no environment).
  local cf=(--sysroot="$SYS" -O2 -g -Wall -Wextra -fPIE -pie -I"$VKH/include" -I"$P"
            -fuse-ld=lld -Wl,--enable-new-dtags,-rpath,$PREFIX/lib -Wl,--build-id=sha1)
  clang "${cf[@]}" -o "$STAGE$PREFIX/bin/vkenum" "$P/vkenum.c" -ldl
  clang "${cf[@]}" -o "$STAGE$PREFIX/bin/vkcube" "$P/vkcube.c" -ldl -lm
  if [ "$WSI" = x11 ]; then
    # vkcubex links libxcb: DT_RPATH (old dtags) /CP/X11/lib:/CP/VK/lib so the
    # X libraries resolve from /CP/X11/lib ahead of LD_LIBRARY_PATH=/LIB64.
    local cx=(--sysroot="$SYS" -O2 -g -Wall -Wextra -fPIE -pie -I"$VKH/include" -I"$P" -I"$X11DEV/include"
              -fuse-ld=lld -L"$X11DEV/lib" -Wl,-rpath-link,"$X11DEV/lib"
              -Wl,--disable-new-dtags,-rpath,$X11_RPATH:$PREFIX/lib -Wl,--build-id=sha1)
    clang "${cx[@]}" -o "$STAGE$PREFIX/bin/vkcubex" "$P/vkcubex.c" -lxcb -ldl -lm
  fi
  say "probes: $(ls "$STAGE$PREFIX/bin" | tr '\n' ' ')"
}

# --- 7. stage: strip, ICD JSON, manifest -------------------------------------------
do_stage() {
  local L="$STAGE$PREFIX/lib"
  [ -f "$L/libvulkan_lvp.so" ] || die "no libvulkan_lvp.so staged (run the mesa step)"
  mkdir -p "$WORK/unstripped"
  for f in "$L/libvulkan_lvp.so" "$L/libvulkan.so.1".*; do
    [ -L "$f" ] && continue
    cp -p "$f" "$WORK/unstripped/" 2>/dev/null || true
    llvm-strip --strip-unneeded "$f"
  done
  # Mesa writes library_path as $libdir/libvulkan_lvp.so — absolute /CP/VK/lib,
  # which is exactly where the guest stages it.
  local J="$STAGE$PREFIX/share/vulkan/icd.d/lvp_icd.x86_64.json"
  [ -f "$J" ] || die "no ICD JSON at $J"
  say "stage: ICD JSON $(tr -d '\n ' < "$J")"
  ( cd "$STAGE" && find CP -type f -o -type l | sort | while read -r f; do
      if [ -L "$f" ]; then echo "symlink $f -> $(readlink "$f")"; else
        printf '%s %10d %s\n' "$(sha256sum "$f" | cut -c1-16)" "$(stat -c%s "$f")" "$f"; fi
    done ) > "$WORK/stage-manifest.txt"
  cat "$WORK/stage-manifest.txt"
  say "stage: $(du -sh "$STAGE$PREFIX" | cut -f1) at $STAGE$PREFIX"
}

# --- 8. audit: GLIBC_ requirement + DT_NEEDED closure -----------------------------
do_audit() {
  # C collation: under en_US.UTF-8 (strix) `sort -u` and `comm` disagree on
  # underscore-prefixed names and every libc symbol looked "missing".
  local LC_ALL=C; export LC_ALL
  local bad=0 max="0" f v need
  local allowed_need='^(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|ld-linux-x86-64\.so\.2|libvulkan\.so\.1)$'
  # x11: also the C4-x X client libraries (staged at /CP/X11/lib)
  [ "$WSI" = x11 ] && allowed_need='^(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|ld-linux-x86-64\.so\.2|libvulkan\.so\.1|libX11\.so\.6|libX11-xcb\.so\.1|libxcb\.so\.1|libxcb-[a-z0-9]+\.so\.[0-9]+|libxshmfence\.so\.1|libXrandr\.so\.2|libXext\.so\.6|libXrender\.so\.1)$'
  {
    echo "# cp-vk audit $(date -u +%FT%TZ) — every ELF under $STAGE$PREFIX"
    while IFS= read -r f; do
      file -b "$f" | grep -q ELF || continue
      v=$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
      need=$(readelf -dW "$f" | awk '/NEEDED/{gsub(/[\[\]]/,"",$NF); print $NF}' | tr '\n' ' ')
      printf '%-48s GLIBC_max=%-6s NEEDED: %s\n' "${f#$STAGE}" "${v:-none}" "$need"
      [ -n "$v" ] && max=$(printf '%s\n%s\n' "$max" "$v" | sort -V | tail -1)
      if [ -n "$v" ] && [ "$(printf '%s\n%s\n' "$v" "$GLIBC_MAX_ALLOWED" | sort -V | tail -1)" != "$GLIBC_MAX_ALLOWED" ]; then
        echo "  FAIL: requires GLIBC_$v > $GLIBC_MAX_ALLOWED"; bad=1; fi
      for n in $need; do
        echo "$n" | grep -Eq "$allowed_need" || { echo "  FAIL: DT_NEEDED $n is outside the glibc 2.31 set + libvulkan"; bad=1; }
      done
      # any C++ runtime symbol exported means the hermetic link leaked
      if nm -D --defined-only "$f" 2>/dev/null | grep -Eq ' (_ZNSt|_ZSt|__cxa_|_Unwind_)'; then
        echo "  FAIL: exports C++ runtime symbols"; bad=1; fi
    done < <(find "$STAGE$PREFIX" -type f | sort)
    echo "# GLIBC max over the tree: GLIBC_$max (allowed <= $GLIBC_MAX_ALLOWED)"
    local ex; ex=$(nm -D --defined-only "$STAGE$PREFIX/lib/libvulkan_lvp.so" | awk '{print $NF}')
    echo "# lavapipe exports ($(echo "$ex" | wc -l)): $(echo "$ex" | tr '\n' ' ')"
    # Allowed: the ICD interface + the four symbols Mesa itself marks PUBLIC
    # (gallium debug/trace/noop screen wrappers, util_run_tests) — upstream's
    # libvulkan_lvp.so exports the same set.
    echo "$ex" | grep -Evq '^(vk_icd.*|ddebug_screen_create|noop_screen_create|trace_screen_create|util_run_tests)$' \
      && { echo "  FAIL: lavapipe exports symbols outside the ICD interface + Mesa PUBLIC set"; bad=1; }
    # every undefined symbol of lavapipe must be satisfied by the pinned runtime
    local U; U=$(nm -D --undefined-only "$STAGE$PREFIX/lib/libvulkan_lvp.so" | awk '{print $NF}' | sed 's/@.*//' | sort -u)
    local D; D=$( (for l in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 ld-linux-x86-64.so.2; do
                   nm -D --defined-only "$SYS/lib/x86_64-linux-gnu/$l" 2>/dev/null || nm -D --defined-only "$SYS/lib64/$l"
                 done
                 if [ "$WSI" = x11 ]; then for l in "$X11DEV"/lib/lib*.so.*; do [ -L "$l" ] || nm -D --defined-only "$l"; done; fi
                 ) | awk '{print $NF}' | sed 's/@.*//' | sort -u)
    local miss; miss=$(comm -23 <(echo "$U") <(echo "$D") | grep -v '^_ITM\|^__gmon_start__\|^_Jv_Register\|^__morestack$' || true)
    echo "# lavapipe undefined symbols: $(echo "$U" | wc -l); not provided by the 2.31 runtime: ${miss:-none}"
    [ -z "$miss" ] || bad=1
  } | tee "$WORK/audit.txt"
  grep -q 'FAIL\|not provided by the 2.31 runtime: [^n]' "$WORK/audit.txt" && bad=1
  [ "$bad" = 0 ] && say "audit: PASS ($WORK/audit.txt)" || die "audit FAILED ($WORK/audit.txt)"
}

case "$STEP" in
  sysroot)  pins; do_sysroot ;;
  runtimes) pins; do_sysroot; do_runtimes ;;
  llvm)     pins; do_sysroot; do_llvm ;;
  loader)   pins; do_loader ;;
  mesa)     pins; do_sysroot; do_mesa ;;
  probes)   do_probes ;;
  stage)    do_stage ;;
  audit)    do_audit ;;
  all)      pins; do_sysroot; do_runtimes; do_llvm; do_loader; do_mesa; do_probes; do_stage; do_audit ;;
  *) die "unknown step $STEP (sysroot|runtimes|llvm|loader|mesa|probes|stage|audit|all)" 2 ;;
esac
