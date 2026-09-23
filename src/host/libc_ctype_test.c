/*
 * HobbyOS Phase-1 host test: ctype.h vs glibc.
 *
 * src/libc/src/ctype.c compiled with -DHOST_TEST (hb_* names) and compared
 * against glibc's classification and conversion functions for EVERY value
 * in [-3..255] in the C locale. Exit 0 = full pass.
 */
#include <stdio.h>
#include <ctype.h>

extern int hb_isalnum(int);
extern int hb_isalpha(int);
extern int hb_isblank(int);
extern int hb_iscntrl(int);
extern int hb_isdigit(int);
extern int hb_isgraph(int);
extern int hb_islower(int);
extern int hb_isprint(int);
extern int hb_ispunct(int);
extern int hb_isspace(int);
extern int hb_isupper(int);
extern int hb_isxdigit(int);
extern int hb_tolower(int);
extern int hb_toupper(int);

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                   \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s (line %d)\n", what, __LINE__);  \
        }                                                   \
    } while (0)

#define CMP(fn)                                                     \
    do {                                                            \
        int v;                                                      \
        for (v = -3; v <= 255; v++) {                               \
            checks++;                                               \
            if (!!hb_##fn(v) != !!fn(v)) {                          \
                failures++;                                         \
                printf("FAIL " #fn "(%d) hb=%d glibc=%d\n",         \
                       v, !!hb_##fn(v), !!fn(v));                   \
            }                                                       \
        }                                                           \
    } while (0)

int main(void)
{
    CMP(isalnum);
    CMP(isalpha);
    CMP(isblank);
    CMP(iscntrl);
    CMP(isdigit);
    CMP(isgraph);
    CMP(islower);
    CMP(isprint);
    CMP(ispunct);
    CMP(isspace);
    CMP(isupper);
    CMP(isxdigit);

    /* Literal spot-checks that pin the C-locale table directly. */
    CHECK(hb_isspace(' ') && hb_isspace('\t') && hb_isspace('\n') &&
          hb_isspace('\v') && hb_isspace('\f') && hb_isspace('\r'),
          "isspace set");
    CHECK(!hb_isspace('x') && !hb_isspace(0x80), "isspace clear");
    CHECK(hb_isdigit('0') && hb_isdigit('9') && !hb_isdigit('A'), "isdigit");
    CHECK(hb_isxdigit('a') && hb_isxdigit('F') && hb_isxdigit('5') &&
          !hb_isxdigit('g'), "isxdigit");
    CHECK(hb_isalpha('A') && hb_isalpha('z') && !hb_isalpha('1') &&
          !hb_isalpha('_'), "isalpha");
    CHECK(hb_isalnum('9') && hb_isalnum('Z') && !hb_isalnum('!'), "isalnum");
    CHECK(hb_isblank(' ') && hb_isblank('\t') && !hb_isblank('\n'), "isblank");
    CHECK(hb_ispunct('.') && hb_ispunct('!') && hb_ispunct('~') &&
          !hb_ispunct('a'), "ispunct");
    CHECK(!hb_isgraph(' ') && hb_isgraph('!') && !hb_isgraph('\x7f'), "isgraph");
    CHECK(hb_isprint(' ') && hb_isprint('~') && !hb_isprint('\x7f'), "isprint");
    CHECK(hb_iscntrl('\x00') && hb_iscntrl('\x1f') && hb_iscntrl('\x7f') &&
          !hb_iscntrl(' '), "iscntrl");
    CHECK(hb_islower('a') && hb_islower('z') && !hb_islower('A'), "islower");
    CHECK(hb_isupper('A') && hb_isupper('Z') && !hb_isupper('a'), "isupper");

    /* tolower/toupper parity across the full range (incl EOF and 0x80+). */
    {
        int v;
        for (v = -3; v <= 255; v++) {
            checks++;
            if (hb_tolower(v) != tolower(v)) {
                failures++;
                printf("FAIL tolower(%d) hb=%d glibc=%d\n", v, hb_tolower(v), tolower(v));
            }
            checks++;
            if (hb_toupper(v) != toupper(v)) {
                failures++;
                printf("FAIL toupper(%d) hb=%d glibc=%d\n", v, hb_toupper(v), toupper(v));
            }
        }
    }
    CHECK(hb_tolower('A') == 'a' && hb_tolower('Z') == 'z', "tolower alpha");
    CHECK(hb_tolower('5') == '5', "tolower non-alpha");
    CHECK(hb_toupper('a') == 'A' && hb_toupper('z') == 'Z', "toupper alpha");
    CHECK(hb_toupper('5') == '5', "toupper non-alpha");

    printf("libc_ctype_test: %d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("LIB C CTYPE TEST PASSED\n");
        return 0;
    }
    printf("LIB C CTYPE TEST FAILED\n");
    return 1;
}
