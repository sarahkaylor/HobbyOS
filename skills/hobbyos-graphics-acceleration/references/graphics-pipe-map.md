# HobbyOS graphics pipe — driver map, seams, recipes

Companion depth for `hobbyos-graphics-acceleration` SKILL.md. Repo facts drift
— re-check the cited files before relying on a number.

## Data path (both arches)

- `map_fb()` (SYS_MAP_FB = 9) → `vm_map_fb()` maps the kernel's static
  `framebuffer[1024*768]` into the caller's address space at the reserved FB
  slot (VMK_FB leaves, shared, idempotent per process). Apps write the real
  pixels — there is no copy at flush time.
- `flush_fb()` (SYS_FLUSH_FB = 10) → `virtio_gpu_flush()` in
  `src/kernel/virtio_gpu.c`.
  - **ARM**: virtio-gpu MMIO at `0x0A000000 + slot*0x200`, device id 16.
    Commands ride one control vq; flush = full-frame TRANSFER_TO_HOST_2D +
    RESOURCE_FLUSH, synchronous polling with the lock held `irqsave` (IRQs off
    for the round trip).
  - **x64**: Bochs VGA (PCI `1234:1111`), found by PCI scan; flush = a raw
    786,432-dword memcpy from the kernel array to the card LFB. The x64 QEMU
    command line has no GPU device flag — the default std VGA serves.
- Desktop: `paint_frame()` (desktop.c) diffs chrome via `desktop_damage()`
  (DMG_MAX 12 rects; -1 or ≥ max = full repaint), clips per-rect passes, then
  calls `graphics_flush()` once per frame. The rect list is discarded at the
  flush — this is the plumbing seam.

## Syscall / ABI notes

- Rows are frozen through `SYS_SPAWN_EX` = 86 in `src/include/syscall.h`; new
  rows go above. A new syscall needs: header row, both arch dispatch paths
  (`arch/{arm,x64}/trap.c`), libc wrapper + user header, and a host-test compat
  stub — one change.
- Keep `flush_fb()` (full-frame) behavior intact for existing callers; the
  compositor's per-frame flush call must stay unconditional (harnesses count it
  as the frame boundary).

## x64 virtio transport realities

- `virtio_net.c` implements **legacy** virtio PCI: vendor `0x1AF4`,
  device `0x1000`-style ids, I/O-port register offsets (0x00–0x13), queue PFN.
  Copy that pattern for any new PCI virtio device.
- `virtio-blk-pci` init is broken on x64 (the system boots NVMe instead), so
  "net works" is the ONLY proven PCI-virtio precedent; keep a fallback path
  (BGA for GPU) behind any new PCI driver.
- ARM side: virtio MMIO IRQs are discovered as `48 + slot` (see
  `virtio_blk.c`), and `main.c` dispatches blk/net/input handlers — reuse for
  GPU IRQ completion. The GPU driver currently has no IRQ handler (pure
  polling).

## De-risk recipe: boot + screendump against a gl device

    qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 8 -m 8192M \
      -accel tcg,thread=multi -bios $HOME/.local/share/AAVMF/AAVMF_CODE.fd \
      -display gtk,gl=on -serial file:/tmp/gx-serial.log \
      -drive if=none,file=disk.img,format=raw,id=hd0 \
      -device virtio-blk-device,drive=hd0 \
      -device virtio-gpu-gl-device -device virtio-keyboard-device \
      -device virtio-tablet-device \
      -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
      -semihosting -action shutdown=poweroff -qmp unix:/tmp/gx.qmp,server,nowait

Same shape for x64 with `-M q35`, OVMF, and `-device virtio-gpu-gl-pci`.

Minimal QMP client: connect to the socket, `qmp_capabilities`, then
`screendump` with `{"filename": ..., "format": "ppm"}`, then `quit`. Verify
non-black pixels and checksum parity against the non-GL path. Serial output
only proves the guest booted — rendering must be checked from pixels.
