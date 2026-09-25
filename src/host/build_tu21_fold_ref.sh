#!/bin/bash
# build_tu21_fold_ref.sh — build GNU textutils-2.1's ORIGINAL fold as a strict
# byte-exact reference for the HobbyOS fold port (src/host/fold_parity.sh).
#
# The textutils-2.1 sources are vendored at third_party/textutils-2.1
# (GPL v2, the same lineage our port was transcribed from).  It predates
# modern glibc, so this script hands it a minimal config.h and compiles the
# small set of .c files fold needs.  Output: $OUT (default
# /tmp/gnufold-ref/tu21_fold).
#
# Files (vs. build_tu21_cut_ref.sh, fold's extra gnulib surface):
#   src/fold.c        the program
#   lib/xstrtol.c     xstrtol(), the -w width parser
#   lib/posixver.c    posix2_version(), the obsolete `-N' syntax gate
#   lib/{getopt,getopt1,error,closeout}.c, plus stubs.c
#
# Note on posix2_version(): 2.1's lib/posixver.c takes its default from the
# compile-time _POSIX2_VERSION of <unistd.h>.  Modern glibc says 200809L, so
# this reference would hard-error on `fold -10' ("`-10' option is obsolete;
# use `-w10'").  The port's sysroot publishes no POSIX version (default 0 ->
# accept silently), which is what this release did on the systems it shipped
# for, so fold_parity.sh runs BOTH binaries with _POSIX2_VERSION=199901
# (< 200112) set, exactly as src/host/head_parity.sh does for head.
#
# Usage: bash src/host/build_tu21_fold_ref.sh [output-path]
set -e
BASE="$(cd "$(dirname "$0")/../../third_party/textutils-2.1" && pwd)"
OUT="${1:-/tmp/gnufold-ref/tu21_fold}"
WD="$(mktemp -d /tmp/tu21fref.XXXXXX)"
trap 'rm -rf "$WD"' EXIT

cat > "$WD/config.h" <<'EOF'
/* minimal config for host-compiling textutils-2.1 src/fold.c against
   modern glibc (hand-rolled; avoids running 1999-era autoconf) */
#define HAVE_CONFIG_H 1
#define PACKAGE "textutils"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"
#define PACKAGE_VERSION "2.1"
#define VERSION "2.1"
#define HAVE_UNISTD_H 1
#define HAVE_STDLIB_H 1
#define HAVE_MALLOC 1
#define HAVE_REALLOC 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_INTTYPES_H 0
#define HAVE_WCHAR_H 0
#define HAVE_MBRTOWC 0
#define gettext(x) (x)
#define ngettext(a,b,c) ((c)==1?(a):(b))
#define _(x) (x)
#define N_(x) (x)
#define STREQ(a,b) (strcmp((a),(b))==0)
#define HAVE_DECL_GETENV 1
#define HAVE_DECL_FREE 1
#define HAVE_DECL_MALLOC 1
#define HAVE_DECL_REALLOC 1
#define HAVE_DECL_STRDUP 1
#define HAVE_DECL_STRTOL 1
#define HAVE_DECL_STRTOUL 1
#define HAVE_DECL_STRTOULL 1
#define HAVE_DECL_STRTOUMAX 1
#define HAVE_DECL_STRTOIMAX 1
#define HAVE_STRTOUMAX 1
#define HAVE_STRTOIMAX 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_LONG_LONG 1
#define HAVE_STRNCASECMP 1
#define HAVE_STRCASECMP 1
#define HAVE_SIGACTION 1
#define HAVE_SIGNAL_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
#define ENABLE_NLS 0
#define LOCALEDIR "/tmp/share/locale"
#define HAVE_DECL_MEMCHR 1
#define HAVE_DECL_MEMCMP 1
#define HAVE_DECL_MEMMOVE 1
#define HAVE_DECL_STRCHR 1
#define HAVE_DECL_STRRCHR 1
#define HAVE_DECL_STRSTR 1
#define HAVE_DECL_STRCMP 1
#define HAVE_DECL_STRNCMP 1
#define HAVE_DECL_STRCPY 1
#define HAVE_DECL_STRNCPY 1
#define HAVE_DECL_STRCAT 1
#define HAVE_DECL_STRNLEN 0
#define HAVE_DECL_WCWIDTH 0
#define HAVE_DECL_STRERROR_R 1
#define HAVE_DECL___FPENDING 1
typedef unsigned long long uintmax_t;
typedef long long intmax_t;
#include <stdio.h>
extern size_t __fpending (FILE *fp);
EOF

cat > "$WD/unlocked-io.h" <<'EOF'
/* stubs: modern glibc stdio needs no unlocked variants here */
#define clearerr_unlocked clearerr
#define feof_unlocked feof
#define ferror_unlocked ferror
#define fflush_unlocked fflush
#define fgetc_unlocked fgetc
#define fgets_unlocked fgets
#define fputc_unlocked fputc
#define fputs_unlocked fputs
#define fread_unlocked fread
#define fwrite_unlocked fwrite
#define getc_unlocked getc
#define getchar_unlocked getchar
#define getline_unlocked getline
#define putc_unlocked putc
#define putchar_unlocked putchar
#define puts_unlocked puts
#define fseek_unlocked fseek
#define ftell_unlocked ftell
EOF

# sys_errlist/sys_nerr + other vanished glibc bits (see build_tu21_cut_ref.sh)
cat > "$WD/stubs.c" <<'EOF'
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
int sys_nerr = 134;
char *sys_errlist[134] = {
  "Success", "Operation not permitted", "No such file or directory",
  "No such process", "Interrupted system call", "Input/output error",
  "No such device or address", "Argument list too long", "Exec format error",
  "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
  "Cannot allocate memory", "Permission denied", "Bad address",
  "Block device required", "Device or resource busy", "File exists",
  "Invalid cross-device link", "No such device", "Not a directory",
  "Is a directory", "Invalid argument", "Too many open files in system",
  "Too many open files", "Inappropriate ioctl for device", "Text file busy",
  "File too large", "No space left on device", "Illegal seek",
  "Read-only file system", "Too many links", "Broken pipe", "Numerical argument out of domain",
  "Numerical result out of range", "Resource deadlock avoided", "File name too long",
  "No locks available", "Function not implemented", "Directory not empty",
  "Too many levels of symbolic links", "Unknown error 41", "No message of desired type",
  "Identifier removed", "Channel number out of range", "Level 2 not synchronized",
  "Level 3 halted", "Level 3 reset", "Link number out of range",
  "Protocol driver not attached", "No CSI structure available", "Level 2 halted",
  "Invalid exchange", "Invalid request descriptor", "Exchange full",
  "No anode", "Invalid request code", "Invalid slot", "Unknown error 58",
  "Bad font file format", "Device not a stream", "No data available",
  "Timer expired", "Out of streams resources", "Machine is not on the network",
  "Package not installed", "Object is remote", "Link has been severed",
  "Advertise error", "Srmount error", "Communication error on send",
  "Protocol error", "Multihop attempted", "RFS specific error",
  "Bad message", "File descriptor in bad state", "Not a XENIX named type file",
  "No such device or address", "Value too large for defined data type",
  "Name not unique on network", "File descriptor in bad state",
  "Remote address changed", "Can not access a needed shared library",
  "Accessing a corrupted shared library", ".lib section in a.out corrupted",
  "Attempting to link in too many shared libraries",
  "Cannot exec a shared library directly", "Invalid or incomplete multibyte or wide character",
  "Interrupted system call should be restarted", "Streams pipe error",
  "Too many users", "Socket operation on non-socket", "Destination address required",
  "Message too long", "Protocol wrong type for socket", "Protocol not available",
  "Protocol not supported", "Socket type not supported", "Operation not supported",
  "Protocol family not supported", "Address family not supported by protocol",
  "Address already in use", "Cannot assign requested address",
  "Network is down", "Network is unreachable", "Network dropped connection on reset",
  "Software caused connection abort", "Connection reset by peer", "No buffer space available",
  "Transport endpoint is already connected", "Transport endpoint is not connected",
  "Cannot send after transport endpoint shutdown", "Too many references: cannot splice",
  "Connection timed out", "Connection refused", "Host is down", "No route to host",
  "Operation already in progress", "Operation now in progress", "Stale file handle",
  "Structure needs cleaning", "Not a XENIX named type file", "Is a named type file",
  "Remote I/O error", "Disk quota exceeded", "No medium found", "Wrong medium type",
  "Operation canceled", "Required key not available", "Key has expired",
  "Key has been revoked", "Key was rejected by service", "Owner died",
  "State not recoverable", "Operation not possible due to RF-kill", "Memory page has hardware error",
};
/* fold (2.1) prints its --version through version_etc; the port prints the
   same line from case_GETOPT_VERSION_CHAR, so keep the exact 2.1 wording. */
void version_etc(FILE *stream, const char *command_name, const char *package,
                 const char *version, const char *authors0, ...)
{ fprintf (stream, "%s (%s) %s\n", command_name, package, version); (void)authors0; }
char *quotearg_colon(const char *s) { return (char *)s; }
const char *argmatch(const char *arg, const char *const *arglist,
                     const char *vallist, size_t valsize) { (void)arg; (void)arglist; (void)vallist; (void)valsize; return NULL; }
size_t __fpending (FILE *fp) { (void)fp; return 0; }
/* lib/xmalloc.c needs 1999-era autoconf malloc probes; provide the four
   entry points fold.c actually calls, with 2.1-faithful OOM behavior. */
#include <stdlib.h>
#include <string.h>
void xalloc_die (void) { fputs ("memory exhausted\n", stderr); exit (1); }
void *xmalloc (size_t n) { void *p = malloc (n); if (!p) xalloc_die (); return p; }
void *xrealloc (void *p, size_t n) { void *q = realloc (p, n); if (!q) xalloc_die (); return q; }
char *xstrdup (const char *s) { size_t n = strlen (s) + 1; char *p = xmalloc (n); memcpy (p, s, n); return p; }
EOF

CFLAGS="-O1 -w -DHAVE_CONFIG_H"
for i in "$WD" "$BASE/lib" "$BASE/src"; do
    CFLAGS="$CFLAGS -I$i"
done
CFLAGS="$CFLAGS -DVA_START(args,lastarg)=va_start(args,lastarg)"
CFLAGS="$CFLAGS -DVA_END(args)=va_end(args) -DHAVE_VPRINTF=1"
CFLAGS="$CFLAGS -include string.h -include stdlib.h"

FILES=(
    "$BASE/src/fold.c"
    "$BASE/lib/xstrtol.c"
    "$BASE/lib/posixver.c"
    "$BASE/lib/getopt.c"
    "$BASE/lib/getopt1.c"
    "$BASE/lib/error.c"
    "$BASE/lib/closeout.c"
    "$WD/stubs.c"
)

mkdir -p "$(dirname "$OUT")"
OBJS=()
i=0
for f in "${FILES[@]}"; do
    o="$WD/ref_$i.o"
    clang $CFLAGS -c "$f" -o "$o"
    OBJS+=("$o")
    i=$((i+1))
done
clang -o "$OUT" "${OBJS[@]}"
echo "built $OUT"
"$OUT" --version 2>/dev/null | head -1 || true
