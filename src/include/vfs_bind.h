#ifndef VFS_BIND_H
#define VFS_BIND_H

/*
 * vfs_bind.h - prefix ("bind mount") aliases inside the VFS namespace.
 *
 * Why: a path that lives under an NFS mount cannot be `mmap`ed, because the
 * only file-backed VM object on this kernel is FAT16's (M5b is the NFS
 * page-in object and is not landed).  Mono `mmap`s every managed assembly, so
 * the game needs `RimWorldLinux_Data/Managed/` - 40 MiB, 102 DLLs - to be
 * reached through FAT16 while the other 730 MiB of `Data/` stays on NFS.
 * A bind makes one subtree of the namespace resolve to a *different*
 * absolute path, so opens, stats, readdirs and therefore mmaps land on the
 * FAT volume without the engine knowing anything changed (M5c, AD-10).
 *
 * Semantics, deliberately small:
 *   - a bind is (from -> to), both absolute, matched on component
 *     boundaries: "/A/B" matches "/A/B" and "/A/B/c", never "/A/BC";
 *   - the LONGEST matching `from` wins;
 *   - exactly one substitution is applied, and a registered bind is required
 *     to be non-recursive (see the add-time checks), so applying the
 *     translation twice is the identity.  Paths cross the VFS layer more than
 *     once (openat canonicalises, file_open canonicalises again), so
 *     idempotence is a correctness requirement, not a nicety;
 *   - no allocation, no locking: the table is written by the launcher before
 *     it spawns the game and read-only afterwards.
 */

#define VFS_BIND_MAX      4
#define VFS_BIND_PATH_MAX 160

/* Register `from -> to`.  Returns
 *    0  registered
 *   -1  malformed path (not absolute, "/", empty, "." / ".." component,
 *       "//", or longer than VFS_BIND_PATH_MAX - 1)
 *   -2  the bind would chain or recurse into a registered one, or `to` lies
 *       inside `from` (translation would stop being idempotent)
 *   -3  table full
 */
int vfs_bind_add(const char *from, const char *to);

int vfs_bind_count(void);

/* Copy entry `idx` out (either pointer may be NULL).  0 / -1. */
int vfs_bind_info(int idx, char *from, int fcap, char *to, int tcap);

/* Translate an absolute path.  Returns
 *    1  a bind matched; `out` holds the translation
 *    0  no match; `out` is untouched
 *   -1  matched but the result does not fit `cap`; `out` is untouched
 *       (never truncate a path: that would name a different file)
 */
int vfs_bind_translate(const char *abs, char *out, int cap);

/* Drop every bind.  Exists for the host test and for a future umount; the
 * guest only ever adds. */
void vfs_bind_reset(void);

#endif /* VFS_BIND_H */
