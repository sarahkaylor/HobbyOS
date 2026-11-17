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
| render server off the desktop GPU | every QEMU / render-server process and thread holds only renderD129; the one enumeration open of renderD128 is refused by the shim | `render-node-fencing.txt` |
| kernel transport, no Mesa | every request and error row ok; 1 MiB shmem mapped WB and verified | `step3-transport-probe.txt` |
| vulkaninfo --summary in the guest | GPU0 `Virtio-GPU Venus (AMD Radeon RX 9070 XT (RADV GFX1201))`, venus, Mesa 26.2.6, API 1.4.307, 171 device extensions -- and now the only GPU | `vulkaninfo-fence.txt` |
| GPU work on the host | 4 MiB fill + copy into mapped host-visible memory, 0 mismatches | `vulkaninfo-fence.txt` |
| **fence latency** (vkQueueSubmit -> vkWaitForFences, 10 000, TSC) | empty cmdbuf p50 101-162 us, p99 276-322 us; empty batch p50 56-66 us | `vulkaninfo-fence.txt` |
| timeline vkWaitSemaphores (2 000) | **p50 76 us, p99 200 us** (timeline syncs; was 10 ms on venus' feedback path) | `vulkaninfo-fence.txt` |
| **Linux vkcube on Venus in X** | **PASS**: 226.6 fps for 65 s, two screendumps | `probe1-vkcubex-venus.txt` |
| **D3D12 triangle (vkd3d-proton 3.1) under Wine on Venus** | **PASS**: 12 374 frames / 65 s = 190 fps, two screendumps | `probe2-d3d-venus-PASS.txt` |
| D3D11 triangle (DXVK 3.1.1) under Wine on Venus | PASS: 7 994 frames / 65 s = 123 fps | `probe2-d3d-venus-PASS.txt` |
| vkcube.exe under Wine on Venus | PASS (with the P5 device-name override) | `probe2-wine-venus.txt` |
| full batch at parity | see `gate-batch.txt` | `gate-batch.txt` |

### 6.1 The D3D stall (closed): root cause and fixes

Symptom (runs 1-10): d3d12tri rendered frame 0 and timed out on its fence for
frame 1; d3d11tri stopped after frame 1; vkcube.exe and Linux vkcube were fine.

1. **A transport bug, found first and fixed** (`[VENUS] stall` dump): a
   waiter parked 2 s on a fence whose used-ring entry had long been written.
   The GPU's INTx (line 10) is shared with the legacy virtio-net and was in
   edge mode (ELCR before = 0x00), so its edge was lost while the net held the
   line; and `gic_init()` re-masks every PIC line after the GPU's bring-up.
   Fix: level mode + re-unmask on the first `CTX_CREATE2`, and a per-tick
   re-check by parked waiters.  The stall stayed.
2. **The root cause: `VK_KHR_present_wait`.**  With the transport fixed, the
   stall dump showed no waiter in the kernel and every issued fence retired,
   while every thread of the process sat in a futex.  Venus advertises
   `VK_KHR_present_wait{,2}` unconditionally; lavapipe -- on which Gate P5
   passed with the same Wine, DXVK, vkd3d-proton and X server -- does not.
   DXVK and vkd3d-proton pace frames with `vkWaitForPresentKHR` when it
   exists, and on the X11 software-WSI path (XHOBBY: no DRI3, PutImage) the
   present id they wait for never completes.  Hiding both extensions on
   HobbyOS (`tools/mesa/venus/build.sh`, `#ifndef VN_HOBBYOS`) removes the
   stall: run 11 passed both D3D probes.
3. **Venus' async present must stay off** (the second, smaller workaround).
   Run 12 restored it (present-wait still hidden): vkcube.exe passed, but
   d3d11tri and d3d12tri both died at frame 2 with a user fault in
   `pthread_mutex_lock` on address 0x9 (libpthread+0xafc4, `mov 0x10(%rdi)`,
   rdi=9).  The build presents synchronously (`HOBBYOS_VN_ASYNC_PRESENT=1`
   restores the async path at run time); the fault is open (likely in the
   async thread's queue/fence hand-off with timeline syncs; Linux virtgpu runs
   the same path with real drm syncobjs).
   The proper fix belongs to P7 (scanout present, AD-6), where presents
   complete through the device and present-wait can come back.
4. Not causes (each measured): venus' async present (run 3 synchronous,
   same stall); binary vs timeline syncs (runs 1-8 binary, 8-11 timeline);
   Vulkan errors (`VN_DEBUG=result,wsi`, none).
