# Lane w9-dirent — FAT16 dirent writer fix (S2 twin / lost-clean link failure)

Branch `selfhost/w9-dirent`, worktree `/home/sarah/hobbyos-selfhost-lanes/w9-dirent`.
Main repo untouched; local commits only. Raw serials + pulled artifacts under
`docs/selfhost/lanes/evidence-w9/`.

## Symptom (w7, prior lane)

During the in-OS S2 kernel build, each object written into `::/SRC/obj/arm`
ended up with a mangled duplicate dirent (the "twin"), and for exactly two of
59 names (`nfs_test.o`, `cpu.o`) the CLEAN entry was lost — `LLD.BIN: error:
cannot open /SRC/obj/arm/nfs_test.o` (+ `cpu.o`), so the selfhost link
failed.  The twins look like `FAT16-AB.O.T` / `CPU-8167.O.T`: an 8.3 name
whose extension field carries an embedded dot and whose LFN records claim a
temp-file name like `cpu-81674d23.o.tmp`.

## Root cause (this lane)

clang (LLVM) writes **every** `-o` object through a `TempFile` whose fd stays
**OPEN across the rename to the final name and is closed AFTERWARDS**
(LLVM `TempFile::keep()` renames, then the fd closes).  Sequence on the
unfixed kernel:

1. `open("/SRC/obj/probe/cpu-DEADBEEF.o.tmp")` → `alloc_lfn_run` writes the
   temp's 0x0F run + 8.3 alias (`CPU-09EF.O.T` — `format_83` of the long
   temp name, embedded dot in the extension field).
2. write object; dirty.
3. `rename(tmp, "/SRC/obj/probe/cpu.o")` → `fat16_rename` deletes the temp
   run (records + short, correctly — see the rename trace) and allocates the
   clean `CPU.O` entry, often **reusing the just-freed slot**.
4. `close(fd)` (AFTER the rename) → `fat16_close` → dirty
   `fat16_sync_entry` wrote the stale cached temp dirent back over that
   slot:
   - the temp alias is **resurrected** (twin, no LFN records: the records
     were already marked 0xE5 in step 3, only the short comes back), and
   - when the clean entry had reused that slot, it was **clobbered** → only
     the twin survives → nothing opens by the clean name.

The two shapes in w7 (`FAT16-AB.O.T` record-less twins for most files;
`NFS_TEST.O.T`/`CPU-8167.O.T` WITH records for the two lost-clean files)
are the same one bug at two points of the slot lifecycle.

Evidence that this is the mechanism:

- In-OS probe (MODE=selfhost, real `CLANG.BIN` on `cpu.c` + `nfs_test.c` +
  `fat16.c` into `::/SRC/obj/probe`, unfixed kernel): dirent dump shows
  `CPU.O` + `CPU-09EF.O.T` sharing a cluster, `NFS_TEST.O` + `NFS_TEST.O.T`,
  `FAT16.O` + `FAT16-B8.O.T` —
  `docs/selfhost/lanes/evidence-w9/inos-probe-prefix.dirents.txt`.
- The rename trace (`inos-probe-prefix-ren.trace.serial`) shows
  `lfn_del sec=.. off=6` targeting the temp's SHORT correctly while the
  twin still survives on disk — the resurrection happens AFTER the
  `lfn_del`, at the close's sync.
- The unit test `test_fat16_clang_temp_rename_no_twins` reproduces it in the
  kernel unit tier once the clang order is used (rename **then** close);
  it PASSES when the handle is closed before the rename (the order all older
  tests used, which is why the bug escaped).

## Fix

`src/kernel/fat16.c`, `fat16_sync_entry`: before writing a dirty entry back
at close, read the on-disk slot at `(dir_sector, dir_offset)` and **skip**
when:

- the slot is free (`0x00` or `0xE5`) — the entry was deleted by a
  rename/unlink under this still-open handle, or
- the slot now holds a different file's 11-byte name — the rename's clean
  entry reused this slot (the clobber case).

A handle whose entry a rename deleted must not resurrect it.  This extends
the existing "read descriptor must not clobber" intent to stale WRITE
handles (the LLVM TempFile pattern) and also guards the synthetic-root
handle (`dir_sector == 0`).

Reader side is untouched and still resolves pre-existing images that contain
twins (each twin is an ordinary orphan entry sharing the clean entry's
cluster; lookups by clean name are unaffected).

## Verification

| gate | result |
|---|---|
| Failing unit test on tip | **FAIL** (Tests run 319, failed 1) — twins resurrected |
| Unit test after fix | **PASS** |
| ARM unit tier `./run_unit_tests.sh` | **PASS 319/0 → 320/0** (with + collision test) |
| x64 unit tier `EXTRA_QEMU_ARGS='-enable-kvm' ./run_unit_tests_intel.sh` | **PASS 323/0** |
| In-OS probe, unfixed kernel | twins present (prefix dump) |
| In-OS probe, fixed kernel | **ZERO twins** — CPU.O/NFS_TEST.O/FAT16.O only (postfix dump) |
| S2 attempt 1 (writer fix only) | 59 objects compiled rc=0, NO 'cannot open', zero twins — link then hit the 8.3-collision reader bug: `undefined symbol: net_rdma_fast_write` (net_rdma.o resolved to net_rdma_test.o's alias) |
| S2 attempt 2 (writer + reader fix) | **PASS** — `phase=s2-kernel-done rc=0`, `System halt`, w5 outcome=0 elapsed 4321s; hobbyos.elf 1371192 B + hobbyos.bin 218036 B; obj/arm exactly 59 clean entries, ZERO twins |

## Second bug found by the crown proof: 8.3-alias reader collision

With the writer fixed, lld walked all 59 objects and failed with
`undefined symbol: net_rdma_fast_write` referenced by `net.c:447`
(`net.o:(handle_udp)`).  `net_rdma.o` (8 chars base, genuine 8.3 entry)
and `net_rdma_test.o` (long name whose format_83 alias truncates to the
SAME `NET_RDMA.O`, i.e. `NET_RDMAO` with ext `O`) coexist in obj/arm.
`find_entry_in_*` returned the FIRST directory-order entry with a matching
8.3 name, so opening `/SRC/obj/arm/net_rdma.o` handed lld `net_rdma_test.o`
(the 840-B object, `.text` ~empty, no definitions): a clean
"undefined symbol" instead of a parse error.

`fix`: find_entry_in_root / find_entry_in_subdir now prefer an LFN-free
entry whose 8.3 name EXACTLY matches an 8.3-representable query over any
longer name's truncating alias; long-name lookups still match by the exact
0x0F chain.  Same-pair collisions in the S2 set: virtio_blk/_-test,
virtio_gpu/_-test/-x64, virtio_input, virtio_net, net_rdma, program_loader,
process_test.  Covered by `test_fat16_83_alias_collision_lookup`.

## Artifacts in `docs/selfhost/lanes/evidence-w9/`

- `inos-probe-prefix.serial` — in-OS probe boot 1 (unfixed kernel, compiles done)
- `inos-probe-prefix.dirents.txt` — `::/SRC/obj/probe` dump (twins present)
- `inos-probe-prefix-ren.trace.serial` — same boot with rename/lfn_del trace
- `inos-probe-postfix.serial` — in-OS probe boot after the fix
- `inos-probe-drive.txt`, `w9cpu.sh`, `w9nfs.sh`, `w9ft16.sh` — probe drive + scripts
- `serial-w9-s2.serial` — S2-stage crown proof (in progress)
