---
name: hobbyos-kernel-constraints
description: "Use when writing or reviewing HobbyOS kernel/bare-metal code, or when debugging a synchronous exception / alignment fault / boot hang. Encodes the ARM64 EL1 hardware rules (no unaligned access, no SIMD, byte-wise struct parsing, Device memory) and the project's non-negotiable requirements."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [hobbyos, arm64, aarch64, el1, baremetal, alignment, mmu, constraints]
    related_skills: [hobbyos-build-and-run, hobbyos-run-tests, hobbyos-add-userland-program]
---

# HobbyOS: Kernel / Bare-Metal Coding Constraints

## Overview

HobbyOS runs in **AArch64 EL1 with memory typed `Device` / `Normal-NonCacheable`**, which makes the hardware far less forgiving than a hosted OS. A whole class of "mysterious" boot hangs and synchronous exceptions trace back to a handful of violated rules: unaligned memory access, compiler-emitted SIMD, or a packed-struct field read as a wide load. Internalize these rules *before* writing kernel code — they prevent the bug rather than debug it. When a trap or hang does appear, this is the first checklist to run against the change. Root-cause the fault; don't just move code around until it stops (see [[hobbyos-run-tests]]'s deadlock rule).

## When to Use

- Writing/reviewing anything under `src/kernel/` or `src/kernel/arch/`.
- Debugging a Synchronous Exception, SError, alignment fault, or a boot that hangs before the desktop/tests.
- Parsing on-disk or on-wire binary structures (FAT BPB, VirtIO, packet headers).

Don't use for: userland programs (→ [[hobbyos-add-userland-program]]) — EL0 code runs under the MMU with Normal memory and doesn't hit most of these.

## Non-Negotiable Requirements (never regress these)

- **ARM64 only.** This OS targets AArch64 (Cortex-A53) and no other CPU as the primary. x86_64 is a secondary port behind `#ifdef __x86_64__`.
- **SMP, ≥4 cores.** Never reduce `-smp` below 4. Multi-processing and IPC must always work.
- **Memory protection + user mode.** EL0/EL1 separation and sandboxing must always be preserved.

## The Hardware Rules (violating these = traps)

1. **No unaligned memory access.** With the MMU disabled (or Normal-NC), memory defaults to `Device-nGnRnE`, where unaligned loads/stores are **hardware-prohibited** and fault immediately. Never cast a `uint8_t*` at an arbitrary offset to a wider type and dereference it.

2. **No SIMD/NEON in EL1.** Clang uses `q0-q31` to vectorize `memcpy`/array loops, but the SIMD units aren't enabled (no `CPACR_EL1` setup), so those instructions raise a Synchronous Exception. **All kernel compilation enforces `-mgeneral-regs-only`** — keep it. If you add a new arch source dir or flag set, carry the flag through.

3. **Parse binary structures byte-by-byte.** Packed structs (FAT BPB, etc.) place multi-byte fields at unaligned offsets (e.g. `bytes_per_sector` at offset 11), and Clang turns a struct read into a 16-bit `ldrh` → alignment fault. **Rule:** map raw metadata as `volatile uint8_t*` and assemble integers from individual bytes:
   ```c
   volatile uint8_t *p = base;
   uint16_t bytes_per_sector = (uint16_t)p[11] | ((uint16_t)p[12] << 8);  // little-endian, byte-wise
   ```
   Do **not** reintroduce `__attribute__((packed))` struct field reads for on-disk/on-wire data.

4. **Memory is Normal-Non-Cacheable by design.** Kernel/RAM buffers use `MAIR_NORMAL_NC`, which gives unaligned-access tolerance for *Normal* buffers and — critically — lets VirtIO/DMA share buffers with the host **without cache maintenance**. Don't add cacheable mappings or assume you can skip coherence some other way; the NC choice *is* the coherence strategy. DMA/queue structures (VirtIO v1 `desc`/`avail`/`used`) must stay physically contiguous and page-aligned.

5. **Preserve context exactly in exception handlers.** On an IRQ, the wrapper must save/restore the full register state; notably `x30` (LR) must be preserved before calling into C or the stack frame corrupts on `eret`. GICv2 routes SPIs to CPU 0; the vector table is `.align 11`. Don't trim register saves to "optimize" a handler.

6. **Prefer WFI over busy-poll.** Drivers sleep the core with `wfi` and wake on GIC assertion rather than spinning. A busy-poll that never yields can starve other cores/interrupts and present as a hang.

## x86_64 Parity Notes

When touching shared subsystems, keep the x86_64 port working too: DHCP/networking must stay dynamic (no hardcoded static IP to "simplify" x86_64), and PCIe/RDMA/IOMMU code is `#ifdef __x86_64__`-guarded (ARM compiles an empty TU). See [[hobbyos-proxmox-gpu]] for the x86_64 GPU path.

## Debugging a trap / hang with these rules

1. Reproduce headless with serial captured (`make unit_tests` or `make test`, `-serial file:...`). A hang with no log is still a signal (deadlock rule).
2. Suspect the **most recent** change against rules 1–3 first — a new struct read, a new loop Clang may have vectorized, a new cast.
3. Check the faulting PC against `objdump` (`hobbyos.elf`; there's a committed `objdump.txt`) for an `ldrh`/`ldr` at an odd offset or a stray `q`-register instruction — the fingerprint of an alignment or SIMD fault.
4. Confirm `-mgeneral-regs-only` still applies to the translation unit that regressed.

## Common Pitfalls

1. **Reintroducing packed-struct reads** for FAT/VirtIO/packets — the classic alignment fault. Use byte-wise assembly.
2. **A helper that memcpys/loops over a struct** Clang vectorizes — even with the flag, a newly added source outside the normal `CFLAGS` path can slip. Verify the flag reaches it.
3. **Lowering `-smp` below 4** to dodge an SMP race — forbidden; fix the race.
4. **Adding cacheable mappings** and then chasing DMA coherence bugs — the design is Normal-NC on purpose.
5. **Trimming exception-handler register saves** — corrupts `eret`.

## Verification Checklist

- [ ] No new unaligned wide loads; binary structures parsed byte-by-byte.
- [ ] New/changed kernel TUs still compiled with `-mgeneral-regs-only` (no `q`-register instructions in the objdump).
- [ ] `-smp` ≥ 4 preserved; SMP/IPC still pass (`make test`).
- [ ] DMA/queue buffers still contiguous, page-aligned, Normal-NC.
- [ ] x86_64 parity: no hardcoded-IP shortcut; `#ifdef __x86_64__` guards intact.
- [ ] Any trap/hang was root-caused against rules 1–6, not worked around.
