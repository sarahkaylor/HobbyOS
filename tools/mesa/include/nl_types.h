/* nl_types.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * libc++'s <locale> includes <nl_types.h> on Linux for the message-catalog
 * surface (nl_catd / catopen / catgets / catclose).  Mesa never opens a
 * catalog - the header is pulled in by <locale> - so the type is opaque and
 * the three entry points fail closed in gl_stubs.c.  glibc's values. */
#ifndef HOBBYOS_NL_TYPES_H
#define HOBBYOS_NL_TYPES_H 1

#ifdef __cplusplus
extern "C" {
#endif

typedef void *nl_catd;
typedef int nl_item;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *name, int flag);
char *catgets(nl_catd catalog, int set_number, int message_number,
              const char *message);
int catclose(nl_catd catalog);

#ifdef __cplusplus
}
#endif

#endif
