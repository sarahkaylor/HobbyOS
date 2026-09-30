/* icu_target_probe.cpp -- ICU 78.3 target link probe (third_party/icu-78.3).
 *
 * A tiny C++ program (C API only, the JSC-class consumption surface;
 * browser.md L6): init, Unicode properties, case mapping, UTF-8/UTF-16
 * conversion, collation, break iteration, date formatting.  Compiled as C++
 * because ICU's ptypes.h pulls the C11 <uchar.h> in C mode only and the
 * HobbyOS sysroot has no uchar.h yet (see ../../cross-notes.md "gaps").
 * It is compiled and LINKED for the target by build-target.sh against
 *   icu_probe.o libicui18n.a libicuuc.a libicudata.a libcxx.a libc.a
 * and is never executed (the target is bare metal).  The link is the test:
 * static-link closure -- any symbol the pulled ICU objects need that the
 * libc/libc++ sysroot cannot provide fails the link by name.  See
 * ../../cross-notes.md "link probe".
 */
#include <unicode/utypes.h>
#include <unicode/uclean.h>
#include <unicode/ustring.h>
#include <unicode/uchar.h>
#include <unicode/ucol.h>
#include <unicode/ubrk.h>
#include <unicode/udat.h>

/* crt0 calls the plain C symbol `main`; C++ TUs must wrap it like
 * src/user/cxx_t.cpp does (see that file's comment). */
extern "C" int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UErrorCode status = U_ZERO_ERROR;
  UChar buf[64];
  UChar utf16[] = {0x68, 0xE9, 0x6C, 0x6C, 0x6F, 0}; /* "héllo" */
  UChar out[128];
  int32_t len = 0;

  u_init(&status);
  (void)u_charType(0x41);              /* Unicode property query */
  (void)u_getIntPropertyValue(0x41, UCHAR_GENERAL_CATEGORY);

  u_strToUpper(buf, 64, utf16, -1, "en", &status);
  u_strToLower(buf, 64, utf16, -1, "en", &status);
  u_strToUTF8(NULL, 0, &len, utf16, -1, &status);
  status = U_ZERO_ERROR;
  u_strFromUTF8(out, 128, NULL, "hello", 5, &status);

  {
    UCollator *coll = ucol_open("en_US", &status);
    if (coll != NULL) {
      UChar a[] = {0x61, 0};
      UChar b[] = {0x62, 0};
      (void)ucol_strcoll(coll, a, 1, b, 1);
      ucol_close(coll);
    }
  }
  {
    UBreakIterator *brk = ubrk_open(UBRK_WORD, "en", utf16, -1, &status);
    if (brk != NULL) {
      (void)ubrk_first(brk);
      ubrk_close(brk);
    }
  }
  {
    UDateFormat *df = udat_open(UDAT_NONE, UDAT_NONE, "en", NULL, -1, NULL, 0, &status);
    if (df != NULL) {
      (void)udat_format(df, 0.0, out, 128, NULL, &status);
      udat_close(df);
    }
  }
  /* Never executed on-device; keep the compiler from optimizing it away. */
  return (int)(buf[0] + out[0] + len + (int32_t)status);
}
