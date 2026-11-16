# cp/wine2 — toward Gate P2 (lane C2-wine2, wave 3)

Plan: `docs/cyberpunk.md` §6 P2 and §7.2c (C2-wine2 row).  Wine citations are
`file:line` in the pinned tree (`strix:~/cp/wine-build/src/wine-11.0`).  Every
number below was measured on **strix** (Ryzen AI MAX+ 395, Zen 5, AVX-512, KVM).

## 1. `MAP_PRIVATE` copy-on-write of file-backed objects

**Before:** `vm_mmap` answered `-ENOTSUP` for `MAP_PRIVATE` of a memfd or a
tmpfs file (`[LCOMPAT] mmap fail ... -> -95 path=/tmp/.wine-0/server-…/tmpmap-…`),
so `server/mapping.c:340-381` `check_current_dir_for_exec` moved every section
file (KUSER_SHARED_DATA among them) onto the FAT config dir, and every
read-only section view the client maps (`dlls/ntdll/unix/virtual.c:2339-2398`
`map_file_into_view`: read-only views are `MAP_PRIVATE` on Linux *because*
file changes stay visible through them) would have hit the same `-ENOTSUP`,
which Wine turns into `STATUS_NO_MEMORY` (only `EINVAL`/`ENOEXEC`/`ENODEV`/
`EACCES`/`EPERM` fall back to `read()`).

**Now (Linux semantics):**

| access to a page of a private object mapping | result |
|---|---|
| read / exec, page absent | the **object's own frame**, mapped without write (`VMK_SHARED` leaf: never freed by unmap/teardown).  Later `write()`s and `MAP_SHARED` stores by any process are visible through it |
| write, page absent | a **private copy** of the object's page (zeros if the object never materialized it — the object is not grown) |
| write, object frame resident read-only | the existing COW fault (`vm_cow_page_locked`): copy, remap writable |
| kernel-side write (`read()` into it, any `vm_kwrite`) | `vm_touch` breaks any read-only leaf in a private object region first |
| `mprotect` | unchanged rule: never grants write on an object frame in a private region; private copies keep the region's protection |
| `MADV_DONTNEED` | drops private copies: the page reads the object again (Linux) |
| fork | object-frame leaves stay the object's frame and read-only in the child (now for every object kind: memfd and NFS were missing); a parent COW copy is copied for the child, also when the object has no frame at that offset (that case used to try to "refill" the object and failed the fork for FAT/ext2) |
| `F_SEAL_WRITE` | refuses `MAP_SHARED`+`PROT_WRITE` only (Linux); a private writable view is allowed |

Owned code: `src/kernel/vm.c` (`vm_obj_private_fill_locked`,
`vm_touch_leaf_ro_locked`, the memfd branch of `vm_mmap`, the fault/touch
hooks, `vm_as_clone_into`), `src/kernel/linux_compat_mem.c` (`l_mem_mmap` no
longer rewrites `-1` to `-ENOMEM`: with an address space a `-1` *is*
`-EPERM`, the seal refusal).  A private region over an object is `VMK_LIB`
(as a private file mapping), never `VMK_SHARED`: its COW copies are mapped with
the region's kind and a SHARED-marked leaf is never freed.

FAT (`VM_OBJ_FILE`), NFS (`VM_OBJ_NFS`) and ext2 (`VM_OBJ_EXT2`) already had
private COW through `vm_file_obj_materialize`; they do not share the new fill
path.  ext2 is re-verified here (probe rows C14).

**Two original known limits, fixed after the first merge (625add68):**
- **`ftruncate` shrink under live mappings** used to free object frames that
  `MAP_SHARED` views and read-through private pages still mapped — a
  use-after-free once a frame was reused.  `vm_object_truncate` now shrinks
  in two phases: under `vm_lock` it drops every leaf, in every v2 address
  space (`vm_as_pool`), that maps a cut-off object frame — plus, in private
  regions, the COW copies of those pages, as Linux's `truncate_pagecache`
  does (`even_cows`) — and shrinks the object, leaving the frames *parked*
  past `nr_frames`; then it shoots down the touched address spaces and frees
  what is still parked.  A regrow over a parked frame zeroes it; object free
  covers parked frames.  A touch past the new end faults (HobbyOS kills with
  SIGSEGV; Linux sends SIGBUS).  Private COW copies freed in phase 1 follow
  the munmap rule (local flush, then the shootdown).
- **`mremap(MREMAP_MAYMOVE)` of a private object mapping** carries its COW
  copies (`vm_mremap_carry_private`); pages still reading through refault
  from the object at the new address.  Also: `MREMAP_FIXED` always moves (it
  returned the old address when the size did not grow).

**Known limits still open:**
- FAT private mappings do not see later `write()`s (every FAT write
  invalidates the page-cache object); tmpfs/memfd/ext2 do.
- `munmap` (pre-existing) flushes only the local TLB; other CPUs running
  threads of the same group are not shot down.

**Evidence:** `src/user/linux_probe_cow.c` (C1–C16) passes on the Linux host
(strix, reference) and in HobbyOS; base fails 11 rows (all the `-95`s plus the
`EPERM`→`ENOMEM` rewrite).  Unit test `test_vm_memfd_private_cow`
(`vm_test.c`) checks frames exactly (object frame identity, writability, fork,
and `frame_free_count()` back to baseline).

## 2. The P2 harness (`tools/cp/run-wine-p2.sh`)

One boot, one native launcher (`src/user/winep2.c`, the only wave member):
`ps` → NFS dir counts (`tools/cp/p2/pfxls.c`) → `wineboot -i` (WINEPREFIX
`/tmp/pfx` on the compat tmpfs) → wait for the whole wineboot tree to exit →
prefix listing → `wine cmd /c echo hello` → `wine Z:\CP\WPROBE\winprobe.exe` →
`cmd /c copy` + `wine C:\winprobe.exe` (the PE mapped from tmpfs, extra) →
`wineserver -k` → `ps`.  Graded from the serial log: **G1** a `hello` line in
the echo span and exit 0; **G2** every marker in `tools/cp/winprobe-markers.txt`
(41, taken from the host reference) plus `[WP] done rc=0` and exit 0; **G3**
`wineserver -k` exits 0 and the after-census has no process the before-census
lacked.

Staging: the strix-built `/CP/WINE` over a temporary NFS export,
`127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)`
only.  Everything that touches `/etc/exports` runs under
`/tmp/cp-exports.lock` (outer to the QEMU lock); the exit path removes this
run's exact line and `cmp`s the file against the pre-run copy; the export dir
is removed.  Iteration knobs (never a gate result): `P2_BOOT_ONLY`,
`P2_LS_ONLY`, `P2_STEP_WINEDEBUG`, `P2_WINEDEBUG`.

## 3. `winprobe.exe` (`src/user/cp/winprobe.c`)

Built by `tools/cp/build-winprobe.sh` with clang 21 `-target x86_64-windows`,
Wine's headers and PE import libs (winegcc, no MinGW).  W1 TEB via
`%gs:[0x30]`, W2 threads, W3 TLS (+ TEB TlsSlots), W4 SEH (`__try/__except`
on #AV and #DE, a vectored handler for both), W5 QPC, W6 GetTickCount64 and
the live KUSER page, W7 `C:\` file I/O.  41/41 on the Linux host (pinned 2.31
chroot, `docs/cp-wine2-evidence/winprobe-host-reference.txt`).

## 4. What the P2 gate found and fixed (strix)

| # | symptom under Wine | cause | fix |
|---|---|---|---|
| 1 | preloader: `/lib64/ld-linux-x86-64.so.2: could not open` | `compat_open` translated the full path to cwd-relative `LD64.SO` | real path first (`trap.c`) |
| 2 | wineboot.exe AV at `mov %eax,0x260(%rsi)`, rsi = `&"\n"` in kernel rodata | `trap_exit_frame_check` ran after the user GPRs were reloaded and saved only rax/rcx/rdi; its window flagged every compat RIP < 64 GiB (`[BADIRET]`) | save rsi/rdx/r8-r11; only the F1 hole is implausible |
| 3 | (latent) rdi/rsi changed by openat/newfstatat fallbacks | argument shuffle in the trap frame | restored |
| 4 | no Wine debug output at all | console and `/dev/null` both `st_rdev` 0 → Wine's `init_options` saw stderr == /dev/null | Linux device numbers (`fs.c`) |
| 5 | next step's `wine` SIGKILLed | pid reused at once; wineserver's `process_sigkill` hit it | launcher waits for quiescence; C1-thr's cyclic pids (3521564d) |
| 6 | `could not load kernel32.dll, status c0000135` in every process after wineboot | NFS readdir stopped at 256 entries → fake-DLL install skipped kernel32/ntdll/start/cmd (337 of 795 system32 entries) | `NFS_DIR_CACHE_MAX` 4096 (`nfs.h`) |
| 7 | winprobe W4 `#DE`: `Got unexpected trap 0`, process dead | every user exception but #PF was SIGSEGV; int3 was a #GP (DPL 0 gate) | Linux vector→signal map for compat; DPL 3 for #BP/#OF (`trap.c`); `linux_probe_trapsig` |

Results (strix, KVM, 12 GiB guest): run16 — G1 PASS, G3 PASS, G2 stopped at
W4 #DE (fix 7).  run18 (after fix 7) — `hello` + exit 0, winprobe `[WP] done
rc=0` + exit 0 from `Z:` **and** from `C:\` on tmpfs, `wineserver -k` exit 0,
`ps` after = before; graded FAIL only by two grader defects (fixed: no
`pipefail` around `span | grep -q`; one `WriteFile` per winprobe row so kernel
rows cannot splice into markers).  run17/run19 — the wineboot process tree
hung with every client BLOCKED (no futex, no wake timer) and wineserver in its
poll loop: a lost wakeup, the shape of the still-open pipe park race.

**Open:** that hang (C1-thr: pipe.c read/write parks); wineboot's install
over NFS takes ~15 min (1.3 KiB per READ RPC; the 5-minute boot-event wait in
Wine expires, `status=53`, and the tree finishes on its own); fstat of an NFS
fd reports mtime 0 (path stat is right) so `.update-timestamp` holds `0`.
