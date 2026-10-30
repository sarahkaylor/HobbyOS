/*
 * Host test for the libc pieces added for the X11 client stack (icewm.md M2,
 * lane xlibs-core): src/libc/src/inet.c, uio.c, utsname.c compiled as hbl_*
 * and compared against glibc.
 *
 *   make x11_libc_test_host && ./x11_libc_test_host
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>

int hbl_inet_pton(int af, const char *src, void *dst);
const char *hbl_inet_ntop(int af, const void *src, char *dst, socklen_t size);
int hbl_inet_aton(const char *cp, struct in_addr *inp);
in_addr_t hbl_inet_addr(const char *cp);
char *hbl_inet_ntoa(struct in_addr in);
int hbl_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                    struct addrinfo **res);
void hbl_freeaddrinfo(struct addrinfo *res);
int hbl_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen,
                    char *serv, socklen_t servlen, int flags);
struct hostent *hbl_gethostbyname(const char *name);
ssize_t hbl_writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t hbl_readv(int fd, const struct iovec *iov, int iovcnt);
int hbl_uname(struct utsname *u);
int hbl_gethostname(char *name, size_t len);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
  printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *pton4_cases[] = {
  "0.0.0.0", "127.0.0.1", "255.255.255.255", "1.2.3.4", "192.168.0.217",
  "256.1.1.1", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.04", "", "a.b.c.d",
  "1..2.3", "1.2.3.4 ", " 1.2.3.4", "1.2.3.-4", "0x1.2.3.4", "1.2.3.255", NULL };

static const char *pton6_cases[] = {
  "::", "::1", "1::", "1::1", "fe80::1", "2001:db8::ff00:42:8329",
  "2001:0db8:0000:0000:0000:ff00:0042:8329", "::ffff:192.168.0.1", "::1.2.3.4",
  "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "1:2:3:4:5:6:1.2.3.4",
  ":::", "1:2:3:4:5:6:7:8:9", "1::2::3", "12345::", "1:", ":1", "g::",
  "1:2:3:4:5:6:7", "::ffff:1.2.3", "1::1.2.3.4", "ABCD:ef01::", "0:0:0:0:0:0:0:0",
  "1:0:0:2:0:0:0:3", "0:0:1:0:0:0:0:0", "1:0:0:0:0:0:0:0", NULL };

static void test_pton_ntop(void) {
  for (int i = 0; pton4_cases[i]; i++) {
    unsigned char a[4] = {0}, b[4] = {0};
    int ra = inet_pton(AF_INET, pton4_cases[i], a), rb = hbl_inet_pton(AF_INET, pton4_cases[i], b);
    CHECK(ra == rb && (ra != 1 || memcmp(a, b, 4) == 0), "pton4 '%s' glibc=%d hb=%d", pton4_cases[i], ra, rb);
    if (ra == 1) {
      char x[INET_ADDRSTRLEN], y[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, a, x, sizeof x);
      hbl_inet_ntop(AF_INET, a, y, sizeof y);
      CHECK(strcmp(x, y) == 0, "ntop4 '%s' vs '%s'", x, y);
    }
  }
  for (int i = 0; pton6_cases[i]; i++) {
    unsigned char a[16] = {0}, b[16] = {0};
    int ra = inet_pton(AF_INET6, pton6_cases[i], a), rb = hbl_inet_pton(AF_INET6, pton6_cases[i], b);
    CHECK(ra == rb && (ra != 1 || memcmp(a, b, 16) == 0), "pton6 '%s' glibc=%d hb=%d", pton6_cases[i], ra, rb);
    if (ra == 1) {
      char x[INET6_ADDRSTRLEN], y[INET6_ADDRSTRLEN];
      inet_ntop(AF_INET6, a, x, sizeof x);
      hbl_inet_ntop(AF_INET6, a, y, sizeof y);
      CHECK(strcmp(x, y) == 0, "ntop6 '%s': glibc '%s' hb '%s'", pton6_cases[i], x, y);
    }
  }
  /* randomized ntop6 round-trips against glibc, biased towards zero words */
  srand(12345);
  for (int n = 0; n < 20000; n++) {
    unsigned char a[16];
    for (int k = 0; k < 8; k++) {
      int z = rand() % 3 == 0 ? 0 : rand() & 0xffff;
      if (rand() % 4 == 0) z = 0xffff;
      a[2 * k] = (unsigned char)(z >> 8); a[2 * k + 1] = (unsigned char)z;
    }
    if (rand() % 3 == 0) memset(a, 0, (size_t)(rand() % 13));
    char x[INET6_ADDRSTRLEN], y[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, a, x, sizeof x);
    hbl_inet_ntop(AF_INET6, a, y, sizeof y);
    if (strcmp(x, y) != 0) { CHECK(0, "rand ntop6 glibc '%s' hb '%s'", x, y); break; }
    unsigned char b[16];
    if (hbl_inet_pton(AF_INET6, x, b) != 1 || memcmp(a, b, 16) != 0) { CHECK(0, "rand pton6 '%s'", x); break; }
  }
  char small[4];
  CHECK(hbl_inet_ntop(AF_INET, "\x7f\0\0\x01", small, sizeof small) == NULL && errno == ENOSPC, "ntop ENOSPC");
  CHECK(hbl_inet_pton(12345, "1.2.3.4", small) == -1 && errno == EAFNOSUPPORT, "pton EAFNOSUPPORT");
}

static void test_aton(void) {
  static const char *c[] = { "127.0.0.1", "127.1", "0x7f.1", "0177.0.0.1", "1.2.3", "4294967295",
    "4294967296", "1.256.3.4", "1.2.65535", "1.2.65536", "", "x", "1.2.3.4.5", "0x", "08",
    "1.2.3.4 trailing", NULL };
  for (int i = 0; c[i]; i++) {
    struct in_addr a = {0}, b = {0};
    int ra = inet_aton(c[i], &a), rb = hbl_inet_aton(c[i], &b);
    CHECK(ra == rb && (!ra || a.s_addr == b.s_addr), "aton '%s' glibc=%d/%08x hb=%d/%08x",
          c[i], ra, a.s_addr, rb, b.s_addr);
    CHECK(inet_addr(c[i]) == hbl_inet_addr(c[i]), "inet_addr '%s'", c[i]);
  }
  struct in_addr x = { htonl(0xc0a800d9) };
  CHECK(strcmp(hbl_inet_ntoa(x), "192.168.0.217") == 0, "ntoa");
}

static void test_gai(void) {
  struct addrinfo h = {0}, *r = NULL;
  h.ai_family = AF_INET; h.ai_socktype = SOCK_STREAM;
  CHECK(hbl_getaddrinfo("10.1.2.3", "6000", &h, &r) == 0 && r, "gai numeric");
  if (r) {
    struct sockaddr_in *s = (struct sockaddr_in *)r->ai_addr;
    CHECK(r->ai_family == AF_INET && r->ai_socktype == SOCK_STREAM && r->ai_protocol == IPPROTO_TCP &&
          r->ai_addrlen == sizeof *s && ntohs(s->sin_port) == 6000 &&
          s->sin_addr.s_addr == htonl(0x0a010203) && r->ai_next == NULL, "gai numeric contents");
    char host[64], serv[16];
    CHECK(hbl_getnameinfo(r->ai_addr, r->ai_addrlen, host, sizeof host, serv, sizeof serv,
                          NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
          strcmp(host, "10.1.2.3") == 0 && strcmp(serv, "6000") == 0, "getnameinfo");
    hbl_freeaddrinfo(r);
  }
  r = NULL;
  CHECK(hbl_getaddrinfo("localhost", NULL, NULL, &r) == 0 && r, "gai localhost");
  int n4 = 0, n6 = 0;
  for (struct addrinfo *p = r; p; p = p->ai_next) {
    if (p->ai_family == AF_INET) n4++;
    if (p->ai_family == AF_INET6) n6++;
  }
  CHECK(n4 == 2 && n6 == 2, "gai localhost families %d/%d", n4, n6);
  hbl_freeaddrinfo(r);
  r = NULL;
  h.ai_family = AF_UNSPEC; h.ai_flags = AI_PASSIVE;
  CHECK(hbl_getaddrinfo(NULL, "6000", &h, &r) == 0 && r &&
        ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(INADDR_ANY), "gai passive");
  hbl_freeaddrinfo(r);
  r = (struct addrinfo *)1;
  CHECK(hbl_getaddrinfo("example.invalid", NULL, NULL, &r) == EAI_NONAME && r == NULL, "gai NONAME");
  CHECK(hbl_getaddrinfo("1.2.3.4", "x11", NULL, &r) == EAI_SERVICE, "gai SERVICE");
  h.ai_flags = AI_NUMERICHOST;
  CHECK(hbl_getaddrinfo("localhost", NULL, &h, &r) == EAI_NONAME, "gai NUMERICHOST");
  h.ai_flags = 0; h.ai_family = AF_INET6;
  CHECK(hbl_getaddrinfo("::1", "1", &h, &r) == 0 && r && r->ai_family == AF_INET6 &&
        r->ai_addrlen == sizeof(struct sockaddr_in6), "gai v6");
  hbl_freeaddrinfo(r);
  struct hostent *he = hbl_gethostbyname("localhost");
  CHECK(he && he->h_addrtype == AF_INET && he->h_length == 4 &&
        memcmp(he->h_addr_list[0], "\x7f\0\0\x01", 4) == 0 && he->h_addr_list[1] == NULL, "ghbn");
  CHECK(hbl_gethostbyname("no.such.host") == NULL, "ghbn miss");
}

static void test_uio(void) {
  char a[] = "hello ", b[] = "vectored ", c[] = "world";
  struct iovec w[3] = { { a, 6 }, { b, 9 }, { c, 5 } };
  /* pipe: sendmsg -> ENOTSOCK -> write loop */
  int p[2];
  CHECK(pipe(p) == 0, "pipe");
  CHECK(hbl_writev(p[1], w, 3) == 20, "writev pipe");
  char r1[7] = {0}, r2[64] = {0};
  struct iovec rv[2] = { { r1, 6 }, { r2, sizeof r2 } };
  CHECK(hbl_readv(p[0], rv, 2) == 20 && strcmp(r1, "hello ") == 0 && strcmp(r2, "vectored world") == 0,
        "readv pipe '%s' '%s'", r1, r2);
  close(p[0]); close(p[1]);
  /* socketpair: one sendmsg/recvmsg each */
  int s[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0, "socketpair");
  CHECK(hbl_writev(s[0], w, 3) == 20, "writev sock");
  memset(r1, 0, sizeof r1); memset(r2, 0, sizeof r2);
  CHECK(hbl_readv(s[1], rv, 2) == 20 && strcmp(r2, "vectored world") == 0, "readv sock");
  close(s[0]); close(s[1]);
  CHECK(hbl_writev(1, w, -1) == -1 && errno == EINVAL, "writev EINVAL");
  CHECK(hbl_writev(1, w, 0) == 0, "writev 0");
  /* regular file */
  char path[] = "/tmp/x11_libc_test_XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0 && hbl_writev(fd, w, 3) == 20, "writev file");
  lseek(fd, 0, SEEK_SET);
  memset(r1, 0, sizeof r1); memset(r2, 0, sizeof r2);
  CHECK(hbl_readv(fd, rv, 2) == 20 && strcmp(r2, "vectored world") == 0, "readv file");
  close(fd); unlink(path);
}

static void test_uname(void) {
  struct utsname u;
  CHECK(hbl_uname(&u) == 0 && strcmp(u.sysname, "HobbyOS") == 0 && u.machine[0], "uname");
  char h[64];
  unsetenv("HOSTNAME");
  CHECK(hbl_gethostname(h, sizeof h) == 0 && strcmp(h, "hobbyos") == 0, "gethostname default");
  setenv("HOSTNAME", "box", 1);
  CHECK(hbl_gethostname(h, sizeof h) == 0 && strcmp(h, "box") == 0, "gethostname env");
  CHECK(hbl_gethostname(h, 2) == -1 && errno == ENAMETOOLONG, "gethostname short");
}

int main(void) {
  test_pton_ntop();
  test_aton();
  test_gai();
  test_uio();
  test_uname();
  if (fails) { printf("x11_libc_test: %d FAILURES\n", fails); return 1; }
  printf("x11_libc_test: all passed\n");
  return 0;
}
