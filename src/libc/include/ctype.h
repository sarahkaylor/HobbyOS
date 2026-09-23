#ifndef HOBBYOS_CTYPE_H
#define HOBBYOS_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

/* HobbyOS Phase-1 libc: ctype.h — full classification and conversion set,
 * C locale, driven by a char-class table in src/libc/src/ctype.c. Inputs
 * are interpreted as in glibc: c in [-128, 255] is looked up as
 * (unsigned char)c (EOF=-1 passes through in tolower/toupper); anything
 * else is undefined (returns 0). */


int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_CTYPE_H */
