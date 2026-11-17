# cp-sync: merging `main` into `cyberpunk` (wave 5, lane C0-sync)

`main` @ b1f3ef56 merged into `cyberpunk` @ 949ba008 (fork point f7a51cfa;
main 1,449 commits ahead, cyberpunk 233).  Branch `cp-sync`:

| commit | what |
|---|---|
| f752acca | the merge: six conflicted files resolved, both sides' behaviour kept |
| 464dafcf | fix-up: MAP_SHARED write-back through the direct map, region by region |
| 30445eb0 | fix-up: `file_read_user` materializes the destination instead of bouncing it |
| 0259c84c | fix-up: compat fsync/fdatasync routing (main's FAT16 flush + every other fd) |
| 8f23a155 | fix-up: LibreOffice on `disk.img` only when its X prefix exists; duplicate define |
| f2a83672 | fix-up: castscan and panic-sink allowlists for main's GB-308 sites |
| (this) | `docs/cp-sync.md`, `docs/cp-sync-evidence/`, `tools/cp/sync/munmapwb.c` |

What main brought (all on the x86_64 Linux-ABI lane): OpenJDK 27 kernel fixes
(GB-308: `VM_OBJ_MAX_BYTES` 4 GiB, MAP_SHARED write-back on msync/munmap/fsync,
atomic `pread`/`pwrite`, `getcwd` returning the length, `open(dir, O_RDONLY)`,
the FAT `getdents` dot filter, `exit_group` → `SYS_EXIT_GROUP`, AF_INET
loopback stream sockets, compat `mkdir`/`mkdirat`, `COMPAT_MAX_ARGV` 128,
`execve` path 256, FAT16 files reported 0755, the badvec per-CPU window), the
LibreOffice suite and its build, fonts for `java.desktop`, and the prune of
`evidence/`, `continuation/` and root-level agent scratch.

## 1. Conflict table

"cp" is the cyberpunk side, "main" is main.  The test column names what
covers the row on the merged tree (§3 has the results).

| file | cp did | main did | resolution | covered by |
|---|---|---|---|---|
| `src/include/vm.h` `VM_OBJ_MAX_BYTES` | 64 MiB → 1 GiB (cp/sys: Wine's 64 MiB memfd, 192 MiB GPU buffer; cp/pfx sized the frame array to the object) | 64 MiB → 4 GiB (JDK: 140 MiB `lib/modules` jimage in one map, 3 GiB `MAP_NORESERVE` heap) | **4 GiB**, the larger.  Safe because nothing is sized by the cap: the frame array is sized to the object (a 4 GiB object = 2049 contiguous frames, a 200 MiB one still 101), the slot table is `VM_OBJ_MAX` records, pages are demand-zero, page counts ≤ 2^20 fit vm.c's ints.  `vm_test`'s cost row now expects 2049 (was 513) | unit `test_vm_big_object_cost`; probe `bigobj` |
| `src/kernel/arch/x64/trap.c` `badvec_frame_window` | strip the DMAP alias (kernel-task stacks are reached through the direct map since P1/S5), then `p < 0x70000000` | ask the per-CPU stacks (they live in the image above 0x70000000, GB-308); main also turned DR0 arming off (`BADVEC_WATCH_ENABLE 0`, auto-merged) | **union**: DMAP strip, then aligned and (below the image **or** inside a per-CPU 64 KiB stack window).  Kernel text between them stays "not a frame" | unit `test_badvec_fatal_frame_window` (0x1F36 → 0, 0x23FFE2A0/0x25FFB640 → 1, 0x7008A3FE → 0) |
| `src/kernel/linux_compat_mem.c` `l_mem_munmap` | find the first region with `vm_region_above` (P1: no page walk over a 64 GiB hole) | write MAP_SHARED pages back before `sys_munmap` drops the regions | **both**: cp's start, then main's write-back (made region-wise in 464dafcf) | main `MUNMAPW`+`READIT`; probes `mem`, `vmcommit`, `cow` |
| `src/kernel/process.c` fork | `child->abi_flags = group->abi_flags` (P1) | the same line (GB-308) plus a loop copying `fd_cloexec` | **cp's line once**; main's loop dropped because cp/exec's `fd_cloexec_copy()` already runs a few lines later in the same function | probes `exec`, `execthr`, `fd`; exec_gate |
| `src/kernel/fs.c` includes | `ext2_vfs.h`, `tmpfs.h` | `arch/cpu.h` (bounce buffer CPU id) | all three in the merge; `arch/cpu.h` left with the bounce in 30445eb0 | build |
| `src/kernel/fs.c` `fs_init` | — (record locks moved to `vfs_lock.c`) | init `f1_pos_locks[]` (per-fd positioned-I/O lock) and its own `file_locks_lock` | `f1_pos_locks` init kept; `file_locks_lock` (main's table) does not exist here | build; main `pread` users (JDK) |
| `src/kernel/fs.c` lock comment + `file_lock_fcntl` set path | record locks on the generic `vfs_lock` manager (cp/ext2b: FAT16, ext2, tmpfs, NFS keys; OFD + flock) | F_SETLKW blocks in this function (halt loop, 10 s ceiling) because on main it also served the compat fcntl | **cp's**.  The same thing was implemented twice: on this tree the compat F_SETLK/F_SETLKW/F_OFD_* rows are `vfs_compat.c`'s `vx_fcntl`, which really waits (rewound-syscall park, EDEADLK on a cycle) and is what the JDK's `FileChannel.lock()` reaches.  `file_lock_fcntl` is the native face only and keeps its one-attempt contract | probes `wpfx`, `pfx`; ext2_gate; main `LOCKHOLD` pair |
| `src/kernel/linux_compat_misc.c` `readv` | `l_user_ok` (materialize writable) then `file_read`; cancel the park on a short read (C1-k4) | `file_read_user` (bounce) | main's call, cp's cancel; the materialization moved into `file_read_user` (30445eb0) | probes `fd`, `pipestorm`, `glibc` |
| `linux_compat_misc.c` `pread64`/`pwrite64` | seek/check/read/restore with `l_user_ok` | `file_pread_user`/`file_pwrite_user`: the same sequence under a per-fd lock (concurrent class loading read each other's offsets) | **main's** (it is cp's sequence plus the lock); cp's source check re-added inside `file_pwrite_user` | probes `fatcoh`, `pfx`, `glibc`; Gate P2 |
| `linux_compat_misc.c` `l_misc_set_cwd`/`fchdir` vs `mkdir`/`mkdirat` | cwd helper + fchdir (cp/sys) | FAT16 mkdir/mkdirat with POSIX errnos | **both functions**.  ext2/tmpfs keep their own mkdir rows, consulted first; main's row serves FAT16 | probes `tmpfs`, `wpfx`; ext2_gate |
| `linux_compat_misc.c` `l_tab_misc` | + fchdir (81) | + mkdir, mkdirat | **union** | build; probes |

### 1.1 Auto-merged code adapted to this tree (the fix-ups)

These merged without a marker and were wrong for cyberpunk; each is its own
commit.

| where | main's code | why it is wrong here | fix | covered by |
|---|---|---|---|---|
| `mem_writeback_range`, `l_mem_fsync_file` (464dafcf) | `file_writeback_fat16(..., (const void *)fr, ...)` with `fr` a **physical** frame ("frames are identity-mapped") | compat roots have no identity map of RAM (F1, `kmem.h`); the pointer is a user VA.  Also a per-page `vm_region_find` over the whole munmap range (Wine unmaps multi-GiB reservations) and `file_obj_page()` reading never-touched pages from disk to write them back | `phys_to_virt(fr)`; walk region by region (holes skipped with `vm_region_above`); only resident pages (`vm_object_peek`) | castscan; main `MAPFSYNC`, `MUNMAPW`; probes `mem`, `vmcommit` |
| `file_read_user` (30445eb0) | every syscall-side read through a 32 KiB per-CPU bounce + `vm_kwrite` | one extra copy of every byte read, and large reads split into 32 KiB `file_read()` calls.  Measured (interleaved A/B, same load): one 176 MiB NFS `read()` 282 MB/s with the bounce vs 404–448 without (base5 453); 64 KiB reads 225 vs 231–235.  The NFS guards pass either way | P1's mechanism under main's API: `vm_range_ok(write)` then `file_read` directly into the user range; partial-then-park cancels the park; `file_pread_user` passes the calling thread | `nfs-bench-ab.txt`; rsize_guard; main `HELLO`, `DIRPROBE`; probes |
| `l_fd_fsync` + `e2c_fdatasync` (0259c84c) | fsync/fdatasync rows at the head of `l_tab_fd` that return `file_fat16_ident()`'s error for every non-FAT16 fd | `l_tab_fd` runs before `l_tab_misc`: fsync of a pipe, socket, memfd, NFS file or `/dev/fb0` became -ENOTSUP and `/dev/fb0` lost its GB-032 present verb.  Conversely cp's `e2c_fdatasync` answered 0 for every non-ext2 fd, so main's fdatasync flush never ran | `l_fd_fsync`: -EBADF for a closed fd, FAT16 flush, else `l_misc_fsync`'s answer; `e2c_fdatasync` declines non-ext2 fds like `e2c_fsync` | main `MAPFSYNC` (fsync **and** fdatasync); probes `wpfx`, `pfx` |
| `castscan.allow`, `panic_sink_allowlist_test.py` (f2a83672) | five new int↔pointer sites (main never ran castscan) and seven new panic-sink references in x64 `trap.c` (main did not raise its own allowlist for GB-308) | gates 5 and 6 red on the merged tree, green on base5 | the five sites reviewed and listed with a reason (the five rows they replace removed); trap.c 341 → 348 (the per-CPU `kernel_stack` rows printed just before the not-a-frame halt) | gates 5, 6 |
| Makefile (8f23a155) | every `disk.img` depends on the seven LibreOffice binaries unless `X11_BUILD_DIR` is x11stub | without `third_party/x11/build/<arch>/prefix`, `lo-x11-libs` runs `third_party/x11/build.sh --stage all` inside `make disk.img`; the cyberpunk gate trees have no prefix (base5 neither) | `LO_ON_DISK ?=` 1 only when `$(LO_X11_DIR)/prefix/lib/libX11.a` exists; `LO_ON_DISK=1`/`LO_ON_DISK=` force it | gate 1 (the build) |
| `linux_syscall.h` (8f23a155) | `L_SYS_mkdir` defined twice | harmless, removed | — | build |

## 2. Limits and constants

| constant | base (cp) | main | merged | why |
|---|---|---|---|---|
| `VM_OBJ_MAX_BYTES` | 1 GiB | 4 GiB | **4 GiB** | §1; also raises `TMPFS_MAX_FILE` (= the cap) and the ext2 mapping capacity ceiling to 4 GiB |
| `COMPAT_MAX_ARGV` (program_loader.c) | 16 | 128 | **128** | main only; the arrays are `uint64_t[129]` on the stack |
| `sys_exec` `pathbuf` (native exec) | 32 | 256 | **256** | main only; truncated paths now ENAMETOOLONG |
| `MAX_PROCESSES` (x64) | 512 | 192 | **512** | cp only (main did not change it) |
| `MAX_OPEN_FDS`, `MAX_GLOBAL_FILES`, `MAX_SOCKETS` | 256, 512, 16 | same | same | untouched by either side (C1-k5 owns them) |
| `COMPAT_MAX_ENV` | 24 | 24 | 24 | untouched |
| new static arrays | — | `f1_pos_locks[MAX_GLOBAL_FILES]` (ylock), `fs_rd_bounce[8][32 KiB]` | `f1_pos_locks` only | the bounce went with 30445eb0 (−256 KiB BSS) |

## 3. Gates (strix, KVM, Zen 5; evidence in `docs/cp-sync-evidence/`)

| gate | base5 (949ba008) | cp-sync | notes |
|---|---|---|---|
| 1 build | rc=0 | rc=0 | |
| 2 Linux probes (36) | 36/36 | 36/36 | `batch-*.txt` |
| 3 ext2_gate | PASS | PASS | |
| 4 unit | 453 run, 2 failed (virtio_gpu_test :214/:244) | same | `[VMCOST] 4 GiB ... frame array=2049` |
| 5 host_tests | 38/40 | 37/40 → **38/40** after f2a83672 | the extra red was main's panic-sink count |
| 6 castscan | rc=0 | rc=1 → **rc=0** after f2a83672 | five reviewed sites |
| 7 ARM build | rc=0 | rc=0 | |
| 8 exec_gate, rsize_scan, rsize_guard | PASS ×3 | PASS ×3 | G4 rs=32768 issued<=2; G5 32764 B/RPC |
| 9 pfx_gate | PASS | PASS | |
| NFS throughput A/B | — | parity | `nfs-bench-ab.txt`: interleaved runs, lane 257 / 453 MB/s (seq65536 / one176m) vs base5's 248 / 453; the low batch numbers (110 / 79) were host load (the base worktree measured 90–98 / 130–330 in the same window).  main's bounce buffer: 282 MB/s on one176m |
| Gate P2 | PASS (base worktree) | run 1 G2 FAIL (3 rows spliced into a kernel dump), run 2 PASS | `p2.txt` |
| Gate P4 | PASS (k4b reference) | PASS G1–G3, with LibreOffice on the disk | `p4.txt` |
| Gate P5 | PASS (run 7 reference) | PASS: vkcube, d3d11, d3d12 each ≥ 60 s with two distinct screendumps, LibreOffice on the disk | `p5.txt` |
| main: glibc ABI probes | — | HELLO, DIRPROBE, MMAPRO, MAPFSYNC, LOCKHOLD pair, MUNMAPWB pass | `main-probes.txt`; MUNMAPW+READIT race in one wave (READIT ran first), replaced by the single-process `tools/cp/sync/munmapwb.c` |
| main: A/B of the fix-ups | — | merge-commit `linux_compat_mem.c`: kernel #PF (vector 14) in the first munmap write-back; merge-commit fsync routing: `fdatasync` leaves `disk[1]=A` | both seen failing, then passing with the fix-up |
| main: OpenJDK sdktest, Doom ELF | — | not run | absent inputs, §5 |

## 4. The prune

Main deleted 515 files: `evidence/**`, `continuation/**` and root-level
scratch (`*-REPORT.json`, `serial_dump.txt`, `UNTITLED.TXT`, ...), plus
`tools/gate/t5_doom_manifest.sh`, `tools/x64_blk_bench.sh`,
`tools/wk_probe_https.py`, `tools/board/resolve_makefile_*.py`.  None of them
was touched on the cyberpunk side since the fork, and nothing on the
cyberpunk side references them.  `docs/cp-*` (122 files) and `tools/cp/`
(104 files) are identical in number and content before and after the merge.

## 5. Not run: what main's own gates need that strix does not have

- `tools/jdk/sdktest.sh` (version, javac, run, the 18-check suite) and the
  `java.desktop` UI checks: the Adoptium OpenJDK 27.0.1.0.2+1 linux/x64
  hotspot tree at `third_party/jdk-27` (fetched by `tools/jdk/build.sh`; not
  in the main checkout either) and a host `javac` on strix (none installed).
- The Doom ELF (`tools/doom_elf/run_doom_elf_test.py`): `doom10.elf` /
  `doomgen.elf` from `tools/linux-doom/build-*.sh`, which download Alpine
  musl X11 packages; no prebuilt copy exists on strix or the Mac.
  `doom1.wad` is in the repo.  The `wad` Linux probe ran (gate 2).
- LibreOffice's own `lo_test` / `libreoffice_test`: not attempted (main's
  E2E harness, not an x86_64 ABI regression); the seven apps did build and
  stage under P4/P5.

## 6. Facts for the other lanes

- **`file_read_user(p, fd, ubuf, size, tf)`** is now the way a syscall reads
  into a user range (native `read`, compat `readv`/`pread64`/`recvfrom`). It
  materializes the range writable first.  New read paths should call it
  rather than `file_read` on a user pointer.
- **Positioned I/O takes `f1_pos_locks[gfd]`** (fs.c), a per-global-file
  ylock held across seek+read+restore.  C1-k5: it is a static
  `ylock_t[MAX_GLOBAL_FILES]`; growing the system file table grows it.
- **compat fsync/fdatasync**: ext2 and tmpfs tables answer first; a table that
  wants the FAT16 MAP_SHARED flush or the `/dev/fb0` present verb must
  DECLINE (return 0) for fds that are not its own.  C3-ext2w: if ext2 gains
  write-back, `e2c_fsync`/`e2c_fdatasync` remain the ext2 rows.
- **AF_INET loopback** (main, net.c/fs.c): `bind`/`listen`/`accept`/
  `getsockname` now also accept `FILE_TYPE_SOCKET`; `connect` to 127.0.0.1
  pairs pcbs in-kernel; `bind` to anything but 127.0.0.1/0.0.0.0 is EINVAL.
  The loopback `accept` installs its fd with its own loop over
  `open_fds[]` (fs.c `file_accept`), a second fd-install site for C1-k5.
  Loopback recv spins in `safe_wfi()` without a timeout.
- **FAT16 regular files stat as 0755** (main: Linux vfat's `mode=` default),
  both by path and by fd; `access(X_OK)` passes for them.
- **`exit_group` (231) on the old musl dispatch path** is `SYS_EXIT_GROUP`
  (0x2000, an internal marker, never a syscall number).
- **`badvec` DR0 arming is compiled out** (`BADVEC_WATCH_ENABLE 0`, main):
  it wedged the machine once the window accepted real frames.
- **`VM_OBJ_MAX_BYTES` is 4 GiB**: a 4 GiB object's frame array is 8 MiB of
  contiguous frames; `vm_object_create` at the cap can fail with ENOMEM on a
  fragmented pool where 1 GiB would not have.
- **`LO_ON_DISK`**: a tree that has `third_party/x11/build/intel/prefix`
  (every P4/P5 tree) now builds and stages the seven LibreOffice binaries
  into `disk.img` by default, as main does.  `LO_ON_DISK=` keeps them out.
- **OpenJDK tooling** is in `tools/jdk/` (main).  `build-probes.sh` and
  `stage.sh` take `GLIBC231=$HOME/cp/sysroot-glibc231` on strix; `boot.sh`
  is Mac/TCG-only (edk2 path), so on strix stage through `T3_STAGE` and boot
  with `tools/t3_tier.sh` instead:
  `T3_STAGE="GLIBC231=$HOME/cp/sysroot-glibc231 bash tools/jdk/stage.sh --probe obj/jdk/X.exe:X.EXE --wave X.EXE" T3_MIN_LINES=1 bash tools/cp/qemu-slot.sh bash tools/t3_tier.sh intel`.
  A wave runs its lines concurrently, so two-process checks race.
- **X stack without a download**: `third_party/x11/dl/` (30 sha-pinned
  tarballs) copied from the main checkout lets `third_party/x11/build.sh
  --arch intel --stage all` build offline; `fetch.sh` verifies every sum.
