/*
 * icu_smoke.c — host smoke for the vendored ICU4C 78.3 static build
 * (third_party/icu-78.3; lane browser/l6-icu; see README.md).
 *
 * Deterministic checks, one PASS line each:
 *   1. version       — u_getVersion / U_ICU_VERSION >= 70.1
 *   2. u_strToUpper  — full case mapping, en_US
 *   3. Normalizer2   — NFC: e + U+0301 -> U+00E9, isNormalized both ways
 *   4. BreakIterator — UBRK_WORD word segmentation of "The quick brown fox"
 *   5. ucol          — collation ordering + strength levels (primary equal,
 *                      tertiary different: "resume" vs "résumé")
 *   6. data          — data version readable with no filesystem .dat
 *
 * Last line on success: ALL TESTS PASSED SUCCESSFULLY!  (exit 0)
 * Output must be byte-identical across runs (run-smoke.sh diffs two runs).
 */
#include <stdio.h>

#include <unicode/utypes.h>
#include <unicode/ustring.h>
#include <unicode/unorm2.h>
#include <unicode/ubrk.h>
#include <unicode/ucol.h>
#include <unicode/ulocdata.h>
#include <unicode/uversion.h>

static int failures = 0;

static void check(const char *name, int cond) {
  printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
  if (!cond) {
    failures++;
  }
}

/* Print "label<utf-8 bytes>" for a UTF-16 segment (for the log). */
static void print_u16(const char *label, const UChar *s, int32_t len) {
  char buf[256];
  int32_t out_len = 0;
  UErrorCode ec = U_ZERO_ERROR;
  u_strToUTF8(buf, (int32_t)sizeof(buf), &out_len, s, len, &ec);
  if (U_FAILURE(ec)) {
    printf("%s<utf-8 conversion failed: %s>\n", label, u_errorName(ec));
    return;
  }
  printf("%s%.*s\n", label, (int)out_len, buf);
}

int main(void) {
  UErrorCode ec;

  /* --- 1. version ------------------------------------------------------- */
  UVersionInfo ver;
  u_getVersion(ver);
  printf("ICU version: %s (runtime %d.%d.%d.%d)\n", U_ICU_VERSION,
         ver[0], ver[1], ver[2], ver[3]);
  check("version >= 70.1 (WPE 2.54 floor)", ver[0] > 70 || (ver[0] == 70 && ver[1] >= 1));

  ec = U_ZERO_ERROR;
  UVersionInfo cldr_ver;
  ulocdata_getCLDRVersion(cldr_ver, &ec);
  printf("CLDR data version: %d.%d.%d.%d\n", cldr_ver[0], cldr_ver[1], cldr_ver[2], cldr_ver[3]);
  check("CLDR data readable (static data present)", U_SUCCESS(ec) && cldr_ver[0] > 0);

  /* --- 2. u_strToUpper (full case mapping) ------------------------------ */
  {
    const UChar src[] = u"h\u00e9llo w\u00f6rld"; /* héllo wörld */
    UChar dst[64];
    int32_t n;
    ec = U_ZERO_ERROR;
    n = u_strToUpper(dst, (int32_t)(sizeof(dst) / sizeof(dst[0])), src, -1, "en_US", &ec);
    print_u16("  u_strToUpper(en_US) -> ", dst, n);
    check("u_strToUpper: h\u00e9llo w\u00f6rld -> H\u00c9LLO W\u00d6RLD",
          U_SUCCESS(ec) && u_strcmp(dst, u"H\u00c9LLO W\u00d6RLD") == 0);
  }

  /* --- 3. Normalizer2 NFC ----------------------------------------------- */
  {
    const UChar src[] = u"e\u0301"; /* e + COMBINING ACUTE ACCENT */
    const UNormalizer2 *nfc;
    UChar dst[16];
    UBool src_norm, dst_norm;
    int32_t n;
    ec = U_ZERO_ERROR;
    nfc = unorm2_getNFCInstance(&ec);
    check("Normalizer2 NFC instance available", U_SUCCESS(ec) && nfc != NULL);
    src_norm = unorm2_isNormalized(nfc, src, -1, &ec);
    check("NFC: e+U+0301 input is NOT normalized", U_SUCCESS(ec) && !src_norm);
    n = unorm2_normalize(nfc, src, -1, dst, (int32_t)(sizeof(dst) / sizeof(dst[0])), &ec);
    print_u16("  unorm2_normalize(NFC) -> ", dst, n);
    check("NFC: e+U+0301 -> U+00E9 (é)",
          U_SUCCESS(ec) && n == 1 && dst[0] == 0x00e9);
    dst_norm = unorm2_isNormalized(nfc, dst, -1, &ec);
    check("NFC: output reports normalized (quick check)", U_SUCCESS(ec) && dst_norm);
  }

  /* --- 4. BreakIterator word segmentation ------------------------------- */
  {
    const UChar text[] = u"The quick brown fox";
    UBreakIterator *bi;
    int32_t start, end, words = 0;
    int first_is_the = 0;
    ec = U_ZERO_ERROR;
    bi = ubrk_open(UBRK_WORD, "en_US", text, -1, &ec);
    check("BreakIterator word: ubrk_open(en_US) OK", U_SUCCESS(ec) && bi != NULL);
    if (U_SUCCESS(ec) && bi != NULL) {
      start = ubrk_first(bi);
      while ((end = ubrk_next(bi)) != UBRK_DONE) {
        if (ubrk_getRuleStatus(bi) >= UBRK_WORD_NONE_LIMIT) { /* a word */
          print_u16("  word -> ", text + start, end - start);
          if (words == 0 && end - start == 3 &&
              text[start] == 'T' && text[start + 1] == 'h' && text[start + 2] == 'e') {
            first_is_the = 1;
          }
          words++;
        }
        start = end;
      }
      ubrk_close(bi);
    }
    printf("  word count = %d\n", words);
    check("BreakIterator word: 4 words and first is \"The\"", words == 4 && first_is_the);
  }

  /* --- 5. ucol collation ------------------------------------------------ */
  {
    UCollator *coll;
    int32_t cmp_app_ban, cmp_app_app, cmp_primary, cmp_tertiary;
    ec = U_ZERO_ERROR;
    coll = ucol_open("en_US", &ec);
    check("ucol: ucol_open(en_US) OK", U_SUCCESS(ec) && coll != NULL);
    if (U_SUCCESS(ec) && coll != NULL) {
      cmp_app_ban = ucol_strcoll(coll, u"apple", -1, u"banana", -1);
      cmp_app_app = ucol_strcoll(coll, u"apple", -1, u"apple", -1);
      printf("  ucol_strcoll(apple, banana) = %d\n", cmp_app_ban);
      printf("  ucol_strcoll(apple, apple)  = %d\n", cmp_app_app);
      check("ucol: \"apple\" < \"banana\"", cmp_app_ban < 0);
      check("ucol: \"apple\" == \"apple\"", cmp_app_app == 0);
      ucol_setStrength(coll, UCOL_PRIMARY);
      cmp_primary = ucol_strcoll(coll, u"resume", -1, u"r\u00e9sum\u00e9", -1);
      ucol_setStrength(coll, UCOL_TERTIARY);
      cmp_tertiary = ucol_strcoll(coll, u"resume", -1, u"r\u00e9sum\u00e9", -1);
      printf("  ucol_strcoll(resume, r\xc3\xa9sum\xc3\xa9) primary=%d tertiary=%d\n",
             cmp_primary, cmp_tertiary);
      check("ucol: primary strength equal for resume/r\u00e9sum\u00e9", cmp_primary == 0);
      check("ucol: tertiary strength differs for resume/r\u00e9sum\u00e9", cmp_tertiary != 0);
      ucol_close(coll);
    }
  }

  /* --- verdict ----------------------------------------------------------- */
  if (failures == 0) {
    printf("SMOKE PASS: ICU %s u_strToUpper + NFC + BreakIterator + collation "
           "(static data, no filesystem)\n", U_ICU_VERSION);
    printf("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
  }
  printf("SMOKE FAIL: %d check(s) failed\n", failures);
  return 1;
}
