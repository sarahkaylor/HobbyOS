/* src/user/linux_probe_x11.c — Linux-ABI X11 transport probe (no libX11).
 *
 * WHY: the Doom target talks X11 over an AF_UNIX stream socket to HobbyOS's own
 * X server.  When it fails, "the game is black" says nothing about WHERE.  This
 * probe does the same first two things libX11 does, by hand, so the failure has
 * a name:
 *
 *   1. socket(AF_UNIX, SOCK_STREAM) + connect("/tmp/.X11-unix/X0")
 *   2. the X11 connection setup exchange: a 12-byte little-endian setup request,
 *      then the reply header (8 bytes) followed by a variable body -- one big
 *      read, which is also the test that a large reply arrives intact
 *   3. one request/reply round trip (GetInputFocus, opcode 43): proves write +
 *      blocking read on the same fd, not just the initial handshake
 *
 * It prints the vendor string, the number of screens and the first screen's
 * root window id + root depth, so a run also says WHAT server answered (the
 * depth is the port's business: 24 today, 8 once the depth-8 mode exists).
 *
 * Protocol reference: X11 protocol, "Connection Setup" (section 8) and
 * GetInputFocus (section 9, opcode 43).  Runs identically on a Linux host
 * against a real X server, which is how the expectations were checked:
 *     Xvfb :98 -screen 0 640x480x24 & DISPLAY=:98 ./probe_x11
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;

static void row(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  /* vsnprintf returns the length it WOULD have written; past the buffer that is
     a write of uninitialised stack (measured: a long PATH row printed heap
     garbage).  Clamp before writing. */
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static int read_full(int fd, void *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, (char *)buf + got, len - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    got += (size_t)n;
  }
  return 0;
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int main(void) {
  /* DISPLAY selects the socket ("":98" -> /tmp/.X11-unix/X98), so the same
     binary can be checked against a host Xvfb and against the guest's :0. */
  static char path[64];
  snprintf(path, sizeof path, "/tmp/.X11-unix/X0");
  const char *dpy = getenv("DISPLAY");
  if (dpy && dpy[0] == ':') {
    int idx = 1, val = 0;
    while (dpy[idx] >= '0' && dpy[idx] <= '9' && idx < 8) {
      val = val * 10 + (dpy[idx] - '0');
      idx++;
    }
    snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", val);
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    row("[LP] x11 socket FAILED errno=%d (%s)\n", errno, strerror(errno));
    row("[LP] done rc=1\n");
    return 1;
  }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    row("[LP] x11 connect %s FAILED errno=%d (%s)\n", path, errno, strerror(errno));
    row("[LP] done rc=1\n");
    return 1;
  }
  row("[LP] x11 connect ok\n");

  /* Connection setup request: byte order 'l', pad, major 11, minor 0,
     auth-name len 0, auth-data len 0, pad 2. */
  unsigned char req[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  if (write(fd, req, sizeof req) != (ssize_t)sizeof req) {
    row("[LP] x11 setup write FAILED errno=%d\n", errno);
    fails++;
  }

  unsigned char hdr[8];
  if (read_full(fd, hdr, sizeof hdr) != 0) {
    row("[LP] x11 setup reply header FAILED errno=%d\n", errno);
    row("[LP] done rc=%d\n", fails + 1);
    return fails + 1;
  }
  int success = hdr[0];
  int extra = (int)le16(hdr + 6) * 4;
  row("[LP] x11 setup status=%d body=%d bytes\n", success, extra);
  if (success != 1) {
    /* Failure replies carry a reason string; read it so the row is actionable. */
    unsigned char b[256];
    int n = extra < (int)sizeof b - 1 ? extra : (int)sizeof b - 1;
    if (n > 0 && read_full(fd, b, (size_t)n) == 0) {
      b[n] = 0;
      row("[LP] x11 setup refused: %s\n", b + 1);
    }
    row("[LP] done rc=%d\n", fails + 1);
    return fails + 1;
  }

  unsigned char body[8192];
  if (extra <= 0 || extra > (int)sizeof body) {
    row("[LP] x11 setup body size out of range (%d)\n", extra);
    row("[LP] done rc=1\n");
    return 1;
  }
  if (read_full(fd, body, (size_t)extra) != 0) {
    row("[LP] x11 setup body FAILED errno=%d\n", errno);
    row("[LP] done rc=1\n");
    return 1;
  }
  /* setup body: release(4) idbase(2) idmask(2) vendorlen(2) maxreq(2)
     roots(1) formats(1) imagedata(1) pad(1) vendor vendorpad formats screens */
  uint32_t release = le32(body);
  int vendor_len = le16(body + 16);
  int roots = body[20];
  int vendor_off = 32;
  char vendor[64];
  int vn = vendor_len < (int)sizeof vendor - 1 ? vendor_len : (int)sizeof vendor - 1;
  memcpy(vendor, body + vendor_off, (size_t)vn);
  vendor[vn] = 0;
  row("[LP] x11 release=%u vendor=%s roots=%d\n", release, vendor, roots);

  int root_depth = -1;
  uint32_t root_id = 0;
  if (roots > 0) {
    /* screens start after vendor + padding + formats (8 bytes each); walk to
       the first screen and read root-window id and root depth. */
    int p = vendor_off + vendor_len;
    p = (p + 3) & ~3;
    int formats = body[21];
    p += formats * 8;
    if (p + 40 <= extra) {
      root_id = le32(body + p);
      root_depth = body[p + 38];
    }
  }
  row("[LP] x11 screen0 root=0x%08x depth=%d %s\n", root_id, root_depth,
      root_depth >= 0 ? "ok" : "UNPARSED");
  if (root_depth < 0) fails++;

  /* GetInputFocus (opcode 43, one 4-byte unit after the header). */
  unsigned char gi[4] = {43, 0, 1, 0};
  if (write(fd, gi, sizeof gi) != (ssize_t)sizeof gi) {
    row("[LP] x11 GetInputFocus write FAILED errno=%d\n", errno);
    fails++;
  } else {
    unsigned char r[32];
    if (read_full(fd, r, sizeof r) != 0) {
      row("[LP] x11 GetInputFocus reply FAILED errno=%d\n", errno);
      fails++;
    } else if (r[0] != 1) {
      row("[LP] x11 GetInputFocus unexpected reply type=%d\n", r[0]);
      fails++;
    } else {
      row("[LP] x11 GetInputFocus reply ok focus=0x%08x\n", le32(r + 8));
    }
  }
  close(fd);
  row("[LP] done rc=%d\n", fails);
  return fails;
}
