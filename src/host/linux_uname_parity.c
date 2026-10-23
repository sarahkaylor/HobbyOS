/* src/host/linux_uname_parity.c — GB-016 host T4 differential on the uname VALUES.
 *
 * The kernel shim (linux_fill_utsname, merged PR #61) produces the Linux-ABI
 * identity a compat process must see (sysname "Linux", machine per arch) — but
 * nothing yet verifies those VALUES equal what a real Linux uname() reports.
 * The existing host probe's `uname sysname=Linux` line is real-musl (leg A) vs
 * real-glibc (leg B) on the SAME host — both are Linux, so it cannot catch a
 * kernel shim that returns the wrong identity.
 *
 * This differential runs the actual kernel shim (src/kernel/linux_uname.c)
 * against the host's real uname() and asserts byte-equality where the ABI
 * requires it:
 *   - sysname  == "Linux"          (the value SDL/games sniff)
 *   - machine  == the host's uname machine (the shim uses the same per-arch)
 *   - the struct is NUL-padded at 390 bytes and no field overflows its 65-byte
 *     cell (the linux_utsname_t layout, already golden in linux_layouts.golden).
 *
 * Exits 0 on parity, non-zero on any mismatch.  This is the host-verifiable
 * "compare the values end-to-end" piece the GB-016 remainder asked for; it runs
 * on the Linux box without the intel-guest leg (which stays GB-014-gated).
 */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include "linux_uname.h"

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(void) {
  struct utsname real;
  linux_utsname_t shim;
  int fails = 0;

  if (uname(&real) != 0) { printf("real uname failed\n"); return 1; }
  if (linux_fill_utsname(&shim) != 0) { printf("shim fill failed\n"); return 1; }

  /* 390-byte Linux utsname layout (golden) */
  if (sizeof(shim) != 390ul) { printf("FAIL size=%lu\n", (unsigned long)sizeof(shim)); fails++; }

  /* sysname must be exactly "Linux" — the value SDL/games sniff.  The kernel
   * reports the compat identity, not the toolchain's "HobbyOS" triple. */
  if (!eq(shim.sysname, "Linux")) { printf("FAIL sysname='%s'\n", shim.sysname); fails++; }

  /* machine must equal the real Linux uname machine for this arch.  The shim
   * uses the same __x86_64__/else split as the sysroot, so on x86_64 it is
   * "x86_64" and the host real uname machine must say the same. */
  if (!eq(shim.machine, real.machine)) {
    printf("FAIL machine shim='%s' real='%s'\n", shim.machine, real.machine); fails++;
  }

  /* NUL-padded, no overflow past the 65-byte cell end */
  if (shim.sysname[64] != '\0' || shim.machine[64] != '\0') {
    printf("FAIL NUL-padding\n"); fails++;
  }

  if (fails == 0) {
    printf("uname-parity: PASS sysname=%s machine=%s (matches real Linux)\n",
           shim.sysname, shim.machine);
    return 0;
  }
  printf("uname-parity: FAIL (%d)\n", fails);
  return 1;
}
