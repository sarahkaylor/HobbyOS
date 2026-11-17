# cp/p7 — Zero-copy presentation on Venus (lane C6-p7, wave 6)

Plan: `docs/cyberpunk.md` §5 AD-6, §6 P7, §7.2f (C6-p7 row), §7.3 F3/F5.
ABI: `docs/gpu-abi.md` §10.7 (the `GPU_IOC_SCANOUT_BLOB` amendment).
Groundwork: `docs/cp-venus.md` (P6), `docs/cp-x3.md` (XHOBBY, RandR).
Every number is from **strix** (Ryzen AI MAX+ 395 / Zen 5, RX 9070 XT on
renderD129, QEMU 10.2.1, virglrenderer 1.2.0), guest `-m 12G -smp 8 -cpu
host` under KVM, other lanes' guests on the same host and GPU at the same time.
Evidence: `docs/cp-p7-evidence/`; screendumps `evidence/cp-p7/` (git-ignored).

## 1. Result

| row | lane (P7 path) | before (P6 path) | evidence |
|---|---|---|---|
| Linux `vkcubex` fullscreen 1920x1080 FIFO, 65 s | **96.9 fps**, every present a blob flip | 39.9 fps (`HB_SCANOUT=0`: put_image) | `vkcubex-linux-fullscreen.txt` |
| `vkcube.exe` (Wine) fullscreen 1920x1080 FIFO, 65 s | **93.3 fps** (105–110 fps while the host was quiet) | 37.1 fps (put_image build, `HB_SCANOUT=0`) | `wine-vkcube-fs-and-p6-windowed.txt`, `wine-baselines.txt` |
| per-present cost, scanout (`[HBP]`, µs avg) | acquire 0, submit 80, wait 3 300, **flip 6 300** (SET_SCANOUT_BLOB 100 + RESOURCE_FLUSH 6 100) | flip 24 000 (put_image of 8 MB) | same |
| D3D11 / D3D12 triangle fullscreen 1920x1080 (synchronous present) | 70.8 / 54.7 fps on the scanout | — | §6 |
| `VK_KHR_present_wait` | **exposed again**; ids complete after put_image or the flip; DXVK `presentWait: 1`, vkd3d-proton paces with it | hidden | `wine-vkcube-fs-and-p6-windowed.txt` |
| venus async present | **root-caused and fixed** (§3.2); d3d11 114.8, d3d12 166.9 fps for 65 s | d3d11/d3d12 died at frame 2 | `async-present-rootcause.txt` |
| P6 rows windowed, present_wait on | vkcube.exe 207.9, d3d11 121.4, d3d12 188.4 fps | 204 / 118–123 / 176–190 | same |
| F5 hand-back: a window on top, RandR 1280x720 and back, exit | X takes the screen back each time, the client gets it back after | — | `f5-handback.txt` |

**Why not "the host's refresh rate".**  There is none: strix's monitor is
off (no CRTC enabled on either amdgpu card, `debugfs dri/*/state`), and QEMU
runs `-display egl-headless`, which has no vblank.  So FIFO is unpaced and the
measured ceiling is the host display itself: on egl-headless every
`RESOURCE_FLUSH` makes QEMU read the scanout back into its console surface
(`egl_scanout_flush`: blit + `glReadPixels` of 1920x1080, on QEMU's main
thread), **6.1 ms per flip** measured from the guest, and nothing else on the
virtio-gpu control queue moves while it runs.  That caps a fullscreen 1080p
client at ~100–160 fps; 60 Hz is well inside it.

## 2. The path

```
app (vkcube.exe, DXVK, vkd3d-proton)
 └ vkQueuePresentKHR -> venus (async present thread) -> wsi_common_queue_present
     the software X11 WSI: the frame is rendered into a device image and blitted
     into a LINEAR HOST-VISIBLE buffer = a Venus host blob (the render server's
     dma-buf).  The WSI queue thread then:
       hb_x11_scanout(): wsi_device.hobbyos_scanout(memory, w, h, stride, fmt, X window)
         venus: VkDeviceMemory -> bo -> res_id -> GPU_IOC_SCANOUT_BLOB
           kernel: granted for this window?  SET_SCANOUT_BLOB(res) + RESOURCE_FLUSH(res)
             QEMU: virgl_cmd_set_scanout_blob -> dma-buf -> egl-headless texture
       -> 0: done, no X traffic; present id complete
       -> -EPERM / -EBUSY: xcb_put_image as before (present id complete after it)
```

Pixels never cross into the guest: the one copy left is the GPU's own blit of
the frame into the linear buffer (on the host GPU, microseconds), which the
software WSI does anyway.

### 2.1 Kernel (`src/kernel/virtio_gpu_x64.c`, `gpu_abi.c`)

`GPU_IOC_SCANOUT_BLOB` (request 20, `docs/gpu-abi.md` §10.7) checks that the
blob is the fd's own and big enough, then `virtio_gpu_x64_scanout_flip()`:
`SET_SCANOUT_BLOB` and `RESOURCE_FLUSH` as two synchronous control-queue
transactions under the GPU lock.  The flip's return is the present's
completion.  Owner = (process group, fd); while a blob owns scanout 0 the
desktop's flushes are dropped (the X server keeps drawing into the
framebuffer), and the hand-back is `SET_SCANOUT(resource 1)` plus a full
transfer and flush.  Hand-back triggers: the owner's `res_id 0`, a flip that is
no longer granted, the X server's report changing, the fd closing
(`gpu_abi_release_fd`: exit and kills too), and `RES_UNREF` of the blob on the
scanout.  `[P7] scanout taken|back (why)|refused|X fullscreen` rows (64 at
most) show each transition.

### 2.2 F5: who owns the screen

The X server decides.  XHOBBY (`third_party/x11/ddx/hobbyos/hbscreen.c`,
`hbScanoutPoll` in the block handler) reports **the one window whose clipList
is the whole screen** -- clip lists partition the screen, so there is at most
one -- and the screen size, whenever either changes.  The report is a single
control rect on the flush syscall the server already makes (`{"HBP7", 1,
window, w << 16 | h}`; the kernel's syscall-boundary check accepts exactly that
rect alone, so no new syscall number).  The kernel grants a flip only for that
window at exactly that size.  Consequences, all measured (§5):

* a window mapped over the game (focus loss, a dialog, a menu) shrinks the
  game window's clipList -> the report becomes 0 -> the kernel puts the
  desktop back at once, inside the X server's own syscall; the client's next
  flip is refused and goes through put_image until the window is gone;
* a RandR mode change makes the screen 1280x720 while the client's images are
  1920x1080 -> refused -> put_image (the mode is shown top-left, black
  around, as before) until the mode or the swapchain matches again;
* exit, crash or kill closes the fd -> desktop.

The client never has to notice: the WSI just tries the flip every present.
Games that stop presenting when they lose focus are covered, because the
revocation is the X server's, not the client's.

### 2.3 Mesa (`tools/mesa/venus/`)

* `hb_wsi_x11.h` (into `wsi_common_x11.c`): the scanout attempt in
  `x11_present_to_x11_sw()`, format mapping (B8G8R8A8 -> virtio B8G8R8X8,
  R8G8B8A8 -> R8G8B8X8), the switch-off on hard errors, and present-id
  completion on both paths.  A flip skips the `xcb_get_geometry` round trip
  (a geometry change ends the grant first).
* `wsi_common.h`: `wsi_device.hobbyos_scanout`, filled by `vn_wsi_init()` when
  the kernel offers `GPU_FEAT_SCANOUT_BLOB`.
* `vn_renderer_hobbyos.c`: `vn_renderer_hobbyos_scanout()` (the ioctl).
* Environment: `HB_SCANOUT=0` (put_image only), `HB_PRESENT_WAIT=0` (hide
  present_wait), `HOBBYOS_VN_ASYNC_PRESENT=0` (synchronous present),
  `HB_PRESENT_STATS_MS` (row window, 0 = off).  Wine's processes inherit the
  launcher's fixed environment, so `HB_MESA_ENV_DEFAULTS="K=V;..."` at build
  time bakes defaults the environment still overrides.

## 3. present_wait and async present

### 3.1 present_wait

C6-venus hid `VK_KHR_present_wait{,2}` because on the software WSI the
present id never completed (completion came only from X Present events, which
XHOBBY never sends): DXVK and vkd3d-proton waited forever after frame 0/1.
`hb_x11_present_done()` now completes the id when the frame has gone out --
after `xcb_put_image`, or when the flip ioctl returns, i.e. when the host
display has taken the image.  Windowed P6 rows with present_wait on: DXVK
logs `presentWait : 1`, vkd3d-proton "Ensure maximum latency of 3 frames with
KHR_present_wait", and both run their 65 s at parity.

### 3.2 The async-present fault, root-caused

Symptom (C6-venus run 12): with venus' async present, d3d11tri and d3d12tri
died at frame 2 in `pthread_mutex_lock(0x9)`.

Measured (`async-present-rootcause.txt`): the stack's first words symbolize
(`libvulkan_virtio.so` from the build tree, load base from the one
`mtx_lock` call site whose return address matches) to `hobbyos_submit()`
locking `batch->syncs[1]->mutex` with `syncs[1] == (sync *)1`, inside the
**app** thread's `vkQueueSubmit2` (winevulkan then asserts on the failed
call).  A check that venus filled as many syncs as it counted stayed silent,
so the entry was written correctly and overwritten afterwards.

Cause: the batch's arrays live in the queue's shared temp storage
(`queue->storage`, reused by every submit on that `VkQueue`), and venus' async
present thread submits on the same queue (the blit, then its fence).
`vn_wsi_flush()` makes an app submit *wait* for the present thread to release
`queue_mutex`, but does not *hold* it; and on a renderer with neither
implicit fencing nor external sync (HobbyOS, vtest) `vn_wsi_fence_wait()`
releases `queue_mutex` while the present thread waits for the blit fence.  So
the app's submit starts, the present thread re-locks and submits again, and
its `sync_vals[0] == 1` lands on the app's `syncs[1]`.  Linux virtgpu never
opens that window (`has_external_sync`), which is why the same venus code
works there.

Fix (`build.sh`): once async present runs, an app-thread `vkQueueSubmit`,
`vkQueueSubmit2` or `vkQueueBindSparse` holds `queue_mutex` for its whole
duration (`vn_wsi_queue_lock/unlock`); the present thread, which already holds
it, is unaffected.  With it async present is on by default.

## 4. Seeing the glass on a headless host

QMP `screendump` cannot see a GL scanout on QEMU 10.2.1
(`qemu_console_surface()` is NULL unless the scanout is a plain surface), and
C6-venus' `pmemsave` fallback reads the kernel framebuffer -- the desktop,
exactly what a blob flip does not show.  egl-headless does read every flushed
scanout back into the console surface, so a VNC listener sees the glass:
`host-venus.sh` adds `-vnc unix:WORK/vnc-sock` with `VENUS_VNC=1` (a unix
socket; nothing on the network or the desktop), and `tools/cp/venus/vncshot.py`
(a 100-line RFB 3.8 client, Raw encoding) writes the PPM.  `p5venus.py`'s
screendump hook prefers it when the socket exists.

## 5. The present-cost instrument (`[HBP]`)

`tools/mesa/venus/hb_present_stats.h`: TSC per stage, one row per 2 s window
(`HB_PRESENT_STATS_MS`), per process:

```
[HBP] pid=14 n=200 fps=99.7 path=scanout acq=0/2 sub=120/8522 wait=3271/14801
      flip=6439/22003 geom=0/0 set=123 flush=6163 us scanout=200 putimage=0 refused=0 total=396
```

avg/max µs of: `acq` vkAcquireNextImageKHR; `sub` vkQueuePresentKHR minus its
fence waits (with async present: the venus present thread's share);
`wait` the software WSI's fence waits (the frame + its blit on the GPU);
`flip` the WSI queue thread's present (the ioctl, or put_image); `geom` the
X geometry round trip; `set`/`flush` the kernel's two round trips.

## 6. Runs (strix)

| run | config | result |
|---|---|---|
| vkcubex1 | Linux vkcubex fs, scanout, sync present | 96.9 fps, PASS (two VNC shots of the cube, no desktop) |
| vkcubex-put | same, `HB_SCANOUT=0` | 39.9 fps |
| vkfs1 | vkcube.exe fs, scanout, sync present | 93.3 fps, PASS |
| vkput1 | vkcube.exe fs, `HB_SCANOUT=0` baked in | 37.1 fps, PASS |
| win1 | P6 rows windowed, present_wait on | 207.9 / 121.4 / 188.4 fps, PASS |
| async2 | async present on, before the fix | d3d11/d3d12 fault at frame 2 (reproduced) |
| async3 | async present on, with the fix | d3d11 114.8, d3d12 166.9 fps, PASS |
| d3dfs1 | D3D12/D3D11 fs, scanout, sync present | 54.7 / 70.8 fps, PASS |
| f5a | vkcubex fs + p7occlude (occluder, RandR round trip), async on | every transition as §2.2 |

## 7. Not done / open

See the lane report.
