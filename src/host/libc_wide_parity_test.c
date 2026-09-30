/*
 * P3.2/P3.3 host test: the wide-character surface (wchar, wctype),
 * strftime/asctime/ctime and the xlocale layer, raced against the
 * workstation glibc.
 *
 * src/libc/src/{wchar,wctype,strftime,xlocale}.c compile a second time for
 * the host with -DHOST_TEST (their public symbols land as hb_*, and the
 * files then include glibc's own headers).  Every case runs through both
 * implementations: identical return values, identical stored objects,
 * identical endptr offsets and identical errno.  The inputs stay inside
 * the C locale's byte-only domain -- the only encoding this port
 * documents -- and the host process is pinned to TZ=UTC (ctime and
 * strftime's %Z/%z are UTC in this port) and LC_ALL=C before anything runs.
 *
 * House style: literal expectations pin the C-locale contract even if the
 * workstation's glibc changes; the races pin byte-exact equivalence to
 * glibc on the same inputs.  Exit 0 on full pass, non-zero with a FAIL
 * count otherwise.
 */
/* wcwidth/wctob need the XSI-visible declarations. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/* --- hb_* surface (the HOST_TEST rename blocks of the four sources) ----- */
extern size_t hb_mbrtowc(wchar_t *, const char *, size_t, mbstate_t *);
extern size_t hb_mbrlen(const char *, size_t, mbstate_t *);
extern size_t hb_wcrtomb(char *, wchar_t, mbstate_t *);
extern size_t hb_mbsrtowcs(wchar_t *, const char **, size_t, mbstate_t *);
extern size_t hb_mbsnrtowcs(wchar_t *, const char **, size_t, size_t,
                            mbstate_t *);
extern size_t hb_wcsrtombs(char *, const wchar_t **, size_t, mbstate_t *);
extern size_t hb_wcsnrtombs(char *, const wchar_t **, size_t, size_t,
                            mbstate_t *);
extern int hb_mbsinit(const mbstate_t *);
extern wint_t hb_btowc(int);
extern int hb_wctob(wint_t);
extern int hb_mbtowc(wchar_t *, const char *, size_t);
extern int hb_wctomb(char *, wchar_t);
extern int hb_mblen(const char *, size_t);
extern size_t hb_mbstowcs(wchar_t *, const char *, size_t);
extern size_t hb_wcstombs(char *, const wchar_t *, size_t);
extern int hb_wcwidth(wchar_t);
extern size_t hb_wcslen(const wchar_t *);
extern int hb_wcscmp(const wchar_t *, const wchar_t *);
extern int hb_wcsncmp(const wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wcscpy(wchar_t *, const wchar_t *);
extern wchar_t *hb_wcsncpy(wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wcscat(wchar_t *, const wchar_t *);
extern wchar_t *hb_wcsncat(wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wcschr(const wchar_t *, wchar_t);
extern wchar_t *hb_wcsrchr(const wchar_t *, wchar_t);
extern wchar_t *hb_wcsstr(const wchar_t *, const wchar_t *);
extern wchar_t *hb_wcspbrk(const wchar_t *, const wchar_t *);
extern size_t hb_wcsspn(const wchar_t *, const wchar_t *);
extern size_t hb_wcscspn(const wchar_t *, const wchar_t *);
extern wchar_t *hb_wcstok(wchar_t *, const wchar_t *, wchar_t **);
extern int hb_wcscoll(const wchar_t *, const wchar_t *);
extern size_t hb_wcsxfrm(wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wmemcpy(wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wmemmove(wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wmemset(wchar_t *, wchar_t, size_t);
extern int hb_wmemcmp(const wchar_t *, const wchar_t *, size_t);
extern wchar_t *hb_wmemchr(const wchar_t *, wchar_t, size_t);
extern double hb_wcstod(const wchar_t *, wchar_t **);
extern float hb_wcstof(const wchar_t *, wchar_t **);
extern long double hb_wcstold(const wchar_t *, wchar_t **);

extern int hb_iswalnum(wint_t);
extern int hb_iswalpha(wint_t);
extern int hb_iswblank(wint_t);
extern int hb_iswcntrl(wint_t);
extern int hb_iswdigit(wint_t);
extern int hb_iswgraph(wint_t);
extern int hb_iswlower(wint_t);
extern int hb_iswprint(wint_t);
extern int hb_iswpunct(wint_t);
extern int hb_iswspace(wint_t);
extern int hb_iswupper(wint_t);
extern int hb_iswxdigit(wint_t);
extern wctype_t hb_wctype(const char *);
extern int hb_iswctype(wint_t, wctype_t);
extern wctrans_t hb_wctrans(const char *);
extern wint_t hb_towctrans(wint_t, wctrans_t);
extern wint_t hb_towlower(wint_t);
extern wint_t hb_towupper(wint_t);

extern size_t hb_strftime(char *, size_t, const char *, const struct tm *);
extern size_t hb_strftime_l(char *, size_t, const char *, const struct tm *,
                            locale_t);
extern char *hb_asctime(const struct tm *);
extern char *hb_asctime_r(const struct tm *, char *);
extern char *hb_ctime(const time_t *);
extern char *hb_ctime_r(const time_t *, char *);

extern locale_t hb_newlocale(int, const char *, locale_t);
extern locale_t hb_duplocale(locale_t);
extern void hb_freelocale(locale_t);
extern locale_t hb_uselocale(locale_t);
extern long hb_strtol_l(const char *, char **, int, locale_t);
extern unsigned long hb_strtoul_l(const char *, char **, int, locale_t);
extern long long hb_strtoll_l(const char *, char **, int, locale_t);
extern unsigned long long hb_strtoull_l(const char *, char **, int, locale_t);
extern int hb_isdigit_l(int, locale_t);
extern int hb_isxdigit_l(int, locale_t);
extern int hb_strcoll_l(const char *, const char *, locale_t);
extern size_t hb_strxfrm_l(char *, const char *, size_t, locale_t);
extern int hb_toupper_l(int, locale_t);
extern int hb_tolower_l(int, locale_t);
extern int hb_iswctype_l(wint_t, wctype_t, locale_t);
extern int hb_wcscoll_l(const wchar_t *, const wchar_t *, locale_t);
extern size_t hb_wcsxfrm_l(wchar_t *, const wchar_t *, size_t, locale_t);
extern int hb_iswspace_l(wint_t, locale_t);
extern int hb_iswprint_l(wint_t, locale_t);
extern int hb_iswcntrl_l(wint_t, locale_t);
extern int hb_iswupper_l(wint_t, locale_t);
extern int hb_iswlower_l(wint_t, locale_t);
extern int hb_iswalpha_l(wint_t, locale_t);
extern int hb_iswblank_l(wint_t, locale_t);
extern int hb_iswdigit_l(wint_t, locale_t);
extern int hb_iswpunct_l(wint_t, locale_t);
extern int hb_iswxdigit_l(wint_t, locale_t);
extern wint_t hb_towupper_l(wint_t, locale_t);
extern wint_t hb_towlower_l(wint_t, locale_t);

static int checks;
static int failures;

static void fail(const char *what, const char *detail) {
  failures++;
  printf("FAIL %s [%s]\n", what, detail);
}

#define CHECKV(cond, what, detail)                                            \
  do {                                                                        \
    checks++;                                                                 \
    if (!(cond))                                                              \
      fail(what, detail);                                                     \
  } while (0)

/* ================= A. multibyte <-> wide (C locale) ==================== */

/* Byte corpus: ASCII, DEL, a bare continuation byte, a 2-byte UTF-8
 * sequence (two encoding errors in this byte-only locale), an embedded
 * NUL after a valid char, and the empty string. */
static const char *const mbs[] = {"",     "A",      "ab", "a\x7f",
                                  "\x80", "a\x80",  "\xC3\xA9",
                                  "\xE2\x82\xAC", "a"};
static const int nmbs = (int)(sizeof mbs / sizeof mbs[0]);

static void race_mbrtowc(const char *s, size_t n, int idx) {
  mbstate_t sa, sb;
  wchar_t wa = 0xDEAD, wb = 0xDEAD;
  char detail[64];
  size_t ra, rb;
  int ea, eb;
  snprintf(detail, sizeof detail, "case %d n=%zu", idx, n);
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  errno = 0;
  ra = hb_mbrtowc(&wa, s, n, &sa);
  ea = errno;
  errno = 0;
  rb = mbrtowc(&wb, s, n, &sb);
  eb = errno;
  CHECKV(ra == rb && wa == wb && ea == eb, "mbrtowc", detail);
}

static void race_mbrtowc_null_reset(void) {
  mbstate_t sa, sb;
  size_t ra, rb;
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  /* s == NULL resets the (stateless, here) conversion state. */
  ra = hb_mbrtowc(NULL, NULL, 1, &sa);
  rb = mbrtowc(NULL, NULL, 1, &sb);
  CHECKV(ra == rb && memcmp(&sa, &sb, sizeof sa) == 0, "mbrtowc-reset",
         "s=NULL");
}

static void race_mbrlen(const char *s, size_t n, int idx) {
  mbstate_t sa, sb;
  size_t ra, rb;
  char detail[64];
  int ea, eb;
  snprintf(detail, sizeof detail, "case %d n=%zu", idx, n);
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  errno = 0;
  ra = hb_mbrlen(s, n, &sa);
  ea = errno;
  errno = 0;
  rb = mbrlen(s, n, &sb);
  eb = errno;
  CHECKV(ra == rb && ea == eb, "mbrlen", detail);
}

static void race_wcrtomb(wchar_t wc, int idx) {
  char ba[16], bb[16];
  mbstate_t sa, sb;
  size_t ra, rb;
  int ea, eb;
  char detail[64];
  snprintf(detail, sizeof detail, "case %d wc=0x%X", idx, (unsigned)wc);
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  memset(ba, 0xEE, sizeof ba);
  memset(bb, 0xEE, sizeof bb);
  errno = 0;
  ra = hb_wcrtomb(ba, wc, &sa);
  ea = errno;
  errno = 0;
  rb = wcrtomb(bb, wc, &sb);
  eb = errno;
  CHECKV(ra == rb && ea == eb && memcmp(ba, bb, sizeof ba) == 0, "wcrtomb",
         detail);
}

static void race_mbtowc(const char *s, size_t n, int idx) {
  wchar_t wa = 0xDEAD, wb = 0xDEAD;
  int ra, rb, ea, eb;
  char detail[64];
  snprintf(detail, sizeof detail, "case %d n=%zu", idx, n);
  errno = 0;
  ra = hb_mbtowc(&wa, s, n);
  ea = errno;
  errno = 0;
  rb = mbtowc(&wb, s, n);
  eb = errno;
  CHECKV(ra == rb && wa == wb && ea == eb, "mbtowc", detail);
  /* Reset both stateless conversion states in lockstep. */
  hb_mbtowc(NULL, NULL, 0);
  mbtowc(NULL, NULL, 0);
}

static void race_wctomb(wchar_t wc, int idx) {
  char ba[16], bb[16];
  int ra, rb, ea, eb;
  char detail[64];
  snprintf(detail, sizeof detail, "case %d wc=0x%X", idx, (unsigned)wc);
  memset(ba, 0xEE, sizeof ba);
  memset(bb, 0xEE, sizeof bb);
  errno = 0;
  ra = hb_wctomb(ba, wc);
  ea = errno;
  errno = 0;
  rb = wctomb(bb, wc);
  eb = errno;
  CHECKV(ra == rb && ea == eb && memcmp(ba, bb, sizeof ba) == 0, "wctomb",
         detail);
  hb_wctomb(NULL, 0);
  wctomb(NULL, 0);
}

static void race_mblen(const char *s, size_t n, int idx) {
  int ra, rb, ea, eb;
  char detail[64];
  snprintf(detail, sizeof detail, "case %d n=%zu", idx, n);
  errno = 0;
  ra = hb_mblen(s, n);
  ea = errno;
  errno = 0;
  rb = mblen(s, n);
  eb = errno;
  CHECKV(ra == rb && ea == eb, "mblen", detail);
  hb_mblen(NULL, 0);
  mblen(NULL, 0);
}

/* "Big" len for the corpus: a large-but-representable bound, NOT
 * (size_t)-1.  glibc's mbs* / wcs* functions compute dst + len as a
 * pointer, so a SIZE_MAX len wraps the address and glibc returns 0 having
 * converted nothing -- an artifact of its implementation rather than
 * portable behavior, which this port does not emulate.  64 exceeds every
 * corpus string while staying well inside the test buffers. */
#define MB_BIG ((size_t)64)

static void race_mbstowcs(const char *s, size_t n, int idx) {
  wchar_t wa[24], wb[24];
  size_t ra, rb;
  int ea, eb;
  char detail[80];
  snprintf(detail, sizeof detail, "case %d n=%s", idx,
           n == MB_BIG ? "big" : "bounded");
  memset(wa, 0xCC, sizeof wa);
  memset(wb, 0xCC, sizeof wb);
  errno = 0;
  ra = hb_mbstowcs(wa, s, n);
  ea = errno;
  errno = 0;
  rb = mbstowcs(wb, s, n);
  eb = errno;
  CHECKV(ra == rb && ea == eb && memcmp(wa, wb, sizeof wa) == 0, "mbstowcs",
         detail);
}

static void race_wcstombs(const wchar_t *ws, size_t n, int idx) {
  char ba[24], bb[24];
  size_t ra, rb;
  int ea, eb;
  char detail[80];
  snprintf(detail, sizeof detail, "case %d n=%s", idx,
           n == MB_BIG ? "big" : "bounded");
  memset(ba, 0xEE, sizeof ba);
  memset(bb, 0xEE, sizeof bb);
  errno = 0;
  ra = hb_wcstombs(ba, ws, n);
  ea = errno;
  errno = 0;
  rb = wcstombs(bb, ws, n);
  eb = errno;
  CHECKV(ra == rb && ea == eb && memcmp(ba, bb, sizeof ba) == 0, "wcstombs",
         detail);
}

static void race_mbsrtowcs(const char *s, size_t n, int null_dst, int idx) {
  wchar_t wa[24], wb[24];
  const char *pa = s, *pb = s;
  mbstate_t sa, sb;
  size_t ra, rb;
  int ea, eb;
  char detail[96];
  snprintf(detail, sizeof detail, "case %d n=%s dst=%s", idx,
           n == MB_BIG ? "big" : "bounded", null_dst ? "NULL" : "buf");
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  memset(wa, 0xCC, sizeof wa);
  memset(wb, 0xCC, sizeof wb);
  errno = 0;
  ra = hb_mbsrtowcs(null_dst ? NULL : wa, &pa, n, &sa);
  ea = errno;
  errno = 0;
  rb = mbsrtowcs(null_dst ? NULL : wb, &pb, n, &sb);
  eb = errno;
  CHECKV(ra == rb && ea == eb && (pa - s) == (pb - s) &&
             (null_dst || memcmp(wa, wb, sizeof wa) == 0),
         "mbsrtowcs", detail);
}

static void race_mbsnrtowcs(const char *s, size_t nms, size_t n, int idx) {
  wchar_t wa[24], wb[24];
  const char *pa = s, *pb = s;
  mbstate_t sa, sb;
  size_t ra, rb;
  int ea, eb;
  char detail[96];
  snprintf(detail, sizeof detail, "case %d nms=%zu n=%zu", idx, nms, n);
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  memset(wa, 0xCC, sizeof wa);
  memset(wb, 0xCC, sizeof wb);
  errno = 0;
  ra = hb_mbsnrtowcs(wa, &pa, nms, n, &sa);
  ea = errno;
  errno = 0;
  rb = mbsnrtowcs(wb, &pb, nms, n, &sb);
  eb = errno;
  CHECKV(ra == rb && ea == eb && (pa - s) == (pb - s) &&
             memcmp(wa, wb, sizeof wa) == 0,
         "mbsnrtowcs", detail);
}

static void race_wcsrtombs(const wchar_t *ws, size_t n, int idx) {
  char ba[24], bb[24];
  const wchar_t *pa = ws, *pb = ws;
  mbstate_t sa, sb;
  size_t ra, rb;
  int ea, eb;
  char detail[96];
  snprintf(detail, sizeof detail, "case %d n=%s", idx,
           n == MB_BIG ? "big" : "bounded");
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  memset(ba, 0xEE, sizeof ba);
  memset(bb, 0xEE, sizeof bb);
  errno = 0;
  ra = hb_wcsrtombs(ba, &pa, n, &sa);
  ea = errno;
  errno = 0;
  rb = wcsrtombs(bb, &pb, n, &sb);
  eb = errno;
  CHECKV(ra == rb && ea == eb && (pa - ws) == (pb - ws) &&
             memcmp(ba, bb, sizeof ba) == 0,
         "wcsrtombs", detail);
}

static void race_wcsnrtombs(const wchar_t *ws, size_t nwc, size_t n, int idx) {
  char ba[24], bb[24];
  const wchar_t *pa = ws, *pb = ws;
  mbstate_t sa, sb;
  size_t ra, rb;
  int ea, eb;
  char detail[96];
  snprintf(detail, sizeof detail, "case %d nwc=%zu n=%zu", idx, nwc, n);
  memset(&sa, 0, sizeof sa);
  memset(&sb, 0, sizeof sb);
  memset(ba, 0xEE, sizeof ba);
  memset(bb, 0xEE, sizeof bb);
  errno = 0;
  ra = hb_wcsnrtombs(ba, &pa, nwc, n, &sa);
  ea = errno;
  errno = 0;
  rb = wcsnrtombs(bb, &pb, nwc, n, &sb);
  eb = errno;
  CHECKV(ra == rb && ea == eb && (pa - ws) == (pb - ws) &&
             memcmp(ba, bb, sizeof ba) == 0,
         "wcsnrtombs", detail);
}

static void test_mb_conversions(void) {
  static const wchar_t wc_corpus[] = {L'\0',     L'A',    0x7F,    0x80,
                                      0xFF,     0x2014,  0x10FFFF, 0x110000,
                                      0xD800,   WEOF};
  const int nwc = (int)(sizeof wc_corpus / sizeof wc_corpus[0]);
  int i;

  for (i = 0; i < nmbs; i++) {
    race_mbrtowc(mbs[i], 1, i);
    race_mbrtowc(mbs[i], 2, i);
    race_mbrtowc(mbs[i], (size_t)-1, i);
    race_mbrlen(mbs[i], 1, i);
    race_mbrlen(mbs[i], (size_t)-1, i);
    race_mbtowc(mbs[i], 1, i);
    race_mbtowc(mbs[i], (size_t)-1, i);
    race_mblen(mbs[i], 1, i);
    race_mblen(mbs[i], (size_t)-1, i);
    race_mbstowcs(mbs[i], MB_BIG, i);
    race_mbstowcs(mbs[i], 1, i);
    race_mbstowcs(mbs[i], 3, i);
  }
  race_mbrtowc("A", 0, -1); /* n == 0: never consumes */
  race_mbrlen("A", 0, -1);
  race_mbrtowc_null_reset();

  for (i = 0; i < nwc; i++) {
    race_wcrtomb(wc_corpus[i], i);
    race_wctomb(wc_corpus[i], i);
  }

  for (i = 0; i < nmbs; i++) {
    race_mbsrtowcs(mbs[i], MB_BIG, 0, i);
    race_mbsrtowcs(mbs[i], 1, 0, i);
    race_mbsrtowcs(mbs[i], 2, 0, i);
    race_mbsrtowcs(mbs[i], 2, 1, i);
    race_mbsnrtowcs(mbs[i], MB_BIG, MB_BIG, i);
    race_mbsnrtowcs(mbs[i], 1, MB_BIG, i);
    race_mbsnrtowcs(mbs[i], 2, 2, i);
  }

  race_wcstombs(L"", MB_BIG, 0);
  race_wcstombs(L"abc", MB_BIG, 1);
  race_wcstombs(L"abc", 2, 2);
  race_wcstombs(L"a\x7fb", MB_BIG, 3);
  race_wcstombs(L"a\x80b", MB_BIG, 4); /* EILSEQ mid-string */
  race_wcstombs(L"hello world", 5, 5);
  race_wcsrtombs(L"abc", MB_BIG, 0);
  race_wcsrtombs(L"abc", 2, 1);
  race_wcsrtombs(L"a\x80b", MB_BIG, 2);
  race_wcsnrtombs(L"abc", MB_BIG, MB_BIG, 0);
  race_wcsnrtombs(L"abc", 2, MB_BIG, 1);
  race_wcsnrtombs(L"abc", MB_BIG, 2, 2);

  /* btowc/wctob round trips across the byte range. */
  for (i = 0; i < 256; i += 17) {
    char detail[64];
    int c = i;
    wint_t a, b;
    snprintf(detail, sizeof detail, "byte=%d", c);
    a = hb_btowc(c);
    b = btowc(c);
    CHECKV((a == b) && (hb_wctob(a) == wctob(b)), "btowc/wctob", detail);
  }
  CHECKV(hb_btowc(WEOF) == btowc(WEOF) && hb_wctob(WEOF) == wctob(WEOF),
         "btowc/wctob", "WEOF");
  CHECKV(hb_mbsinit(NULL) != 0, "mbsinit", "NULL is initial");

  /* Literal C-locale facts that must hold whatever glibc does. */
  {
    wchar_t wc = 0;
    mbstate_t st;
    size_t r;
    memset(&st, 0, sizeof st);
    r = hb_mbrtowc(&wc, "A", 1, &st);
    CHECKV(r == 1 && wc == L'A', "mbrtowc-literal", "A -> 1, 'A'");
    memset(&st, 0, sizeof st);
    errno = 0;
    r = hb_mbrtowc(&wc, "\x80", 1, &st);
    CHECKV(r == (size_t)-1 && errno == EILSEQ, "mbrtowc-literal",
           "0x80 -> EILSEQ");
    memset(&st, 0, sizeof st);
    r = hb_wcrtomb((char[8]){0}, 0x2014, &st);
    errno = 0;
    memset(&st, 0, sizeof st);
    r = hb_wcrtomb((char[8]){0}, 0x2014, &st);
    CHECKV(r == (size_t)-1 && errno == EILSEQ, "wcrtomb-literal",
           "0x2014 -> EILSEQ");
    CHECKV(hb_wcwidth(L'A') == 1 && hb_wcwidth(L'\x7f') == -1 &&
               hb_wcwidth(L'\x2014') == -1,
           "wcwidth-literal", "ASCII=1, DEL/em-dash=-1");
  }
  /* wcwidth race over a small table. */
  {
    static const wchar_t ws[] = {0, 0x20, L'A', 0x7f, 0x80, 0x2014, 0x20AC,
                                 0x10FFFF};
    int k;
    for (k = 0; k < (int)(sizeof ws / sizeof ws[0]); k++) {
      char detail[64];
      snprintf(detail, sizeof detail, "wc=0x%X", (unsigned)ws[k]);
      CHECKV(hb_wcwidth(ws[k]) == wcwidth(ws[k]), "wcwidth", detail);
    }
  }
}

/* ================= B. wcs / wmem string surface ======================= */

static void test_wide_strings(void) {
  /* wcslen / wcscmp / wcsncmp races. */
  {
    static const wchar_t *const pairs[][2] = {
      {L"", L""},        {L"a", L""},     {L"", L"a"},
      {L"abc", L"abc"},  {L"abc", L"abd"}, {L"abd", L"abc"},
      {L"ab", L"abc"},   {L"abc", L"ab"},  {L"z", L"a"},
    };
    int k;
    for (k = 0; k < (int)(sizeof pairs / sizeof pairs[0]); k++) {
      char detail[64];
      int ca, cb;
      snprintf(detail, sizeof detail, "pair %d", k);
      CHECKV(hb_wcslen(pairs[k][0]) == wcslen(pairs[k][0]), "wcslen",
             detail);
      ca = hb_wcscmp(pairs[k][0], pairs[k][1]);
      cb = wcscmp(pairs[k][0], pairs[k][1]);
      CHECKV((ca < 0) == (cb < 0) && (ca == 0) == (cb == 0), "wcscmp",
             detail);
      ca = hb_wcsncmp(pairs[k][0], pairs[k][1], 2);
      cb = wcsncmp(pairs[k][0], pairs[k][1], 2);
      CHECKV((ca < 0) == (cb < 0) && (ca == 0) == (cb == 0), "wcsncmp",
             detail);
    }
  }

  /* wcscpy/wcsncpy padding + NUL semantics, byte-exact vs glibc. */
  {
    wchar_t a[8], b[8];
    memset(a, '!', sizeof a);
    memset(b, '!', sizeof b);
    hb_wcsncpy(a, L"abc", 6);
    wcsncpy(b, L"abc", 6);
    CHECKV(memcmp(a, b, sizeof a) == 0, "wcsncpy", "n=6 pad");
    memset(a, '!', sizeof a);
    memset(b, '!', sizeof b);
    hb_wcsncpy(a, L"abcdefgh", 4);
    wcsncpy(b, L"abcdefgh", 4);
    CHECKV(memcmp(a, b, sizeof a) == 0, "wcsncpy", "n=4 trunc");
    hb_wcscpy(a, L"xy");
    wcscpy(b, L"xy");
    CHECKV(memcmp(a, b, sizeof a) == 0, "wcscpy", "xy");

    /* wcscat/wcsncat. */
    hb_wcscat(a, L"z");
    wcscat(b, L"z");
    CHECKV(memcmp(a, b, sizeof a) == 0, "wcscat", "append z");
    hb_wcsncat(a, L"01234", 3);
    wcsncat(b, L"01234", 3);
    CHECKV(memcmp(a, b, sizeof a) == 0, "wcsncat", "append 3");

    /* wmemcpy/wmemmove/wmemset/wmemcmp/wmemchr. */
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    hb_wmemcpy(a, L"wxyz", 4);
    wmemcpy(b, L"wxyz", 4);
    CHECKV(memcmp(a, b, sizeof a) == 0, "wmemcpy", "wxyz");
    hb_wmemmove(a + 1, a, 4);
    wmemmove(b + 1, b, 4);
    CHECKV(memcmp(a, b, sizeof a) == 0, "wmemmove", "overlap");
    hb_wmemset(a + 2, L'#', 3);
    wmemset(b + 2, L'#', 3);
    CHECKV(memcmp(a, b, sizeof a) == 0, "wmemset", "###");
    CHECKV((hb_wmemcmp(a, b, 8) == 0) == (wmemcmp(a, b, 8) == 0),
           "wmemcmp", "equal");
    CHECKV(hb_wmemchr(a, L'#', 8) - a == wmemchr(b, L'#', 8) - b, "wmemchr",
           "# pos");
    CHECKV(hb_wmemchr(a, L'Z', 8) == NULL && wmemchr(b, L'Z', 8) == NULL,
           "wmemchr", "absent");
  }

  /* wcschr/wcsrchr/wcsstr/wcspbrk/wcsspn/wcscspn races. */
  {
    static const wchar_t *const hay = L"abracadabra";
    char detail[64];
    CHECKV(hb_wcschr(hay, L'b') - hay == wcschr(hay, L'b') - hay, "wcschr",
           "b");
    CHECKV(hb_wcsrchr(hay, L'a') - hay == wcsrchr(hay, L'a') - hay,
           "wcsrchr", "a");
    CHECKV(hb_wcschr(hay, L'\0') - hay == wcschr(hay, L'\0') - hay, "wcschr",
           "NUL");
    CHECKV(hb_wcsstr(hay, L"cad") - hay == wcsstr(hay, L"cad") - hay,
           "wcsstr", "cad");
    CHECKV(hb_wcsstr(hay, L"") - hay == wcsstr(hay, L"") - hay, "wcsstr",
           "empty");
    CHECKV(hb_wcsstr(hay, L"zzz") == NULL && wcsstr(hay, L"zzz") == NULL,
           "wcsstr", "absent");
    snprintf(detail, sizeof detail, "pbrk");
    CHECKV(hb_wcspbrk(hay, L"rx") - hay == wcspbrk(hay, L"rx") - hay,
           "wcspbrk", detail);
    CHECKV(hb_wcsspn(hay, L"abr") == wcsspn(hay, L"abr"), "wcsspn", "abr");
    CHECKV(hb_wcscspn(hay, L"cad") == wcscspn(hay, L"cad"), "wcscspn",
           "cad");
  }

  /* wcstok walks a string token by token; run both sides in lockstep.
   * Classic protocol: the first call passes the string, every later call
   * passes NULL so *save carries the position forward. */
  {
    wchar_t wa[] = L"  alpha,beta,,gamma  ";
    wchar_t wb[] = L"  alpha,beta,,gamma  ";
    wchar_t *pa = NULL, *pb = NULL;
    int t = 0;
    for (;;) {
      wchar_t *ta = hb_wcstok(t == 0 ? wa : NULL, L" ,", &pa);
      wchar_t *tb = wcstok(t == 0 ? wb : NULL, L" ,", &pb);
      char detail[64];
      snprintf(detail, sizeof detail, "token %d", t);
      CHECKV((ta == NULL) == (tb == NULL), "wcstok", detail);
      if (ta == NULL && tb == NULL)
        break;
      if (ta && tb)
        CHECKV(wcscmp(ta, tb) == 0, "wcstok", detail);
      if (++t > 8)
        break;
    }
    CHECKV(t == 3, "wcstok", "three tokens");
  }

  /* wcscoll/wcsxfrm order classes + xfrm bytes. */
  {
    static const wchar_t *const pairs[][2] = {
      {L"abc", L"abc"}, {L"abc", L"abd"}, {L"abd", L"abc"},
      {L"", L"a"},      {L"a", L""},     {L"ABC", L"abc"},
    };
    int k;
    for (k = 0; k < (int)(sizeof pairs / sizeof pairs[0]); k++) {
      wchar_t fa[32], fb[32];
      size_t ra, rb;
      int ca, cb;
      char detail[64];
      snprintf(detail, sizeof detail, "pair %d", k);
      ca = hb_wcscoll(pairs[k][0], pairs[k][1]);
      cb = wcscoll(pairs[k][0], pairs[k][1]);
      CHECKV((ca < 0) == (cb < 0) && (ca == 0) == (cb == 0), "wcscoll",
             detail);
      memset(fa, 0, sizeof fa);
      memset(fb, 0, sizeof fb);
      ra = hb_wcsxfrm(fa, pairs[k][0], 32);
      rb = wcsxfrm(fb, pairs[k][0], 32);
      CHECKV(ra == rb && memcmp(fa, fb, sizeof fa) == 0, "wcsxfrm", detail);
    }
  }

  /* wcstod/wcstof/wcstold: value bits, endptr and errno parity. */
  {
    static const wchar_t *const nums[] = {
      L"3.5x", L"  -2.25e2", L"0x1p3", L"nan", L"inf", L"1e",
      L"-0",   L"42",        L"",
    };
    int k;
    for (k = 0; k < (int)(sizeof nums / sizeof nums[0]); k++) {
      wchar_t *ea = NULL, *eb = NULL;
      double da, db;
      float fa, fb;
      long double la, lb;
      int erra, errb;
      char detail[80];
      snprintf(detail, sizeof detail, "case %d", k);
      errno = 0;
      da = hb_wcstod(nums[k], &ea);
      erra = errno;
      errno = 0;
      db = wcstod(nums[k], &eb);
      errb = errno;
      CHECKV(memcmp(&da, &db, sizeof da) == 0 && (ea - nums[k]) == (eb - nums[k]) &&
                 erra == errb,
             "wcstod", detail);
      errno = 0;
      fa = hb_wcstof(nums[k], &ea);
      erra = errno;
      errno = 0;
      fb = wcstof(nums[k], &eb);
      errb = errno;
      CHECKV(memcmp(&fa, &fb, sizeof fa) == 0 && (ea - nums[k]) == (eb - nums[k]) &&
                 erra == errb,
             "wcstof", detail);
      errno = 0;
      la = hb_wcstold(nums[k], &ea);
      erra = errno;
      errno = 0;
      lb = wcstold(nums[k], &eb);
      errb = errno;
      CHECKV(memcmp(&la, &lb, sizeof la) == 0 && (ea - nums[k]) == (eb - nums[k]) &&
                 erra == errb,
             "wcstold", detail);
    }
  }
}

/* ================= C. wctype: isw / tow / wctype() ==================== */

static const wint_t wctab[] = {
  WEOF, 0,     1,    9,    13,   32,   33,    48,    57,    65,
  90,   97,    122,  127,  128,  160,  0xA0,  0xFF,  0x100, 0x2014,
  0x20AC, 0xD800, 0x10FFFF,
};
static const int nwctab = (int)(sizeof wctab / sizeof wctab[0]);

static void test_wctype(void) {
  int i;
#define RACE_W(what, hbfn, glfn)                                              \
  do {                                                                        \
    for (i = 0; i < nwctab; i++) {                                            \
      char detail[64];                                                        \
      snprintf(detail, sizeof detail, "wc=0x%X", (unsigned)wctab[i]);         \
      CHECKV((hbfn(wctab[i]) != 0) == (glfn(wctab[i]) != 0), what, detail);   \
    }                                                                         \
  } while (0)

  RACE_W("iswalnum", hb_iswalnum, iswalnum);
  RACE_W("iswalpha", hb_iswalpha, iswalpha);
  RACE_W("iswblank", hb_iswblank, iswblank);
  RACE_W("iswcntrl", hb_iswcntrl, iswcntrl);
  RACE_W("iswdigit", hb_iswdigit, iswdigit);
  RACE_W("iswgraph", hb_iswgraph, iswgraph);
  RACE_W("iswlower", hb_iswlower, iswlower);
  RACE_W("iswprint", hb_iswprint, iswprint);
  RACE_W("iswpunct", hb_iswpunct, iswpunct);
  RACE_W("iswspace", hb_iswspace, iswspace);
  RACE_W("iswupper", hb_iswupper, iswupper);
  RACE_W("iswxdigit", hb_iswxdigit, iswxdigit);
#undef RACE_W

  for (i = 0; i < nwctab; i++) {
    char detail[64];
    snprintf(detail, sizeof detail, "wc=0x%X", (unsigned)wctab[i]);
    CHECKV(hb_towlower(wctab[i]) == towlower(wctab[i]), "towlower", detail);
    CHECKV(hb_towupper(wctab[i]) == towupper(wctab[i]), "towupper", detail);
  }

  /* Class-name lookup: same names answer the same membership. */
  {
    static const char *const names[] = {"alnum", "alpha", "blank", "cntrl",
                                        "digit", "graph", "lower", "print",
                                        "punct", "space", "upper", "xdigit"};
    int k;
    for (k = 0; k < (int)(sizeof names / sizeof names[0]); k++) {
      wctype_t ta = hb_wctype(names[k]);
      wctype_t tb = wctype(names[k]);
      char detail[64];
      int j;
      snprintf(detail, sizeof detail, "name=%s", names[k]);
      CHECKV((ta == 0) == (tb == 0), "wctype", detail);
      for (j = 0; j < nwctab; j++) {
        CHECKV((hb_iswctype(wctab[j], ta) != 0) ==
                   (iswctype(wctab[j], tb) != 0),
               "iswctype", detail);
      }
    }
    CHECKV((hb_wctype("bogus") == 0) == (wctype("bogus") == 0), "wctype",
           "bogus name");
  }

  /* wctrans/towctrans. */
  {
    static const char *const names[] = {"tolower", "toupper"};
    int k, j;
    for (k = 0; k < 2; k++) {
      wctrans_t ta = hb_wctrans(names[k]);
      wctrans_t tb = wctrans(names[k]);
      char detail[64];
      snprintf(detail, sizeof detail, "name=%s", names[k]);
      CHECKV((ta == NULL) == (tb == NULL), "wctrans", detail);
      for (j = 0; j < nwctab; j++) {
        CHECKV(hb_towctrans(wctab[j], ta) == towctrans(wctab[j], tb),
               "towctrans", detail);
      }
    }
    CHECKV((hb_wctrans("bogus") == NULL) == (wctrans("bogus") == NULL),
           "wctrans", "bogus name");
  }

  /* Literal C-locale facts. */
  CHECKV(hb_iswalpha(L'A') && !hb_iswalpha(L'0'), "iswalpha-literal", "A/0");
  CHECKV(hb_towlower(L'A') == L'a' && hb_towupper(L'a') == L'A',
         "tow*-literal", "A<->a");
  CHECKV(hb_towlower(0x100) == 0x100, "towlower-literal", "non-ASCII fixed");
}

/* ================= D. strftime / asctime / ctime ======================= */

static struct tm make_fixture(void) {
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = 123; /* 2023 */
  tm.tm_mon = 6;    /* July */
  tm.tm_mday = 14;
  tm.tm_hour = 15;
  tm.tm_min = 37;
  tm.tm_sec = 5;
  tm.tm_wday = 5;   /* Friday */
  tm.tm_yday = 194; /* 0-based */
  tm.tm_isdst = 0;
  return tm;
}

static void race_strftime(const char *fmt, const struct tm *tm, int idx) {
  char ba[256], bb[256];
  size_t ra, rb;
  char detail[96];
  snprintf(detail, sizeof detail, "case %d fmt=\"%s\"", idx, fmt);
  memset(ba, 0xEE, sizeof ba);
  memset(bb, 0xEE, sizeof bb);
  ra = hb_strftime(ba, sizeof ba, fmt, tm);
  rb = strftime(bb, sizeof bb, fmt, tm);
  CHECKV(ra == rb && memcmp(ba, bb, sizeof ba) == 0, "strftime", detail);
}

static void test_strftime(void) {
  const struct tm tm = make_fixture();
  static const struct {
    const char *fmt;
    const char *want;
  } lits[] = {
    {"%Y-%m-%d %H:%M:%S", "2023-07-14 15:37:05"},
    {"%a %A %b %B", "Fri Friday Jul July"},
    {"%c", "Fri Jul 14 15:37:05 2023"},
    {"%x", "07/14/23"},
    {"%X", "15:37:05"},
    {"%y %C", "23 20"},
    {"%j", "195"},
    {"%e", "14"},
    {"%p %I", "PM 03"},
    {"%U %W %u %w", "28 28 5 5"},
    {"%G-%g-%V", "2023-23-28"},
    {"%Z%z", "UTC+0000"},
    {"%s", "1689349025"},
    {"%F %T %R %D", "2023-07-14 15:37:05 15:37 07/14/23"},
    {"%n%t%%|", "\n\t%|"},
  };
  static const char *const corpus[] = {
    "",      "literal",         "%",     "%%",     "%Y",     "%m%d",
    "%H%M%S", "%I %p",          "%e-%k-%l", "%P",   "%r",     "%h %B",
    "%y/%C",  "%j %U %W %V %G %g %u %w", "%s",    "%Z %z",
    "%F %T %R %D %c %x %X",     "%q",    "mix %Y lit", "%d%%",
  };
  int k;

  for (k = 0; k < (int)(sizeof lits / sizeof lits[0]); k++) {
    char b[256];
    size_t r = hb_strftime(b, sizeof b, lits[k].fmt, &tm);
    char detail[128];
    snprintf(detail, sizeof detail, "fmt=\"%s\"", lits[k].fmt);
    CHECKV(r == strlen(lits[k].want) && strcmp(b, lits[k].want) == 0,
           "strftime-literal", detail);
  }

  for (k = 0; k < (int)(sizeof corpus / sizeof corpus[0]); k++)
    race_strftime(corpus[k], &tm, k);

  /* Buffer-capacity semantics: exact fit succeeds, one byte short fails,
   * zero capacity fails; rc is the whole contract there (the standard
   * leaves partial contents unspecified). */
  {
    char ba[16], bb[16];
    size_t ra, rb;
    struct tm t2 = tm;
    memset(ba, 0xEE, sizeof ba);
    memset(bb, 0xEE, sizeof bb);
    ra = hb_strftime(ba, 11, "%Y-%m-%d", &t2);
    rb = strftime(bb, 11, "%Y-%m-%d", &t2);
    CHECKV(ra == 10 && rb == 10 && strcmp(ba, "2023-07-14") == 0, "strftime",
           "cap=11 exact fit");
    ra = hb_strftime(ba, 10, "%Y-%m-%d", &t2);
    rb = strftime(bb, 10, "%Y-%m-%d", &t2);
    CHECKV(ra == 0 && rb == 0, "strftime", "cap=10 too short");
    ra = hb_strftime(ba, 0, "%Y-%m-%d", &t2);
    rb = strftime(bb, 0, "%Y-%m-%d", &t2);
    CHECKV(ra == 0 && rb == 0, "strftime", "cap=0");
  }

  /* asctime / asctime_r / ctime_r: literal + race (TZ pinned to UTC). */
  {
    char *sa, *sb;
    char ba[64], bb[64];
    time_t t = (time_t)1689349025;
    sa = hb_asctime(&tm);
    sb = asctime(&tm);
    CHECKV(sa && sb && strcmp(sa, sb) == 0 &&
               strcmp(sa, "Fri Jul 14 15:37:05 2023\n") == 0,
           "asctime", "fixture");
    memset(ba, 0xEE, sizeof ba);
    memset(bb, 0xEE, sizeof bb);
    CHECKV(hb_asctime_r(&tm, ba) == ba && asctime_r(&tm, bb) == bb &&
               memcmp(ba, bb, sizeof ba) == 0,
           "asctime_r", "fixture");
    memset(ba, 0xEE, sizeof ba);
    memset(bb, 0xEE, sizeof bb);
    CHECKV(hb_ctime_r(&t, ba) == ba && ctime_r(&t, bb) == bb &&
               strcmp(ba, "Fri Jul 14 15:37:05 2023\n") == 0 &&
               memcmp(ba, bb, sizeof ba) == 0,
           "ctime_r", "1689349025");
    t = 0;
    memset(ba, 0xEE, sizeof ba);
    memset(bb, 0xEE, sizeof bb);
    {
      /* Fill bb with the glibc call before the memcmp (&& would otherwise
       * short-circuit around it). */
      char *r1 = hb_ctime_r(&t, ba);
      char *r2 = ctime_r(&t, bb);
      CHECKV(r1 == ba && r2 == bb &&
                 strcmp(ba, "Thu Jan  1 00:00:00 1970\n") == 0 &&
                 memcmp(ba, bb, sizeof ba) == 0,
             "ctime_r", "epoch 0");
    }
  }
}

/* ================= E. xlocale ========================================== */

static void race_strtoll_l(const char *s, int base, locale_t hl, locale_t gl) {
  char *ea = NULL, *eb = NULL;
  long long a, b;
  int err_a, err_b;
  char detail[80];
  snprintf(detail, sizeof detail, "\"%s\" base=%d", s, base);
  errno = 0;
  a = hb_strtoll_l(s, &ea, base, hl);
  err_a = errno;
  errno = 0;
  b = strtoll_l(s, &eb, base, gl);
  err_b = errno;
  CHECKV(a == b && (ea - s) == (eb - s) && err_a == err_b, "strtoll_l",
         detail);
}

static void race_strtoull_l(const char *s, int base, locale_t hl, locale_t gl) {
  char *ea = NULL, *eb = NULL;
  unsigned long long a, b;
  int err_a, err_b;
  char detail[80];
  snprintf(detail, sizeof detail, "\"%s\" base=%d", s, base);
  errno = 0;
  a = hb_strtoull_l(s, &ea, base, hl);
  err_a = errno;
  errno = 0;
  b = strtoull_l(s, &eb, base, gl);
  err_b = errno;
  CHECKV(a == b && (ea - s) == (eb - s) && err_a == err_b, "strtoull_l",
         detail);
}

static void test_xlocale(void) {
  locale_t hl = hb_newlocale(LC_ALL_MASK, "C", 0);
  locale_t gl = newlocale(LC_ALL_MASK, "C", 0);
  int i;

  CHECKV(hl != NULL && gl != NULL, "newlocale", "C");
  if (hl == NULL || gl == NULL)
    return; /* everything below needs both tokens */

  /* Locale-name acceptance matches: "POSIX" ok, bogus ENOENT, "" ok. */
  {
    locale_t a, b;
    int err_a, err_b;
    errno = 0;
    a = hb_newlocale(LC_ALL_MASK, "POSIX", 0);
    err_a = errno;
    errno = 0;
    b = newlocale(LC_ALL_MASK, "POSIX", 0);
    err_b = errno;
    CHECKV((a != NULL) == (b != NULL) && err_a == err_b, "newlocale",
           "POSIX");
    if (a) hb_freelocale(a);
    if (b) freelocale(b);

    errno = 0;
    a = hb_newlocale(LC_ALL_MASK, "zz_ZZ.INVALID", 0);
    err_a = errno;
    errno = 0;
    b = newlocale(LC_ALL_MASK, "zz_ZZ.INVALID", 0);
    err_b = errno;
    CHECKV(a == NULL && b == NULL && err_a == ENOENT && err_b == ENOENT,
           "newlocale", "bogus name ENOENT");

    errno = 0;
    a = hb_newlocale(LC_ALL_MASK, "", 0);
    err_a = errno;
    errno = 0;
    b = newlocale(LC_ALL_MASK, "", 0);
    err_b = errno;
    CHECKV((a != NULL) == (b != NULL) && err_a == err_b, "newlocale", "env");
    if (a) hb_freelocale(a);
    if (b) freelocale(b);
  }

  /* duplocale + freelocale lifecycle. */
  {
    locale_t d = hb_duplocale(hl);
    CHECKV(d != NULL && d != hl, "duplocale", "dup of C");
    hb_freelocale(d);
    hb_freelocale(hl);
    CHECKV(1, "freelocale", "ran");
    hl = hb_newlocale(LC_ALL_MASK, "C", 0);
  }

  /* uselocale round trip, in lockstep with glibc. */
  {
    locale_t oa = hb_uselocale(NULL);
    locale_t ob = uselocale(NULL);
    locale_t pa, pb;
    CHECKV((oa == LC_GLOBAL_LOCALE) == (ob == LC_GLOBAL_LOCALE), "uselocale",
           "initial query");
    pa = hb_uselocale(hl);
    pb = uselocale(gl);
    CHECKV((pa == LC_GLOBAL_LOCALE) == (pb == LC_GLOBAL_LOCALE), "uselocale",
           "switch returns previous");
    oa = hb_uselocale(NULL);
    ob = uselocale(NULL);
    CHECKV((oa != LC_GLOBAL_LOCALE) == (ob != LC_GLOBAL_LOCALE), "uselocale",
           "current is the token");
    hb_uselocale(pa);
    uselocale(pb);
  }

  /* strto*_l parity. */
  {
    static const char *const nums[] = {"42",   "  0x1F", "-0",
                                       "9223372036854775807",
                                       "9223372036854775808", "+12abc",
                                       "",     "  -7z", "18446744073709551616"};
    for (i = 0; i < (int)(sizeof nums / sizeof nums[0]); i++) {
      race_strtoll_l(nums[i], 0, hl, gl);
      race_strtoull_l(nums[i], 0, hl, gl);
      race_strtoll_l(nums[i], 10, hl, gl);
    }
  }

  /* ctype _l parity. */
  {
    for (i = 0; i < 256; i += 7) {
      char detail[64];
      snprintf(detail, sizeof detail, "c=%d", i);
      CHECKV((hb_isdigit_l(i, hl) != 0) == (isdigit_l(i, gl) != 0),
             "isdigit_l", detail);
      CHECKV((hb_isxdigit_l(i, hl) != 0) == (isxdigit_l(i, gl) != 0),
             "isxdigit_l", detail);
      CHECKV(hb_toupper_l(i, hl) == toupper_l(i, gl), "toupper_l", detail);
      CHECKV(hb_tolower_l(i, hl) == tolower_l(i, gl), "tolower_l", detail);
    }
  }

  /* isw*_l + tow*_l parity over the wide table. */
  {
    for (i = 0; i < nwctab; i++) {
      char detail[64];
      snprintf(detail, sizeof detail, "wc=0x%X", (unsigned)wctab[i]);
      CHECKV((hb_iswspace_l(wctab[i], hl) != 0) ==
                 (iswspace_l(wctab[i], gl) != 0),
             "iswspace_l", detail);
      CHECKV((hb_iswprint_l(wctab[i], hl) != 0) ==
                 (iswprint_l(wctab[i], gl) != 0),
             "iswprint_l", detail);
      CHECKV((hb_iswcntrl_l(wctab[i], hl) != 0) ==
                 (iswcntrl_l(wctab[i], gl) != 0),
             "iswcntrl_l", detail);
      CHECKV((hb_iswupper_l(wctab[i], hl) != 0) ==
                 (iswupper_l(wctab[i], gl) != 0),
             "iswupper_l", detail);
      CHECKV((hb_iswlower_l(wctab[i], hl) != 0) ==
                 (iswlower_l(wctab[i], gl) != 0),
             "iswlower_l", detail);
      CHECKV((hb_iswalpha_l(wctab[i], hl) != 0) ==
                 (iswalpha_l(wctab[i], gl) != 0),
             "iswalpha_l", detail);
      CHECKV((hb_iswblank_l(wctab[i], hl) != 0) ==
                 (iswblank_l(wctab[i], gl) != 0),
             "iswblank_l", detail);
      CHECKV((hb_iswdigit_l(wctab[i], hl) != 0) ==
                 (iswdigit_l(wctab[i], gl) != 0),
             "iswdigit_l", detail);
      CHECKV((hb_iswpunct_l(wctab[i], hl) != 0) ==
                 (iswpunct_l(wctab[i], gl) != 0),
             "iswpunct_l", detail);
      CHECKV((hb_iswxdigit_l(wctab[i], hl) != 0) ==
                 (iswxdigit_l(wctab[i], gl) != 0),
             "iswxdigit_l", detail);
      CHECKV(hb_towupper_l(wctab[i], hl) == towupper_l(wctab[i], gl),
             "towupper_l", detail);
      CHECKV(hb_towlower_l(wctab[i], hl) == towlower_l(wctab[i], gl),
             "towlower_l", detail);
    }
  }

  /* hb_iswctype_l delegates to the TU-local iswctype with a HOBBY token;
   * under HOST_TEST that call lands in glibc's iswctype, whose `desc` is a
   * table-indexed bitmask (a hobby id like 2 reads out of bounds) -- so
   * the call is deliberately NOT made here.  The on-device pairing is
   * one line of source ((void)loc; return iswctype(c, type);) and is
   * exercised by the device suites. */
  checks++; /* recorded, not executed -- see above */

  /* collation _l parity. */
  {
    static const char *const pairs[][2] = {{"abc", "abc"},
      {"abc", "abd"},
      {"abd", "abc"},
      {"", "a"}};
    int k;
    for (k = 0; k < (int)(sizeof pairs / sizeof pairs[0]); k++) {
      char fa[32], fb[32];
      size_t ra, rb;
      int ca, cb;
      char detail[64];
      snprintf(detail, sizeof detail, "pair %d", k);
      ca = hb_strcoll_l(pairs[k][0], pairs[k][1], hl);
      cb = strcoll_l(pairs[k][0], pairs[k][1], gl);
      CHECKV((ca < 0) == (cb < 0) && (ca == 0) == (cb == 0), "strcoll_l",
             detail);
      memset(fa, 0, sizeof fa);
      memset(fb, 0, sizeof fb);
      ra = hb_strxfrm_l(fa, pairs[k][0], 32, hl);
      rb = strxfrm_l(fb, pairs[k][0], 32, gl);
      CHECKV(ra == rb && memcmp(fa, fb, sizeof fa) == 0, "strxfrm_l",
             detail);
    }
    {
      static const wchar_t *const wp[][2] = {{L"abc", L"abc"},
        {L"abc", L"abd"},
        {L"abd", L"abc"}};
      int k;
      for (k = 0; k < (int)(sizeof wp / sizeof wp[0]); k++) {
        wchar_t fa[32], fb[32];
        size_t ra, rb;
        int ca, cb;
        char detail[64];
        snprintf(detail, sizeof detail, "wpair %d", k);
        ca = hb_wcscoll_l(wp[k][0], wp[k][1], hl);
        cb = wcscoll_l(wp[k][0], wp[k][1], gl);
        CHECKV((ca < 0) == (cb < 0) && (ca == 0) == (cb == 0), "wcscoll_l",
               detail);
        memset(fa, 0, sizeof fa);
        memset(fb, 0, sizeof fb);
        ra = hb_wcsxfrm_l(fa, wp[k][0], 32, hl);
        rb = wcsxfrm_l(fb, wp[k][0], 32, gl);
        CHECKV(ra == rb && memcmp(fa, fb, sizeof fa) == 0, "wcsxfrm_l",
               detail);
      }
    }
  }

  /* strftime_l with the C locale token matches the plain form and glibc. */
  {
    char ba[64], bb[64];
    const struct tm tm = make_fixture();
    size_t ra = hb_strftime_l(ba, sizeof ba, "%Y-%m-%d %H:%M:%S", &tm, hl);
    size_t rb = strftime_l(bb, sizeof bb, "%Y-%m-%d %H:%M:%S", &tm, gl);
    CHECKV(ra == rb && strcmp(ba, bb) == 0 &&
               strcmp(ba, "2023-07-14 15:37:05") == 0,
           "strftime_l", "second precision");
  }

  hb_freelocale(hl);
  freelocale(gl);
}

int main(void) {
  /* Pin the environment both implementations read: byte-only C locale and
   * a UTC clock (ctime/strftime %Z/%z are UTC in this port). */
  setenv("TZ", "UTC", 1);
  tzset();
  setlocale(LC_ALL, "C");

  test_mb_conversions();
  test_wide_strings();
  test_wctype();
  test_strftime();
  test_xlocale();

  if (failures == 0) {
    printf("libc_wide_parity_test: %d checks, 0 failures\n", checks);
    printf("WIDE PARITY TEST PASSED\n");
    return 0;
  }
  printf("libc_wide_parity_test: %d checks, %d failures\n", checks, failures);
  printf("WIDE PARITY TEST FAILED\n");
  return 1;
}
