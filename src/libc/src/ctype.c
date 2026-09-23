/* HobbyOS Phase-1 libc: ctype.c — C-locale character classes via a
 * 256-entry class table (glibc __ctype_b-compatible semantics).
 *
 * Under HOST_TEST the public functions compile as hb_* so host tests can
 * compare byte-exact behavior against glibc on every value 0..255.
 */
#include <stddef.h>

#ifdef HOST_TEST
#define isalnum hb_isalnum
#define isalpha hb_isalpha
#define isblank hb_isblank
#define iscntrl hb_iscntrl
#define isdigit hb_isdigit
#define isgraph hb_isgraph
#define islower hb_islower
#define isprint hb_isprint
#define ispunct hb_ispunct
#define isspace hb_isspace
#define isupper hb_isupper
#define isxdigit hb_isxdigit
#define tolower hb_tolower
#define toupper hb_toupper
#else
#include "ctype.h"
#endif

/* Bit positions in the class table. */
#define C_UP (1u << 0)  /* upper-case alpha */
#define C_LO (1u << 1)  /* lower-case alpha */
#define C_DI (1u << 2)  /* decimal digit */
#define C_SP (1u << 3)  /* whitespace */
#define C_PU (1u << 4)  /* punctuation */
#define C_GR (1u << 5)  /* printing, excluding space */
#define C_PR (1u << 6)  /* printing, including space */
#define C_CN (1u << 7)  /* control */
#define C_BL (1u << 8)  /* blank (space or tab) */
#define C_XD (1u << 9)  /* hex digit */

static const unsigned short hb_ctype[256] = {
    /* 0x00-0x1F: control; 0x09 tab is blank+space; 0x0A-0x0D space */
    [0x00] = C_CN, [0x01] = C_CN, [0x02] = C_CN, [0x03] = C_CN,
    [0x04] = C_CN, [0x05] = C_CN, [0x06] = C_CN, [0x07] = C_CN,
    [0x08] = C_CN, [0x09] = C_CN | C_SP | C_BL, [0x0A] = C_CN | C_SP,
    [0x0B] = C_CN | C_SP, [0x0C] = C_CN | C_SP, [0x0D] = C_CN | C_SP,
    [0x0E] = C_CN, [0x0F] = C_CN, [0x10] = C_CN, [0x11] = C_CN,
    [0x12] = C_CN, [0x13] = C_CN, [0x14] = C_CN, [0x15] = C_CN,
    [0x16] = C_CN, [0x17] = C_CN, [0x18] = C_CN, [0x19] = C_CN,
    [0x1A] = C_CN, [0x1B] = C_CN, [0x1C] = C_CN, [0x1D] = C_CN,
    [0x1E] = C_CN, [0x1F] = C_CN,
    /* 0x20 ' ' is print+space+blank; 0x21-0x7E are graph (+print) */
    [0x20] = C_PR | C_SP | C_BL,
    [0x21] = C_PU | C_GR | C_PR, [0x22] = C_PU | C_GR | C_PR,
    [0x23] = C_PU | C_GR | C_PR, [0x24] = C_PU | C_GR | C_PR,
    [0x25] = C_PU | C_GR | C_PR, [0x26] = C_PU | C_GR | C_PR,
    [0x27] = C_PU | C_GR | C_PR, [0x28] = C_PU | C_GR | C_PR,
    [0x29] = C_PU | C_GR | C_PR, [0x2A] = C_PU | C_GR | C_PR,
    [0x2B] = C_PU | C_GR | C_PR, [0x2C] = C_PU | C_GR | C_PR,
    [0x2D] = C_PU | C_GR | C_PR, [0x2E] = C_PU | C_GR | C_PR,
    [0x2F] = C_PU | C_GR | C_PR,
    /* 0x30-0x39 digits */
    [0x30] = C_DI | C_XD | C_GR | C_PR, [0x31] = C_DI | C_XD | C_GR | C_PR,
    [0x32] = C_DI | C_XD | C_GR | C_PR, [0x33] = C_DI | C_XD | C_GR | C_PR,
    [0x34] = C_DI | C_XD | C_GR | C_PR, [0x35] = C_DI | C_XD | C_GR | C_PR,
    [0x36] = C_DI | C_XD | C_GR | C_PR, [0x37] = C_DI | C_XD | C_GR | C_PR,
    [0x38] = C_DI | C_XD | C_GR | C_PR, [0x39] = C_DI | C_XD | C_GR | C_PR,
    /* 0x3A-0x40 punct */
    [0x3A] = C_PU | C_GR | C_PR, [0x3B] = C_PU | C_GR | C_PR,
    [0x3C] = C_PU | C_GR | C_PR, [0x3D] = C_PU | C_GR | C_PR,
    [0x3E] = C_PU | C_GR | C_PR, [0x3F] = C_PU | C_GR | C_PR,
    [0x40] = C_PU | C_GR | C_PR,
    /* 0x41-0x46 upper + hex */
    [0x41] = C_UP | C_XD | C_GR | C_PR, [0x42] = C_UP | C_XD | C_GR | C_PR,
    [0x43] = C_UP | C_XD | C_GR | C_PR, [0x44] = C_UP | C_XD | C_GR | C_PR,
    [0x45] = C_UP | C_XD | C_GR | C_PR, [0x46] = C_UP | C_XD | C_GR | C_PR,
    /* 0x47-0x5A upper */
    [0x47] = C_UP | C_GR | C_PR, [0x48] = C_UP | C_GR | C_PR,
    [0x49] = C_UP | C_GR | C_PR, [0x4A] = C_UP | C_GR | C_PR,
    [0x4B] = C_UP | C_GR | C_PR, [0x4C] = C_UP | C_GR | C_PR,
    [0x4D] = C_UP | C_GR | C_PR, [0x4E] = C_UP | C_GR | C_PR,
    [0x4F] = C_UP | C_GR | C_PR, [0x50] = C_UP | C_GR | C_PR,
    [0x51] = C_UP | C_GR | C_PR, [0x52] = C_UP | C_GR | C_PR,
    [0x53] = C_UP | C_GR | C_PR, [0x54] = C_UP | C_GR | C_PR,
    [0x55] = C_UP | C_GR | C_PR, [0x56] = C_UP | C_GR | C_PR,
    [0x57] = C_UP | C_GR | C_PR, [0x58] = C_UP | C_GR | C_PR,
    [0x59] = C_UP | C_GR | C_PR, [0x5A] = C_UP | C_GR | C_PR,
    /* 0x5B-0x60 punct */
    [0x5B] = C_PU | C_GR | C_PR, [0x5C] = C_PU | C_GR | C_PR,
    [0x5D] = C_PU | C_GR | C_PR, [0x5E] = C_PU | C_GR | C_PR,
    [0x5F] = C_PU | C_GR | C_PR, [0x60] = C_PU | C_GR | C_PR,
    /* 0x61-0x66 lower + hex */
    [0x61] = C_LO | C_XD | C_GR | C_PR, [0x62] = C_LO | C_XD | C_GR | C_PR,
    [0x63] = C_LO | C_XD | C_GR | C_PR, [0x64] = C_LO | C_XD | C_GR | C_PR,
    [0x65] = C_LO | C_XD | C_GR | C_PR, [0x66] = C_LO | C_XD | C_GR | C_PR,
    /* 0x67-0x7A lower */
    [0x67] = C_LO | C_GR | C_PR, [0x68] = C_LO | C_GR | C_PR,
    [0x69] = C_LO | C_GR | C_PR, [0x6A] = C_LO | C_GR | C_PR,
    [0x6B] = C_LO | C_GR | C_PR, [0x6C] = C_LO | C_GR | C_PR,
    [0x6D] = C_LO | C_GR | C_PR, [0x6E] = C_LO | C_GR | C_PR,
    [0x6F] = C_LO | C_GR | C_PR, [0x70] = C_LO | C_GR | C_PR,
    [0x71] = C_LO | C_GR | C_PR, [0x72] = C_LO | C_GR | C_PR,
    [0x73] = C_LO | C_GR | C_PR, [0x74] = C_LO | C_GR | C_PR,
    [0x75] = C_LO | C_GR | C_PR, [0x76] = C_LO | C_GR | C_PR,
    [0x77] = C_LO | C_GR | C_PR, [0x78] = C_LO | C_GR | C_PR,
    [0x79] = C_LO | C_GR | C_PR, [0x7A] = C_LO | C_GR | C_PR,
    /* 0x7B-0x7E punct */
    [0x7B] = C_PU | C_GR | C_PR, [0x7C] = C_PU | C_GR | C_PR,
    [0x7D] = C_PU | C_GR | C_PR, [0x7E] = C_PU | C_GR | C_PR,
    /* 0x7F DEL is a control */
    [0x7F] = C_CN,
    /* 0x80-0xFF: no classes in the C locale (matches glibc __ctype_b) */
};

static const unsigned short hb_class_mask[12] = {
    /* isalnum */   (C_UP | C_LO | C_DI),
    /* isalpha */   (C_UP | C_LO),
    /* isblank */   (C_BL),
    /* iscntrl */   (C_CN),
    /* isdigit */   (C_DI),
    /* isgraph */   (C_GR),
    /* islower */   (C_LO),
    /* isprint */   (C_PR),
    /* ispunct */   (C_PU),
    /* isspace */   (C_SP),
    /* isupper */   (C_UP),
    /* isxdigit */  (C_XD),
};

/* glibc semantics: c in [-128, 255] is looked up as (unsigned char)c (its
 * __ctype_b table is indexed by that 8-bit value for the whole range, so
 * e.g. iscntrl(EOF=-1) == iscntrl(0xFF)). Values outside are not defined;
 * return 0. */
static int hb_ctype_test(int c, unsigned short mask)
{
    if (c < -128 || c > 255) return 0;
    return (hb_ctype[(unsigned char)c] & mask) != 0;
}

int isalnum(int c)  { return hb_ctype_test(c, hb_class_mask[0]); }
int isalpha(int c)  { return hb_ctype_test(c, hb_class_mask[1]); }
int isblank(int c)  { return hb_ctype_test(c, hb_class_mask[2]); }
int iscntrl(int c)  { return hb_ctype_test(c, hb_class_mask[3]); }
int isdigit(int c)  { return hb_ctype_test(c, hb_class_mask[4]); }
int isgraph(int c)  { return hb_ctype_test(c, hb_class_mask[5]); }
int islower(int c)  { return hb_ctype_test(c, hb_class_mask[6]); }
int isprint(int c)  { return hb_ctype_test(c, hb_class_mask[7]); }
int ispunct(int c)  { return hb_ctype_test(c, hb_class_mask[8]); }
int isspace(int c)  { return hb_ctype_test(c, hb_class_mask[9]); }
int isupper(int c)  { return hb_ctype_test(c, hb_class_mask[10]); }
int isxdigit(int c) { return hb_ctype_test(c, hb_class_mask[11]); }

int tolower(int c)
{
    unsigned char u;
    if (c == -1) return c; /* EOF, glibc passes it through */
    if (c < -128 || c > 255) return c;
    u = (unsigned char)c;
    if (u >= 'A' && u <= 'Z') return (int)(u + ('a' - 'A'));
    return (int)u; /* glibc returns the unsigned-char value for the range */
}

int toupper(int c)
{
    unsigned char u;
    if (c == -1) return c; /* EOF, glibc passes it through */
    if (c < -128 || c > 255) return c;
    u = (unsigned char)c;
    if (u >= 'a' && u <= 'z') return (int)(u - ('a' - 'A'));
    return (int)u;
}
