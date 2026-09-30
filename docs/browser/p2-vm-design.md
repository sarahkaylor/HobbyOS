# P2 design note — VM & memory overhaul (P2.1–P2.6: sparse address spaces, demand-zero, mmap v2, memfd, loader v2)

Status: **design for integrator review; no code in this commit.** Milestone: browser.md §6 P2 (lane L2;
base main@77b872e). Consumers: the P2 implementer (kernel/L2), L3 (`src/libc/src/mman.c`, `pthread.c`,
crt0/linker.ld), L9 (tests), and P4/P5/P7 which build on the map/fd surface. Grounded in `process.c`
(user-region block allocator, zombie reclamation, switch/fork/teardown, sys_brk/sys_mmap), 
`program_loader.c`, `arch/{arm,x64}/{mmu.c,trap.c}`, `src/include/process.h`, `syscall.h`, `errno.h`,
`fs.h` — plus two runtime probes against the loaded images (2026-09-30): an ARM QEMU boot with QMP
inspection (kernel runtime load base, TTBR0 walk, identity maps) and the built ELFs (`readelf`).

## 0. Constraints carried into the design
- Kernel C stays `-mgeneral-regs-only`, no unaligned access, byte-wise binary parsing; all new VM code
  is kernel C (bitmap/word ops only). User words are alignment-checked as today; the range check changes
  to an address-space lookup (§4.1), the check itself never relaxes.
- SMP ≥ 4 never reduced; `MAX_PROCESSES` stays 64 (`pid` == slot index); all AS metadata lives under the
  existing `proc_lock`; no new blocking primitives in the fault path.
- x64 rules stand: never preempt a user process in kernel mode; kernel tasks own their stacks; syscall
  dispatch stays an if/else chain (no absolute function-pointer tables — the kernel is loaded at an
  arbitrary base, §1.6).
- Memory stays **Normal-NonCacheable** (ARM MAIR idx 1) / WB (x64) for all user pages — the NC choice is
  the DMA-coherence strategy; do not add cacheable mappings.
- "Slow-but-working is success; broken-but-fast is not" (§8.3). The 1 MiB `MAX_PROGRAM_SIZE` is a
  *constraint to remove only as far as budgeted* — v2 raises the loader cap, the read-path loader
  stays the mechanism (§6 P2.5, §3.3).

## 1. Address-space model v2 (P2.1)

### 1.1 Today (one paragraph, verified)
One 32 MiB pre-mapped region per process at `USER_VIRT_BASE` 0x44000000, backed by one 32 MiB physical
block from `phys_blocks_used[]` (`proc_table[i].user_phys_base = PROC_PHYS_POOL_BASE + idx*32MiB`).
ARM: per-CPU `l1_table[cpu]` (TTBR0), user window = `l2_table_1[cpu][32..47]` rewritten on every
`mmu_switch_user_mapping(phys_base)` + full `tlbi vmalle1`; **no ASID**. x64: per-CPU `cpu_pd1[cpu]`
entries 32..47 (2 MiB huge pages) + full CR3 reload. The whole 32 MiB is mapped RWX/user (PXN=1 only),
so brk/mmap/munmap are pure bookkeeping (`sys_mmap` first-fit over `anon_maps[8]`; 8 map cap).
There is **no demand paging**: `process_create` zeroes 1 MiB + 256 KiB eagerly and the loader copies the
image into a pre-mapped block.

### 1.2 v2 VA layout (both arches, one shared base — decision)
```
USER_VA_BASE   0x1_0000_0000 (64 GiB)        32 GiB window per address space, sparse
  +0x0_0000_0000 .. +0x0_0400_0000   IMAGE     64 MiB   (loader v2 target; flat .bin, RWX)
  +0x0_4000_0000 .. +0x0_8000_0000   HEAP      1 GiB    (brk; RW, demand-zero)
  +0x0_8000_0000 .. +0x5_0000_0000   MMAP      18 GiB   (mmap arena, first-fit; per-region prot)
  +0x5_0000_0000 .. +0x6_0000_0000   TSTACKS   4 GiB    (per-thread stacks + guards)
  +0x7_FF80_0000 .. +0x8_0000_0000   MAIN STK  8 MiB    (demand reserve + 64 KiB guard below)
  (0x6..0x7_FF reserved; unlisted = unmapped hole → clean SEGV)
```
`USER_VA_BASE + 32 GiB = 0x1_8000_0000 (96 GiB)`. Every slot boundary is an unmapped hole; the region
allocator additionally keeps a ≥64 KiB raw gap between temporally distinct regions inside the arenas
(guards are a allocator invariant, not a line item). Why 64 GiB: it must clear **all RAM identity VAs
on both arches** (ARM identity maps cover 0x40000000–0x240000000 = 9 GiB; x64 covers 0–8 GiB) and leave
room under the ARM 39-bit VA ceiling (T0SZ=25 → 512 GiB max). Runtime probe: the ARM kernel is loaded by
Limine near RAM top (observed load base **0x23A680000**, bss→0x23EF3BC10, stacks→0x23EFBBC10 with
`-m 8192M`) and accesses its own memory at load-base-relative VAs; the old 0x44000000 window only
missed kernel data by luck of that base. v2 makes the separation structural: **user VAs never overlap
the RAM identity range on either arch** (§9 risk 3).
- Main stack: sp = window top (ARM) / top−8 (x64, ABI-shaped, unchanged convention from
  `program_loader.c`); demand-committed up to 8 MiB, 64 KiB guard below the reserve, beyond → SIGSEGV.
- Per-group, not per-thread: threads share the AS (§6). fork clones it (§6.4).

### 1.3 Region record & address-space object
```c
struct vm_region {            /* sorted by base; per AS, under proc_lock      */
  uint64_t base, len;         /* 4 KiB-aligned VA span                        */
  uint16_t prot;              /* PROT_READ|WRITE|EXEC|NONE                    */
  uint16_t kind;              /* VMK_IMG/HEAP/ANON/SHARED/STACK/FB           */
  uint32_t flags;             /* persistent MAP_* flags                       */
  struct vm_object *obj;      /* memfd / fb backing, else NULL                */
  uint64_t obj_off;           /* offset into obj                              */
};
struct addr_space {           /* one per group (leader PCB owned)             */
  int      ver;               /* AS_V1 (legacy block) | AS_V2                 */
  uint64_t root_phys;         /* ARM L1 frame / x64 PML4 frame                */
  uint16_t asid;              /* ARM: 1..63 == pid; unused on x64             */
  struct vm_region *regions;  /* frame-backed array; cap 16 → ×2 → cap 1024   */
  int      nr, cap;
  /* accounting: resident frames, peak frames, table frames — for sysinfo §4.5 */
};
```
New fields appended to `struct process` (additive merge, same rule as P1): `struct addr_space *as;`.
For `AS_V1` processes `as == NULL` and `user_phys_base`/`phys_block_idx` remain the truth (kernel tasks,
`load_and_run_program`, fork-of-v1). The map/unmap/mprotect syscalls and the fault path are v2-only;
v1 processes keep today's exact semantics so the wave stays green mid-migration (§8).
The old `anon_maps[8]`/`USER_ANON_MAX_REGS` table is superseded (v2); keep the fields until the flip.

### 1.4 ARM: per-AS TTBR0 + ASID (decision)
- **One L1 root frame (4 KiB) per AS**, allocated from the frame allocator. The root is a *copy of the
  kernel entries* (L1[0..8]: device 0–1 GiB + RAM identity 1–9 GiB, kernel attrs, nG=0 global) plus
  user entries: L1[64] (64 GiB) → per-AS L2 → per-AS L3, four KB pages at leaves.
- Leaf formats (4 KiB granule, T0SZ=25, 3 levels): L2 table desc `addr|0b11`; L3 **page** desc
  `addr[47:12] | AttrIdx=1 | AF=1 | AP[2:1] | UXN | PXN=1 | nG=1 | 0b11`.
  `AP[2:1]`: 01 = EL0/EL1 RW; 11 = EL0/EL1 RO; 00 = EL1-only (PROT_NONE is *not* installed, see §4.4).
  `UXN=0` for executable regions, `1` for heap/mmap/stack. AF=1 on every leaf (no AF-fault path).
- **ASID**: `TCR_EL1.AS=0` (8-bit); TTBR0[7:0] = group ASID (use pid, 1..63; 0 = kernel/idle). Kernel
  entries nG=0 → ASID flushes never touch them; user leaves nG=1. No full `tlbi vmalle1` on switch —
  the switch is `msr ttbr0_el1, root_phys|asid; isb`. ASIDs are recycled only after a broadcast
  `tlbi aside1is, asid`; freeing the root frame does the same (§5.3).
- Switch cost: today = 16 table writes + full TLB flush per context switch; v2 = 1 register write.
- Kernel direct map: the boot `l1_table[cpu]` tables remain for pre-scheduler boot and for v1
  processes; v2 roots *duplicate* the kernel half so both classes coexist (§8). The v1 overlay entries
  are global; mode changes (v1↔v2) therefore use a full `tlbi vmalle1` on the switching CPU (§7.1).

### 1.5 x64: per-AS PML4 + NX (decision)
- **One PML4 frame per AS**, plus one PDPT frame; PML4[0] → AS PDPT; AS PDPT[0..7] = the shared kernel
  PD pointers (PD0 device/RAM, PD1 RAM 1–2 GiB, PD2..PD7 RAM 2–8 GiB — all supervisor, non-PS where a
  PT level is needed is *not* needed for kernel: kernel stays huge pages). User window at 64 GiB:
  PDPT[64..] → per-AS PD copies → PTs → 4 KiB **PTE** leaves
  `phys | P | RW? | US | NX?` (bit 1 = writable, bit 63 = NX).
- Today's per-CPU `cpu_pd1[MAX_CPUS]` user overlay **disappears**; PD1 becomes a shared kernel
  identity table like PD0/PD2..7. Per-CPU PML4/PDPT arrays remain only for pre-scheduler boot.
- **EFER.NXE must be enabled** (bit 11) in `arch/x64/boot.s` (boot + AP paths) and idempotently in
  `mmu_init_core_with_id` — today only LME/SCE are set, so bit 63 is reserved and would #PF. Until
  then PROT_NONE/noexec work only via non-present leaves.
- Switch: `mov cr3, root_phys` — full flush per switch (no PCID in P2); unmap/mprotect use a
  shootdown IPI (§7.2).
- Recall the kernel itself sits at 0x70000000 (1.75 GiB, inside PD1) and the pool starts below it;
  the user window is 64 GiB — disjoint by construction.

### 1.6 Where tables live; kernel access
All table frames come from the frame allocator (identity: a frame's kernel VA == its physical address
on both arches, since both direct-map all pool RAM supervisor-side; ARM via L1[1..8], x64 via
PD0/PD1..PD7). Roots and tables are therefore writable by the kernel with ordinary stores; the MMU
walks them by physical address. Table frames are marked used in the frame bitmap and are freed by the
AS teardown walk (§5.3). No static per-process table arrays: the 64-group ceiling does not become 64×4
KiB of spare tables, and `user_l2_table` (a vestigial field) is deleted in the implementation.

## 2. Physical memory: allocator v2 + migration (P2.2)

### 2.1 Frame allocator (decision: bitmap, 4 KiB, same pool extents)
- **Granularity**: 4 KiB frames (matches the page-table leaf size; 2 MiB would defeat demand-zero).
- **Structure**: one bitmap over the pool, `uint64_t frame_bits[NUM_FRAMES/64]` + a rotating alloc hint;
  alloc = find-first-zero word scan from hint (amortized O(1)); free = clear bits. Bitmap, not
  free-list: metadata is 232 KiB ARM / 40–172 KiB x64 (vs ~15 MB for 8-byte free-list nodes over 1.9M
  frames), zero per-frame allocation cost, trivially unit-testable. Fixed-size static (bss), no growth
  bookkeeping of its own.
- **Pool v2** (same physical extents as today, plus one documented extension):
  - ARM: `[0x70000000, 0x240000000)` — 1,900,544 frames, 232 blocks worth. (Already RAM-top; no
    growth needed. The `[0x40000000,0x44000000)` low RAM is left alone: firmware/DTB residue and the
    v1 overlay hole are not worth the risk.)
  - x64: `[0x20000000, 0x70000000)` = 327,680 frames **plus growth** `[0x80000000, 0x180000000)`
    = 1,048,576 frames. The extension is already identity-mapped (PD2..PD5) and currently unused —
    this is the P2.2 "growth" item made concrete (total x64: 1,376,256 frames = 5.25 GiB).
  - Both tops are **VMM constants validated by a boot log line** (`frames=… total=…`); QEMU RAM is
    fixed per target (ARM 8192M, x64 6144M) per the Makefile. No DTB/multiboot parsing in P2 (recorded
    as a follow-up in §11 OQ-level; a RAM-size probe is a risk if the runner's -m changes).
- **Accounting** (P2.2): `frames_total/used/free`, per-AS resident peak, table frames; end-of-wave
  numbers printed as part of evidence (§9). `sysinfo(2)` (memory usage) reports from these counters
  (§4.5).

### 2.2 The 32 MiB block layer survives the transition (decision: thin layer on the bitmap)
Keep `phys_block_free_count()` and block semantics for `AS_V1` (kernel tasks, `load_and_run_program`)
and for the v1-vs-v2 overlap window, implemented on top of the same bitmap:
- block alloc = 8192 contiguous, 8192-aligned frames (32 MiB); block free = clear its bits.
- `phys_blocks_used[]` is deleted as the authority; `phys_block_free_count()` = count of free aligned
  runs (bounded scan over `NUM_PHYS_BLOCKS` = 232/40 iterations — same cost class as today).
- v2 processes never allocate blocks: `process_create_internal` (v2 path) allocates the AS (root +
  region array), no 32 MiB block, and drops the 1 MiB + 256 KiB eager zeroing (demand-zero replaces it).
- **WAVE_LOAD_RESERVE** (loader headroom, `program_loader.c`): generalizes to **frames**
  (`WAVE_FRAME_RESERVE` = 6 blocks worth = 49,152 frames ≈ 192 MiB) checked while any v1 class is
  still possible; at the flip (§8) the check becomes `free_frames > reserve` with the same rationale
  (children must always find memory), numeric value revisited with the flip evidence.

### 2.3 Zombie reclamation interaction
Today: a zombie (EXITED, unreapable parent) is reclaimed by `phys_block_alloc_locked` /
`process_create_internal` slot scans, which free its block when no CPU claims it
(`process_still_running`). v2 mirrors that exactly at the frame level: the frame allocator's
low-memory path runs the same zombie scan and frees a reclaimable zombie's **AS frames + tables**
(never a live-parent zombie's — the parent may still `waitpid`; the program image itself is not needed
for that). `AS_V1` zombies keep today's block behavior. Freeing stays *lazy* (allocator pressure or
slot reuse), not at exit — preserving the existing zombie/waitpid contract exactly; an eager
exit-time release is a later tweak, not P2. The v2 claim-drain is identical (one teardown point,
`group_teardown`, §6).

## 3. Demand paging scope (P2.2)

- **In**: anonymous zero-fill demand pages for HEAP, MMAP(anon private or shared), STACKS (main +
  thread) regions; MEMFD-backed pages materialize on first touch from the object (§4.3). A fault maps
  exactly one 4 KiB frame, zeroed before anything can read it; mappings are per-region prot.
- **Committed on load**: the IMAGE region (loader v2 reads the file into fresh frames — the read path
  is the mechanism, §6). No file-backed demand: **deferred** (AD-10 keeps it a stretch; §10 Q5's
  load-time measurement decides if it ever becomes needed).
- **Swap: explicitly out of scope for P2 and not designed here.** No page eviction, no dirty tracking,
  no backing store. Overcommit = commit-on-touch against free frames; a fault with zero free frames
  (after the reclaim scan) **kills the faulting process** with a precise `ENOMEM`-class report
  (§5.4); the kernel stays alive. (This is the honest v1 policy for a no-swap OS; revisit only with a
  concrete WebKit memory-pressure requirement.)
- `MAP_NORESERVE` is accepted and advisory (no commit accounting exists to disable).

## 4. mmap family v2 & syscall surface (§A.1b amendment; P2.3/P2.4)

### 4.1 Semantics (v2 AS only)
- **Range checks**: every syscall argument that is a user pointer goes through the AS walk
  (`vm_touch(grp, uaddr, len, write)`): region lookup (sorted binary search + one-entry cache) →
  demand-materialize pages per region kind/prot → return OK or `-EFAULT`. **Kernel code never faults
  on a user VA**: after `vm_touch` succeeds the live mapping is present and the kernel reads/writes
  directly (same class as today's range-checked accesses; the check is where the old
  `USER_VIRT_BASE..+32MiB` compare was). Alignment checks stay per-syscall. Audit: ~110
  `USER_VIRT_BASE` sites in the two `trap.c` files + 11 `process.c` + 10 `program_loader.c` +
  `u_strcpy` helpers — all become `vm_touch` + direct access (or are deleted with the v1 path).
- **mmap(addr, len, prot, flags, fd, offset)** — 6-arg (Linux-shaped), see §4.2:
  - `MAP_PRIVATE|MAP_ANONYMOUS` (default for fd==-1): carve VA from the MMAP arena (first-fit bottom-up
    from `USER_MMAP_BASE`; the `addr` hint is honored when free and inside the arena; `MAP_FIXED`
    replaces any existing mappings in range, page-aligned, else `-EINVAL`), prot per argv.
  - `MAP_SHARED|MAP_ANONYMOUS`: same carve, backed by an internal anonymous `vm_object`; visible to
    forked children, unreachable from other processes (no fd) — Linux-compatible enough for the tests.
  - `fd >= 0`: **memfd only in P2** (`-ENOTSUP` for FAT16/NFS/socket — file-backed waits for P6);
    `offset` must be 4 KiB-aligned; mapping prot may not exceed the memfd's seals.
  - `len == 0` → `-EINVAL`; arena exhausted or region cap hit → `-ENOMEM`.
- **munmap(addr, len)**: full or partial (region split); resident private pages freed; shared pages
  just dropped (object keeps them). Unaligned/outside → `-EINVAL`. Idempotent per-region removal keeps
  the sorted array (merge adjacent same-kind regions).
- **mprotect(addr, len, prot)**: range must be fully mapped (else `-ENOMEM`, Linux value); updates every
  region in the span, re-encodes PTEs of **resident** pages, and zaps them for `PROT_NONE` (§4.4).
  `PROT_EXEC` is honored as noexec removal (UXN / NX clear); read-only text still needs a future ELF
  loader to be *set* (flat .bin images load RWX, documented, §3).
- **madvise(addr, len, advice)**: advisory. `MADV_DONTNEED` (4) and `MADV_FREE` (8) zap resident
  private pages (zero again on next touch — MADV_FREE treated eagerly, safe); everything else returns
  0 (ignored). Invalid range → `-EINVAL`.
- **brk(addr)** (61): unchanged shape; range now `[USER_HEAP_BASE, USER_HEAP_TOP)`; shrink does not
  unmap (Linux-like; `madvise(DONTNEED)` is the reclaim tool).
- **mremap**: **not implemented in P2** (§6 P2.3 "verify need" → not needed while `USE_SYSTEM_MALLOC`
  is the allocator and no libpas; if P7 flips to bmalloc/libpas, append it later — OQ1).

### 4.2 Rows (§A.1b amendment proposal — integrator consent via OQ1)
```
SYS_MEMFD_CREATE 80  (name*, flags)                          -> fd | -errno          [P2, freeze]
SYS_MPROTECT     81  (addr, len, prot)                       -> 0 | -errno           [P2, freeze]
SYS_MADVISE      82  (addr, len, advice)                     -> 0 | -errno (advisory)[P2, freeze]
SYS_MMAP         62  (addr, len, prot, flags, fd, offset)    -> va | -errno  [semantic extension, 6-arg]
SYS_MUNMAP       63  (addr, len)                             -> 0 | -errno   [extended: partial unmap]
SYS_FTRUNCATE    33  (fd, size)                              -> 0 | -errno   [implemented for memfd]
SYS_BRK          61  (addr)                                  -> 0/brk | -errno       [unchanged shape]
```
- 80/81/82 are exactly the §A.1b provisional rows already tagged `[P2]`; **no renumber is needed** —
  P2 freezes them in place, `SYS_MAX` 75 → 82 at the P2 gate (single-writer header edit by the
  integrator, same protocol as the P1 gate). Rows 76–79 (P4) stay provisional gaps.
- `SYS_MMAP` keeps row 62 per §A.1b's explicit note ("semantic extensions … rather than new numbers —
  record exact ABI at P2"); the exact ABI is 6 args: **ARM x0..x5 = `regs[0..5]`; x64
  rdi/rsi/rdx/r10/r8/r9 = `regs[5]/[4]/[3]/[9]/[7]/[8]`** (r9 is already saved at `[rsp+64]` in the
  trap wrapper, so no assembly change; the user-side 4-arg/5-arg helpers (`hb_syscall4` in `mman.c`,
  `hb_syscall5` in `stat.c`) already match this mapping, so only a 6-arg variant is new — arg6 =
  x5 / r9 — and existing callers are untouched). This is an ABI change to a non-frozen row — consent
  requested in OQ1.
- Dispatch: one `else-if` per syscall added to BOTH `arch/{arm,x64}/trap.c` (never a table; kernel
  load-base rule). `errno`: `-EINVAL`, `-ENOMEM`, `-EFAULT`, `-EBADF`, `-ENOTSUP`, `-EPERM` for seal
  violations — all exist in errno.h.
- libc wrappers: `src/libc/src/mman.c` gains `mprotect/madvise` + real `mmap` fd/offset path;
  `memfd_create` wrapper; `pthread.c` stack allocation moves to mmap+guard (§6.2); `ftruncate` wrapper
  already exists.

### 4.3 memfd object (P2.4)
`SYS_MEMFD_CREATE(name*, flags)` → fd in the existing global file table (new `FILE_TYPE_MEMFD`; the
fd's `file` carries the object pointer). `name` accepted for compat (not visible in the FAT namespace;
recorded for diagnostics). `MFD_CLOEXEC` (0x1) recorded (no exec until P5); `MFD_ALLOW_SEALING` (0x2)
recorded. The object is a frame array (sparse; frames materialize on first touch of any mapping or on
`ftruncate`-grow? — decision: materialize on touch, size tracked separately), refcounted by fd +
mappings; freed at last ref. `ftruncate(fd, size)` sets the size for memfd (grow/shrink; shrink frees
frames above size unless `F_SEAL_SHRINK`). Sealing: `fcntl(F_ADD_SEALS/F_GET_SEALS)` recorded and
enforced for SHRINK/GROW at ftruncate time; `F_SEAL_WRITE` recorded but advisory (a live writable
mapping makes true enforcement racy) — verify WTF's `SharedMemory::create` needs against the host
copy before implementing (OQ7). Two-process test: child inherits the fd via fork, both map, writes are
mutually visible (P2.4's acceptance; §10).

### 4.4 PROT_NONE (decision, divergence recorded)
`mprotect(PROT_NONE)` **zaps resident pages and frees their frames**; re-protecting re-commits as
zero-fill. Divergence from Linux (which keeps pages) — accepted because the dominant use is
reserve-then-commit of never-written memory (JSC/allocators), and keeping frames would need a
non-resident-but-attached page list. MMTEST pins the divergence explicitly; OQ4 records it for the
integrator, with a "keep-pages" variant as the fallback if a ported component is found to depend on
data surviving PROT_NONE. `RW→RO`/`RO→RW` changes PTE prot only (resident pages preserved) + §7 flush.

### 4.5 Accounting surface
`sysinfo(2)` (`struct sys_meminfo`) starts reporting **frame-based** totals
(`total_bytes = frames_total*4096`, `free_bytes = frames_free*4096`); the struct is *appended* with
`used_frames/high_water_frames` following the `sys_netinfo.dns` size-gated pattern so `free.bin` and
old callers keep working (OQ8).

## 5. Fault paths

### 5.1 ARM: EL0 synchronous exceptions (existing `sync_lower_handler_c`)
EC=0x24 (data abort) / 0x20 (instruction abort) / 0x00 unknown currently print `ELR` and
`process_exit(tf)` — no FAR read, no classification. v2 replaces that with:
1. `far_el1` = fault VA; ESR_EL1.ISS: WnR bit 6 (data aborts; 0 for instruction aborts), DFSC
   [5:0]: translation faults (0b0001xx) → demand; permission faults (0b0011xx) → prot kill; other
   (alignment 0b100001, external, etc.) → kill with the specific DFSC in the report.
2. `vm_handle_fault(grp, va, write, exec, pc)` (§5.5) under `proc_lock`.
3. On success: nothing else (the mapping is live; `eret` re-executes the faulting instruction — ELR
   untouched).
4. On failure: kill path (§5.4) — the offending process exits; other processes unaffected (P2.1's
   "clean segmentation errors").
EC=0x15 (SVC/syscall) dispatch is untouched; EC not listed here stays the existing fatal path.

### 5.2 x64: #PF (vector 14) and friends
`vector < 32` currently prints and exits for ring-3. v2 special-cases:
- **#PF (14)**: read `cr2` (`movq %cr2` in a tiny arch helper) = fault VA; `tf->error_code` bits:
  P(0)=present — only `P=0` is demand-paged; `W(1)` write; `U(2)` (only handle U=1); `I/D(4)`.
  `P=1` = protection → kill (prot report). `RSVD` set → kill + report (a kernel table bug).
- Other exceptions (13 #GP, 12 #SS, 6 #UD, 0 #DE): unchanged kill path, but the report gains the
  current AS info (VA unavailable — no CR2; print RIP/error code; keep it raw-sink style to avoid
  recursing).
- Alloc+map happen on the current CPU's CR3 context (the process is current); the shootdown rule
  (§7.2) covers sibling threads on other CPUs.

### 5.3 Growth rules, guards, teardown
- **Stack growth**: main stack and thread stacks are demand regions with a fixed reserve (8 MiB main,
  per-thread size + 4 KiB guard for pthread stacks). A fault below the current committed area but
  inside the reserve commits (grows); the guard page and everything below the reserve is a hole →
  clean kill. No auto-grow beyond the reserve (deterministic, no OOM-vs-growth surprises).
- **Guard pages** (closes P1 OQ6): the 64 KiB/4 KiB unmapped gaps are real holes; overflow faults
  with `in=HOLE` and kills the process with a SIGSEGV-status exit.
- **munmap/teardown**: `vm_as_teardown` walks regions → frees private leaf frames (kind != SHARED/FB)
  → walks root → frees L2/L3 (ARM) / PDPT/PD/PT (x64) frames → frees the root. It runs at the same
  lifecycle point the v1 block is freed (claim-drained group teardown / zombie reclaim, §2.3), under
  `proc_lock`; the per-AS frame count is asserted back to zero in a kernel unit test (§10).
- **fork clone**: resident private pages are copied eagerly (frame-by-frame; holes stay holes) into a
  fresh AS; memfd/shared mappings are re-established against the same objects (like today's fd
  inheritance). COW is explicitly **deferred** (§6.4).

### 5.4 Fault reporting (P2.1's "clean segmentation errors")
One line, both arches, bounded (no recursive faults — print via the existing raw-ish path):
`[KERNEL] pid=N (NAME) memory fault VA=0x… PC=0x… rw=x in=HOLE|PROT prot=…  -> killed`
Exit status recorded in the existing waitpid layout (signal byte = 11 SIGSEGV) so P5 can turn it into
real signals; P2's contract is: **precise message + only the faulting process dies**. OOM during
demand is the same shape with `in=OOM` (maps 1:1 to ENOMEM at P5).

### 5.5 Fault/schedule integration
- `vm_handle_fault` never yields and never blocks: table/frame allocation is bitmap-scan
  (non-blocking) under `proc_lock → frame_lock` ordering (same order as every other VM writer).
- No per-thread kernel stacks are needed for this: the handler runs entirely on kernel memory
  (identical to today's EL0 handler) and `vm_touch` is the only user-page toucher (§4.1).
- The handler uses `current_process()` + `process_group()` (P1) and only the *group's* AS.
- A fault on a v1 process keeps today's print+exit path verbatim.
- Timer/IRQ preemption rules unchanged; the fault path is not preemptible (runs with IRQs as the trap
  delivered them) and does not re-enter `schedule()`.

## 6. P1 threads interaction (must stay correct)
1. **Threads share the AS** (P1): the AS hangs off the group anchor; `vm_touch`/faults/region updates
   all route through `process_group()`. Sibling threads on multiple CPUs can fault concurrently →
   region lookups under `proc_lock`; duplicate table fills for the same page are benign (both allocate
   a zero frame and install it; loser's frame is freed — resolve by re-checking the leaf under
   `frame_lock` and freeing the loser; single-page writes are atomic 64-bit stores).
2. **Thread user stacks**: libpthread's `pthread_create` switches from `malloc` heap blocks (P1) to
   `mmap(STACK_SIZE+4 KiB) + mprotect(guard, PROT_NONE)`; guard page overflow kills the process with a
   clean report (P1 OQ6 close, OQ6 here is the L3 diff-request consent). The `pthread_t` TCB/TLS
   layout is unchanged (TLS register untouched, §4 of the P1 note); `PTHREAD_STACK_MIN` etc. keep
   their existing defaults.
3. **Per-thread kernel stacks: still none** — no P2 requirement forces them (faults and syscalls run
   on kernel memory only). Documented as a non-goal; revisit only with a concrete need (e.g. if a
   future preemptible-kernel work item appears).
4. **fork + threads (COW decision)**: **COW is explicitly deferred to a later gate** — §6 P2 does not
   list it, WK's process launches are fork→exec (cold AS), and §8.2's inventory has no COW case. P2's
   v2 fork copies *resident* pages (bounded by what the process actually touched, not the sparse
   window); the child is a single-threaded copy of the CALLER (P1 OQ5 unchanged). Revisit when P5
   lands real execve/waitpid or if WK-2 measurements show fork cost dominating process launch (OQ3).
5. **Kernel tasks** keep v1 blocks as their stack region (unchanged, §8); `process_create_kernel*`
   never touches v2.

## 7. TLB / ASID / shootdown policy; framebuffer & pipes

### 7.1 ARM
- Switch v2→v2: TTBR0 = root|asid, `isb` only (no flush). v1→v2 / v2→v1 / v1→v1: today's path plus a
  full `tlbi vmalle1` on mode changes (v1 overlay leaves are global — §1.4).
- Map (fault or vm_touch): after installing the leaf, `tlbi vae1is, va|(asid<<48)` — broadcast
  (inner-shareable) so sibling threads on any CPU re-walk; one instruction, no IPI on ARM.
- Unmap/mprotect/PROT_NONE-zap: same broadcast by-VA form per touched range.
- ASID free/reuse: `tlbi aside1is, asid`. ASID table: 1..63 = pid, allocated at AS create, released at
  teardown. ASID rollover cannot occur (≤63 concurrent groups).

### 7.2 x64
- Switch always reloads CR3 (full flush). For unmap/mprotect/sibling-visible changes: a **shootdown
  IPI** (new vector, propose 0x82 next to the 0x81 yield IPI) whose handler does `if (current group's
  AS == target) reload CR3` (coarse but correct; page-granular `invlpg` is a later optimization).
  Send only when the AS is claimed on another CPU (scan `cpu_current_pids` for the tgid — 8 entries,
  no new state).
- Map: `invlpg` for the page on the faulting CPU; other CPUs get correctness from the CR3 reload at
  their next switch + the AS-claim rule (an AS runs on one CPU per thread-claim; sibling threads are
  covered by the shootdown rule above when they could have cached the negative entry — map also
  triggers a shootdown if the AS is claimed elsewhere; cheap, rare).

### 7.3 Framebuffer & pipes (today's shared-mapping reality)
- `SYS_MAP_FB`/`SYS_FLUSH_FB` keep their ABI; the mapping moves into the v2 layout: fb mapped at a
  fixed VA slot in the process window (proposal: `USER_VA_BASE + 0x6FFF000000`-class slot — pick a
  reserved 4 MiB slot inside the reserved 0x6–0x7.FF GiB band, documented in vm.h) **shared by every
  process** (same physical framebuffer, `VMK_FB` region kind, writable, noexec). It is the first
  `MAP_SHARED` citizen and MMTEST checks two processes see each other's pixels through it.
- The fb physical address is the kernel's `framebuffer` static (identity-mapped, §1.6); kernel keeps
  flushing unchanged. Pipes remain byte-stream fds — no VM interplay (VA space untouched; fd path
  unchanged).
- `mmu_map_user_framebuffer` is replaced by an AS-window mapping of the same frame(s) (2 MiB region
  per current shape, prot RW).

## 8. Migration & staging (R4 mitigation; "keep the block allocator alive during transition")
Staged delivery, each stage green on all tiers before the next:
1. **S1 — frames**: frame allocator + accounting + block layer re-implemented on the bitmap; v1
   behavior bit-identical (unit tests only; no process changes). Evidence: unit tiers + wave.
2. **S2 — v2 AS plumbing**: AS objects, per-arch roots/tables, v2 switch, loader v2 for an *opt-in*
   class only (`AS_V2` explicitly selected by the new `MMTEST` wiring; everything else v1). Fault path
   still kills (no demand). Evidence: unit tests for map/switch/teardown + MMTEST basic.
3. **S3 — demand + mmap v2**: zero-fill faults, region mprotect/madvise/munmap, guard/segv rules,
   `vm_touch` audit across both trap.c files. Evidence: MMTEST full + host/unit + wave (v1 default).
4. **S4 — shared**: memfd + MAP_SHARED + fb slot + pthread guard stacks (L3 diff request). Evidence:
   MMTEST shm cases + THRD_T unchanged green.
5. **S5 — flip + sweep**: scheduler loader switches ALL user programs to v2 (kernel tasks stay v1);
   full wave ARM + x64 + boot/memory deltas + QMP E2E; loader-v2 30–60 MiB load-time measurement
   (§6 P2.5) recorded. This is the gate commit set: P2.6's sweep + §11 fix log.
The gate: "memory stress suite (map/fault/free loops, shared-page test, two-process shm test, existing
suites) green both arches; boot delta within recorded budget" (browser.md §6 P2).
Rollback lever: `AS_V2` selection is a one-line default in the loader — flipping back restores the
pre-P2 behavior through S5 with the bitmap underneath.

## 9. Performance budget & measurement (no boot-time regression)
- **Method** (Gate F1 method, §11 record): unit-tier wall clock, warm dirs, touch-refreshed, 3 runs,
  median; plus full-wave wall (run_waves.sh) and the end-of-wave frame accounting line. Baselines to
  beat: Gate P1 unit-tier ARM 8.98 s / x64 ~1.3 s; wave ARM ~85 s.
- **Budget (proposed gate numbers)**: unit-tier boot delta ≤ +0.5 s per arch; wave wall ≤ +15 %
  (≈ +13 s ARM); no new FAIL tokens; frames-free at wave end recorded (expected ≥ ~1.7 GiB ARM).
- Expected wins to offset fault costs: switch no longer rewrites 16 entries + full-flushes (ARM ASID
  flushes are per-VA/per-ASID); create no longer zeroes 1.25 MiB per process; images touch only what
  they use. Costs: first-touch fault per 4 KiB page (~1 extra trap+alloc per page; TCG makes traps
  dear — measure, and if the wave delta exceeds budget, the knob is per-region 2 MiB *block* remaps
  for big IMAGE/HEAP commits (noted as a pre-approved fallback lever, not designed here)).

## 10. Test plan
Kernel units (new `src/kernel/vm_test.c` + additions in `process_test.c`/`mmu_test.c`, registered in
`unit_test.c`, `KERNEL_MODE_UNIT_TEST`, both arches):
1. Frame allocator: alloc/free/reuse; hint behavior; contiguous block carve; counts; exhaustion path;
   zeroing guarantee for demand frames.
2. Region list: insert/lookup (binary search), hole find, split/merge, overlap rejection, cap growth.
3. Fault classifier (pure function, table-driven): VA in each kind × read/write/exec × prot → the
   expected action (demand / grow-stack / prot-kill / hole-kill / shared-fill).
4. Page-table walk: map→leaf present with expected attrs; unmap→absent; prot re-encode; both arches
   (mmu_test pattern; assert against the raw table words like the existing tests do).
5. memfd: create/ftruncate/seals; two fake groups map one object → same frame visible; refcount/teardown.
6. AS clone (fork): resident copied, holes stay holes; then teardown returns the frame count to baseline.
7. Teardown leak check: N create/teardown cycles leave `frames_used` unchanged.
Userland acceptance `src/user/mm_test.c` → **`MMTEST.BIN`** (8.3-safe; loaded LATE like THRD_T, 30 s
watchdog; both arches; each negative case in a FORKED CHILD so the parent proves isolation):
1. brk: grow 8 MiB, verify zero-fill, write pattern, shrink, re-grow.
2. anon mmap 64 MiB sparse: touch first/last page; verify zero; write/read pattern; munmap a middle
   span; child touches the hole → killed with the fault report; parent unaffected.
3. Guard: child overflows a pthread stack guard → killed; parent + siblings unaffected (P1 OQ6 close).
4. mprotect: RW→RO child write faults; PROT_NONE→RW recommit zero-fill divergence asserted.
5. memfd shm: ftruncate + MAP_SHARED in parent, child (fork-inherited fd) writes; mutual visibility both
   directions; munmap/fd-close teardown; second shm round via mmap after close.
6. fb sharing: two processes map fb; cross-visible pixels (piggybacks the GRAPHICS path if cheaper).
7. Fault-report format greps for the harness (VA/in=HOLE present).
Regression guards (green both arches before merge): `make host_tests` (F1 baseline 482/0),
`./run_unit_tests.sh` (53/0) + `./run_unit_tests_intel.sh` (55/0 KVM), full in-OS waves (ARM + x64,
existing suites + THRD_T/TLS_T untouched), QMP E2E (`run_xcalc_test.py`), boot delta (§9). Evidence per
§8.4: raw logs, zero FAIL tokens, boot-time delta recorded like Gate F1, frame accounting line.
Makefile/disk/wave diffs: the v2 lane applies (single writer this wave; integrator resolves merges).
L3 deps: `mman.c` wrappers, `pthread.c` stack change, crt0/linker.ld new base — diff requests to L3
owners (OQ6).

## 11. Decisions & open questions for integrator review

**Decisions**
- **D1** v2 layout: sparse 32 GiB window at `USER_VA_BASE 0x1_0000_0000` (64 GiB), fixed slots
  IMAGE/HEAP/MMAP/TSTACKS/MAIN-STK + structural guard gaps; same base both arches; chosen to clear all
  RAM identity VAs (esp. the ARM load-base-relocatable kernel, probe §1.2).
- **D2** Per-AS page tables: ARM per-group L1 root + L3 4 KiB leaves, 8-bit ASIDs (pid), broadcast
  TLBI, kernel entries global; x64 per-group PML4/PDPT + PT 4 KiB leaves, CR3 reload, EFER.NXE enabled,
  shootdown IPI for cross-CPU invalidations; no PCID in P2.
- **D3** 4 KiB frames, bitmap allocator over the existing pool extents (+ x64 growth 2–6 GiB); the
  32 MiB block layer survives as a thin aligned-run layer on the same bitmap; v1 blocks remain for
  kernel tasks/legacy through the transition.
- **D4** Dual-mode AS (`as_ver`): v1 paths frozen and bit-identical; v2 is opt-in during S2–S4, the
  loader default flips at S5; kernel tasks stay v1.
- **D5** Demand-zero in: heap/mmap/stacks/memfd; images committed by the read-path loader v2;
  file-backed deferred; **swap explicitly out of scope**; OOM kills the faulting process precisely.
- **D6** Faults: EL0/#PF classified against the region list; demand commit / clean SEGV kill with
  precise report (signal byte 11); EL1/kernel faults stay fatal; no blocking in the fault path.
- **D7** Syscalls: MEMFD_CREATE 80 / MPROTECT 81 / MADVISE 82 frozen in place (no renumber);
  SYS_MMAP 62 = 6-arg extension (x0..x5 / rdi..r9-as-mapped); partial munmap; ftruncate for memfd;
  mremap deferred.
- **D8** PROT_NONE zaps+frees frames (documented divergence); madvise DONTNEED/FREE zap; the rest of
  madvise returns 0.
- **D9** Stacks: main 8 MiB demand reserve + 64 KiB guard; pthread stacks → mmap+4 KiB guard (P1 OQ6
  closed); no per-thread kernel stacks still.
- **D10** COW fork deferred; v2 fork copies resident pages; child = single-threaded copy of caller
  (P1 OQ5 stands).
- **D11** fb: shared window mapping via SYS_MAP_FB (same ABI); pipes untouched.
- **D12** Measurement: Gate F1 method; budgets per §9; accounting line per §2.1.

**Open questions**
- **OQ1** §A.1b: consent to (a) freeze 80/81/82 in place + `SYS_MAX 82` at the P2 gate, and (b) the
  6-arg `SYS_MMAP` ABI extension on row 62 (non-frozen row; x4/x5 + r8/r9 trap-register mapping;
  user-side 6-arg `syscall()` variant added). Fallback if (b) is declined: keep 4-arg mmap and add
  `SYS_MMAP2` — not recommended (extra row + libc fork). Also confirms mremap stays unnumbered
  (append-only if a later gate needs it).
- **OQ2** The user VA base move (0x44000000 → 64 GiB) is cross-cutting: `src/user/linker.ld`, 
  `src/user/malloc.c`'s HOST_TEST constant, `mem_test.c`'s literal, `graphics`/desktop assumptions,
  and every hardcoded 0x44… in tests/logs. Confirm the single-new-base plan (vs keeping a legacy com
  pat window at 0x44000000 for v1 processes — rejected as a permanent second layout, but it is a
  possible S2/S3 crutch).
- **OQ3** COW deferral (D10): accept resident-copy fork for P2, or require COW now for WK process
  launches (fork→exec)? Cost estimate if required: +write-fault path + refcounts on frames (≈ the P2
  critical path grows; we recommend deferring with a measurement at WK-2).
- **OQ4** PROT_NONE divergence (D8): accept zap+free (documented), or require keep-pages semantics
  (adds a non-resident page list per region).
- **OQ5** x64 shootdown IPI: confirm vector allocation (propose 0x82; 0x80/0x81 in use) and the
  CR3-reload-handler approach; note the x64 wave's known fragility classes (§11 record) — the IPI must
  join the existing guards (no preemption of kernel-mode windows, etc.).
- **OQ6** L3 diff requests: `pthread.c` stack mmap+guard and `mman.c`/sysroot wrapper updates; who
  applies (P1 precedent: lane applies, integrator resolves) — plus crt0/linker.ld base change review.
- **OQ7** memfd sealing scope: verify WTF `SharedMemory::create` needs against the host copy; keep the
  minimal seals set (SHRINK/GROW enforced, WRITE advisory) or drop seals to flags-only if unused.
- **OQ8** sysinfo(2) extension (frame-based totals + appended fields, size-gated): confirm `free.bin`
  /`ps.bin` consumers accept, and whether the wave's gate log should include the accounting line
  (we propose yes, as P2.2 evidence).

## 12. Integrator review — resolutions (2026-09-30; binding for the P2 implementation lane)

Reviewed against the merged tree and the P1-precedent protocol.

- **OQ1 — CONSENTED.** (a) MEMFD_CREATE 80 / MPROTECT 81 / MADVISE 82 freeze in place at the P2
  gate; the `SYS_MAX` 75 → 82 edit is a gate-time single-writer change by the integrator. (b) The
  6-arg `SYS_MMAP` extension on row 62 is approved — Linux-shaped (ARM x0..x5; x64
  rdi/rsi/rdx/r10/r8/r9 = `regs[5]/[4]/[3]/[9]/[7]/[8]`; r9 already in the trap frame, no asm
  change). The `SYS_MMAP2` fallback is declined. mremap stays unnumbered (append-only if a later
  gate needs it). The lane may arm 80–82 + the 6-arg path in its branch exactly as P1 did; the
  freeze record lands at the gate.
- **OQ2 — APPROVED, single new base.** v2 processes live at `USER_VA_BASE` (64 GiB) only; no
  permanent 0x44000000 compat window and no S2/S3 crutch window — v1 keeps today's layout
  bit-identical through S4, so the two modes coexist without a shared second layout. The S5 flip
  commit set must include the full hardcoded-0x44… sweep (linker.ld, crt0, malloc.c host constant,
  mem_test literals, graphics/desktop) with a grep-clean proof in the evidence.
- **OQ3 — ACCEPTED (COW deferred).** Resident-copy fork is the P2 contract (bounded by touched
  pages; holes stay holes). Revisit only at the WK-2 launch-cost measurement; P5 execve/waitpid
  does not by itself force COW.
- **OQ4 — ACCEPTED (zap+free).** PROT_NONE zaps resident pages and frees frames; re-protect
  re-commits zero-fill. MMTEST pins the divergence as a named check. Fallback trigger recorded: if
  any ported component (JSC/GLib) is found to rely on data surviving a PROT_NONE round-trip,
  revisit with the keep-pages variant.
- **OQ5 — APPROVED.** Shootdown IPI vector 0x82 (next to 0x81). Binding: the handler never
  schedules, never preempts kernel-mode windows, reads only snapshots; the send path fires only
  when the target AS is claimed on another CPU (`cpu_current_pids` scan). MMTEST's cross-process
  shm cases must exercise it on x64 and report the counters; any new x64 wave class is a stop
  signal (standing classes recorded in browser.md §11).
- **OQ6 — APPROVED (P1 precedent).** The P2 lane applies the L3-file diffs it needs (pthread.c
  stack mmap+guard at S4; mman.c/sysroot wrappers; crt0/linker.ld base at S5); integrator resolves
  merges. Keep each staged file-set inside its stage's commit set (§8).
- **OQ7 — APPROVED (minimal seals).** SHRINK/GROW enforced at ftruncate; WRITE recorded advisory.
  Implementation-time check: grep the WebKit host copy (`~/webkit-hobbyos`, WTF SharedMemory) for
  F_*SEAL usage and record the result here; if unused, flags-only is acceptable — keep the
  recorded stance until that evidence exists.
- **OQ8 — APPROVED.** sysinfo(2) frame-based totals + appended size-gated fields; verify
  free.bin/ps.bin host-test mocks still pass; include the end-of-wave frame-accounting line in the
  gate log (P2.2 evidence).

Cross-cutting notes: (a) budgets (§9) are accepted as gate numbers; if S5 measurements exceed the
unit-tier budget while suites stay green, the budget is renegotiated with evidence at the gate —
never silently relaxed. (b) x64 wave evidence follows the standing baseline treatment: unit-x64 is
the hard gate; the wave is judged against the recorded classes (no new class = pass). (c) Protocol
unchanged from P1: staged S1→S5, each stage green on all tiers before the next; evidence = raw
logs, zero FAIL tokens, boot delta by the Gate F1 method, the frame-accounting line.
