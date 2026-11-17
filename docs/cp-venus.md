# Cyberpunk P6 — Venus on HobbyOS (lane C6-venus)

Status: **see §6 (Gate P6 rows).**  Plan: `docs/cyberpunk.md` §6 P6, §7.2e, §7.3 F3.
ABI: `docs/gpu-abi.md` §10 (the F3 freeze).  P0 reference: `docs/cp-p0-guest.md`.
Every number below was measured on `strix` (Ryzen AI MAX+ 395 / Zen 5, 93 GB,
RX 9070 XT `1002:7550` on renderD129; QEMU 10.2.1, virglrenderer 1.2.0), guest
`-m 12G -smp 8 -cpu host` under KVM.  Evidence: `docs/cp-venus-evidence/`.

## 1. The stack

```
guest (HobbyOS, Linux-ABI compat class, glibc 2.31)        strix (host)
──────────────────────────────────────────────────          ─────────────────────────────────────
app (vkcubex / vn_info / Wine+vkd3d-proton)                  QEMU 10.2.1 virtio-gpu-gl-pci
 └ libvulkan.so.1 (C5-vk2's loader)                            venus=true,blob=true,hostmem=8G
    └ libvulkan_virtio.so  Mesa 26.2.6 venus                   memfd guest RAM (share=on)
       └ vn_renderer_hobbyos.c ── ioctl /dev/gpu ──┐           └ virglrenderer 1.2.0
kernel: gpu_abi.c (requests 11-20)                 │              └ virgl_render_server (LD_PRELOAD
        virtio_gpu_3d.c (blobs, rings, fences)     │                 dma-buf RDWR shim)
        virtio_gpu_x64.c (ctrlq, async slots, INTx)┴── virtio ──►     └ RADV 26.0.8 ─► RX 9070 XT
        host-visible window: BAR 4, 8 GiB, mapped into the app      egl-headless on renderD129
```

## 2. Host recipe (`tools/cp/host-venus.sh`)

`check | argv DISK WORK | env | boot DISK WORK` — always under
`tools/cp/qemu-slot.sh`.  The line it runs (`obj/*/cmdline.txt`):

```
LD_PRELOAD=~/cp/venus-build/shim/dmabuf-rdwr-shim.so
VK_DRIVER_FILES=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json MESA_VK_DEVICE_SELECT=1002:7550
env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY qemu-system-x86_64 -name hobbyos-venus,debug-threads=on
  -machine q35,accel=kvm,memory-backend=mem0 -cpu host -smp 8 -m 12288M
  -object memory-backend-memfd,id=mem0,size=12288M,share=on
  -drive if=pflash,format=raw,file=WORK/ovmf.fd
  -device virtio-gpu-gl-pci,blob=true,hostmem=8G,venus=true -vga none
  -display egl-headless,rendernode=/dev/dri/renderD129
  -serial file:WORK/serial.log  (+ the P5 nvme / edu / virtio-net set)  -qmp unix:WORK/qmp-sock
```

* `render_check` refuses to boot unless the node is `1002:7550` on amdgpu.
* The render server inherits QEMU's environment: radeon ICD only, 9070 XT
  ordered first (no `!`: P0 §2.1 item 4).  The render server still enumerates
  the 8060S (renderD128) as GPU1 — it opens the render node, nothing renders on
  it; every probe here uses GPU0.
* The dma-buf shim is P0's (`tools/cp/p0guest/dmabuf-rdwr-shim.c`), built into
  `~/cp/venus-build/shim/`.
* No `-daemonize` (the render server reports only on QEMU's stderr, which the
  harnesses write to `WORK/qemu-stderr.txt`).

The guest kernel's view (`docs/cp-venus-evidence/step1-boot-check.txt`):
features `0x3000001B` (virgl, edid, blob, context_init), the host-visible
window `cfg_type 8 shmid 1` = BAR 4 at `0x380800000000`, 8 GiB (QEMU moves the
virtio registers to BAR 2 when `hostmem` is set), capsets virgl (1), virgl2
(2), **venus (4, version 0, 160 bytes)**.

## 3. Kernel transport (`docs/gpu-abi.md` §10)

* **Negotiation.** `RESOURCE_BLOB` + `CONTEXT_INIT` are negotiated only as a
  pair and only next to `VIRGL` — a plain GL device stays on the M1 encoding
  (the M1 probe rows are unchanged on the Venus device too).
* **Control queue.** 128 descriptors: 0..2 the one synchronous command (as
  before), 3..66 thirty-two async fence slots.  `vgpu_drain_locked()` consumes
  every used-ring entry (head 0 = the sync command, others = a slot) and is run
  by every transaction, by the INTx handler and by fence waits.
* **INTx.** Only a blob-capable device gets its interrupt (line 10 → vector 42
  on q35/OVMF); `trap.c` dispatches it to `virtio_gpu_x64_handle_irq()` (ISR
  read first, then drain, then wake).  While no async fence is in flight the
  driver sets `VRING_AVAIL_F_NO_INTERRUPT`, so the 2D present path costs no
  interrupts.
* **Fences.** Seqnos start above 2^32 (QEMU retires M1 global fences by id
  across the whole queue); `done[ctx][ring]` is monotonic per ring.
  `FENCE_WAIT2` kick-polls first (below), then parks with the compat `-2`
  restart convention; the absolute deadline is written back into the user
  struct so a restarted wait keeps it.
* **Why kick-poll.** The first fence benchmark gave submit→signal p50
  **10.0 ms**; the kernel's own post→retire histogram (`[VENUS] fence stats`)
  showed 2449 of 4002 fences retiring 5–11 ms after posting: QEMU 10.2.1
  retires a finished virgl fence when it handles a queue notification, and
  otherwise only from its 10 ms poll timer.  A notify with nothing new in the
  queue is enough, so a waiter notifies + drains in a loop for up to 4 ms
  (`GPU_M2_KICK_SPIN_US`) before it parks.  Result: p50 **225 µs** (§6).
  Ring 0 (the CPU ring) is still 10 ms (only `vn_renderer_submit_simple_sync`
  uses it).
* **Blob maps.** First-fit window allocator (unit-tested); the pages are
  VMK_FB-class (shared, never freed by teardown or munmap) at
  `0x7000_0000_0000+` in a compat address space.  Memory type from the host's
  `map_info`: CACHED → WB, WC → PAT 4 (programmed WC on every core in
  `mmu_init_core_with_id`; entries 0–3 untouched), UNCACHED → PCD|PWT.
  Why it matters on this host: on AMD, KVM's nested paging takes the guest's
  PAT type for the window, so a WB guest mapping of memory the host maps WC
  would cache lines the GPU writes behind the CPU's back.  Measured: Venus'
  shmems answer `map_info=1` (CACHED).
* **Copies.** `vm_user_read`/`vm_kwrite` refuse a device page (`-EFAULT`)
  instead of dereferencing it through the direct map, which does not cover the
  window.  `read()` into a mapped blob is unaffected (cp-sync's
  `file_read_user()` writes through the caller's mapping).  Blob mappings are
  not VM objects, so `VM_OBJ_MAX_BYTES` (4 GiB since cp-sync) does not bound
  them; the 8 GiB window and `GPU_WIN_MAX_EXTENTS` (4096) do.

## 4. Mesa (`tools/mesa/venus/build.sh`, `src/user/cp/vn_renderer_hobbyos.c`)

Mesa 26.2.6 (sha256-checked tarball), `-Dvulkan-drivers=virtio -Dplatforms=x11`,
`-Dllvm=disabled`, clang `--sysroot` glibc 2.31, lld, libdrm as Mesa's static
subproject (from C5-vk2's packagecache, hidden), C5-vk2's libc++ only because
meson wants a C++ compiler, X11 from `~/cp/vk2-x11`.  34 s on strix.  Three
patches, applied by the script: the backend file, `vn_renderer_create()` trying
it first (`-DVN_HOBBYOS`), and `vn_wsi_init()` taking the software WSI when the
renderer cannot import dma-bufs (XHOBBY has no DRI3; without it
`vkGetPhysicalDeviceSurfaceSupportKHR` answers false — measured, first vkcube
run: `FAIL no graphics queue that can present to the surface`).

Backend choices: shmems are host3d `blob_id 0` MAPPABLE blobs (the render
server's own shmem, `supports_blob_id_0`); device memory is a host3d blob named
by the Venus memory id with the `vkAllocateMemory` stream as the create's
`cmd`; syncs are binary (like Mesa's simulated syncobj): a sync holds the
(ring, seqno) of the submit that signals it.  No dma-buf, no sync_file, no
placed maps (`VK_EXT_map_memory_placed` is still advertised by venus: a placed
`vkMapMemory2` fails).

Audit: no `libdrm`/C++ runtime `DT_NEEDED`, highest `GLIBC_2.29`, RPATH
`/CP/X11/lib` only, `vk_icdGetInstanceProcAddr` exported.  ICD:
`/CP/VK/share/vulkan/icd.d/virtio_icd.x86_64.json` → `/CP/VK/lib/libvulkan_virtio.so`
(next to lavapipe; harnesses either set `VK_DRIVER_FILES` or stage only this
JSON).

## 5. Probes and harnesses (`tools/cp/venus/`)

| harness | what |
|---|---|
| `boot_check.sh` | step 1: kernel rows on the Venus device |
| `run_probe.sh SRC.c` | a musl Linux-ABI probe in a MODE=test wave on the Venus device (`src/user/cp/vn_probe_transport.c`: the raw ABI, no Mesa) |
| `run_vk.sh ARGS` | `src/user/cp/vn_info.c` (vulkaninfo --summary block, GPU work check, fence benchmark, bandwidth) in a wave |
| `run_vkcubex_venus.py` | C5-vk2's `tools/cp/p5/run_vkcubex.py`, unchanged, with the Venus QEMU line and Venus as the only ICD |
| `run-wine-venus.sh` | C5-vk2's `tools/cp/p5/run-wine-p5.sh`, unchanged, with a merged Vulkan tree (Venus ICD only) and `p5venus.py` hooked into `boot_wine_p5.py` through `pyhook/sitecustomize.py` (`VENUS_P5=1`) |

## 6. Gate P6 (strix, cp-venus merged with cyberpunk 15ad4a35)

| row | result | evidence |
|---|---|---|
| host recipe: device, window, capsets | features `0x3000001B`; window BAR 4, 8 GiB; capset 4 (Venus) v0, 160 B | `step1-boot-check.txt` |
| kernel transport, no Mesa | every request and error row ok; 1 MiB shmem mapped WB and verified; 100 fenced submits through 32 slots | `step3-transport-probe.txt` |
| vulkaninfo --summary in the guest | GPU0 `Virtio-GPU Venus (AMD Radeon RX 9070 XT (RADV GFX1201))`, venus, Mesa 26.2.6, API 1.4.307, 171 device extensions; GPU1 is the 8060S | `vulkaninfo-fence.txt` |
| GPU work on the host | `vkCmdFillBuffer` + copy of 4 MiB into mapped host-visible memory, 0 mismatches | `vulkaninfo-fence.txt` |
| **fence latency** (vkQueueSubmit → vkWaitForFences, 10 000 fences, TSC) | empty cmdbuf: **p50 101 µs, p90 190, p99 276, max 4213**; empty batch: p50 56, p99 144; submit p50 52 µs | `vulkaninfo-fence.txt` |
| reference: Linux guest on Venus (P0) | D3D12 fence round trip p50 276 µs, p99 596–682 | `docs/cp-p0-guest.md` §3.3 |
| kernel post→retire | 20 002 fences: 93 % ≤ 200 µs, none > 5 ms; 9 300 retired by INTx, 10 702 by the waiter's kick-poll drain | `vulkaninfo-fence.txt` |
| timeline semaphores (vkWaitSemaphores) | correct, but p50 10.0 ms: venus' feedback path sleeps in `vn_relax` and the guest's sleep is tick-granular | `vulkaninfo-fence.txt` |
| **probe 1: Linux vkcube on Venus in X** | **PASS**: 14 731 frames / 65 s = 226.6 fps (FIFO, 640×480, software present), two screendumps with the cube | `probe1-vkcubex-venus.txt`, `evidence/cp-venus/vkcubex-t{20,50}s.png` |
| vkcube.exe under Wine (winevulkan) on Venus | 13 265 frames / 65 s (~200 fps) | `probe2-wine-venus.txt` |
| **probe 2: D3D12 triangle (vkd3d-proton 3.1) under Wine on Venus** | **FAIL**: device, swapchain (3 images), PSO and frame 0 (124–140 ms) render; the second frame's `ID3D12Fence` wait times out after 10 s, three runs (with async and with synchronous venus present) | `probe2-wine-venus.txt` |
| full batch at parity | PASS (one timing flake, exitgroup X7, 2/2 on rerun) | `gate-batch.txt` |

### 6.1 The open row: DXVK / vkd3d-proton stall after their first frames

What is known (`probe2-wine-venus.txt`, runs 1–5):
* Wine + winevulkan + Venus works: `vkcube.exe` renders for 65 s (~204 fps) and
  both D3D adapters are created on the Radeon (vkd3d-proton: FL 12_1 path,
  SM 6.0 asked, swapchain of 3 images; DXVK: FL 11_1).
* `d3d12tri.exe` renders frame 0 (124–153 ms), then the `ID3D12Fence` wait for
  frame 1 times out (10 s).  `d3d11tri.exe` renders frames 0 and 1, then stops
  making progress.  Lavapipe passes both (Gate P5), so the difference is
  Venus' own path, not Wine or the X server.
* Not the cause: venus' async present (run 3 presented synchronously: same
  stall); sync wait-for-submit semantics (run 2 onward); a Vulkan error (run 5
  with `VN_DEBUG=result,wsi` baked in logged none; `VENUS_DEBUG_DEFAULT` in
  `tools/mesa/venus/build.sh`).
* The kernel's global fence counters show only 4 ring fences retired across
  every Venus process of run 5 -- the stalled process never retired a
  renderer-sync fence, i.e. no `vkQueueSubmit(..., fence)` it waited on came
  back.  Timeline semaphores (vkd3d's D3D12 fences) are on venus' feedback
  path and do work in isolation (`vn_info timeline`).
* Next step: per-process fence state at the stall.  `FENCE_WAIT2` now prints
  `[VENUS] fence_wait2 timeout ... issued= done= ctx_inflight=` for finite
  waits; WSI waits are infinite, so the next lane should add the same row to
  a periodic dump (or a SysRq-style hook) and check whether the stalled
  queue's ring has fences issued and never answered by the host (a host queue
  blocked on a semaphore wait) or no fence issued at all (a guest-side wait
  that never submits).  Suspects, in order: a host-side `VkQueue` blocked on a
  semaphore the guest signalled only through venus' feedback path; a WSI
  image fence on a queue whose ring differs from the one the wait uses.
