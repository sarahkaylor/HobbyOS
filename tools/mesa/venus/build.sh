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

# P7: HB_MESA_ENV_DEFAULTS="K=V;K2=V2" bakes environment defaults into the
# build (hb_getenv in hb_present_stats.h), for processes whose environment the
# harness cannot set (Wine under the P5 launcher).  The environment wins.
hb_env_defaults() {
  local d="${HB_MESA_ENV_DEFAULTS:-}" f="$SRC/src/vulkan/wsi/hb_env_defaults.h"
  local want="#define HB_ENV_DEFAULTS \"$d\""
  [ -f "$f" ] && [ "$(cat "$f")" = "$want" ] || echo "$want" > "$f"
  [ -n "$d" ] && say "env defaults baked in: $d"
  return 0
}

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
  cp "$ROOT/tools/mesa/venus/hb_present_stats.h" "$ROOT/tools/mesa/venus/hb_wsi_x11.h" \
     "$SRC/src/vulkan/wsi/"
  hb_env_defaults
  python3 - "$V" "$SRC/src/vulkan/wsi" <<'EOF'
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
# Venus' async present (vn_wsi_present_async, used when the renderer has no
# implicit fencing).  C6-venus turned it off (docs/cp-venus.md §6.1: d3d11tri
# and d3d12tri died at frame 2 in pthread_mutex_lock(0x9)); P7 found the cause
# (the queue-lock race below, docs/cp-p7.md §3.2) and fixed it, so it is on
# again.  HOBBYOS_VN_ASYNC_PRESENT=0 presents synchronously.
old = """   if (!dev->renderer->info.has_implicit_fencing &&
       !VN_PERF(NO_ASYNC_PRESENT)) {"""
assert w.count(old) == 1
w = w.replace(old, """   if (!dev->renderer->info.has_implicit_fencing &&
       !VN_PERF(NO_ASYNC_PRESENT) && !vn_hobbyos_sync_present()) {""", 1)
w = w.replace('static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL\nvn_wsi_proc_addr', """static bool
vn_hobbyos_sync_present(void)
{
#ifdef VN_HOBBYOS
   const char *e = hb_getenv("HOBBYOS_VN_ASYNC_PRESENT");
   return e && e[0] == '0';
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
#
# P7 (lane C6-p7, docs/cp-p7.md §3): the software WSI now completes present
# ids (tools/mesa/venus/hb_wsi_x11.h: after put_image, or once the image is on
# the scanout), so the extensions come back.  HB_PRESENT_WAIT=0 (environment or
# HB_MESA_ENV_DEFAULTS) hides them again.
pd = open(v + '/vn_physical_device.c').read()
for ext in ('KHR_present_wait', 'KHR_present_wait2'):
    old = '   exts->%s = true;\n' % ext
    assert pd.count(old) == 1, ext
    pd = pd.replace(old, '   exts->%s = vn_hobbyos_present_wait();\n' % ext, 1)
anchor = '#include "vn_physical_device.h"\n'
assert pd.count(anchor) == 1
pd = pd.replace(anchor, anchor + '''#include "hb_present_stats.h" /* hb_getenv (P7) */

static bool
vn_hobbyos_present_wait(void)
{
#ifdef VN_HOBBYOS
   const char *e = hb_getenv("HB_PRESENT_WAIT");
   return !(e && e[0] == '0');
#else
   return true;
#endif
}
''', 1)
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
# ---- P7 (lane C6-p7, docs/cp-p7.md §3.2): the async-present fault.
# vn_queue_submission_count_semaphore() counts a signal semaphore whose payload
# is an imported sync fd (vn_semaphore_is_sync_fd), but
# vn_queue_submission_init_syncs() fills only SYNC / TIMELINE_SYNC payloads, and
# it reads sem->payload again, later.  When the two disagree the renderer
# batch carries sync_count entries of which the last were never written: the
# per-queue temp storage is reused, so the hole holds the previous submit's
# sync_vals[0] == 1, and hobbyos_submit() locks &((sync *)1)->mutex == 0x9
# (docs/cp-venus.md §6.1 item 3).  Log the mismatch (once per kind) and submit
# only the entries that were filled.
q = open(v + '/vn_queue.c').read()
old = """   if (submit->fence_handle != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(vn_fence, fence, submit->fence_handle);

      assert(fence->payload->type == VN_SYNC_TYPE_SYNC);

      submit->temp.syncs[sync_index] = fence->payload->sync;
      submit->temp.sync_vals[sync_index] = 1;

      /* fence is backed by renderer sync without renderer object */
      submit->fence_handle = VK_NULL_HANDLE;
   }
}
"""
assert q.count(old) == 1
q = q.replace(old, """   if (submit->fence_handle != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(vn_fence, fence, submit->fence_handle);

      assert(fence->payload->type == VN_SYNC_TYPE_SYNC);

      submit->temp.syncs[sync_index] = fence->payload->sync;
      submit->temp.sync_vals[sync_index++] = 1;

      /* fence is backed by renderer sync without renderer object */
      submit->fence_handle = VK_NULL_HANDLE;
   }
#ifdef VN_HOBBYOS
   if (sync_index != submit->sync_count) {
      static int hb_rows;
      if (hb_rows++ < 16) {
         char types[64] = "";
         int n = 0;
         for (uint32_t i = 0; i < sig_count && n < 56; i++) {
            VK_FROM_HANDLE(vn_semaphore, sem, vn_get_signal_semaphore(submit, i));
            n += snprintf(types + n, sizeof types - n, "%s%d%s", i ? "," : "",
                          (int)sem->payload->type, sem->sync_fd_export ? "x" : "");
         }
         fprintf(stderr, "[VNQ] submit counted %u renderer syncs, filled %u; "
                 "signal semaphore payload types now [%s] -- submitting the filled ones\\n",
                 submit->sync_count, sync_index, types);
      }
      submit->sync_count = sync_index;
   }
#endif
}
""", 1)
open(v + '/vn_queue.c', 'w').write(q)

# ---- P7 (lane C6-p7, docs/cp-p7.md §3.2): THE ROOT CAUSE of the async-present
# fault (pthread_mutex_lock(0x9) at frame 2, docs/cp-venus.md §6.1 item 3).
# Measured (run async2): the fault is hobbyos_submit() locking
# batch->syncs[1] == (sync *)1 inside the APP thread's vkQueueSubmit2, with
# the count/fill check above silent -- syncs[1] was filled right and then
# overwritten.  The batch lives in the queue's shared temp storage
# (queue->storage), and venus' async present thread submits on the same
# VkQueue.  vn_wsi_flush() only *waits* for the present thread to release
# queue_mutex at the start of an app submit; it does not hold it.  On a
# renderer without implicit fencing or external sync (HobbyOS, vtest), the
# present thread's vn_wsi_fence_wait() drops queue_mutex while it waits for
# the blit fence, so the app's submit starts, then the present thread
# re-locks and submits again (the present's own fence/semaphore submits)
# into the same storage: its sync_vals[0] == 1 lands on the app's syncs[1].
# Linux virtgpu never opens that window (has_external_sync).
# Fix: an app-thread submit HOLDS queue_mutex for its whole duration once
# async present is running (vn_wsi_queue_lock/unlock), instead of the
# lock/unlock barrier.
w = open(v + '/vn_wsi.c').read()
old = """void
vn_wsi_flush(VkQueue queue_handle)
{"""
assert w.count(old) == 1
w = w.replace(old, """/* P7: hold queue_mutex across an app-thread queue submission (see build.sh). */
bool
vn_wsi_queue_lock(VkQueue queue_handle)
{
   struct vn_queue *queue = vn_queue_from_handle(queue_handle);
   if (!queue->async_present.initialized ||
       queue->async_present.tid == vn_gettid())
      return false;
   simple_mtx_lock(&queue->async_present.queue_mutex);
   return true;
}

void
vn_wsi_queue_unlock(VkQueue queue_handle, bool locked)
{
   struct vn_queue *queue = vn_queue_from_handle(queue_handle);
   if (locked)
      simple_mtx_unlock(&queue->async_present.queue_mutex);
}

""" + old, 1)
open(v + '/vn_wsi.c', 'w').write(w)
hh = open(v + '/vn_wsi.h').read()
old = 'vn_wsi_flush(VkQueue queue_handle);\n'
assert hh.count(old) >= 1
hh = hh.replace(old, old + '\nbool\nvn_wsi_queue_lock(VkQueue queue_handle);\n\nvoid\nvn_wsi_queue_unlock(VkQueue queue_handle, bool locked);\n', 1)
open(v + '/vn_wsi.h', 'w').write(hh)
q = open(v + '/vn_queue.c').read()
wrappers = []
for name, params, args in (
        ('vn_QueueSubmit', 'VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence',
         'queue, submitCount, pSubmits, fence'),
        ('vn_QueueSubmit2', 'VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence',
         'queue, submitCount, pSubmits, fence'),
        ('vn_QueueBindSparse', 'VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo *pBindInfo, VkFence fence',
         'queue, bindInfoCount, pBindInfo, fence')):
    old = 'VKAPI_ATTR VkResult VKAPI_CALL\n%s(' % name
    assert q.count(old) == 1, name
    i = q.index(old)
    q = q[:i] + 'static VkResult\nvn_hb_%s_locked(' % name + q[i + len(old):]
    j = q.index('   vn_wsi_flush(queue);\n', i)
    assert j - i < 600, name
    q = q[:j] + q[j + len('   vn_wsi_flush(queue);\n'):]
    wrappers.append("""
VKAPI_ATTR VkResult VKAPI_CALL
%s(%s)
{
   const bool locked = vn_wsi_queue_lock(queue);
   VkResult result = vn_hb_%s_locked(%s);
   vn_wsi_queue_unlock(queue, locked);
   return result;
}
""" % (name, params, name, args))
q = q.rstrip('\n') + '\n\n/* P7: app-thread submissions hold the async-present queue lock (build.sh). */' + ''.join(wrappers)
open(v + '/vn_queue.c', 'w').write(q)

# ---- P7 (lane C6-p7, docs/cp-p7.md): zero-copy fullscreen present + the
# present-cost instrument.  The software X11 WSI gets a driver hook
# (wsi_device.hobbyos_scanout) that venus fills with GPU_IOC_SCANOUT_BLOB;
# x11_present_to_x11_sw() tries it before xcb_put_image
# (tools/mesa/venus/hb_wsi_x11.h) and completes present ids
# (VK_KHR_present_wait) on both paths; [HBP] rows split every present into
# acquire / submit / wait / flip / geometry (tools/mesa/venus/hb_present_stats.h).
ws = sys.argv[2]
h = open(ws + '/wsi_common.h').read()
old = '   bool sw;\n'
assert h.count(old) == 1
h = h.replace(old, old + """
   /* HobbyOS P7 (tools/mesa/venus/hb_wsi_x11.h): put `memory` (a linear
    * width x height image, `stride` bytes per row, virtio-gpu `format`) on
    * the scanout for X window `window`.  0, -EPERM/-EBUSY (not now: present
    * through X), other -errno (never again). */
   int (*hobbyos_scanout)(VkDevice device, VkDeviceMemory memory, uint32_t width,
                          uint32_t height, uint32_t stride, uint32_t format,
                          uint32_t window, uint32_t *set_us, uint32_t *flush_us);
""", 1)
open(ws + '/wsi_common.h', 'w').write(h)

c = open(ws + '/wsi_common.c').read()
old = '#include "wsi_common_private.h"\n'
assert c.count(old) == 1
c = c.replace(old, old + '#define HB_PSTATS_DEFINE\n#include "hb_present_stats.h"\n', 1)
old = """   VkResult result = swapchain->acquire_next_image(swapchain, pAcquireInfo,
                                                   pImageIndex);
"""
assert c.count(old) == 1
c = c.replace(old, """   const uint64_t hb_t0 = hb_tsc();
   VkResult result = swapchain->acquire_next_image(swapchain, pAcquireInfo,
                                                   pImageIndex);
   hb_pstats_add(HB_PS_ACQ, hb_tsc() - hb_t0);
""", 1)
old = """VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         struct vk_queue *queue,
                         const VkPresentInfoKHR *pPresentInfo)
{
"""
assert c.count(old) == 1
c = c.replace(old, """static VkResult
hb_wsi_common_queue_present(const struct wsi_device *wsi,
                            struct vk_queue *queue,
                            const VkPresentInfoKHR *pPresentInfo);
static __thread uint64_t hb_wait_cyc;

/* P7 instrument: [HBP] sub = this call minus its fence waits (wait). */
VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         struct vk_queue *queue,
                         const VkPresentInfoKHR *pPresentInfo)
{
   hb_wait_cyc = 0;
   const uint64_t t0 = hb_tsc();
   VkResult r = hb_wsi_common_queue_present(wsi, queue, pPresentInfo);
   const uint64_t dt = hb_tsc() - t0;
   hb_pstats_add(HB_PS_SUB, dt > hb_wait_cyc ? dt - hb_wait_cyc : 0);
   hb_pstats_add(HB_PS_WAIT, hb_wait_cyc);
   return r;
}

static VkResult
hb_wsi_common_queue_present(const struct wsi_device *wsi,
                            struct vk_queue *queue,
                            const VkPresentInfoKHR *pPresentInfo)
{
""", 1)
old = """         MESA_TRACE_SCOPE("throttle");
         results[i] = wsi->WaitForFences(vk_device_to_handle(dev),
                                         1, &swapchain->fences[image_index],
                                         true, ~0ull);
"""
assert c.count(old) == 1
c = c.replace(old, """         MESA_TRACE_SCOPE("throttle");
         const uint64_t hb_tw = hb_tsc();
         results[i] = wsi->WaitForFences(vk_device_to_handle(dev),
                                         1, &swapchain->fences[image_index],
                                         true, ~0ull);
         hb_wait_cyc += hb_tsc() - hb_tw;
""", 1)
old = """      if (wsi->sw) {
         wsi->WaitForFences(vk_device_to_handle(dev),
                            1, &swapchain->fences[image_index], true, ~0ull);
      }
"""
assert c.count(old) == 1
c = c.replace(old, """      if (wsi->sw) {
         const uint64_t hb_tw = hb_tsc();
         wsi->WaitForFences(vk_device_to_handle(dev),
                            1, &swapchain->fences[image_index], true, ~0ull);
         hb_wait_cyc += hb_tsc() - hb_tw;
      }
""", 1)
open(ws + '/wsi_common.c', 'w').write(c)

x = open(ws + '/wsi_common_x11.c').read()
old = """/**
 * Send image to X server unaccelerated (software drivers).
 */
static VkResult
x11_present_to_x11_sw(struct x11_swapchain *chain, uint32_t image_index)
{"""
assert x.count(old) == 1
x = x.replace(old, '#include "hb_wsi_x11.h"\n\n' + old, 1)
old = """   uint64_t max_req_len = xcb_get_maximum_request_length(chain->conn);

   if (image->rectangle_count > 0) {"""
assert x.count(old) == 1
x = x.replace(old, """   uint64_t max_req_len = xcb_get_maximum_request_length(chain->conn);

   const uint64_t hb_t0 = hb_tsc();
   int hb_refused = 0;
   const int hb_so = hb_x11_scanout(chain, image_index, &hb_refused);
   if (hb_so)
      goto hb_after_put;

   if (image->rectangle_count > 0) {""", 1)
old = """   xcb_flush(chain->conn);

   /* We don't have queued present here.
    * Immediately let application acquire again, but query geometry first so
    * we can report OUT_OF_DATE on resize. */
   xcb_generic_error_t *err;

   xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(chain->conn, geom_cookie, &err);
   VkResult result = VK_SUCCESS;"""
assert x.count(old) == 1
x = x.replace(old, """hb_after_put:
   if (hb_so) {
      /* On the scanout: no X traffic.  A size change of a fullscreen window
       * ends the X server's grant, and the next present (put_image) checks
       * the geometry again. */
      hb_pstats_add(HB_PS_FLIP, hb_tsc() - hb_t0);
      xcb_discard_reply(chain->conn, geom_cookie.sequence);
      hb_x11_present_done(chain, image);
      hb_pstats_present(1, 0);
      wsi_queue_push(&chain->acquire_queue, image_index);
      return x11_swapchain_result(chain, VK_SUCCESS);
   }
   xcb_flush(chain->conn);
   hb_pstats_add(HB_PS_FLIP, hb_tsc() - hb_t0);

   /* We don't have queued present here.
    * Immediately let application acquire again, but query geometry first so
    * we can report OUT_OF_DATE on resize. */
   xcb_generic_error_t *err;

   const uint64_t hb_tg = hb_tsc();
   xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(chain->conn, geom_cookie, &err);
   hb_pstats_add(HB_PS_GEOM, hb_tsc() - hb_tg);
   hb_x11_present_done(chain, image);
   hb_pstats_present(2, hb_refused);
   VkResult result = VK_SUCCESS;""", 1)
open(ws + '/wsi_common_x11.c', 'w').write(x)

# venus: fill the hook
w = open(v + '/vn_wsi.c').read()
old = """   physical_dev->wsi_device.supports_scanout = false;
"""
assert w.count(old) == 1
w = w.replace(old, old + """#ifdef VN_HOBBYOS
   if (vn_renderer_hobbyos_has_scanout(physical_dev->instance->renderer))
      physical_dev->wsi_device.hobbyos_scanout = vn_hobbyos_wsi_scanout;
#endif
""", 1)
old = 'static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL\nvn_wsi_proc_addr'
assert w.count(old) == 1
w = w.replace(old, """#ifdef VN_HOBBYOS
/* P7 (docs/cp-p7.md): the software WSI's scanout hook -> the blob behind
 * the image's VkDeviceMemory -> GPU_IOC_SCANOUT_BLOB. */
static int
vn_hobbyos_wsi_scanout(VkDevice dev_h, VkDeviceMemory mem_h, uint32_t width,
                       uint32_t height, uint32_t stride, uint32_t format,
                       uint32_t window, uint32_t *set_us, uint32_t *flush_us)
{
   struct vn_device *dev = vn_device_from_handle(dev_h);
   struct vn_device_memory *mem = vn_device_memory_from_handle(mem_h);
   if (!dev || !mem || !mem->base_bo)
      return -EINVAL;
   return vn_renderer_hobbyos_scanout(dev->renderer, mem->base_bo, width, height,
                                      stride, format, window, set_us, flush_us);
}
#endif

""" + old, 1)
if '#include <errno.h>' not in w:
    w = '#include <errno.h>\n' + w
w = w.replace('#include "vn_wsi.h"\n', '#include "vn_wsi.h"\n#include "hb_present_stats.h" /* hb_getenv (P7) */\n', 1)
assert 'hb_present_stats.h' in w
open(v + '/vn_wsi.c', 'w').write(w)

h = open(v + '/vn_renderer.h').read()
old = """#ifdef VN_HOBBYOS
/* HobbyOS /dev/gpu (src/user/cp/vn_renderer_hobbyos.c, docs/gpu-abi.md §10) */
VkResult
vn_renderer_create_hobbyos(struct vn_instance *instance,
                           const VkAllocationCallbacks *alloc,
                           struct vn_renderer **renderer);
"""
assert h.count(old) == 1
h = h.replace(old, old + """/* P7 (docs/gpu-abi.md §10.7): blob scanout of a bo; 0 or -errno. */
bool
vn_renderer_hobbyos_has_scanout(struct vn_renderer *renderer);
int
vn_renderer_hobbyos_scanout(struct vn_renderer *renderer, struct vn_renderer_bo *bo,
                            uint32_t width, uint32_t height, uint32_t stride,
                            uint32_t format, uint32_t window, uint32_t *set_us,
                            uint32_t *flush_us);
""", 1)
open(v + '/vn_renderer.h', 'w').write(h)
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
  cp "$ROOT/tools/mesa/venus/hb_present_stats.h" "$ROOT/tools/mesa/venus/hb_wsi_x11.h" \
     "$SRC/src/vulkan/wsi/"
  hb_env_defaults
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
