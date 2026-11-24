#!/usr/bin/env bash
# tools/nv/nvk/build.sh -- build Mesa's NVK Vulkan ICD for HobbyOS's Linux-ABI
# compat class (the class a real Vulkan client is: a glibc x86_64 ELF).
#
# Why not tools/mesa/build-hobbyos.sh: that build targets x86_64-none-elf with
# HobbyOS headers and replays the host's compile lines.  A Vulkan ICD has to be
# a normal glibc DSO (the loader dlopen()s it into a glibc process), so this
# build is a *cross build against the pinned glibc 2.31 sysroot* -- the same
# technique tools/cp/vk/build.sh uses for lavapipe: the target IS
# x86_64-linux-gnu, so meson's probes and the Rust/cbindgen/bindgen steps run on
# the host, and only the headers/libraries are the 2.31 pin.
#
# Mesa version: 25.0.7, not the tree's 26.2.6 pin.  Reason, measured: 26.2's
# NVK needs (a) the Rust NAK (rustc + bindgen + cbindgen) and (b) mesa-clc with
# LLVM + SPIRV-LLVM-Translator, because 26.2 compiles two OpenCL kernels into
# the driver.  25.0.7 is the last series whose NVK is plain C and has no CL,
# and the ICD ABI it speaks is the same one (interface version 5).  Everything
# below the port patch is upstream code either way.
#
# Output: $WORK/build-mesa-nvk25/src/nouveau/vulkan/libvulkan_nouveau.so
#         (SONAME libvulkan_nouveau.so, DT_NEEDED: libc/libm/libdl/libpthread
#          + ld-linux only -- no libz, no libdrm, no libstdc++/libc++)
#
# Steps: bash tools/nv/nvk/build.sh [sysroot|zlib|libelf|libdrm|mesa|probe|audit|all]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
W="${NVK_WORK:-$HOME/cp/vk-build}"
SYS="$W/sysroot-glibc231"
DL="$W/dl"
LLVM_LIBCXX="${NVK_LIBCXX:-$W/libcxx}"
STEP="${1:-all}"

MESA_VER=25.0.7
MESA_URL=https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz
LIBDRM_VER=2.4.131

say()  { printf 'nvk-build: %s\n' "$*"; }
die()  { say "ERROR: $*"; exit "${2:-1}"; }
need() { for t in "$@"; do command -v "$t" >/dev/null || die "missing host tool: $t"; done; }
sha_ok() { echo "$2  $1" | sha256sum -c --quiet - >/dev/null 2>&1; }

mkdir -p "$W/src" "$W/log" "$DL"

# --- 1. glibc 2.31 sysroot (same pins as tools/cp/vk/build.sh) --------------
# The compat class runs glibc 2.31.  Compiling against the host's headers names
# symbols 2.31 does not have (__isoc23_*, fstat64), which is a load-time failure
# in the guest, so every header comes from the pin.
do_sysroot() {
  [ -f "$SYS/.ok" ] && { say "sysroot: $SYS (cached)"; return; }
  local pins="
libc6_2.31-0ubuntu9.18_amd64.deb|1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb
libc6-dev_2.31-0ubuntu9.18_amd64.deb|11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb
linux-libc-dev_5.4.0-218.238_amd64.deb|0aa210458df2ab7bc4536e8edf0f9423a7018e74e2eac0b5777fd2a36f170c2f|http://archive.ubuntu.com/ubuntu/pool/main/l/linux/linux-libc-dev_5.4.0-218.238_amd64.deb
libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb|4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb
libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb|ebe5e967fca32ea5811946180f7822ee749857861eadaa29514e65857ce3b897|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb"
  mkdir -p "$SYS"
  local n sha url
  while IFS='|' read -r n sha url; do
    [ -n "$n" ] || continue
    [ -f "$DL/$n" ] || curl -fsSL -o "$DL/$n" "$url" || die "cannot fetch $url"
    sha_ok "$DL/$n" "$sha" || die "sha256 mismatch: $n"
    dpkg-deb -x "$DL/$n" "$SYS"
  done <<< "$pins"
  local l t cnt=0
  while IFS= read -r l; do t=$(readlink "$l"); ln -sfn "$SYS$t" "$l"; cnt=$((cnt+1)); done \
    < <(find "$SYS" -type l -lname '/*')
  say "sysroot: $SYS ($cnt absolute symlinks re-pointed)"
  touch "$SYS/.ok"
}

# --- 2. zlib, static and PIC ------------------------------------------------
# Mesa's DSO otherwise carries DT_NEEDED libz.so.1 (its disk cache compresses),
# and the compat class stages no libz.  A static PIC libz.a inside the sysroot
# removes the dependency instead of adding a guest library.
do_zlib() {
  [ -f "$SYS/usr/lib/libz.a" ] && { say "zlib: static libz.a in sysroot (cached)"; return; }
  local tb="$ROOT/third_party/zlib-1.3.1/zlib-1.3.1.tar.gz"
  [ -f "$tb" ] || die "third_party/zlib-1.3.1/zlib-1.3.1.tar.gz missing"
  tar -xf "$tb" -C "$W/src"
  ( cd "$W/src/zlib-1.3.1" &&
    CC="clang --sysroot=$SYS" CFLAGS="-O2 -fPIC" ./configure --static >/dev/null &&
    make -j"$(nproc)" libz.a >/dev/null ) || die "zlib build failed"
  cp "$W/src/zlib-1.3.1/libz.a" "$SYS/usr/lib/libz.a"
  cp "$W/src/zlib-1.3.1/zlib.h" "$W/src/zlib-1.3.1/zconf.h" "$SYS/usr/include/"
  say "zlib: static PIC libz.a installed into the sysroot"
}

# --- 3. libelf (NVK requires it) -------------------------------------------
# Mesa's nir/cubin loader uses libelf.  nvk refuses to configure without it
# ("NVK requires libelf").  The system's static libelf.a is used; the link audit
# in `audit` proves it pulls nothing newer than the 2.31 pin.
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

# --- 4. libdrm (nouveau), static -------------------------------------------
do_libdrm() {
  [ -f "$W/libdrm/lib/libdrm_nouveau.a" ] && { say "libdrm: $W/libdrm (cached)"; return; }
  [ -f "$DL/libdrm-$LIBDRM_VER.tar.xz" ] || \
    curl -fsSL -o "$DL/libdrm-$LIBDRM_VER.tar.xz" "https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VER.tar.xz" \
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
  meson setup "$W/build-libdrm" "$W/src/libdrm-$LIBDRM_VER" \
    --cross-file "$W/libdrm-cross.ini" --prefix="$W/libdrm" \
    -Dnouveau=enabled -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
    -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
    -Dtests=false -Dman-pages=disabled -Dvalgrind=disabled \
    > "$W/log/libdrm-setup.log" 2>&1 || die "libdrm configure failed"
  ninja -C "$W/build-libdrm" install > "$W/log/libdrm-build.log" 2>&1 \
    || die "libdrm build failed"
  say "libdrm: $(ls "$W/libdrm/lib"/libdrm*.a | tr '\n' ' ')"
}

# --- 5. Mesa's NVK ICD ------------------------------------------------------
do_mesa() {
  local icd="$W/build-mesa-nvk25/src/nouveau/vulkan/libvulkan_nouveau.so"
  if [ -f "$icd" ] && [ "${NVK_FORCE_MESA:-0}" != 1 ]; then
    say "mesa: $icd (cached; NVK_FORCE_MESA=1 to rebuild)"; return
  fi
  [ -f "$DL/mesa-$MESA_VER.tar.xz" ] || curl -fsSL -o "$DL/mesa-$MESA_VER.tar.xz" "$MESA_URL" \
    || die "cannot fetch mesa $MESA_VER"
  [ -d "$W/src/mesa-$MESA_VER" ] || tar -xf "$DL/mesa-$MESA_VER.tar.xz" -C "$W/src"
  # The one HobbyOS patch: node discovery + named refusals (see the file).
  local P="$ROOT/tools/nv/nvk/patches/0001-nvk-hobbyos-node-and-named-refusals.patch"
  [ -f "$P" ] || die "patch missing: $P"
  if ! grep -q NVK_HOBBYOS_NODE "$W/src/mesa-$MESA_VER/src/nouveau/vulkan/nvk_instance.c"; then
    ( cd "$W/src/mesa-$MESA_VER" && patch -p1 < "$P" ) || die "patch did not apply"
    say "mesa: applied $(basename "$P")"
  fi
  cat > "$W/mesa-nvk-cross.ini" <<EOF
[binaries]
c = ['clang', '--sysroot=$SYS']
cpp = ['clang++', '--sysroot=$SYS', '-stdlib=libc++', '-nostdinc++', '-isystem', '$LLVM_LIBCXX/include/c++/v1']
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
bindgen = '$(command -v bindgen || echo bindgen)'
cbindgen = '$(command -v cbindgen || echo cbindgen)'

[built-in options]
c_args = ['-O2', '-g1', '-fPIC', '-w', '-mtune=generic']
cpp_args = ['-O2', '-g1', '-fPIC', '-w', '-mtune=generic']
c_link_args = ['-fuse-ld=lld', '--sysroot=$SYS', '-Wl,--build-id=sha1']
cpp_link_args = ['-fuse-ld=lld', '--sysroot=$SYS', '-Wl,--build-id=sha1', '-stdlib=libc++', '-L$LLVM_LIBCXX/lib', '-static-libgcc', '-Wl,--as-needed', '-lpthread']
buildtype = 'release'
b_ndebug = 'true'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
  [ -f "$LLVM_LIBCXX/lib/libc++.a" ] || die "no libc++.a under $LLVM_LIBCXX (Mesa has C++ TUs: util/u_qsort.cpp, texcompress_astc.cpp)"
  export PATH="$HOME/.cargo/bin:$PATH"
  export PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$W/libdrm/lib/pkgconfig"
  export PKG_CONFIG_PATH=
  export LIBCLANG_PATH="${LIBCLANG_PATH:-$W/libclang}"
  export BINDGEN_EXTRA_CLANG_ARGS="--sysroot=$SYS -isystem $SYS/usr/include -isystem $SYS/usr/include/x86_64-linux-gnu"
  # Vulkan drivers only; no gallium, no GL, no EGL, no llama/llvm, no zstd, no
  # shader cache (its zlib would be a DT_NEEDED) -- and no platforms, so the
  # WSI is headless (HobbyOS presents through its own X stack).
  meson setup --wipe "$W/build-mesa-nvk25" "$W/src/mesa-$MESA_VER" \
    --cross-file "$W/mesa-nvk-cross.ini" --prefix="$W/nvk-install" \
    -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
    -Dgbm=disabled -Dglvnd=disabled -Dllvm=disabled -Dvideo-codecs= \
    -Dzstd=disabled -Dshader-cache=disabled -Dxmlconfig=disabled \
    -Dbuild-tests=false -Dtools= -Dhtml-docs=disabled \
    > "$W/log/mesa-setup.log" 2>&1 || { tail -20 "$W/log/mesa-setup.log"; die "mesa configure failed"; }
  ninja -C "$W/build-mesa-nvk25" -j"$(nproc)" \
    src/nouveau/vulkan/libvulkan_nouveau.so src/nouveau/vulkan/nouveau_icd.x86_64.json \
    > "$W/log/mesa-build.log" 2>&1 || { tail -30 "$W/log/mesa-build.log"; die "mesa build failed"; }
  say "mesa: $(stat -c%s "$icd") B $icd"
}

# --- 6. the probe ----------------------------------------------------------
do_probe() {
  local icd="$W/build-mesa-nvk25/src/nouveau/vulkan/libvulkan_nouveau.so"
  [ -f "$icd" ] || die "build the ICD first"
  local vkh="${NVK_VKH:-$W/vulkan-headers/include}"
  [ -f "$vkh/vulkan/vulkan.h" ] || die "no Vulkan-Headers at $vkh (NVK_VKH=...)"
  clang --sysroot="$SYS" -O2 -fPIE -pie -Wall -I"$vkh" \
    -o "$W/nvk_probe" "$ROOT/src/user/nvk_probe.c" "$icd" \
    -Wl,-rpath,/CP/VK/lib -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    || die "probe link failed"
  say "probe: $W/nvk_probe"
}

# --- 7. audit --------------------------------------------------------------
# Every claim the report makes about the DSO, checked here so it cannot rot.
do_audit() {
  local icd="$W/build-mesa-nvk25/src/nouveau/vulkan/libvulkan_nouveau.so" bad=0
  [ -f "$icd" ] || die "no ICD to audit"
  local needed
  needed=$(readelf -dW "$icd" | awk '/NEEDED/{gsub(/[\[\]]/,"",$NF); print $NF}' | sort | tr '\n' ' ')
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
  say "audit: PASS"
}

case "$STEP" in
  sysroot) do_sysroot ;;
  zlib)    do_zlib ;;
  libelf)  do_libelf ;;
  libdrm)  do_libdrm ;;
  mesa)    do_mesa ;;
  probe)   do_probe ;;
  audit)   do_audit ;;
  all)
    need clang clang++ llvm-ar meson ninja dpkg-deb readelf curl sha256sum patch
    need python3 bindgen cbindgen rustc
    do_sysroot; do_zlib; do_libelf; do_libdrm; do_mesa; do_probe; do_audit ;;
  *) die "unknown step '$STEP' (sysroot|zlib|libelf|libdrm|mesa|probe|audit|all)" 2 ;;
esac
