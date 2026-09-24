/* HobbyOS port: gnulib's real libc-config.h supports building gnulib code
 * *inside* glibc; that does not apply here.  Beyond the include succeeding
 * (with _LIBC left undefined, selecting the public-API code paths), the
 * glibc-internal trailing macros used by vendored files must compile away:
 * strverscmp.c renames its definition to the public name itself. */
#ifndef _GL_LIBC_CONFIG_H
#define _GL_LIBC_CONFIG_H 1
#define libc_hidden_def(name) /* empty on the outside */
#define weak_alias(name, aliasname) /* already the public name */
#endif
