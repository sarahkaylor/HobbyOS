# HobbyOS Kernel Security Remediation Plan

**Scope:** `src/kernel` — all `.c` files under the kernel directory, plus both arch trap dispatchers (`arch/x64/trap.c`, `arch/arm/trap.c`).  
**Status:** Plan only. No code has been modified.  
**Source:** Top-10 security review of the kernel source (read-only audit). Each entry below lists the nature of the issue, the files involved, and a suggested resolution.

---

## 1. TOCTOU between `O_CREAT | O_EXCL` existence check and file creation

### Nature of the issue
`file_open()` in `fs.c` probes for an existing file with `fat16_resolve_path(abs_path)`, and if absent, calls `fat16_open(filename, ...)` which re-resolves the path from scratch to create it. Between the two calls, another process can create the same name, defeating `O_EXCL` semantics. A privileged/setuid program relying on atomic exclusive creation can be tricked into opening (or overwriting) an attacker-controlled file.

### Files involved
- `src/kernel/fs.c` — `file_open()` (O_CREAT | O_EXCL path)
- `src/kernel/fat16.c` — `fat16_open()`, `fat16_resolve_path()`

### Suggested resolution
Make the check-and-create atomic under `fat_lock`: resolve the parent directory once, verify the final component does not exist, and create it in the same locked critical section. Alternatively, have `fat16_open` accept an `O_EXCL` flag and perform the existence check + slot allocation under a single `fat_lock` acquisition, returning `-EEXIST` if the name is already present. Do not split the operation into two separately-locked path resolutions.

---

## 2. User-pointer validation by range only (not `vm_range_ok`) in console write path

### Nature of the issue
The syscall write-to-console path (`sys_write_console` / equivalent) validates a user pointer solely with a range check (`ptr >= USER_VIRT_BASE && ptr < USER_VIRT_BASE + USER_REGION_SIZE`) and then reads byte-by-byte in kernel mode. The comment explicitly assumes "everything inside is assumed to be a valid, mapped user buffer." For v2 demand-paged processes those pages may be non-resident, so a kernel-mode read can fault. If the mapping is installed attacker-controlled after the check, this becomes an arbitrary kernel read.

### Files involved
- `src/kernel/arch/arm/trap.c` — `sys_write_console` (~line 84)
- `src/kernel/arch/x64/trap.c` — equivalent console-write syscall path
- `src/kernel/vm.c` — `vm_range_ok`, `vm_touch` (the correct validation primitives not used here)

### Suggested resolution
Replace the raw range check with `vm_range_ok(ptr, len, 0)` (read permission) before any kernel-mode dereference, and use the existing `u_strcpy`/byte-copy helpers that fault-safely copy from user memory. For demand-paged images, ensure the pages are touched/resident before the kernel reads them, or copy through a fault-safe wrapper that converts a page fault into `-EFAULT` rather than a kernel oops.

---

## 3. Per-page TOCTOU in `u_strcpy()`

### Nature of the issue
`u_strcpy()` validates each page with `sys_user_range_ok(&src[i], 1, 0)` and immediately reads `src[i]`. Between the validation and the read, the page can be unmapped or repinned by another thread. This is a classic validate-then-use race: validate page N, attacker unmaps it, the subsequent read faults in kernel mode.

### Files involved
- `src/kernel/arch/x64/trap.c` — `u_strcpy()` (~lines 800–860)
- `src/kernel/arch/arm/trap.c` — equivalent `u_strcpy()`

### Suggested resolution
Copy the entire user string in one fault-safe operation rather than page-by-page with intervening validation. Best approach: validate the full range once with `vm_range_ok(src, len, 0)`, then copy through a fault-safe primitive that returns `-EFAULT` on any page fault instead of oopsing. If per-page validation is retained, hold the relevant page-table lock (or pin the pages) across the validate+read window so the mapping cannot change in between.

---

## 4. No SMEP/SMAP equivalent, no stack canaries (architectural)

### Nature of the issue
The kernel and user processes share the same page tables (kernel direct-mapped region + user region). There is no SMEP/SMAP-like hardware bit preventing kernel access to user memory, and no stack canaries to detect stack buffer overflows. Every syscall runs with the caller's address space active, so any kernel bug that dereferences an attacker-controlled pointer becomes arbitrary kernel read/write. Stack smashing is silent.

### Files involved
- `src/kernel/arch/x64/trap.c`, `src/kernel/arch/arm/trap.c` — syscall entry/trap dispatch
- `src/kernel/vm.c` — page-table setup (kernel + user regions)
- `src/kernel/process.c` — kernel stack allocation for each process

### Suggested resolution
- Add stack canaries to kernel syscall entry: push a canary on entry, verify on exit, and panic on mismatch.
- Evaluate enabling SMAP (x64) / PAN (ARM) at boot so the kernel cannot accidentally dereference user pointers without explicitly disabling the protection, then audit every user-pointer dereference to use the proper accessor.
- Add a guard page at the end of each kernel stack (if the MMU supports it) to catch overflow before it reaches the trap frame or adjacent kernel data.

---

## 5. Close-time dirent write-back can clobber concurrent changes

### Nature of the issue
`fat16_close()` → `fat16_sync_entry()` writes back the cached directory entry at close time. The W9 guard only compares the 11-byte name; if another process renamed or recreated the file (same name, different cluster/size) between open and close, the close overwrites the new entry's size/cluster fields with the stale cached copy. This can point a file at the wrong clusters or the wrong size — silent filesystem corruption.

### Files involved
- `src/kernel/fat16.c` — `fat16_close()`, `fat16_sync_entry()`, `fat16_dir_entry_write()`
- `src/kernel/fs.c` — `file_close()` (calls into fat16 close)

### Suggested resolution
Strengthen the guard in `fat16_sync_entry`: before writing back, verify the on-disk entry's cluster, size, and attributes still match the cached copy (not just the name). If they differ, skip the write-back and return an error to the caller so the application can detect the conflict. Alternatively, track a generation/sequence per directory slot and refuse write-back when the slot's generation has changed.

---

## 6. Default file slot type is `FILE_TYPE_FAT16`

### Nature of the issue
`file_alloc()` in `fs.c` sets a free slot's `type` to `FILE_TYPE_FAT16` as the default; callers are expected to overwrite it before use. If a caller fails between allocation and type-setting, a later operation on that slot treats it as a FAT16 file with uninitialized `start_cluster`, `cursor`, etc., leading to out-of-bounds disk reads/writes.

### Files involved
- `src/kernel/fs.c` — `file_alloc()`, all callers that allocate a slot then set `type`
- `src/kernel/fat16.c` — any path reading `f->fat16.*` fields

### Suggested resolution
Initialize newly allocated file slots to a distinct "unused/free" sentinel type (e.g. `FILE_TYPE_NONE` or `FILE_TYPE_INVALID`) and assert/panic if any operation is attempted on a slot whose type is not one of the real types. Make every allocation path explicitly set the real type before the slot is visible to other code, and add a debug invariant check in `file_close`/`file_read`/`file_write` that the type is valid.

---

## 7. User rect fields copied into kernel struct before clamping (`sys_flush_fb_rects`)

### Nature of the issue
`sys_flush_fb_rects()` copies raw `int32_t` x/y/w/h from user memory into a kernel `struct virtio_gpu_xrect krects[32]` array, and `virtio_gpu_rect_clamp()` runs only afterward. Extreme raw values live in kernel memory before any validation. If the clamp path has an edge-case bug (integer overflow, sign mishandling), attacker-controlled rects can drive the GPU driver with out-of-range values.

### Files involved
- `src/kernel/arch/x64/trap.c` — `sys_flush_fb_rects()` (~lines 1500–1560)
- `src/kernel/arch/arm/trap.c` — equivalent path
- `src/kernel/virtio_gpu.c` — `virtio_gpu_rect_clamp()`

### Suggested resolution
Validate each rect's fields (range, sign, `w > 0 && h > 0`, within framebuffer bounds) in user space *before* copying into the kernel struct, or clamp immediately per-field during the copy. Reject any rect that fails validation with `-EINVAL` rather than clamping after the fact. Keep `count <= 32` (already done) and add an upper bound on the total pixel area to avoid excessive GPU work.

---

## 8. Cross-context write page-table race in `vm_kwrite()`

### Nature of the issue
`vm_kwrite()` is used to write reap status and rusage into the parent's address space. The caller validates with `vm_touch()`/`vm_range_ok()` first, then `vm_kwrite` walks page tables byte-by-byte. If the parent's page tables change between the touch and the write (e.g. the parent is being torn down), the write targets a stale physical page — writing into memory that may have been freed and reused by another process.

### Files involved
- `src/kernel/vm.c` — `vm_kwrite()` (~lines 1350–1400)
- `src/kernel/process.c` — callers writing child exit status/rusage

### Suggested resolution
Pin the target pages (increment a pin count or take the relevant mmap lock) for the duration of the `vm_kwrite` operation, or re-validate inside `vm_kwrite` immediately before each write. Alternatively, copy the data through a temporary kernel buffer and have the parent read it via a separate, synchronized syscall, avoiding cross-context writes entirely. Document which callers must hold the parent's mmap lock across the touch+write sequence.

---

## 9. FD close during process teardown without per-file locks

### Nature of the issue
`process_kill_group_sig()` releases `proc_lock`, then iterates `p->open_fds` and calls `fs_close_global()`/`file_close()` for each. Another thread sharing the process (or a late fork child) could be mutating the fd table concurrently. This risks use-after-free or double-close of a file descriptor.

### Files involved
- `src/kernel/process.c` — `process_kill_group_sig()` (~lines 1700–1760)
- `src/kernel/fs.c` — `file_close()`, `fs_close_global()`
- `src/kernel/pipe.c` — `pipe_close()` (called during fd teardown)

### Suggested resolution
Hold the process's fd-table lock (or `proc_lock`) for the entire teardown walk, or snapshot the fd list under the lock and close each fd while re-acquiring the per-file lock at close time. Ensure `file_close` is idempotent (safe to call twice on the same fd) and that closing an fd already being closed by another thread is a no-op. Add a "closing" flag per fd to prevent double-close.

---

## 10. No stack canaries / no stack overflow guard (architectural)

### Nature of the issue
Kernel stacks are fixed-size with no canary and no guard page; the trap frame sits at the top of the stack. A deep recursion or large stack allocation in a syscall handler silently overflows into the trap frame or adjacent kernel data, corrupting control flow. This duplicates entry #4's canary point but is listed separately because it concerns stack layout rather than user-pointer dereference; the two should be fixed together.

### Files involved
- `src/kernel/process.c` — kernel stack allocation
- `src/kernel/arch/x64/trap.c`, `src/kernel/arch/arm/trap.c` — trap frame placement at stack top

### Suggested resolution
Place a canary between the trap frame and the rest of the stack, verify on syscall exit, and panic on corruption. If the MMU allows, map a guard page immediately below each kernel stack so overflow faults immediately instead of corrupting data. Set a maximum syscall recursion depth and audit syscall handlers for large stack allocations (>1–2 KiB) that should be heap-allocated instead.

---

## Notable observations (track, not in top 10)

### A. `spawn_args_pool[caller->pid]` / `spawn_ex_pool[caller->pid]` indexed by PID
Both trap dispatchers index a fixed pool by `caller->pid`. If `MAX_PROCESSES` exceeds the pool size, this is an out-of-bounds read/write. Could not confirm the pool size vs. `MAX_PROCESSES` from the declarations read; verify `process.h`.

**Files:** `src/kernel/arch/x64/trap.c`, `src/kernel/arch/arm/trap.c`, `src/kernel/process.h`  
**Resolution:** Add a compile-time `static_assert` (or runtime check at boot) that the pool size >= `MAX_PROCESS
ES`, or index by a dense process-slot index instead of raw PID.

### B. `fat_lock` held across virtio sector I/O
The block cache and FAT16 paths hold `fat_lock` across `virtio_blk_read_sector`/`write_sector` calls. No lock-order inversion was observed, but the documented order (`proc_lock` → file lock → pipe lock) must be respected wherever these paths intersect process teardown or pipe ops.

**Files:** `src/kernel/fat16.c`, `src/kernel/blk_cache.c`, `src/kernel/virtio_blk.c`  
**Resolution:** Document the lock order in `lock.h` and add lock-order assertions (debug builds) to catch inversions.

### C. Pipe SIGPIPE path handled correctly
`pipe_write()`'s SIGPIPE path yields via `schedule(tf, 0)` after the group is killed, so it does not return to a released image. No action needed.

---

## Remediation order (suggested)

1. **Issue 2 & 3** (user-pointer validation TOCTOU) — highest exploitability, affects every syscall that copies from user memory.
2. **Issue 4 & 10** (canaries / SMEP/SMAP / guard pages) — architectural, defense-in-depth.
3. **Issue 1** (`O_EXCL` TOCTOU) — logic flaw in file creation.
4. **Issue 5** (dirent write-back clobber) — silent filesystem corruption.
5. **Issue 6** (default file type) — uninitialized-field risk.
6. **Issue 7** (rect pre-clamp copy) — GPU driver input validation.
7. **Issue 8** (`vm_kwrite` race) — cross-context write hazard.
8. **Issue 9** (teardown fd close) — use-after-free risk.
9. **Observation A** (PID-indexed pool bounds) — verify and assert.
10. **Observation B** (lock ordering) — document and assert.

Each fix should be paired with a regression test (unit test in `src/host/` or a kernel self-test) before being merged, consistent with the project's existing test-driven approach.
