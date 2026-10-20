/* src/user/t4_probe.c — GB-066 T4 device-leg probe (W0 row set).
 *
 * ONE source, two hosts, through the REAL syscall path on both:
 *
 *   leg A  reference : x86_64-linux-musl static ELF, run on real Linux.
 *   leg B  HobbyOS   : the SAME musl-built static ELF, staged onto disk.img,
 *                      loaded by HobbyOS's ELF loader (GB-013) and run inside
 *                      the guest.  PROC_FLAG_LINUX_ABI routes its write()
 *                      traps through the Linux-compat dispatch (GB-014) to the
 *                      console/serial.  This tests the kernel, never a host
 *                      shim.
 *
 * Output contract (byte-identical on both legs; the diff is on stdout):
 *   t4-probe
 *   row=<name> rc=<n> errno=<n> out=<hex>
 *   t4-probe-arrived     <- sentinel printed by the probe itself: existence
 *                           of this line PROVES the probe ran in the guest
 *                           (an image listing is never the proof).
 *   probe-exit
 *
 * Standard C + write() only, so the same object builds with musl-gcc -static
 * for both legs.  A tiny itoa avoids relying on printf differences.
 */
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/random.h>

static void pstr(const char *s) {
  size_t n = 0; while (s[n]) n++;
  (void)write(1, s, n);
}

static int itoa_dec(unsigned long v, char *dst) {
  char tmp[24]; int n = 0;
  if (v == 0) { dst[0] = '0'; return 1; }
  while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (int i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
  return n;
}

static void prow(const char *name, long rc, long err, const unsigned char *out, int outlen) {
  static const char hex[] = "0123456789abcdef";
  char buf[64]; int n = 0;
  /* "row=<name> rc=<rc> errno=<err> out=<hex>\n" */
  const char *p = "row="; while (*p) buf[n++] = *p++;
  p = name; while (*p) buf[n++] = *p++;
  p = " rc="; while (*p) buf[n++] = *p++;
  if (rc < 0) buf[n++] = '-';
  n += itoa_dec((unsigned long)(rc >= 0 ? rc : -rc), buf + n);
  p = " errno="; while (*p) buf[n++] = *p++;
  n += itoa_dec((unsigned long)(err >= 0 ? err : -err), buf + n);
  if (outlen > 0) {
    p = " out="; while (*p) buf[n++] = *p++;
    for (int i = 0; i < outlen; i++) {
      buf[n++] = hex[(out[i] >> 4) & 0xf];
      buf[n++] = hex[out[i] & 0xf];
    }
  }
  buf[n++] = '\n';
  (void)write(1, buf, (size_t)n);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  pstr("t4-probe\n");

  /* Row 1: write(1, "hi\n", 3) -> rc=3, errno=0, out=68690a. */
  {
    static const unsigned char msg[] = "hi\n";
    long r = write(1, msg, 3);
    prow("write3", r, (r < 0) ? (r == -1 ? 9 : (long)(-r)) : 0, msg, 3);
  }

  /* GB-073: emit every row the M1 -V DoDs name.  All but write3 were the
   * defect's ground: uname/urandom/null/getrandom rows existed ONLY in the
   * host model (t4_compat_probe.c), never in the guest probe — so GB-016-V /
   * GB-017-V etc. were unmeasurable by construction.  Leg A (Linux) runs all
   * of these through the real syscalls; leg B (HobbyOS-intel) does the same
   * once the GB-014 dispatch lands.  rc/errno are the deterministic values
   * (payloads deliberately not compared for the random rows). */
  {
    struct utsname u;
    long urc = uname(&u);
    prow("uname_rc", urc, urc < 0 ? (long)(-urc) : 0, NULL, 0);
    prow("uname_sysname_linux", strcmp(u.sysname, "Linux") == 0, 0, NULL, 0);
    prow("uname_machine_x86_64", strcmp(u.machine, "x86_64") == 0, 0, NULL, 0);
  }
  {
    unsigned char rnd[16];
    long gr = getrandom(rnd, sizeof rnd, 0);
    prow("getrandom_16", gr, gr < 0 ? (long)(-gr) : 0, NULL, 0);
    int ur = open("/dev/urandom", O_RDONLY);
    prow("urandom_open", ur, ur < 0 ? 2 : 0, NULL, 0);
    long urb = read(ur, rnd, sizeof rnd);
    prow("urandom_read16", urb, urb < 0 ? 2 : 0, NULL, 0);
    int nw = open("/dev/null", O_WRONLY);
    prow("null_open_w", nw, nw < 0 ? 2 : 0, NULL, 0);
    long nwb = write(nw, "abc", 3);
    prow("null_write3", nwb, nwb < 0 ? 2 : 0, NULL, 0);
    int nr = open("/dev/null", O_RDONLY);
    prow("null_open_r", nr, nr < 0 ? 2 : 0, NULL, 0);
    char c; long nrb = read(nr, &c, 1);
    prow("null_read1", nrb, nrb < 0 ? 2 : 0, NULL, 0);
  }

  pstr("t4-probe-arrived\n");
  pstr("probe-exit\n");
  return 0;
}
