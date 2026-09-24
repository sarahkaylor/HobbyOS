/* HobbyOS sysroot: langinfo.c — nl_langinfo() for the C locale.
 *
 * HobbyOS has exactly one locale ("C", ASCII). CODESET reports the C
 * locale's codeset name byte-exact with glibc's ("ANSI_X3.4-1968") so GNU
 * code that probes the charset (GNU regex, localcharset) reliably takes
 * its single-byte path; every other item has no data yet and yields "".
 *
 * Implementations compile as hb_* under HOST_TEST for host comparison
 * (see src/host/libc_langinfo_test.c).
 */
#include <langinfo.h>

#ifdef HOST_TEST
#define nl_langinfo hb_nl_langinfo
#endif

char *nl_langinfo(int item) {
  if (item == CODESET)
    return "ANSI_X3.4-1968";
  return "";
}
