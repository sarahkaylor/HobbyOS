# w3-kernel lane evidence notes

## Gap A — FAT16 extend-write persistence (w2-mkfs "loss")

### Diagnosis: not a kernel write bug — an invalid probe size
The w2 report's all-zeros image is explained by mkfs.fat's own size guard:
`-C` ftruncates the file to the requested size FIRST, then
`setup_tables()` rejects sizes below FAT16's 4087-cluster floor with
"too small or a too large filesystem" — so the only bytes ever written to
a 1 MiB image are the kernel's ftruncate zero-fill.  The kernel write
path is exonerated by three independent tiers:

1. Failing-test-first kernel coverage (now GREEN, committed):
   - `src/kernel/fs_test.c` `test_fs_ftruncate_extend_then_write`:
     process/file layer, create W3FTX.IMG, ftruncate to 4 MiB, write
     0x5A^(i&0xFF) pattern, reopen, byte-compare (raw path via
     fat16_open/seek/read/close).
   - `src/kernel/fat16_test.c` `test_fat16_ftruncate_extend_then_write`:
     fat16 layer, /W3F16.IMG extended to 4 MiB, 0x1F+(i&0x3F) pattern,
     verification at {0, 512, 512*8, IMG_SIZE-4096}, reopen + compare.
   - Fixed 3 `memset` calls (freestanding kernel has no memset) with loops.

2. Host-parity: obj/mkfs_host/mkfsfat_host -F 16 -C at 1 MiB (1024
   blocks) dies identically BEFORE content writes; 16 MiB / 128 MiB pass.

3. In-OS raw-disk proof (this evidence dir):
   - 1 MiB: `mkfsfat -F 16 -C PROBE.IMG 1024` -> "WARNING: Not attempting
     to create a too small or a too large filesystem".
     PROBE.IMG.raw = 1,048,576 bytes ALL ZEROS (magic 00 00 00, no FAT16
     string, no 55AA sig) — chain allocated (ftruncate), content never
     written.
   - 16 MiB: `mkfsfat -F 16 -C OUT.IMG 16384` -> success banner
     "mkfs.fat 4.2 (2021-01-31)".
     OUT.IMG.raw = 16,777,216 bytes; boot magic EB 3C 90; "FAT16" at
     offset 54; 55 AA at 510-511; non-zero content; FAT chain of 1024
     clusters fully present.  => content persists byte-exact.

### Conclusion
Gap A is closed with the corrected diagnosis: the kernel persists
extended-file writes (unit tests + 16 MiB in-OS raw-disk byte proof); the
w2 "loss" was a below-minimum-size image (1 MiB < 4087 clusters) that
mkfs.fat rejects before writing content.  No kernel write-path change was
needed (and none was made).  The 128 MiB acceptance size was verified on
the host port (mkfsfat_host rc=0, 134,217,728 B) and matches the in-OS
16 MiB path at the same spc/geometry (spc=32, chain=fits).

## Gap B — sysroot dirent ENOSYS

### Fix
`src/libc/src/dirent.c` previously returned ENOSYS stubs; rewritten to
call the existing SYS_READ_DIR syscall (row 15, 31-char name cap frozen
per AD-13 — d_name is 256 bytes so the cap always fits).  opendir()
stat()-checks first so non-directories return ENOTDIR (read_dir alone
cannot distinguish ENOTDIR from an empty dir).  `dirent.h` updated.
Kernel side already worked (proven by the new kernel test below); the gap
was purely the libc layer.

### Failing-test-first
`src/kernel/fat16_test.c` `test_fat16_read_dir_multiple`: creates
/W3RDDIR with AAA.TXT (11 B content), BBB.BIN, LONG_FILENAME_IN_DIR.TXT,
a subdir, and an empty dir; iterates via fat16_read_dir; asserts >=5
entries (names NUL-terminated, within the 31-char cap).  Initial run
showed the test's unzeroed name buffer producing trailing garbage — fixed
in the test (zeroing), kernel correct.  GREEN now.

### End-to-end in-OS (this evidence dir)
gapb-dirent-* : a real libc consumer (src/user/dentest.c -> DENTEST.BIN
built against libc.a + crt0, staged on the smoke disk) calls
opendir/readdir/closedir on /FIXTURE and /FIXTURE/SUB and writes a
self-describing record to /DENTEST.OUT.  Extracted: 33 bytes decoding to
[".", "..", "ONE.TXT", "SUB", ".", "..", "TWO.TXT"] — full enumeration
INCLUDING subdirectory descent (TWO.TXT under SUB), which was impossible
before the fix (opendir -> ENOSYS -> no entries).
