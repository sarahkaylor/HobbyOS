#!/bin/bash
# build_tu21_head_ref.sh — build GNU textutils-2.1's ORIGINAL head as a
# strict byte-exact reference for the HobbyOS head port
# (src/host/head_parity.sh).
#
# Same approach as build_tu21_wc_ref.sh: vendored textutils-2.1 sources
# (GPL v2, the lineage the port was transcribed from) + a minimal
# config.h + the gnulib .c files head needs.  Output: $OUT (default
# /tmp/gnuhead-ref/tu21_head).
#
# Usage: bash src/host/build_tu21_head_ref.sh [output-path]
set -e
BASE="$(cd "$(dirname "$0")/../../third_party/textutils-2.1" && pwd)"
OUT="${1:-/tmp/gnuhead-ref/tu21_head}"
WD="$(mktemp -d /tmp/tu21href.XXXXXX)"
trap 'rm -rf "$WD"' EXIT

cat > "$WD/config.h" <<'EOF'
/* minimal config for host-compiling textutils-2.1 src/head.c against
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
#define HAVE_DECL_STPCPY 0
#define HAVE_DECL_STRDUP 1
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
#define HAVE_DECL_REALLOC 1
#define HAVE_DECL_MEMCMP 1
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
/* never truly invoked by the parity corpus's head invocations, but must link */
void version_etc(FILE *stream, const char *command_name, const char *package,
                 const char *version, const char *authors0, ...)
{ fprintf (stream, "%s (%s) %s\n", command_name, package, version); (void)authors0; }
char *quotearg_colon(const char *s) { return (char *)s; }
const char *argmatch(const char *arg, const char *const *arglist,
                     const char *vallist, size_t valsize) { (void)arg; (void)arglist; (void)vallist; (void)valsize; return NULL; }
size_t __fpending (FILE *fp) { (void)fp; return 0; }
EOF

CFLAGS="-O1 -w -DHAVE_CONFIG_H"
for i in "$WD" "$BASE/lib" "$BASE/src"; do
    CFLAGS="$CFLAGS -I$i"
done
CFLAGS="$CFLAGS -DVA_START(args,lastarg)=va_start(args,lastarg)"
CFLAGS="$CFLAGS -DVA_END(args)=va_end(args) -DHAVE_VPRINTF=1"
CFLAGS="$CFLAGS -include string.h -include stdlib.h"

FILES=(
    "$BASE/src/head.c"
    "$BASE/lib/getopt.c"
    "$BASE/lib/getopt1.c"
    "$BASE/lib/error.c"
    "$BASE/lib/xstrtol.c"
    "$BASE/lib/xmalloc.c"
    "$BASE/lib/safe-read.c"
    "$BASE/lib/posixver.c"
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
# xstrtoumax.c was generated by old gnulib's autoconf (sed-templated from
# xstrtol.c); reproduce it: same TU, alternate macro bindings.  glibc's
# inttypes.h typedefs clash with config.h's old-style uintmax_t, so we
# declare strtoumax ourselves instead of including inttypes.h.
cat > "$WD/strmax.h" <<'EOF'
unsigned long long strtoumax (const char *nptr, char **endptr, int base);
unsigned long long strtoimax (const char *nptr, char **endptr, int base);
EOF
clang $CFLAGS -include "$WD/strmax.h" \
    -D__strtol=strtoumax \
    '-D__strtol_t=unsigned long long' \
    -D__xstrtol=xstrtoumax \
    -c "$BASE/lib/xstrtol.c" -o "$WD/ref_xsm.o"
OBJS+=("$WD/ref_xsm.o")
clang -o "$OUT" "${OBJS[@]}"
echo "built $OUT"
"$OUT" --version 2>/dev/null | head -1 || true
