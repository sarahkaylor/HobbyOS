# Lane L3 findings — FAT16 VFAT long-name coverage vs the in-OS build payload

Lane: L3 (selfhost/l3-lfn-verify). Base: c59329c. Date: 2026-10-14.
Phase 0 deliverable: understand-before-touching note. Files read: `src/kernel/fat16.c`
(1894 lines, end to end), `src/kernel/fat16_test.c`, `src/kernel/vfs.c`,
`src/kernel/fs.c`, `src/kernel/main.c`, `src/include/fat16.h`, `src/include/fs.h`,
`src/include/vfs.h` (VFS_PATH_MAX 256), `src/kernel/arch/arm/trap.c` (read_dir syscall),
`src/user/fatbig_test.c`, Makefile disk.img recipe + TU21_TEST_RULE, run_unit_tests.sh.

## What already works (verified by reading; existing tests cover parts)

- **LFN read** — `find_entry_in_root`/`find_entry_in_subdir` accumulate 0x0F records
  tail-first (`lfn_add_chunk`), verify the 8.3 checksum (`lfn_checksum` == record sum),
  match case-insensitively (`lfn_matches`). Non-8.3 queries never match a short entry
  (`match_name` rejects base >8 / ext >3). Deep paths resolve per-component.
- **LFN write** — `fat16_open` create path writes a 0x0F run + short entry via
  `alloc_lfn_run` (incl. cross-sector runs); existing tests prove create → write →
  reopen by long name (fat16_test.c test 2) and >1 MiB LFN round-trip.
- **8.3-truncation guard** — `name_is_83` never truncates a long name onto a short entry.

## Uncovered behaviors → gaps (Phase 1 tests will prove each; Phase 2 fixes)

1. **G1 — `fat16_read_dir` returns mangled 8.3 names, never the long name.**
   `fat16_read_dir` (fat16.c:985-1098) skips 0x0F entries and rebuilds the name from
   the 11-byte short field. A listing of a long-name tree (the F2/AD-11 payload) shows
   `PROGRA~1.C`-class names, and any in-OS filename consumer that globs/lists by the
   real name (build driver, Files app, shell) sees wrong names. Fix in fat16.c:
   accumulate the LFN run and emit the long name when checksum matches.
2. **G2 — `fat16_mkdir` writes no 0x0F records.** Only the 8.3 entry is created
   (fat16.c:1332-1404), so a directory whose name is not 8.3 (`user_include`,
   `llvm-project-21.1.8.src`) is unreachable by its real name: resolve fails, and
   creating files inside it fails. Payload dirs are long; in-OS toolchain mkdir of a
   long-named output dir is a stated need.
3. **G3 — `fat16_rename` drops the long name.** Same-dir rename rewrites only the 8.3
   name (fat16.c:1254-1281): the 0x0F records keep the OLD name + OLD checksum, so the
   entry is reachable only by its new short alias and the old records are orphaned
   (wrong checksum). Cross-dir move allocates a bare 8.3 entry (no LFN run) and leaves
   the old 0x0F records dangling. rename long→long must write a fresh LFN run.
4. **G4 — unlink free-edge.** `fat16_unlink` frees preceding 0x0F records in the same
   sector only (fat16.c:1134-1137); a run that spans a sector boundary leaks the
   records in the previous sector (0xE5'd back to free slot). Rare (only the
   cross-sector run placement), but the slot leak is real. Fix opportunistically.
5. **Bounds (document, do not change):** resolve_path component buffer is 64 chars
   (63 max, long-name create is capped the same), `clean_path` caps total path at
   127 chars / 16 components, the read_dir syscall caps listed names at 31 chars
   (frozen ABI, syscall files are parent-owned), `lfn_state.name` at 255. The payload's
   deepest real names (`llvm-project-21.1.8.src` = 26, `program_loader.c` = 16) fit;
   the LFN fixture keeps components ≤ 63 and ≤ 31 for listing asserts; the fixture's
   30+ char name (`llvm-project-21.1.8.src-EXAMPLE.c` = 36) exercises LFN open/read and
   documents the 31-char listing cap.

## Other notes

- FAT16 dirs cannot be unlinked (`fat16_unlink` refuses attr 0x10; no rmdir) — a
  pre-existing limitation; mkdir tests tolerate EEXIST (mirroring the existing suite).
- mtools 4.0.49 on the host writes VFAT 0x0F runs; the disk.img recipe rebuilds the
  disk from scratch (dd + mkfs.fat), so recipe-added fixtures are idempotent.
- Path routing: read_dir syscall → `vfs_read_dir` → `fat16_read_dir`; open → fs.c →
  `fat16_open` (VFS dispatch unchanged). No syscall/ABI changes needed for the fixes.
- Kernel constraints for the fixes: `-mgeneral-regs-only`, byte-wise entry parsing,
  spinlocks as in the existing file, no packed-struct field reads of on-disk data.
