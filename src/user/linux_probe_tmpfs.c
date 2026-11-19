/* src/user/linux_probe_tmpfs.c - Linux-ABI probe for the compat /tmp (cp/sys).
 *
 * A plain musl x86_64 program: the same source is the reference on a Linux
 * host and the measurement inside HobbyOS.  Rows start "[LP] "; the last row
 * is "[LP] done rc=<failures>" (tools/gate/t3_linux_probe.sh accepts rc=0).
 *
 *   T1 directory mode/owner   T2 file I/O        T3 unlink-while-open
 *   T4 rename                 T5 symlinks        T6 getdents / errno
 *   T7 open errno             T8 >64 MiB memfd + /tmp file, MAP_SHARED
 *   T9 memfd I/O              T10 socket inode   T11 locks across fork
 *   T12 shared counter page over AF_UNIX SOCK_STREAM SCM_RIGHTS to a child
 *   T13 two fd records back to back on one stream
 *   T4f rename on the FAT root (relative 8.3 names: the guest's cwd is the FAT
 *       root, a host run renames in its cwd).  fat16_rename passed a NULL
 *       last-component buffer that fat16_resolve_parent wrote through: a
 *       kernel store to VA 0, which hung the CPU (no "[LP] done" row).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static char D[96];

static void out(const char *fmt, ...) {
  char b[384];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}
static const char *P(const char *leaf) {
  static char b[4][192]; static int k; k = (k + 1) & 3;
  snprintf(b[k], sizeof b[k], "%s/%s", D, leaf);
  return b[k];
}
static void nap(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, 0);
}

static void t1_dirs(void) {
  struct stat st;
  umask(022);
  snprintf(D, sizeof D, "/tmp/lpt-%d", (int)getpid());
  ck(mkdir(D, 0700) == 0, "T1 mkdir 0700", errno);
  ck(lstat(D, &st) == 0 && S_ISDIR(st.st_mode), "T1 is dir", (long)st.st_mode);
  ck((st.st_mode & 0777) == 0700, "T1 mode 0700", (long)(st.st_mode & 0777));
  ck(st.st_uid == getuid(), "T1 owner caller", (long)st.st_uid);
  ck(mkdir(D, 0700) == -1 && errno == EEXIST, "T1 EEXIST", errno);
  ck(mkdir(P("no/sub"), 0700) == -1 && errno == ENOENT, "T1 ENOENT parent", errno);
}

static void t2_file(void) {
  struct stat st;
  char b[16] = {0};
  int fd = open(P("f"), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ck(fd >= 0, "T2 open create", fd);
  long w = write(fd, "hello world", 11);
  long o = lseek(fd, 6, SEEK_SET);
  long n = read(fd, b, sizeof b);
  ck(w == 11 && o == 6 && n == 5 && memcmp(b, "world", 5) == 0, "T2 write/seek/read", n);
  ck(fstat(fd, &st) == 0 && st.st_size == 11 && S_ISREG(st.st_mode), "T2 fstat size 11", (long)st.st_size);
  ck(ftruncate(fd, 4) == 0 && fstat(fd, &st) == 0 && st.st_size == 4, "T2 ftruncate 4", (long)st.st_size);
  close(fd);
}

static void t3_unlink_open(void) {
  int fd = open(P("u"), O_RDWR | O_CREAT, 0644);
  write(fd, "persist", 7);
  int r = unlink(P("u"));
  struct stat st;
  int gone = stat(P("u"), &st);
  char b[8] = {0};
  lseek(fd, 0, SEEK_SET);
  long n = read(fd, b, 7);
  ck(r == 0 && gone == -1 && errno == ENOENT && n == 7 && memcmp(b, "persist", 7) == 0,
     "T3 unlink-while-open keeps data", n);
  close(fd);
}

static void t4_rename(void) {
  int fd = open(P("r1"), O_RDWR | O_CREAT, 0644);
  write(fd, "data", 4); close(fd);
  ck(rename(P("r1"), P("r2")) == 0, "T4 rename", errno);
  struct stat st;
  ck(stat(P("r1"), &st) == -1 && stat(P("r2"), &st) == 0, "T4 old gone new there", (long)st.st_size);
  fd = open(P("r3"), O_RDWR | O_CREAT, 0644); write(fd, "xx", 2); close(fd);
  ck(rename(P("r3"), P("r2")) == 0, "T4 rename replaces", errno);
  ck(syscall(SYS_renameat2, AT_FDCWD, P("r2"), AT_FDCWD, P("r4"), 0) == 0, "T4 renameat2", errno);
  fd = open(P("r5"), O_RDWR | O_CREAT, 0644); close(fd);
  ck(syscall(SYS_renameat2, AT_FDCWD, P("r4"), AT_FDCWD, P("r5"), 1 /*NOREPLACE*/) == -1 &&
     errno == EEXIST, "T4 renameat2 NOREPLACE EEXIST", errno);
}

static void t4f_rename_fat(void) {
  unlink("LPFATR1.TMP"); unlink("LPFATR2.TMP");
  int fd = open("LPFATR1.TMP", O_RDWR | O_CREAT | O_TRUNC, 0644);
  long w = fd >= 0 ? write(fd, "fat!", 4) : -1;
  if (fd >= 0) close(fd);
  ck(fd >= 0 && w == 4, "T4f create on FAT", fd < 0 ? errno : w);
  ck(rename("LPFATR1.TMP", "LPFATR2.TMP") == 0, "T4f rename on FAT", errno);
  struct stat st;
  int gone = stat("LPFATR1.TMP", &st), there = stat("LPFATR2.TMP", &st);
  ck(gone == -1 && there == 0 && st.st_size == 4, "T4f old gone new there", there ? errno : (long)st.st_size);
  ck(unlink("LPFATR2.TMP") == 0, "T4f unlink", errno);
}

static void t5_symlink(void) {
  char buf[64] = {0};
  ck(symlink("f", P("l")) == 0, "T5 symlink", errno);
  long n = readlink(P("l"), buf, sizeof buf);
  ck(n == 1 && buf[0] == 'f', "T5 readlink", n);
  struct stat st;
  ck(lstat(P("l"), &st) == 0 && S_ISLNK(st.st_mode), "T5 lstat is link", (long)st.st_mode);
  ck(stat(P("l"), &st) == 0 && S_ISREG(st.st_mode), "T5 stat follows to file", (long)st.st_mode);
  /* a link that leaves /tmp: resolve "../.." style off the tmpfs */
  ck(symlink("/", P("root")) == 0, "T5 symlink to /", errno);
  ck(stat(P("root"), &st) == 0 && S_ISDIR(st.st_mode), "T5 link off /tmp follows", (long)st.st_mode);
}

static void t6_dents(void) {
  DIR *d = opendir(D);
  int seen_f = 0, seen_l = 0, n = 0;
  struct dirent *e;
  while (d && (e = readdir(d))) {
    n++;
    if (!strcmp(e->d_name, "f")) seen_f = 1;
    if (!strcmp(e->d_name, "l")) seen_l = 1;
  }
  if (d) closedir(d);
  ck(seen_f && seen_l && n >= 4, "T6 getdents lists entries", n);
  ck(rmdir(P("f")) == -1 && errno == ENOTDIR, "T6 rmdir a file ENOTDIR", errno);
  ck(unlink(D) == -1 && errno == EISDIR, "T6 unlink a dir EISDIR", errno);
  ck(rmdir(D) == -1 && errno == ENOTEMPTY, "T6 rmdir non-empty ENOTEMPTY", errno);
}

static void t7_open_errno(void) {
  int fd = open(P("f"), O_RDONLY | O_CREAT | O_EXCL, 0644);
  ck(fd == -1 && errno == EEXIST, "T7 O_EXCL EEXIST", errno);
  ck(open(P("absent"), O_RDONLY) == -1 && errno == ENOENT, "T7 ENOENT", errno);
  ck(open(P("f/x"), O_RDONLY) == -1 && errno == ENOTDIR, "T7 ENOTDIR", errno);
  ck(open(D, O_WRONLY) == -1 && errno == EISDIR, "T7 dir O_WRONLY EISDIR", errno);
}

#define BIG (66u * 1024u * 1024u) /* 66 MiB: past Wine's 64.0 MiB memfd */

static void t8_big(void) {
  int fd = open(P("big"), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { ck(0, "T8 open big", errno); return; }
  if (ftruncate(fd, BIG) != 0) { ck(0, "T8 ftruncate 66MiB", errno); close(fd); return; }
  void *m = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(m != MAP_FAILED, "T8 mmap 66MiB MAP_SHARED", m == MAP_FAILED ? errno : 0);
  if (m != MAP_FAILED) {
    volatile char *p = m;
    p[0] = 'A'; p[BIG - 1] = 'Z';           /* first and last page */
    char rb[2] = {0};
    pread(fd, rb, 1, 0);
    char rl[2] = {0};
    pread(fd, rl, 1, BIG - 1);
    ck(rb[0] == 'A' && rl[0] == 'Z', "T8 mmap store visible to read()", (long)rl[0]);
    char w = 'Q';
    pwrite(fd, &w, 1, 4096);
    ck(p[4096] == 'Q', "T8 write() visible in the mapping", (long)p[4096]);
    munmap(m, BIG);
  }
  close(fd);
  unlink(P("big"));
}

static void t9_memfd(void) {
  int fd = (int)syscall(SYS_memfd_create, "lp", 0u);
  if (fd < 0) { ck(0, "T9 memfd_create", errno); return; }
  ck(ftruncate(fd, BIG) == 0, "T9 memfd ftruncate 66MiB", errno);
  long w = pwrite(fd, "mfd", 3, 1024);
  char b[4] = {0};
  long r = pread(fd, b, 3, 1024);
  ck(w == 3 && r == 3 && memcmp(b, "mfd", 3) == 0, "T9 memfd pread/pwrite", r);
  void *m = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(m != MAP_FAILED && ((char *)m)[1025] == 'f', "T9 memfd mmap sees write", m == MAP_FAILED ? -1 : (long)((char *)m)[1025]);
  if (m != MAP_FAILED) munmap(m, BIG);
  struct stat st;
  ck(fstat(fd, &st) == 0 && st.st_size == BIG, "T9 memfd fstat size", (long)st.st_size);
  close(fd);
}

static void t10_sock(void) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a; memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof a.sun_path, "%s/s", D);
  int r = bind(s, (struct sockaddr *)&a, sizeof a);
  struct stat st;
  int ls = lstat(a.sun_path, &st);
  ck(r == 0 && ls == 0 && S_ISSOCK(st.st_mode), "T10 bind makes a socket inode", r ? errno : (long)st.st_mode);
  int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
  ck(bind(s2, (struct sockaddr *)&a, sizeof a) == -1 && errno == EADDRINUSE,
     "T10 second bind EADDRINUSE", errno);
  close(s2); close(s);
  unlink(a.sun_path);
}

static void t11_locks(void) {
  int fd = open(P("lk"), O_RDWR | O_CREAT, 0644);
  ftruncate(fd, 64);
  struct flock fl = {0};
  fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 8;
  ck(fcntl(fd, F_SETLK, &fl) == 0, "T11 F_SETLK write lock", errno);
  int pfd[2]; pipe(pfd);
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    struct flock q = {0};
    q.l_type = F_WRLCK; q.l_whence = SEEK_SET; q.l_start = 0; q.l_len = 8;
    int gr = fcntl(fd, F_GETLK, &q);
    char v = (gr == 0 && q.l_type == F_WRLCK && q.l_pid == getppid()) ? 'Y' : 'N';
    (void)!write(pfd[1], &v, 1);
    _exit(0);
  }
  close(pfd[1]);
  char v = '?';
  (void)!read(pfd[0], &v, 1);
  int st; waitpid(pid, &st, 0);
  ck(v == 'Y', "T11 child sees the lock across fork", (long)v);
  close(fd);
}

/* Send `fd` over `sock` with 4 payload bytes. */
static int send_fd(int sock, int fd, const char *tag) {
  char cbuf[CMSG_SPACE(sizeof(int))];
  memset(cbuf, 0, sizeof cbuf);
  struct iovec io = { (void *)tag, 4 };
  struct msghdr m; memset(&m, 0, sizeof m);
  m.msg_iov = &io; m.msg_iovlen = 1;
  m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
  struct cmsghdr *c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(int));
  return (int)sendmsg(sock, &m, 0);
}
static int recv_fd(int sock, char *tag4) {
  char cbuf[CMSG_SPACE(sizeof(int))];
  char data[4];
  struct iovec io = { data, 4 };
  struct msghdr m; memset(&m, 0, sizeof m);
  m.msg_iov = &io; m.msg_iovlen = 1;
  m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
  long r = recvmsg(sock, &m, 0);
  if (r != 4) return -1;
  if (tag4) memcpy(tag4, data, 4);
  struct cmsghdr *c = CMSG_FIRSTHDR(&m);
  if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
  int fd; memcpy(&fd, CMSG_DATA(c), sizeof(int));
  return fd;
}

/* T12: an unlinked /tmp file, grown with pwrite+ftruncate (the KUSER shape),
 * mapped MAP_SHARED, passed over a SOCK_STREAM pair to a forked child that
 * maps it and watches a counter the parent bumps -- Wine's server-writes,
 * client-reads contract (server/fd.c:356,380-425). */
static void t12_shared_page(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { ck(0, "T12 socketpair", errno); return; }
  int fd = open(P("km"), O_RDWR | O_CREAT | O_EXCL, 0600);
  char z = 0; pwrite(fd, &z, 1, 4095); ftruncate(fd, 4096);
  unlink(P("km")); /* unlinked while open, like tmpmap-* */
  volatile unsigned *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (w == MAP_FAILED) { ck(0, "T12 parent mmap", errno); return; }
  *w = 0;
  pid_t pid = fork();
  if (pid == 0) {
    close(sv[0]);
    char tag[4];
    int gfd = recv_fd(sv[1], tag);
    if (gfd < 0) _exit(11);
    volatile unsigned *r = mmap(0, 4096, PROT_READ, MAP_SHARED, gfd, 0);
    if (r == MAP_FAILED) _exit(12);
    unsigned last = *r;
    int moved = 0;
    for (int i = 0; i < 400 && moved < 5; i++) {
      if (*r != last) { last = *r; moved++; }
      nap(5);
    }
    char v = moved >= 5 ? 'Y' : 'N';
    (void)!write(sv[1], &v, 1);
    _exit(moved >= 5 ? 0 : 13);
  }
  close(sv[1]);
  ck(send_fd(sv[0], fd, "KUSR") == 4, "T12 pass the page fd over SCM_RIGHTS", errno);
  for (int i = 0; i < 60; i++) { *w = *w + 1; nap(5); }
  char v = '?';
  (void)!read(sv[0], &v, 1);
  int st; waitpid(pid, &st, 0);
  ck(v == 'Y', "T12 child observes the counter change", (long)v);
  munmap((void *)w, 4096);
  close(fd); close(sv[0]);
}

/* T13: two fd-carrying sends back to back on one stream; each read that lands
 * on a record boundary takes exactly that record's fd. */
static void t13_two_records(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { ck(0, "T13 socketpair", errno); return; }
  int a = open("/dev/null", O_RDONLY), b = open("/dev/null", O_RDONLY);
  int wa = send_fd(sv[0], a, "AAAA");
  int wb = send_fd(sv[0], b, "BBBB");
  char t1[4], t2[4];
  int ra = recv_fd(sv[1], t1);
  int rb = recv_fd(sv[1], t2);
  ck(wa == 4 && wb == 4 && ra >= 0 && rb >= 0 &&
         memcmp(t1, "AAAA", 4) == 0 && memcmp(t2, "BBBB", 4) == 0,
     "T13 two records each carry their fd", (long)(ra >= 0 && rb >= 0));
  if (ra >= 0) close(ra);
  if (rb >= 0) close(rb);
  close(a); close(b); close(sv[0]); close(sv[1]);
}

static void t14_statfs(void) {
  struct statfs sf;
  int r = statfs("/tmp", &sf);
  ck(r == 0 && sf.f_type == 0x01021994, "T14 statfs /tmp is tmpfs", r ? errno : (long)sf.f_type);
}

int main(void) {
  t1_dirs();
  t2_file();
  t3_unlink_open();
  t4_rename();
  t4f_rename_fat();
  t5_symlink();
  t6_dents();
  t7_open_errno();
  t8_big();
  t9_memfd();
  t10_sock();
  t11_locks();
  t12_shared_page();
  t13_two_records();
  t14_statfs();
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
