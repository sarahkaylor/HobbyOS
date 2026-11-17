#!/usr/bin/env bash
# tools/mesa/venus/build.sh -- Mesa 26.2.6 Venus (-Dvulkan-drivers=virtio) for
# HobbyOS's Linux-ABI compat class, over /dev/gpu (Gate P6, lane C6-venus;
# docs/cp-venus.md §3).
#
#   bash tools/mesa/venus/build.sh [patch|configure|build|stage|audit|all]
#
# Same toolchain discipline as tools/cp/vk/build.sh (C5-vk2, docs/cp-vk.md):
# clang --sysroot=<glibc 2.31 sysroot>, lld, libdrm as Mesa's own pinned static
# subproject (hidden with --exclude-libs), libc++ from C5's hermetic build only
# because meson insists on a working C++ compiler (venus itself is C), X11 WSI
# against C4-x's libraries with DT_RPATH /CP/X11/lib.  OFFLINE: every input is
# already on the build host; nothing is downloaded.
#
# The HobbyOS backend is src/user/cp/vn_renderer_hobbyos.c (+ src/include/
# gpu_abi.h), copied into src/virtio/vulkan/ and tried first by
# vn_renderer_create() (-DVN_HOBBYOS).  The tree is patched by `patch` below
# (idempotent; a fresh copy of the tarball each time `patch` runs).
#
# Output: $WORK/stage/CP/VK/lib/libvulkan_virtio.so and
#         $WORK/stage/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json
#         (library_path /CP/VK/lib/libvulkan_virtio.so: lives next to C5's
#         lavapipe in the guest's /CP/VK; select it with VK_DRIVER_FILES).
#
# Env: VENUS_WORK (~/cp/venus-build), MESA_TARBALL (~/.cache/hobbyos-mesa/
#      mesa-26.2.6.tar.xz), CP_VK_WORK (~/cp/vk2-build: sysroot, libc++,
#      libdrm packagecache), CP_X11_DEV (~/cp/vk2-x11: X11 destdir + pc),
#      JOBS (16).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK="${VENUS_WORK:-$HOME/cp/venus-build}"
TARBALL="${MESA_TARBALL:-$HOME/.cache/hobbyos-mesa/mesa-26.2.6.tar.xz}"
MESA_SHA256=b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92
VKW="${CP_VK_WORK:-$HOME/cp/vk2-build}"
X11W="${CP_X11_DEV:-$HOME/cp/vk2-x11}"
JOBS="${JOBS:-16}"
STEP="${1:-all}"
SYS="$VKW/sysroot-glibc231"
LCXX="$VKW/libcxx"
X11DEV="$X11W/destdir/CP/X11"
X11PC="$X11W/pc"
SRC="$WORK/src/mesa-26.2.6"
B="$WORK/build-venus"
STAGE="$WORK/stage"
PREFIX=/CP/VK
export PATH="$HOME/.local/bin:$PATH"

say() { printf 'cp-venus-mesa: %s\n' "$*"; }
die() { say "ERROR: $*"; exit 1; }

for t in clang clang++ ld.lld llvm-ar meson ninja python3 sha256sum readelf; do
  command -v "$t" >/dev/null || die "missing host tool $t"
done
[ -f "$SYS/usr/include/stdio.h" ] || die "no glibc 2.31 sysroot at $SYS"
[ -f "$LCXX/include/c++/v1/vector" ] || die "no libc++ at $LCXX (C5-vk2's build)"
[ -f "$X11PC/xcb.pc" ] || die "no X11 pc files at $X11PC"

do_patch() {
  echo "$MESA_SHA256  $TARBALL" | sha256sum -c --quiet - || die "Mesa tarball sha256 mismatch"
  rm -rf "$WORK/src"; mkdir -p "$WORK/src"
  tar -C "$WORK/src" -xf "$TARBALL"
  local pc="$VKW/src/mesa-26.2.6/subprojects/packagecache"
  mkdir -p "$SRC/subprojects/packagecache"
  cp "$pc"/libdrm-*.tar.xz "$SRC/subprojects/packagecache/" || die "no libdrm packagecache in $pc"
  local V="$SRC/src/virtio/vulkan"
  cp "$ROOT/src/user/cp/vn_renderer_hobbyos.c" "$V/"
  cp "$ROOT/src/include/gpu_abi.h" "$V/"
  python3 - "$V" <<'EOF'
import sys, re
v = sys.argv[1]
m = open(v + '/meson.build').read()
assert "'vn_renderer_util.c'," in m
m = m.replace("'vn_renderer_util.c',", "'vn_renderer_util.c',\n  'vn_renderer_hobbyos.c',", 1)
m = m.replace("vn_c_args = [", "vn_c_args = [\n  '-DVN_HOBBYOS',", 1)
open(v + '/meson.build', 'w').write(m)
h = open(v + '/vn_renderer.h').read()
decl = '''#ifdef VN_HOBBYOS
/* HobbyOS /dev/gpu (src/user/cp/vn_renderer_hobbyos.c, docs/gpu-abi.md §10) */
VkResult
vn_renderer_create_hobbyos(struct vn_instance *instance,
                           const VkAllocationCallbacks *alloc,
                           struct vn_renderer **renderer);
#endif

static inline VkResult
vn_renderer_create(struct vn_instance *instance,
                   const VkAllocationCallbacks *alloc,
                   struct vn_renderer **renderer)
{
#ifdef VN_HOBBYOS
   if (vn_renderer_create_hobbyos(instance, alloc, renderer) == VK_SUCCESS)
      return VK_SUCCESS;
#endif
'''
old = '''static inline VkResult
vn_renderer_create(struct vn_instance *instance,
                   const VkAllocationCallbacks *alloc,
                   struct vn_renderer **renderer)
{
'''
assert h.count(old) == 1
h = h.replace(old, decl, 1)
open(v + '/vn_renderer.h', 'w').write(h)
# WSI: a renderer that cannot import dma-bufs (HobbyOS) cannot share native
# images with the X server, so take the software WSI path (CPU copy + PutImage)
# instead of requiring DRI3 -- without this, vkGetPhysicalDeviceSurfaceSupportKHR
# answers false on XHOBBY (no DRI3) and nothing can present.
w = open(v + '/vn_wsi.c').read()
old = """   const bool use_sw_device =
      !physical_dev->base.vk.supported_extensions
          .EXT_external_memory_dma_buf ||"""
assert w.count(old) == 1
w = w.replace(old, """   const bool use_sw_device =
      !physical_dev->instance->renderer->info.has_dma_buf_import ||
      !physical_dev->base.vk.supported_extensions
          .EXT_external_memory_dma_buf ||""", 1)
open(v + '/vn_wsi.c', 'w').write(w)
# Present synchronously on HobbyOS (docs/cp-venus.md §6.1): venus' async
# present thread (vn_wsi_present_async, used when the renderer has no implicit
# fencing) deadlocked against DXVK's / vkd3d-proton's present threads and
# Mesa's X11 software-WSI queue thread -- every thread of d3d11tri.exe parked
# in a futex after frame 1.  HOBBYOS_VN_ASYNC_PRESENT=1 restores it at run time.
old = """   if (!dev->renderer->info.has_implicit_fencing &&
       !VN_PERF(NO_ASYNC_PRESENT)) {"""
assert w.count(old) == 1
w = w.replace(old, """   if (!dev->renderer->info.has_implicit_fencing &&
       !VN_PERF(NO_ASYNC_PRESENT) && !vn_hobbyos_sync_present()) {""", 1)
w = w.replace('static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL\nvn_wsi_proc_addr', """static bool
vn_hobbyos_sync_present(void)
{
#ifdef VN_HOBBYOS
   const char *e = getenv("HOBBYOS_VN_ASYNC_PRESENT");
   return !(e && e[0] == '1');
#else
   return false;
#endif
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vn_wsi_proc_addr""", 1)
assert 'vn_hobbyos_sync_present(void)' in w
open(v + '/vn_wsi.c', 'w').write(w)
# No VK_KHR_present_wait{,2} on HobbyOS (docs/cp-venus.md §6.1).  Venus
# advertises them unconditionally; lavapipe does not.  DXVK and vkd3d-proton
# pace frames with vkWaitForPresentKHR when it exists, and on the X11
# software-WSI path (XHOBBY: no DRI3, images go out with PutImage) the present
# id it waits for never completes -- d3d11tri stopped after frame 1 and
# d3d12tri after frame 0, every thread parked in a futex, while vkcube (no
# present wait) ran.  Without the extensions both fall back to fences, as on
# lavapipe.
pd = open(v + '/vn_physical_device.c').read()
for ext in ('KHR_present_wait', 'KHR_present_wait2'):
    old = '   exts->%s = true;\n' % ext
    assert pd.count(old) == 1, ext
    pd = pd.replace(old, '#ifndef VN_HOBBYOS\n' + old + '#endif\n', 1)
open(v + '/vn_physical_device.c', 'w').write(pd)
# Diagnostics: VENUS_DEBUG_DEFAULT=<VN_DEBUG list> bakes a default for builds
# whose processes get no environment (Wine children of the P5 launcher).
import os
dd = os.environ.get('VENUS_DEBUG_DEFAULT', '')
if dd:
    c = open(v + '/vn_common.c').read()
    old = 'parse_debug_string(os_get_option("VN_DEBUG"), vn_debug_options)'
    assert c.count(old) == 1
    c = c.replace(old, 'parse_debug_string(os_get_option("VN_DEBUG") ? os_get_option("VN_DEBUG") : "%s", vn_debug_options)' % dd, 1)
    open(v + '/vn_common.c', 'w').write(c)
    print("VN_DEBUG default:", dd)
print("patched", v)
EOF
  say "patch: Mesa 26.2.6 (sha256 ok) + vn_renderer_hobbyos.c + gpu_abi.h + meson/vn_renderer.h hooks"
}

do_configure() {
  [ -f "$SRC/src/virtio/vulkan/vn_renderer_hobbyos.c" ] || do_patch
  local NF="$WORK/meson-native.ini"
  local xlink="'-L$X11DEV/lib', '-Wl,-rpath-link,$X11DEV/lib', '-Wl,--disable-new-dtags', '-Wl,-rpath,/CP/X11/lib'"
  cat > "$NF" <<EOF
[binaries]
c = ['clang', '--sysroot=$SYS']
cpp = ['clang++', '--sysroot=$SYS', '-stdlib=libc++', '-nostdinc++', '-isystem', '$LCXX/include/c++/v1']
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-g1']
cpp_args = ['-g1']
c_link_args = ['-fuse-ld=lld', '-Wl,--build-id=sha1', '-Wl,--exclude-libs,libdrm.a', $xlink]
cpp_link_args = ['-fuse-ld=lld', '-Wl,--build-id=sha1', '-stdlib=libc++', '-L$LCXX/lib', '-static-libgcc', '-Wl,--as-needed', '-Wl,--exclude-libs,libc++.a:libc++abi.a:libunwind.a:libdrm.a', '-lpthread', $xlink]

[properties]
pkg_config_libdir = '$X11PC:$SYS/usr/lib/x86_64-linux-gnu/pkgconfig'
EOF
  rm -rf "$B"
  say "configure (log $WORK/configure.log)"
  PKG_CONFIG_LIBDIR="$X11PC:$SYS/usr/lib/x86_64-linux-gnu/pkgconfig" PKG_CONFIG_PATH= \
  meson setup "$B" "$SRC" --native-file "$NF" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release -Db_ndebug=true \
    --wrap-mode=nodownload -Dallow-fallback-for=libdrm -Dlibdrm:default_library=static \
    -Dcpp_rtti=false \
    -Dplatforms=x11 -Dvulkan-drivers=virtio -Dgallium-drivers= \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
    -Dllvm=disabled -Dshader-cache=disabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled \
    -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dspirv-tools=disabled -Dbuild-tests=false -Dvideo-codecs= -Dtools= -Dvulkan-layers= \
    -Dandroid-libbacktrace=disabled \
    > "$WORK/configure.log" 2>&1 || { tail -40 "$WORK/configure.log"; die "configure failed"; }
  grep -A12 -E '^  Vulkan' "$WORK/configure.log" | head -14 || true
}

do_build() {
  [ -f "$B/build.ninja" ] || do_configure
  say "build (log $WORK/build.log)"
  # vn_renderer_hobbyos.c / gpu_abi.h may have changed since the patch step.
  cp "$ROOT/src/user/cp/vn_renderer_hobbyos.c" "$SRC/src/virtio/vulkan/"
  cp "$ROOT/src/include/gpu_abi.h" "$SRC/src/virtio/vulkan/"
  ninja -C "$B" -j"$JOBS" src/virtio/vulkan/libvulkan_virtio.so > "$WORK/build.log" 2>&1 || {
    grep -E 'error|Error' "$WORK/build.log" | head -30; tail -20 "$WORK/build.log"; die "build failed"; }
  ninja -C "$B" -j"$JOBS" >> "$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log"; die "build failed"; }
}

do_stage() {
  rm -rf "$STAGE"
  # meson install drops the build-tree RPATH entries (only /CP/X11/lib stays)
  DESTDIR="$STAGE" meson install -C "$B" --no-rebuild --skip-subprojects > "$WORK/install.log" 2>&1 ||
    { tail -20 "$WORK/install.log"; die "install failed"; }
  llvm-strip --strip-debug "$STAGE$PREFIX/lib/libvulkan_virtio.so"
  local json; json=$(ls "$STAGE$PREFIX"/share/vulkan/icd.d/virtio_icd*.json | head -1)
  [ "$json" = "$STAGE$PREFIX/share/vulkan/icd.d/virtio_icd.x86_64.json" ] ||
    mv "$json" "$STAGE$PREFIX/share/vulkan/icd.d/virtio_icd.x86_64.json"
  say "stage: $(cat "$STAGE$PREFIX/share/vulkan/icd.d/virtio_icd.x86_64.json" | tr -d '\n ')"
}

do_audit() {
  local so="$STAGE$PREFIX/lib/libvulkan_virtio.so" bad=0
  readelf -dW "$so" | grep -E 'NEEDED|RPATH|RUNPATH' | tee "$WORK/audit.txt"
  # glibc symbol versions must not exceed 2.31 (AD-2)
  local maxv
  maxv=$(readelf -VW "$so" | sed -n 's/.*Name: GLIBC_\([0-9.]*\).*/\1/p' | sort -V | tail -1)
  echo "max GLIBC_ version: $maxv" | tee -a "$WORK/audit.txt"
  [ "$(printf '%s\n2.31\n' "$maxv" | sort -V | tail -1)" = 2.31 ] || { say "GLIBC $maxv > 2.31"; bad=1; }
  readelf -dW "$so" | grep -q 'NEEDED.*libdrm' && { say "libdrm leaked as DT_NEEDED"; bad=1; }
  readelf -dW "$so" | grep -qE 'NEEDED.*(libc\+\+|libstdc\+\+)' && { say "C++ runtime DT_NEEDED"; bad=1; }
  [ "$(nm -D --defined-only "$so" | grep -c vk_icdGetInstanceProcAddr)" -ge 1 ] || { say "no vk_icdGetInstanceProcAddr export"; bad=1; }
  [ "$(strings "$so" | grep -c 'hobbyos backend initialized')" -ge 1 ] || { say "hobbyos backend not linked in"; bad=1; }
  readelf -dW "$so" | grep -q 'RPATH.*/home/' && { say "build-host RPATH leaked"; bad=1; }
  [ $bad = 0 ] && say "audit: ok ($(stat -c %s "$so") bytes)" || die "audit failed"
}

case "$STEP" in
  patch) do_patch ;;
  configure) do_configure ;;
  build) do_build ;;
  stage) do_stage ;;
  audit) do_audit ;;
  all) do_patch; do_configure; do_build; do_stage; do_audit ;;
  *) die "unknown step $STEP" ;;
esac
