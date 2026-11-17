# cp-pe: Wine's PE loader on a shared page cache (wave 5, lane C1-pe)

Branch `cp-pe`, from `cyberpunk` @ 15ad4a35 (C0-sync landed).  Evidence in
`docs/cp-pe-evidence/`; every number below is from **strix** (Ryzen AI MAX+
395, Zen 5, KVM, QEMU 10.2.1, 8-vCPU intel guest) unless it says Linux host.

## 1. The bug

Wine processes faulted at start-up inside Wine's own PE loader: C4-x3 symbolized
`find_existing_module` (loader.c:2789, the mapped image "has no PE header"),
`LdrProcessRelocationBlock` (loader.c:3614, a write to a read-only page) and a
read of `0x170075860` (an address in ntdll's *link-time* ImageBase range, i.e.
an unrelocated pointer); C8-game saw `msvcrt.dll` import a library with an
empty name in 1 of 9 Wine boots, and the keepalive `vkcube.exe` read
`0x18007DE97` (again a link-time address: ImageBase `0x180000000`) at the same
moment.  NFS logs were clean.

## 2. Root cause: two missing cross-CPU TLB shootdowns (x86_64)

**Copy-on-write.**  A MAP_PRIVATE file (or memfd) page that has only been read
is mapped to the page-cache object's frame with a read-only leaf.  The first
write faults and `vm_cow_page_locked()` replaces the leaf with a private copy.
The user-fault path (`vm_handle_fault`) and the kernel-write path (`vm_touch`,
e.g. `read(2)` into such a page) then flushed **only the local TLB**.  Any
other thread of the process that had read the page on another CPU kept the old
translation: it went on reading the object's frame, i.e. the bytes *before*
the write, until its CPU next reloaded CR3 (a context switch).  Linux flushes
every CPU in the mm here (`ptep_clear_flush` in `wp_page_copy`).

For Wine that is exactly "an unrelocated pointer": a DLL page is relocated
(or a `.data` global is first written) by one thread; another thread that had
the page in its TLB reads the pre-relocation word -- an address in the
ImageBase range (`0x170075860`, `0x18007DE97`), a zero, or whatever the file
holds there.  Which thread reads what is timing; hence intermittent.

**MAP_FIXED.**  `l_mem_mmap` zapped the old leaves (with a shootdown) and then
called `vm_mmap`, which replaced the region records in a *later* `vm_lock`
section.  A peer thread faulting in between refilled the page from the OLD
region (an anonymous page read back as zeros: neither the old bytes nor the
new mapping's), and `vm_mmap`'s own zap then freed that frame with a local
flush only -- a frame another CPU still translated, back in the allocator.

### Proof (linux_probe_pemap, `docs/cp-pe-evidence/`)

Rows V1-V5 are deterministic: thread A spins reading a page from one CPU,
thread B changes it, A must see the change within 2000 of its reads.

| row | base 15ad4a35 | lane | Linux host |
|---|---|---|---|
| V1 COW, memfd | FAIL 12/12: A read the old value 3.5-7 M more times | PASS 12/12 | PASS |
| V2 COW, file (/tmp, /CP NFS) | FAIL 12/12 | PASS | PASS |
| V3 COW by `read(2)` | FAIL 12/12 (28 M stale reads) | PASS | PASS |
| V4 MAP_FIXED anon over anon | PASS | PASS | PASS |
| V5 MAP_FIXED file over anon | FAIL 12/12: A read `0x0` (want the file word) | PASS | PASS |
| S1 storm (Wine's loader pattern) | FAIL every child: sibling read the unrelocated word | PASS | PASS |

The storm's failure rows name the page, the object and both values, e.g.
`sibling read /CP/pemap03.bin+0x2e70 = 0xc924b6a2a623148b, want
0xc924b6a35c2f748b (unrelocated 0xc924b6a2a623148b)`: the sibling saw exactly
the file's bytes (the object frame) after the owner's relocation store and
publish.  The interleaving: the sibling reads the view (its CPU caches leaf ->
object frame); the owner writes the RW "data" pages (COW, local invlpg only);
the owner publishes with a release store; the sibling acquires and reads the
relocated word through its stale translation.

With `-n` (the sibling reads but does not judge) the base kernel passes 4/4
runs x 48 children x 6 loads x 8 files over NFS and ext2: the owner's own view
is always byte-exact.  So the page cache, the NFS readahead windows, the pread
path (`file_read_user`), object teardown under exit churn and the ext2 object
are **not** where the wrong bytes come from; only what another CPU's TLB
still points at.

### Suspects ruled out, with the row that rules each out
- readahead window refilled concurrently: S1 owner checks over 5 NFS files,
  pread + map paths, 8 at a time (`-n` runs): 0 bad bytes on base.
- `file_read_user` resident-then-read: S1 pread-into-anonymous path (25% of
  loads) and the 64 KiB pread row: 0 bad on base.
- MAP_PRIVATE COW racing a second mapper of the same file: 8 processes map
  the same files at once; owner views exact on base.
- mprotect missing a shootdown: downgrades already shoot down (C1-k4);
  upgrades need none (a stale read-only leaf only refaults).  tlb T2/T3 pass.
- page-cache object torn down and reused: exit churn (48 children/run); owner
  views exact on base.
- exec/mmap with `VM_OBJ_MAX_BYTES` 4 GiB: not reached by any failing row.

## 3. The fix (`src/kernel/vm.c`, `arch/x64/mmu.c`, `linux_compat_mem.c`)

- `vm_handle_fault` COW branch and `vm_touch` COW branch: after dropping
  `vm_lock`, `vm_cow_shootdown()` -> `vm_arch_shootdown(as)` before the access
  retries / the kernel writes.  `[PECOW] cow_with_peers=K cows=N pid va peers
  obj off by=user|kernel` rows (first 24, then every 1024th) count the COWs
  that really had other CPUs to flush -- each one a potential stale read on
  base.
- `vm_mmap` MAP_FIXED and `vm_region_install_fixed`: zap the old leaves in
  the same `vm_lock` section as the record rewrite, park the frames and hold
  the objects in a `vm_tlb_batch`, shoot down, then release (C1-k4's munmap
  discipline).  `vm_zap_range` (local flush, immediate free) is gone.
- `l_mem_mmap`: no pre-zap for MAP_FIXED (it opened the window above).
- `vm_file_obj_materialize`: after the unlocked page read, the page is mapped
  only if the region still maps the same object at the same offset with the
  same privacy; otherwise nothing is mapped and the access refaults (Wine maps
  one image's header and sections as separate MAP_FIXED views of one fd).
  `vm_touch` re-examines such a page (bounded).
- `[VMFIX] remap` frees the replaced frame after a shootdown.
- ARM is unaffected: its leaf changes use broadcast TLBI (`vm_arch_shootdown`
  is a no-op there).

Cost: one shootdown per COW fault that has peers (IPIs only to CPUs running
the same AS; none for a single-threaded process).

## 4. Tests and tools

- `src/user/linux_probe_pemap.c` (wildcard probe rule): V1-V6 + S1 over /tmp
  in the default batch (`t3_linux_probe.sh pemap`).  V6: the reader read the
  page and then sleeps in `futex_wait` while the writer copies-on-write and
  wakes it.  `-s` storm only, `-n` sibling reads without judging, `--gen DIR
  N SIZE` writes the data files (same generator on host and guest).
- `tools/cp/pe/run-pemap.sh [tree]`: the probe over a read-only NFS export
  (`/CP`, 5 files) and an ext2 image (`/EXT/pe`, 3 files), `PE_BOOTS` boots x
  `PE_RUNS` runs; the data are checked on the Linux host first.
  `tools/cp/pe/pemapl.c` is the native launcher (mount + spawn).
- `tools/cp/pe/wine-ab.sh TREE TAG N`: N Gate P5 sessions graded for
  Wine-process faults (ntdll PCs symbolized), `[SIGDUMP]` processes and
  `[PECOW]` totals; `PE_AB_MODE=storm` swaps the probes for
  `tools/cp/pe/winestorm.c` (wineboot, keepalive, then `PE_ROUNDS` rounds of
  `PE_PAR` concurrent `vkcube --winprobe` / `cmd /c exit` starts).

## 5. Results (strix)

| | base 15ad4a35 | lane | Linux host |
|---|---|---|---|
| probe V1/V2/V3/V5 (12 iterations each) | FAIL in 6/6 runs (t3 x2, NFS+ext2 x4), 11-12 of 12 iterations stale | PASS in 11/11 runs | PASS 10/10 runs |
| probe V4 (MAP_FIXED anon) | PASS | PASS | PASS |
| probe V6 (reader asleep in futex_wait) | PASS | PASS | PASS |
| probe S1 storm, sibling judging | FAIL 6/6 runs (every child in t3; 4/4 NFS+ext2 runs) | 0 data errors in 11/11 runs; 1 liveness timeout (below) | PASS 10/10 |
| probe S1, `-s -n` (owner views only) | PASS 4/4 runs x 48 children x 6 loads x 8 files | -- | -- |
| Gate P5 sessions, Wine-process faults | 0 in 6 (P5 PASS 5/6; run 2 missed a screendump) | 0 in 6 (P5 PASS 6/6) | -- |
| storm PAR=3 (72 concurrent-start batches) | 0 faults in 3 sessions | 0 faults in 3 sessions | -- |
| storm PAR=6 | fd table exhausted -> NLS init c000011f -> ntdll casemap NULL read | the same | -- |

Lane runs: t3 x3 (batch, rerun, V6) + NFS/ext2 x8 (2 + 6).  Base runs: t3 x2
+ NFS/ext2 x4.  The one lane failure (`run-pemap.sh` boot 1 run 1) is a
liveness timeout -- the sibling did not answer the touch request within 10 s
while `[YLOCK-STALL] site=ext2_lock` was printed (8 children paging the same
ext2 files): C3-ext2w's `ext2_lock` contention, not a wrong byte.

V6 passing on base says which readers were exposed: a thread that sleeps
switches CR3 when it next runs, so only a reader **running on another CPU
across the write** keeps the stale translation.

The lane's `[PECOW]` rows count those COWs in real workloads: 1 per P5
session in 2 of 6 sessions (winedevice.exe), 0 in the PAR=3 storms, and
~330 000 of ~565 000 COWs in a probe run.  In P5 Wine sessions the precondition
is rare, which fits a fault seen in some sessions and not others; on this base
it did not occur in 12 Wine sessions on either kernel, so the Wine-level A/B
cannot show a rate difference (0 vs 0).

## 6. What this does and does not explain

- Explained and fixed: any read of a page another thread of the process has
  just written for the first time (relocation, a `.data` global, a
  `read(2)` into a private file page) returned the pre-write bytes on the
  reader's CPU until it reloaded CR3.  That is exactly the "unrelocated
  pointer" shape (`0x170075860`, `0x18007DE97`: link-time ImageBase
  addresses).  Also fixed: MAP_FIXED's zero-page window and its freed-but-
  still-translated frames.
- Not reproduced: the C8 run-2 event.  There the **single-threaded**
  wineserver rejected gdi32.dll as `c0000130` (STATUS_INVALID_IMAGE_NOT_MZ:
  its `pread` of the header saw no "MZ") while another process (the keepalive)
  read a link-time pointer at the same moment.  A single-threaded reader
  cannot be hit by the COW staleness, so that event points at bytes going
  wrong for two processes at once -- a frame with two owners, or the read
  path.  The probe's owner checks (`-n`) found neither on base.  Proposed
  next step (frame.c is not this lane's file): make `frame_free()` of an
  already-free frame count and print `[FRAME-DFREE] phys caller` (bounded);
  today it is silently ignored, which hides a double free whose frame was
  reallocated in between.
- `vm_obj_unmap_cut_locked` (ftruncate shrink, C3-ext2w's path) still frees
  private COW copies past the cut with `vm_arch_unmap` (immediate free) before
  its shootdown; it should park them like the object frames.
