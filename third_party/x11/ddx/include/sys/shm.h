/* Server-stage <sys/shm.h> (lane C4-x3, docs/cp-x3.md): System V shared
 * memory for MIT-SHM.  struct shmid_ds is x86_64 Linux's 112-byte layout,
 * which SYS_SHM fills on both HobbyOS arches; the calls are the weak wrappers
 * in ddx/compat/hbxcompat.c over SYS_SHM (src/include/syscall.h). */
#ifndef HB_XSERVER_SYS_SHM_H
#define HB_XSERVER_SYS_SHM_H
#include <stddef.h>
#include <sys/ipc.h>

#define SHM_RDONLY 010000
#define SHM_RND    020000
#define SHM_REMAP  040000
#define SHM_EXEC   0100000
#define SHM_LOCK   11
#define SHM_UNLOCK 12
#define SHMLBA 4096

typedef unsigned long shmatt_t;

struct shmid_ds {
    struct ipc_perm shm_perm; /* 0 */
    size_t shm_segsz;         /* 48 */
    time_t shm_atime;         /* 56 */
    time_t shm_dtime;         /* 64 */
    time_t shm_ctime;         /* 72 */
    pid_t shm_cpid;           /* 80 */
    pid_t shm_lpid;           /* 84 */
    shmatt_t shm_nattch;      /* 88 */
    unsigned long __unused4;
    unsigned long __unused5;
};

int shmget(key_t key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int shmdt(const void *shmaddr);
int shmctl(int shmid, int cmd, struct shmid_ds *buf);
#endif
