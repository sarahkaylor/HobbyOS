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

  pstr("t4-probe-arrived\n");
  pstr("probe-exit\n");
  return 0;
}
