/* src/user/cp/p8scmprobe.c - lane C8-game (docs/cp-p8.md, stop 2b): what
 * recvmsg() does when an SCM_RIGHTS message arrives at a FULL fd table.
 * A static musl program (also builds and runs on Linux as the reference).
 *
 * This is Wine's wineserver receive_fd() (server/request.c:396) at P8: the
 * server's table is full, a client sends a request fd, and
 *   Linux:   recvmsg returns the data bytes, the fd is dropped and MSG_CTRUNC
 *            is set; Wine sees ret == sizeof(data) with fd == -1 and fails
 *            only that one request;
 *   HobbyOS: recvmsg returns -1/EMFILE and leaves the message queued
 *            (unix.c:576 / :717); Wine prints "Protocol error: process ...:
 *            recvmsg: Too many open files", kills the client process, and on
 *            the next datagram for the dead process dereferences NULL in
 *            grab_object() (object.c:540) -- the wineserver crash of runs 1/3.
 *
 * Rows:
 *   S1 recvmsg into a full table returns the data (ret == 4)
 *   S2 ... with MSG_CTRUNC set and no fd delivered
 *   S3 the message was consumed (a second recvmsg finds nothing: EAGAIN)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int fill[8192];

int main(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) { perror("[P8SCM] socketpair"); return 2; }
  int pass = open("/dev/null", O_RDONLY);
  if (pass < 0) { perror("[P8SCM] open"); return 2; }
  /* send one 4-byte datagram carrying `pass` */
  int word = 0x50385343;
  struct iovec v = { &word, sizeof word };
  char cbuf[CMSG_SPACE(sizeof(int))];
  memset(cbuf, 0, sizeof cbuf);
  struct msghdr m = { 0 };
  m.msg_iov = &v; m.msg_iovlen = 1; m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
  struct cmsghdr *c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &pass, sizeof pass);
  if (sendmsg(sv[0], &m, 0) != (ssize_t)sizeof word) { perror("[P8SCM] sendmsg"); return 2; }
  close(pass);
  /* fill the table */
  int n = 0;
  while (n < 8192) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) break;
    fill[n++] = fd;
  }
  int fill_errno = errno;
  /* receive */
  int got = 0;
  struct iovec rv = { &got, sizeof got };
  char rbuf[CMSG_SPACE(sizeof(int))];
  memset(rbuf, 0, sizeof rbuf);
  struct msghdr r = { 0 };
  r.msg_iov = &rv; r.msg_iovlen = 1; r.msg_control = rbuf; r.msg_controllen = sizeof rbuf;
  ssize_t ret = recvmsg(sv[1], &r, MSG_DONTWAIT);
  int rerr = ret < 0 ? errno : 0;
  int fdin = -1;
  for (struct cmsghdr *h = CMSG_FIRSTHDR(&r); ret > 0 && h; h = CMSG_NXTHDR(&r, h))
    if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) memcpy(&fdin, CMSG_DATA(h), sizeof fdin);
  int ctrunc = ret >= 0 && (r.msg_flags & MSG_CTRUNC);
  /* free one slot and look again: was the message consumed? */
  close(fill[--n]);
  ssize_t ret2 = recvmsg(sv[1], &r, MSG_DONTWAIT);
  int rerr2 = ret2 < 0 ? errno : 0;
  for (int i = 0; i < n; i++) close(fill[i]);
  printf("[P8SCM] table filled with %d fds (stopped by errno %d)\n", n + 1, fill_errno);
  printf("[P8SCM] S1 recvmsg ret=%zd errno=%d -> %s\n", ret, rerr, ret == (ssize_t)sizeof word ? "PASS" : "FAIL");
  printf("[P8SCM] S2 MSG_CTRUNC=%d fd_delivered=%d -> %s\n", ctrunc, fdin, ctrunc && fdin < 0 ? "PASS" : "FAIL");
  printf("[P8SCM] S3 second recvmsg ret=%zd errno=%d -> %s\n", ret2, rerr2,
         ret2 < 0 && rerr2 == EAGAIN ? "PASS" : "FAIL");
  return 0;
}
