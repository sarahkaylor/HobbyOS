/* HobbyOS sysroot: <langinfo.h> — nl_langinfo() for the C locale.
 *
 * HobbyOS has exactly one locale ("C", ASCII): CODESET names its codeset
 * ("ANSI_X3.4-1968", byte-exact with glibc's C locale); items with no
 * data yet yield "". The item values match glibc's so host comparisons
 * stay honest.
 */
#ifndef HOBBYOS_LANGINFO_H
#define HOBBYOS_LANGINFO_H 1

#define CODESET 14 /* glibc's nl_item value for CODESET */

#ifdef __cplusplus
extern "C" {
#endif

  char *nl_langinfo(int item);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_LANGINFO_H */
