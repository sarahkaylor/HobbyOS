# Merge `rimworld` → `main`: resolution record

Branch `rimworld` (168 commits ahead of `github/main`, 183 behind) was merged
into `main` at `ecaae2d6`. The merge had **29 conflict hunks in 8 files**. This
document records how each hunk was resolved, which invariant it carries, and
what a reviewer should look at first.

Rule applied throughout: **keep upstream's structure where upstream refactored
a function, and re-apply this branch's semantic change on top.** A side is
dropped only where it is *provably* superseded; those cases are called out.

## Per-file resolution table

| File | Hunks | Resolution | Invariant preserved |
|---|---|---|---|
| `src/include/syscall.h` | 1 | Both sides appended a row at 104. Upstream's GB-173 `SYS_EVDEV_PUSH` keeps **104** (it is dispatched in `main`); `SYS_VFS_BIND` moves to **105**, `SYS_MAX` to 105. Only the header spells the number; the one doc row that did (`docs/rimworld-plan.md`) is updated. | — (ABI numbering; no collision) |
| `src/include/program_loader.h` | 1 | Comment only (`exe_entry`). Kept this branch's wording, which also names the AT_ENTRY contract. | (b) signal/auxv contract |
| `src/kernel/arch/x64/trap.c` | 9 | Path copies: upstream GB-178's `copy_user_path` kept as the path-shaped helper, but its body now reads every byte through `vm_user_read` (the old per-byte validate-then-load was a TOCTOU and let a string ending on a region boundary walk the kernel into the hole). `sys_open` / `sys_unlink` / `sys_mkdir` / `sys_chdir` / `compat_open` therefore keep upstream's structure and gain the hole-safety in one place. Upstream's `/dev` node table (GB-171 evdev + GB-167 fb0) and `compat_open` (GB-201 SONAME→8.3) are kept whole. Upstream's inline `arch_prctl(158)` arm is **dropped as provably superseded**: `l_tab_thr`'s `l_thr_arch_prctl` claims 158 in `linux_compat_dispatch()` before the switch and answers SET_FS identically, plus GET_FS/GET_CPUID. `open(2)` keeps upstream's `compat_open`. Watchdog/NFSIO and kernel-#PF diagnostics keep this branch's rows but write through the lock-free *non-panic* sink (`wd_puts`/`wd_hex`), because GB-097 reserves `panic_*` for non-returning callers. | (a)(b) untouched here; (d) untouched; single path-copy guarantee |
| `src/kernel/vm.c` | 5 | `vm_file_obj_materialize` keeps this branch's function (NFS page source, exec verification on *every* publish path, data-page rescue, `vm_pub_note`) and re-integrates upstream GB-201's beyond-EOF zero tail as `beyond_eof` (a page past `nr_frames` is a zero frame and is never copied or verified; a page *inside* the file that fails to read is still `-EIO`). `/dev/fb0` mmap keeps this branch's `hb_fb_mmap_translate` window rules plus upstream's NULL-framebuffer `-ENODEV` check; file/NFS mapping keeps the NFS branch and the ld.so "map longer than the file" rule. | (d) exec page-in verified, refused page is `-EIO`; (c) untouched |
| `src/kernel/fs.c` | 5 | Union of upstream's GB-171/GB-173 evdev node and GB-167 fb0 node with this branch's `/dev/console` (M2 stdio) and `/dev/gpu` (M1) nodes, in `file_open`, `file_read` and `file_write`. The devfb/devinput arms are upstream's, verbatim. | (h) untouched; (k) `/dev/gpu` node |
| `src/kernel/elf_loader.c` | 3 | One loader for both filesystems: this branch's `elf_loader_load_image()` + `el_map_pair()` (PT_INTERP for the Linux track under `ELF_LOAD_COMPAT_INTERP`, mapped and *not* relocated so ld.so self-relocates; native `hb_start` page only on the native track). Upstream's `el_compat_resolve()` (name → basename → 8.3 alias) is **kept and now used** by `load_image`, so a glibc PT_INTERP path still finds `LD64.SO`. **Dropped as superseded:** upstream's inline compat-interp arm in `elf_loader_load()` and its `el_map_closure()` / `el_dyn_*` DT_NEEDED pre-map. Upstream's mechanism pre-mapped the dependency closure in the kernel; on this tree the entered ld.so maps its own closure from `/LIB64` (the startup block exports `LD_LIBRARY_PATH=/LIB64` and `compat_open` resolves SONAMEs), which the M2/M3/M4 gates exercise — and only this shape can also serve an exec from NFS/VFS. Upstream's `GB199_NO_AUXV` falsifier at the call site is kept. | (d) exec page-in; (k) nothing here |
| `src/kernel/program_loader.c` | 3 | Kept this branch's `compat_stack_block` (full auxv set: PHDR/PHENT/PHNUM/PAGESZ/BASE/FLAGS/ENTRY/UID..EGID/PLATFORM/HWCAP/CLKTCK/SECURE/RANDOM/HWCAP2/EXECFN, `LD_LIBRARY_PATH=/LIB64` injection, auxv placed exactly at envp's end, argv 8 mod 16). Upstream's `GB199_NO_AUXV` falsifier is kept and adapted to this signature; the auxv log line now prints `entry` (this branch's name for the executable entry). | (a)(b) auxv/AT_ENTRY contract |
| `src/kernel/linux_compat_fd.c` | 2 | Includes unioned. `/dev/fb0` ioctl keeps this branch's kernel-buffer + `hb_fb_ioctl_route()` + `l_copy_to_user` path (the route helper never sees a user pointer; both structs and FBIOBLANK, bounded by `hb_fb_ioctl_arg_size`) and adds `/dev/gpu`'s `gpu_abi_ioctl()`. | (k) `/dev/gpu` ABI; no user pointer in the fb fill |

## Conflict-free merge damage repaired

These were not conflict hunks, but the auto-merge or an upstream rename left
them broken; each is a one-line/small fix, listed so a reviewer can see them:

1. **`src/include/fs.h`** — both sides added `FILE_TYPE_DEVFB` (upstream GB-167
   and this branch's M3). De-duplicated to one enumerator at upstream's
   position, with both comments merged. Without this the tree does not compile.
2. **`src/kernel/arch/x64/trap.c`, `src/kernel/frame.c`** — upstream's GB-097
   renamed the raw console sink: `uart_puts_raw`/`uart_print_hex_raw` no longer
   exist on x64 (they are `panic_puts`/`panic_print_hex`, non-returning paths
   only; the returning-path sink is `uart_putc_dbg`). This branch's diagnostics
   were converted: fatal paths (`#PF` kill, fault-pc report) use `panic_*`; the
   watchdog and the frame-pool tripwires use `uart_putc_dbg` wrappers.
3. **`src/kernel/fs.c`** — `console_write_user()` wrote through `panic_putc`
   while holding `uart_lock`; after GB-097 that reports a
   `[PANIC-SINK-VIOLATION]` on every console write. Now `uart_putc_dbg`.
4. **`src/kernel/main.c`, `src/kernel/arch/arm/uart.c`** — `print_int()` and
   `uart_print_hex()` took `uart_lock` and then called the panic sink, so every
   `print_int` on a returning path reported a sink violation (visible at boot in
   the `[VIRGL3D] capset_info` row). They now use `uart_putc_dbg`; ARM gains the
   same named sink.
5. **`src/kernel/vm_test.c`** — upstream's GB-217 reclaims an AS-pool entry with
   no live owner instead of refusing the create. The unit tier has no PCB for
   tag 60, so `test_vm_as_lifecycle` planted a CPU claim for the collision
   assertion (the way `process_test.c` does) and a new
   `test_vm_as_orphan_reclaim` asserts the reclaim half (create, abandon,
   create again → non-NULL and the reclaim counter increments).

## Invariants checked after resolution

| Invariant | Where verified |
|---|---|
| (a) compat signal frames staged through kernel RAM; no FXSAVE on a user address | `src/kernel/fpu_user.c` (staging + `arch_fpu_user_image_ok` + `vm_kwrite`/`vm_user_read`) |
| (b) fpstate offset derived from the frame base | `l_sf_fp_off()` in `src/kernel/linux_compat_thr.c` |
| (c) `vm_lock` covers base-select **and** insert (mmap/mremap/MAP_FIXED, JIT arena carve) | one critical section in `vm_mmap`; `vm_jit_alloc_locked` inside it |
| (d) exec page-in verified on every publish path; refused page is `-EIO`/SIGBUS | `vm_file_obj_materialize` (`exec_backed`, `vm_exec_page_verify`, `-EIO`) |
| (e) fd table 256, word-array CLOEXEC mask, atomic install | `MAX_OPEN_FDS 256`; `fd_cloexec_{set,test,clear}` on the word array |
| (f) `sys_brk` walks a split heap | `sys_brk`'s fragment walk (`VMK_HEAP` per fragment, insert only the uncovered tail) |
| (g) NFS: 64-byte dirent names, collision-free ports, one READ retry, xid matching, cached socket per mount | `NFS_NAME_MAX 64`; `net.c` port scan; `nfs.c` retry + `rpc_parse_reply(xid)` + `rd_sock` |
| (h) FAT16: per-path write lock, O_APPEND, no NUL holes | `wr_locks[]`/`wr_slot()` around the whole write; write-time O_APPEND; hole fix |
| (i) NVMe: single in-flight token outside any lock, phase-tagged CQEs, full re-init | `blk_token_*`, `io_cq_phase`, `nvme_reinit()` (CAP/VS, CC, admin+IO queues, namespace Identify) |
| (j) frame pool RAM-only, `hole=` tripwire, extent 1 at 0x100000000 | `frame.c` extents + `frame_ram_probe` + `frame_hole_allocs_n`; boot row `ext1=1 … hole=0` |
| (k) virtio-gpu 3D: VIRGL negotiation, `/dev/gpu` ABI, 4096 resources/client | `GPU_ABI_MAX_RES_PER_CLIENT 4096`; boot row `feat virgl=1 capset id=1` |

## Verification (this tree)

- `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **Tests run: 436, Tests failed: 0**
  (435 inherited + 1 new GB-217 reclaim test).
- `TOOLS_SKIP=1 bash tools/rw/l58-gates.sh` → **10 passed, 0 failed**:
  unit, redzone, mmap_race, m2_mono, m2_tp, m2_threads, m12, m3_window,
  m4_gl, rwnfs.
- Desktop smoke boot (`MODE=desktop DESKTOP=hobby`, `virtio-gpu-gl-pci`,
  `egl-headless,gl=on`, 90–100 s): `Desktop starting...`, 60 menu rows,
  `[FRAME] ram ext0=1 ext1=1`, `[BLKD] … hole=0 mmio=1`, **0** `#PF`/`#UD`/
  `#GP` fault rows, **0** `[PANIC-SINK-VIOLATION]`, no `[KERNEL] FATAL`.
- `bash tools/rw/check-no-game-files.sh` → OK.

## Risky hunks — human review recommended

1. **`src/kernel/elf_loader.c`: upstream GB-199's eager DT_NEEDED closure map
   was not carried.** This is the largest deliberate deletion (~300 lines:
   `el_map_closure`, `el_dyn_view`, `el_dyn_next_needed`, `el_closure_seen`,
   and the inline compat-interp arm in `elf_loader_load`). Rationale: the two
   branches implemented compat PT_INTERP independently; this branch's
   `elf_loader_load_image`/`el_map_pair` is the one the unit tests and the
   M2/M3/M4 gates drive, and it is required for exec-from-NFS. Upstream's
   resolver (`el_compat_resolve`) is kept and now used. If a future program
   relies on the kernel pre-mapping `DT_NEEDED` (rather than ld.so doing it),
   that behaviour is gone.
2. **`src/include/syscall.h`: `SYS_VFS_BIND` renumbered 104 → 105** because
   upstream's GB-173 `SYS_EVDEV_PUSH` owns 104. No compiled caller spelled the
   number; any out-of-tree binary that hard-coded 104 for `vfs_bind` would now
   call `evdev_push`.
3. **`src/kernel/vm.c`: `beyond_eof` interaction with the exec gate.** A page
   past a file's last page is published as a zero frame and skips
   `vm_exec_page_verify` by construction (it has no file bytes). An exec page
   *inside* the file still cannot be zero-filled (`-EIO`). This is upstream
   GB-201's rule, restated in this branch's control flow; it is the one place
   where "never publish an unverified exec page" is intentionally narrowed.
4. **`src/kernel/main.c` / `uart.c`: `print_int`/`uart_print_hex` moved off the
   panic sink.** This is a behaviour change in upstream code (it removes the
   sink-violation rows that upstream's tree also emits). It is required for the
   GB-097 contract, but it modifies upstream-owned helpers.
