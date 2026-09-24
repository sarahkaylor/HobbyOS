/* HobbyOS sysroot: <locale.h> — categories; only the C locale exists.
 *
 * Category values match glibc's so host comparisons stay honest. The
 * setlocale() definition lands with the sed port (its only caller so
 * far); until then this is a declaration only.
 */
#ifndef HOBBYOS_LOCALE_H
#define HOBBYOS_LOCALE_H 1

#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL 6

char *setlocale(int category, const char *locale);

#endif /* HOBBYOS_LOCALE_H */
