/*
 * sqlite_os.c — P6.2 (browser.md §6): the HobbyOS OS port for the vendored
 * SQLite amalgamation (3.49.2.0, third_party/sqlite-amalgamation-3490200,
 * pinned/checksummed by the host build script).
 *
 * sqlite3.c is compiled with -DSQLITE_OS_OTHER=1, which removes every
 * built-in OS backend and obliges the application to supply
 * sqlite3_os_init()/sqlite3_os_end() plus a VFS.  This file is that port:
 *
 *  - ONE VFS ("hobby") over the libc file API (open/close/read/write/
 *    lseek/ftruncate/fsync/fstat/unlink/access/getrandom/usleep/time).
 *    There is no mmap, no sub-journal special-casing and no WAL (built
 *    SQLITE_OMIT_WAL): a single flat namespace, /-rooted names.
 *
 *  - SQLite's 5-state lock protocol over the P6.1 record locks (fcntl
 *    F_GETLK/F_SETLK on <fcntl.h>'s LP64 struct flock).  Byte ranges are
 *    the reference layout from SQLite's own designs: 0x40000000 pending,
 *    +1 reserved, +2 shared (510 bytes).  Conflicting F_SETLK returns
 *    EAGAIN -> SQLITE_BUSY, the non-blocking + busy-handler contract the
 *    pager consumes.
 *
 *  - Mutex methods for SQLITE_THREADSAFE=1: SQLITE_OS_OTHER leaves the
 *    no-op mutex defaults in place, so hobby_sqlite_init() (call it as the
 *    FIRST sqlite touch in main()) installs these over the libc pthread
 *    layer *before* sqlite3_initialize() runs.
 *
 * HobbyOS deltas from a POSIX host, deliberate and documented:
 *  - Temp files (pager temp DBs, sorter spills) arrive as xOpen(zName ==
 *    NULL); POSIX VFSes name them in the OS layer, so this VFS generates
 *    /tmp/etilqs_hbXXXXXXXX (LFN create path) and unlinks at xClose when
 *    DELETEONCLOSE/TEMP flags are set (there is no unlink-on-open).
 *  - xFullPathname is identity: names are used exactly as the library and
 *    the application write them (the in-OS test opens /SQLTEST.DB), which
 *    keeps journal/deletion names simple and stable.
 *  - xDeviceCharacteristics reports nothing (no powersafe-overwrite
 *    claim), so the pager takes the conservative sync path.
 */

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "libc.h" /* P6.1 syscall surface: ftruncate/fsync/getrandom/usleep */

/* ---- lock ranges (SQLite reference layout) ----------------------------- */

#define HBY_PENDING_BYTE  0x40000000LL
#define HBY_RESERVED_BYTE (HBY_PENDING_BYTE + 1)
#define HBY_SHARED_FIRST  (HBY_PENDING_BYTE + 2)
#define HBY_SHARED_SIZE   510

#define HBY_PATH_MAX 128
#define HBY_TMPDIR   "/tmp"

/* ---- the file object --------------------------------------------------- */

typedef struct hby_file {
  const sqlite3_io_methods *pMethods; /* MUST stay the first member */
  int fd;
  int lock_state; /* SQLITE_LOCK_* */
  int delete_on_close;
  char zName[HBY_PATH_MAX];
} hby_file;

/* F_SETLK one range; EAGAIN -> SQLITE_BUSY. */
static int hby_set_lock(int fd, int type, sqlite3_int64 start,
                        sqlite3_int64 len) {
  struct flock fl;

  fl.l_type = type;
  fl.l_whence = 0; /* SEEK_SET */
  fl.l_start = start;
  fl.l_len = len;
  fl.l_pid = 0;
  if (fcntl(fd, F_SETLK, &fl) == 0)
    return SQLITE_OK;
  return (errno == EAGAIN || errno == EACCES) ? SQLITE_BUSY : SQLITE_IOERR_LOCK;
}

/* F_GETLK one range: 1 if some other process holds a conflicting lock. */
static int hby_conflict(int fd, int type, sqlite3_int64 start,
                        sqlite3_int64 len) {
  struct flock fl;

  fl.l_type = type;
  fl.l_whence = 0;
  fl.l_start = start;
  fl.l_len = len;
  fl.l_pid = 0;
  if (fcntl(fd, F_GETLK, &fl) < 0)
    return -1;
  return fl.l_type != F_UNLCK;
}

/* ---- io methods -------------------------------------------------------- */

static int hbyClose(sqlite3_file *pFile) {
  hby_file *f = (hby_file *)pFile;
  int rc = SQLITE_OK;

  if (f->fd >= 0 && close(f->fd) != 0)
    rc = SQLITE_IOERR_CLOSE;
  if (f->delete_on_close)
    unlink(f->zName); /* no unlink-on-open: deletion happens here */
  f->fd = -1;
  f->pMethods = 0;
  return rc;
}

static int hbyRead(sqlite3_file *pFile, void *zBuf, int iAmt,
                   sqlite3_int64 iOfst) {
  hby_file *f = (hby_file *)pFile;
  int got;

  if (lseek(f->fd, (off_t)iOfst, 0) < 0)
    return SQLITE_IOERR_READ;
  got = (int)read(f->fd, zBuf, (size_t)iAmt);
  if (got < 0)
    return SQLITE_IOERR_READ;
  if (got < iAmt) {
    memset((char *)zBuf + got, 0, (size_t)(iAmt - got));
    return SQLITE_IOERR_SHORT_READ;
  }
  return SQLITE_OK;
}

static int hbyWrite(sqlite3_file *pFile, const void *zBuf, int iAmt,
                    sqlite3_int64 iOfst) {
  hby_file *f = (hby_file *)pFile;
  int put;

  if (lseek(f->fd, (off_t)iOfst, 0) < 0)
    return SQLITE_IOERR_WRITE;
  put = (int)write(f->fd, zBuf, (size_t)iAmt);
  return put == iAmt ? SQLITE_OK : SQLITE_IOERR_WRITE;
}

static int hbyTruncate(sqlite3_file *pFile, sqlite3_int64 size) {
  hby_file *f = (hby_file *)pFile;

  return ftruncate(f->fd, (off_t)size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int hbySync(sqlite3_file *pFile, int flags) {
  hby_file *f = (hby_file *)pFile;

  (void)flags; /* every write is already at the device */
  return fsync(f->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int hbyFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize) {
  hby_file *f = (hby_file *)pFile;
  struct stat st;

  if (fstat(f->fd, &st) != 0)
    return SQLITE_IOERR_FSTAT;
  *pSize = (sqlite3_int64)st.st_size;
  return SQLITE_OK;
}

static int hbyLock(sqlite3_file *pFile, int eLock) {
  hby_file *f = (hby_file *)pFile;
  int rc = SQLITE_OK;

  if (eLock <= f->lock_state) {
    f->lock_state = eLock;
    return SQLITE_OK;
  }
  switch (eLock) {
  case SQLITE_LOCK_SHARED:
    rc = hby_set_lock(f->fd, F_RDLCK, HBY_SHARED_FIRST, HBY_SHARED_SIZE);
    break;
  case SQLITE_LOCK_RESERVED:
    rc = hby_set_lock(f->fd, F_WRLCK, HBY_RESERVED_BYTE, 1);
    break;
  case SQLITE_LOCK_PENDING:
    rc = hby_set_lock(f->fd, F_WRLCK, HBY_PENDING_BYTE, 1);
    break;
  case SQLITE_LOCK_EXCLUSIVE:
    /* pending, then reserved, then the shared range in write mode (the
       write lock on the shared range conflicts with other readers). */
    rc = hby_set_lock(f->fd, F_WRLCK, HBY_PENDING_BYTE, 1);
    if (rc == SQLITE_OK)
      rc = hby_set_lock(f->fd, F_WRLCK, HBY_RESERVED_BYTE, 1);
    if (rc == SQLITE_OK)
      rc = hby_set_lock(f->fd, F_WRLCK, HBY_SHARED_FIRST, HBY_SHARED_SIZE);
    break;
  default:
    break;
  }
  if (rc == SQLITE_OK)
    f->lock_state = eLock;
  return rc;
}

static int hbyUnlock_one(int fd, sqlite3_int64 start, sqlite3_int64 len) {
  return hby_set_lock(fd, F_UNLCK, start, len) == SQLITE_OK ? SQLITE_OK
                                                            : SQLITE_IOERR_UNLOCK;
}

static int hbyUnlock(sqlite3_file *pFile, int eLock) {
  hby_file *f = (hby_file *)pFile;
  int rc = SQLITE_OK;

  if (eLock >= f->lock_state) {
    f->lock_state = eLock;
    return SQLITE_OK;
  }
  if (eLock < SQLITE_LOCK_EXCLUSIVE && f->lock_state >= SQLITE_LOCK_EXCLUSIVE) {
    /* back to a reader: downgrade the shared range to a read lock */
    if (hby_set_lock(f->fd, F_RDLCK, HBY_SHARED_FIRST, HBY_SHARED_SIZE) !=
        SQLITE_OK)
      rc = SQLITE_IOERR_UNLOCK;
  }
  if (eLock < SQLITE_LOCK_PENDING && f->lock_state >= SQLITE_LOCK_PENDING &&
      hbyUnlock_one(f->fd, HBY_PENDING_BYTE, 1) != SQLITE_OK)
    rc = SQLITE_IOERR_UNLOCK;
  if (eLock < SQLITE_LOCK_RESERVED && f->lock_state >= SQLITE_LOCK_RESERVED &&
      hbyUnlock_one(f->fd, HBY_RESERVED_BYTE, 1) != SQLITE_OK)
    rc = SQLITE_IOERR_UNLOCK;
  if (eLock == SQLITE_LOCK_NONE &&
      hbyUnlock_one(f->fd, HBY_SHARED_FIRST, HBY_SHARED_SIZE) != SQLITE_OK)
    rc = SQLITE_IOERR_UNLOCK;
  f->lock_state = eLock;
  return rc;
}

static int hbyCheckReservedLock(sqlite3_file *pFile, int *pResOut) {
  hby_file *f = (hby_file *)pFile;
  int c = hby_conflict(f->fd, F_WRLCK, HBY_RESERVED_BYTE, 1);

  if (c < 0)
    return SQLITE_IOERR_CHECKRESERVEDLOCK;
  *pResOut = c;
  return SQLITE_OK;
}

static int hbyFileControl(sqlite3_file *pFile, int op, void *pArg) {
  hby_file *f = (hby_file *)pFile;

  switch (op) {
  case SQLITE_FCNTL_LOCKSTATE:
    *(int *)pArg = f->lock_state;
    return SQLITE_OK;
  case SQLITE_FCNTL_SIZE_HINT:
  case SQLITE_FCNTL_CHUNK_SIZE:
    return SQLITE_OK; /* advisory only */
  default:
    return SQLITE_NOTFOUND;
  }
}

static int hbySectorSize(sqlite3_file *pFile) {
  (void)pFile;
  return 512;
}

static int hbyDeviceCharacteristics(sqlite3_file *pFile) {
  (void)pFile;
  return 0; /* no powersafe-overwrite/atomic claims: conservative syncs */
}

/* WAL is compiled out (SQLITE_OMIT_WAL); these satisfy the struct. */
static int hbyShmMap(sqlite3_file *pFile, int iPg, int pgsz, int bExtend,
                     void volatile **pp) {
  (void)pFile;
  (void)iPg;
  (void)pgsz;
  (void)bExtend;
  (void)pp;
  return SQLITE_IOERR_SHMMAP;
}

static int hbyShmLock(sqlite3_file *pFile, int offset, int n, int flags) {
  (void)pFile;
  (void)offset;
  (void)n;
  (void)flags;
  return SQLITE_IOERR_SHMLOCK;
}

static void hbyShmBarrier(sqlite3_file *pFile) { (void)pFile; }

static int hbyShmUnmap(sqlite3_file *pFile, int deleteFlag) {
  (void)pFile;
  (void)deleteFlag;
  return SQLITE_OK;
}

static const sqlite3_io_methods hbyIoMethods = {
  2, /* iVersion: through the shm block */
  hbyClose,   hbyRead,      hbyWrite,      hbyTruncate, hbySync,
  hbyFileSize, hbyLock,     hbyUnlock,     hbyCheckReservedLock,
  hbyFileControl, hbySectorSize, hbyDeviceCharacteristics,
  hbyShmMap,  hbyShmLock,   hbyShmBarrier, hbyShmUnmap,
  0,          0, /* xFetch/xUnfetch (v3): unused at iVersion 2 */
};

/* ---- VFS level --------------------------------------------------------- */

static unsigned hby_name_counter;

static int hbyOpen(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile,
                   int flags, int *pOutFlags) {
  hby_file *f = (hby_file *)pFile;
  char zGen[HBY_PATH_MAX];
  int oflags;

  (void)pVfs;
  memset(f, 0, sizeof *f);
  f->fd = -1;
  f->lock_state = SQLITE_LOCK_NONE;

  if (!zName) {
    /* The pager delegates temp-file naming to the VFS (POSIX VFSes do this
       in their own layer too).  Unique via counter + clock. */
    snprintf(zGen, sizeof zGen, HBY_TMPDIR "/etilqs_hb%08x%08x",
             (unsigned)hby_name_counter++, (unsigned)time(0));
    zName = zGen;
    f->delete_on_close = 1;
  } else if (flags & (SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_TEMP_DB |
                      SQLITE_OPEN_TEMP_JOURNAL | SQLITE_OPEN_TRANSIENT_DB)) {
    f->delete_on_close = 1;
  }
  strncpy(f->zName, zName, sizeof f->zName - 1);

  oflags = (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;
  if (flags & SQLITE_OPEN_CREATE)
    oflags |= O_CREAT;
  f->fd = open(zName, oflags, 0644);
  if (f->fd < 0)
    return SQLITE_CANTOPEN;

  f->pMethods = &hbyIoMethods;
  if (pOutFlags)
    *pOutFlags = (flags & SQLITE_OPEN_READONLY) ? SQLITE_OPEN_READONLY
                                                : SQLITE_OPEN_READWRITE;
  return SQLITE_OK;
}

static int hbyDelete(sqlite3_vfs *pVfs, const char *zName, int syncDir) {
  (void)pVfs;
  (void)syncDir;
  if (unlink(zName) == 0 || errno == ENOENT)
    return SQLITE_OK;
  return SQLITE_IOERR_DELETE;
}

static int hbyAccess(sqlite3_vfs *pVfs, const char *zName, int flags,
                     int *pResOut) {
  (void)pVfs;
  (void)flags;
  *pResOut = access(zName, 0 /* F_OK */) == 0;
  return SQLITE_OK;
}

static int hbyFullPathname(sqlite3_vfs *pVfs, const char *zName, int nOut,
                           char *zOut) {
  (void)pVfs;
  /* Identity: this VFS is a single flat '/'-rooted namespace and names are
     used exactly as given (documented above). */
  if (nOut <= 0)
    return SQLITE_ERROR;
  strncpy(zOut, zName, (size_t)nOut - 1);
  zOut[nOut - 1] = 0;
  return SQLITE_OK;
}

static int hbyRandomness(sqlite3_vfs *pVfs, int nByte, char *zOut) {
  (void)pVfs;
  getrandom(zOut, (size_t)nByte, 0);
  return nByte;
}

static int hbySleep(sqlite3_vfs *pVfs, int microseconds) {
  (void)pVfs;
  usleep((unsigned)microseconds);
  return microseconds;
}

static int hbyCurrentTime(sqlite3_vfs *pVfs, double *pTime) {
  (void)pVfs;
  *pTime = (double)time(0) / 86400.0 + 2440587.5; /* julian day */
  return SQLITE_OK;
}

static int hbyCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pTime) {
  (void)pVfs;
  *pTime = (sqlite3_int64)(((double)time(0) + 2440587.5) * 86400000.0);
  return SQLITE_OK;
}

static int hbyGetLastError(sqlite3_vfs *pVfs, int nBuf, char *zBuf) {
  (void)pVfs;
  (void)nBuf;
  (void)zBuf;
  return 0;
}

static sqlite3_vfs hbyVfs = {
  3, /* iVersion */
  sizeof(hby_file),
  HBY_PATH_MAX,
  0, /* pNext */
  "hobby",
  0, /* pAppData */
  hbyOpen,
  hbyDelete,
  hbyAccess,
  hbyFullPathname,
  0, /* xDlOpen   (SQLITE_OMIT_LOAD_EXTENSION) */
  0, /* xDlError  */
  0, /* xDlSym    */
  0, /* xDlClose  */
  hbyRandomness,
  hbySleep,
  hbyCurrentTime,
  hbyGetLastError,
  hbyCurrentTimeInt64,
  0, /* xSetSystemCall */
  0, /* xGetSystemCall */
  0, /* xNextSystemCall */
};

/* ---- mutex methods (SQLITE_THREADSAFE=1 over libc pthreads) ------------ */

typedef struct hby_mutex {
  pthread_mutex_t m;
  int ready;
  int dynamic;
} hby_mutex;

static hby_mutex hby_static_mutexes[SQLITE_MUTEX_STATIC_VFS3 -
                                    SQLITE_MUTEX_STATIC_MAIN + 1];
static pthread_mutexattr_t hby_mutex_attr;
static int hby_mutex_attr_ready;

/* All SQLite mutexes are recursive here: the recursive type is a correct
   superset of the fast type for every pattern SQLite uses. */
static void hby_mutex_setup(void) {
  if (!hby_mutex_attr_ready) {
    pthread_mutexattr_init(&hby_mutex_attr);
    pthread_mutexattr_settype(&hby_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    hby_mutex_attr_ready = 1;
  }
}

static int hbyMutexInit(void) {
  hby_mutex_setup();
  return SQLITE_OK;
}

static int hbyMutexEnd(void) { return SQLITE_OK; }

static sqlite3_mutex *hbyMutexAlloc(int id) {
  hby_mutex *m;

  hby_mutex_setup();
  if (id >= SQLITE_MUTEX_STATIC_MAIN && id <= SQLITE_MUTEX_STATIC_VFS3) {
    m = &hby_static_mutexes[id - SQLITE_MUTEX_STATIC_MAIN];
    if (!m->ready) {
      pthread_mutex_init(&m->m, &hby_mutex_attr);
      m->ready = 1;
    }
    return (sqlite3_mutex *)m;
  }
  m = malloc(sizeof *m);
  if (!m)
    return 0;
  pthread_mutex_init(&m->m, &hby_mutex_attr);
  m->ready = 1;
  m->dynamic = 1;
  return (sqlite3_mutex *)m;
}

static void hbyMutexFree(sqlite3_mutex *p) {
  hby_mutex *m = (hby_mutex *)p;

  if (m && m->dynamic) {
    pthread_mutex_destroy(&m->m);
    free(m);
  }
}

static void hbyMutexEnter(sqlite3_mutex *p) {
  pthread_mutex_lock(&((hby_mutex *)p)->m);
}

static int hbyMutexTry(sqlite3_mutex *p) {
  return pthread_mutex_trylock(&((hby_mutex *)p)->m) == 0 ? SQLITE_OK
                                                          : SQLITE_BUSY;
}

static void hbyMutexLeave(sqlite3_mutex *p) {
  pthread_mutex_unlock(&((hby_mutex *)p)->m);
}

/* Held/notheld feed debug asserts only; without per-mutex owner tracking on
   this pthread layer they assume the (single-threaded-test) truth: held. */
static int hbyMutexHeld(sqlite3_mutex *p) {
  (void)p;
  return 1;
}

static int hbyMutexNotheld(sqlite3_mutex *p) {
  (void)p;
  return 0;
}

static sqlite3_mutex_methods const hbyMutexMethods = {
  hbyMutexInit,  hbyMutexEnd,    hbyMutexAlloc,  hbyMutexFree,
  hbyMutexEnter, hbyMutexTry,    hbyMutexLeave,  hbyMutexHeld,
  hbyMutexNotheld,
};

/* ---- the two entry points SQLITE_OS_OTHER demands ---------------------- */

SQLITE_API int sqlite3_os_init(void) {
  sqlite3_temp_directory = (char *)HBY_TMPDIR;
  return sqlite3_vfs_register(&hbyVfs, 1);
}

SQLITE_API int sqlite3_os_end(void) { return SQLITE_OK; }

/*
 * hobby_sqlite_init — the FIRST sqlite call an application must make:
 * installs the mutex methods (while configuration is still unlocked) and
 * then runs sqlite3_initialize(), which calls sqlite3_os_init above.
 */
int hobby_sqlite_init(void) {
  int rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &hbyMutexMethods);

  if (rc != SQLITE_OK)
    return rc;
  return sqlite3_initialize();
}
