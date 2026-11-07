/* sys/shm.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * No SysV shared memory in HobbyOS.  See <sys/ipc.h> for why the declarations
 * must exist anyway.  shmget() always fails (ENOSYS) in gl_stubs.c, which is
 * exactly the condition Mesa's xlib winsys tests before falling back to
 * XPutImage - so even without XLIB_NO_SHM the fast path disables itself. */
#ifndef _HOBBYOS_SYS_SHM_H
#define _HOBBYOS_SYS_SHM_H 1

#include <sys/types.h>
#include <sys/ipc.h>

typedef unsigned long shmatt_t;

struct shmid_ds {
  struct ipc_perm shm_perm;
  size_t shm_segsz;
  time_t shm_atime;
  time_t shm_dtime;
  time_t shm_ctime;
  pid_t shm_cpid;
  pid_t shm_lpid;
  shmatt_t shm_nattch;
  unsigned long __reserved[4];
};

/* shmat()/shmdt() flags (Linux numbering) */
#define SHM_RDONLY 010000
#define SHM_RND    020000
#define SHM_REMAP  040000
#define SHM_EXEC   0100000

#ifdef __cplusplus
extern "C" {
#endif

int   shmget(key_t key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int   shmdt(const void *shmaddr);
int   shmctl(int shmid, int cmd, struct shmid_ds *buf);

#ifdef __cplusplus
}
#endif

#endif /* _HOBBYOS_SYS_SHM_H */
