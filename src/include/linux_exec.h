#ifndef LINUX_EXEC_H
#define LINUX_EXEC_H

/*
 * linux_exec.h - the compat (Linux-ABI) execve(2) / execveat(2) (cp/exec,
 * docs/cp-exec.md).  The rows live in linux_compat_proc.c (l_tab_proc); the
 * body is src/kernel/linux_exec.c.
 *
 * What it is: the spawn loader's ELF machinery (elf_loader_map_buffers, the
 * shared PT_INTERP reader, the compat auxv) driven from inside a running
 * compat process, with an image read from any filesystem the compat VFS
 * routes -- tmpfs /tmp, ext2 /EXT, an NFS mount, FAT16 -- by a full
 * VFS_PATH_MAX path (relative to the cwd or to a dirfd, symlinks followed).
 */

#include <stdint.h>

struct process;
struct trap_frame;

/* Linux limits this kernel applies, as Linux 5.x computes them for the 8 MiB
 * RLIMIT_STACK getrlimit reports: argv+envp strings (NULs included) plus one
 * pointer per string must stay within min(6 MiB, 8 MiB / 4) = 2 MiB, and one
 * string within MAX_ARG_STRLEN (32 pages, NUL included); otherwise E2BIG. */
#define LX_EXEC_ARG_MAX (2ULL << 20)
#define LX_EXEC_STRLEN_MAX (32ULL * 4096ULL)

/* Return values besides -errno. */
#define LX_EXEC_DONE 0 /* the new image is installed; tf enters it */
#define LX_EXEC_GONE 1 /* failed past the point of no return: the process was
                          killed with SIGSEGV and the CPU rescheduled; the
                          caller must not touch tf */

/* execveat(dirfd, upath, uargv, uenvp, flags); execve is
 * (L_AT_FDCWD, ..., 0).  flags: AT_EMPTY_PATH, AT_SYMLINK_NOFOLLOW; anything
 * else is -EINVAL.  Before the point of no return every failure is a -errno
 * with the caller untouched: -EFAULT, -ENAMETOOLONG, -ENOENT, -ENOTDIR,
 * -EACCES (no x bit, a directory, a non-regular file), -ENOEXEC (not ELF and
 * not "#!", empty, wrong class/machine, a native HobbyOS image), -E2BIG,
 * -ELOOP (AT_SYMLINK_NOFOLLOW on a link, or #! nested too deep), -ELIBBAD
 * (the interpreter is not ELF), -ENOMEM. */
int linux_execve(struct process *p, struct trap_frame *tf, int dirfd,
                 uint64_t upath, uint64_t uargv, uint64_t uenvp, int flags);

#endif /* LINUX_EXEC_H */
