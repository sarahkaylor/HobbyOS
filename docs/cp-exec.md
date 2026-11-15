# cp/exec — compat execve(2) / execveat(2) (lane C2-exec, P2)

Plan: `docs/cyberpunk.md` §6 P2, §7.2c.  Wine re-execs itself
(`wine` → `wine-preloader` → `wine64`) and starts wineserver by fork+exec; the
native `SYS_EXEC` (`process_exec_current`) maps flat FAT16 images through a
32-byte path, so a Linux program could not replace its own image.

## 1. What exists now

| Piece | File | Role |
|---|---|---|
| rows | `src/kernel/linux_compat_proc.c` (`l_proc_execve`, `l_proc_execveat`, `linux_compat_sig_exec`) | execve(59), execveat(322) in `l_tab_proc`; they run before the legacy SYS_EXEC translation |
| body | `src/kernel/linux_exec.c`, `src/include/linux_exec.h` | `linux_execve()` |
| shared with spawn | `elf_loader_map_buffers`, `elf_loader_read_interp` (elf_loader.c), `compat_auxv_fill`, `set_exe_path` (program_loader.c) | one ELF mapper, one PT_INTERP reader, one auxv writer for spawn and exec |
| image readers | `tmpfs_read_image` (tmpfs.c), `ext2v_read_image`, `fs_read_image` (NFS), `fat16_resolve_path` | the filesystem is chosen as open() chooses it |
| static-pie | `elf_apply_relocations_ex(ELF_RELOC_DEFER_IRELATIVE)` (elf_image.c) | §3 |
| NFS symlinks | `nfs_lookup` (follows), `nfs_readlink`, `nfs_parse_readlink_reply`, `nfs_symlink_join` | §4 |
| paths | `struct process.cwd`, `exe_path` = `PROC_PATH_MAX` (== `VFS_PATH_MAX` 1024) | the compat chdir row widened to match |
| probe | `src/user/linux_probe_exec.c` | execs copies of itself; no helper to stage |

## 2. execve order (Linux fs/exec.c)

1. Copy the path (VFS_PATH_MAX; `-ENAMETOOLONG`), resolve against the cwd or the
   dirfd (`file_path_of`), `AT_EMPTY_PATH`, `AT_SYMLINK_NOFOLLOW` (`-ELOOP`).
2. Copy argv/envp into a frame run, two passes.  `-E2BIG` past Linux's limits
   for the 8 MiB RLIMIT_STACK getrlimit reports: strings + one pointer each
   > 2 MiB, or one string > 32 pages (MAX_ARG_STRLEN).  The filename counts.
3. Read the image whole (tmpfs → ext2 → NFS → FAT16, symlinks followed,
   `TMPFS_EXTERNAL`/`EXT2_EXTERNAL` hops).  Directory / non-regular / no x bit
   → `-EACCES` (FAT16 has no x bit and is executable); empty or not ELF and not
   `#!` → `-ENOEXEC`.  `#!interp [arg]` rewrites argv to
   `[interp, arg?, filename, argv[1..]]`, at most 4 deep (`-ELOOP`).
4. Parse (`ELF_PARSE_ALLOW_INTERP`); a native `/LDHOBBY.SO` image is `-ENOEXEC`.
   Read and parse the PT_INTERP (`-ENOENT`; not ELF → `-ELIBBAD`).  A tmpfs or
   NFS interpreter is read as a file; any other name goes through the spawn
   loader's ext2 + FAT16/8.3-alias resolver.
5. **Point of no return.**  Kill the other threads
   (`process_exec_terminate_siblings`); release the robust list
   (`linux_compat_thread_exit`, Linux exec_mm_release); `clear_child_tid = 0`.
6. Tear the AS down and create a fresh one in the same tgid slot, COMPAT
   layout (C1-mmu F1: floor 0x10000, kernel image a hole).
7. Map image + interpreter (`ELF_LOAD_COMPAT_INTERP`), main stack, heap slot.
8. Startup block on the new stack: AT_EXECFN string (the path as given,
   `/dev/fd/N/...` for a dirfd), "x86_64", the argv+envp strings, AT_RANDOM,
   auxv (`compat_auxv_fill`: the spawn set, no AT_SYSINFO_EHDR), envp[],
   argv[], argc at a 16-aligned rsp.  No 16/24/512 bounds and no
   LD_LIBRARY_PATH fallback (execve passes exactly the environment given).
9. Reset: handlers → SIG_DFL (ignored kept), flag words 0, the exec'ing
   thread's altstack disabled, FS base 0, GS base 0, FPU/XSAVE init state
   loaded live, FD_CLOEXEC sweep.  Kept: pid/tgid, other fds, the signal mask,
   pending signals (Linux; `process_exec_apply` clears them, so they are
   carried over), cwd.  `/proc/self/exe` = the resolved path (tmpfs: the link
   target); comm = basename.
10. Enter: rip = interpreter or image entry, rsp = argc, every GPR 0, RFLAGS IF.

A failure in 6–8 kills the group with SIGSEGV and reschedules (Linux
`force_fatal_sig(SIGSEGV)`); the row then leaves `tf` alone.

Divergences: exec from a secondary thread keeps that thread's pid as its tid
(`gettid() != getpid()` afterwards; Linux moves the leader pid onto it); a
symlink inside one ext2 volume leaves `/proc/self/exe` naming the link; an NFS
directory is `-ENOENT` instead of `-EACCES`; the exec image cap is
`USER_IMG_SIZE` (128 MiB).

## 3. glibc static-pie (`R_X86_64_IRELATIVE`)

The open item of docs/cp-ext2b.md §5.  Cause: the compat loader applied every
RELATIVE and then zeroed DT_RELASZ/DT_RELACOUNT, so glibc's
`_dl_relocate_static_pie` found an empty table and never called the IFUNC
resolvers — the zero `.got.plt` slot in `__libc_setup_tls`.  Linux applies no
relocation to such an image.  Now IRELATIVE entries are validated (slot and
resolver inside the span), left alone and counted; an image with any keeps its
DT_RELA table (re-applying a RELA RELATIVE stores the same value).  Images
without IRELATIVE — every musl probe — load exactly as before.  The auxv was
not involved.

## 4. Symlinks

tmpfs and ext2 image readers follow (`follow = 1`).  NFS had no READLINK:
`nfs_lookup` returned the link's handle and the read failed, which is why the
native spawn_ex of `/CP/WINE/bin/wineboot -> wine` answered ENOENT.
`nfs_lookup` now walks without following, and while the node is NF3LNK reads
the target (READLINK3), joins it against the link's parent
(`nfs_symlink_join`: absolute replaces, `.`/`..` folded, `..` clamps at the
mount root) and walks again, 12 hops at most.  Every NFS caller (open, stat,
spawn, exec) follows, the Linux default; nothing reaches NFS with lstat
semantics.  A symlink in an intermediate component is not followed yet.

## 5. Cross-lane fixes the probe exposed (minimal)

- `process_fork` (process.c) did not copy `fd_cloexec`, so every post-fork
  exec kept its CLOEXEC fds: one `fd_cloexec_copy` line.
- `compat_open` (x64 trap.c, open(2)) ignored O_CLOEXEC (openat honoured it).
- Not fixed, noted: `F_GETFD` on a closed fd answers 0, not `-EBADF`
  (fs.c `file_fcntl`); the probe tests liveness with `dup()`.
