#!/bin/bash
# build_diffutils_cmp_ref.sh — build GNU diffutils-2.8.1's ORIGINAL cmp as a
# strict byte-exact reference for the HobbyOS cmp port
# (src/host/cmp_parity.sh).
#
# Same approach as build_tu21_tac_ref.sh: the vendored diffutils-2.8.1
# sources (GPL v2, the lineage the port was transcribed from) are compiled
# against a minimal hand-rolled config.h.  cmp's lib surface: error.c +
# exitfail.c (diagnostics), xstrtol.c + xstrtoumax.c (suffix parsing),
# cmpbuf.c (block_read), offtostr.c (the inttostr template instantiated
# for off_t), getopt.c + getopt1.c (getopt_long), freesoft.c (the
# no-warranty banner).  set_binary_mode/c_stack/xmalloc are stubs (see
# build_tu21_tac_ref.sh for the same trick).  Output: $OUT
# (default /tmp/gnudiffutils-cmp-ref/diffutils_cmp).
#
# Usage: bash src/host/build_diffutils_cmp_ref.sh [output-path]
set -e
BASE="$(cd "$(dirname "$0")/../../third_party/_staging/diffutils-2.8.1" && pwd)"
OUT="${1:-/tmp/gnudiffutils-cmp-ref/diffutils_cmp}"
WD="$(mktemp -d /tmp/diffutilscmpref.XXXXXX)"
trap 'rm -rf "$WD"' EXIT

cat > "$WD/config.h" <<'EOF'
/* minimal config for host-compiling diffutils-2.8.1 src/cmp.c against
   modern glibc (hand-rolled; avoids running 2002-era autoconf) */
#define HAVE_CONFIG_H 1
#define PACKAGE "diffutils"
#define PACKAGE_BUGREPORT "bug-gnu-utils@gnu.org"
#define PACKAGE_VERSION "2.8.1"
#define VERSION "2.8.1"
#define STDC_HEADERS 1
#define HAVE_UNISTD_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_WCHAR_H 0
#define ENABLE_NLS 0
#define gettext(x) (x)
#define _(x) (x)
#define N_(x) (x)
#define PARAMS(protos) protos
#define HAVE_DECL_STRTOUMAX 1
#define HAVE_DECL_STRTOIMAX 1
#define HAVE_STRTOUMAX 1
#define HAVE_STRTOIMAX 1
#define HAVE_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_DECL_MEMCHR 1
#define HAVE_DECL_MEMCMP 1
#define HAVE_DECL_STRCHR 1
#define HAVE_DECL_STRRCHR 1
#define HAVE_DECL_STRSTR 1
#define HAVE_DECL_STRCOLL 0
#define HAVE_DECL_STRERROR_R 1
#define HAVE_DECL_FREE 1
#define HAVE_DECL_MALLOC 1
#define HAVE_DECL_REALLOC 1
#define HAVE_DECL_STRDUP 1
#define HAVE_DECL_STRTOUL 1
#define HAVE_DECL_STRTOULL 1
#define HAVE_DECL_GETENV 1
#define HAVE_DECL___FPENDING 1
#define HAVE_DECL_ISASCII 1
#define HAVE_DECL_ISBLANK 1
#define HAVE_MBRTOWC 0
#define HAVE_SIGACTION 1
#define HAVE_SIGNAL_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_STRFTIME 1
#define HAVE_NL_LANGINFO 0
#define LOCALEDIR "/tmp/share/locale"
EOF

cat > "$WD/stubs.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
/* setmode.c is the Windows binary-mode layer; POSIX needs nothing. */
void set_binary_mode (int fd, int mode) { (void) fd; (void) mode; }
void setmode (int fd, int mode) { set_binary_mode (fd, mode); }
/* c-stack.c is Linux-specific stack-overflow introspection; disable. */
int c_stack_action (void (*handler) (int)) { (void) handler; return 0; }
void c_stack_die (int sig) { (void) sig; _exit (2); }
/* lib/xmalloc.c's autoconf probes are not worth running; provide the
   entry points cmp.c calls, with 2.8.1-faithful OOM behavior. */
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
CFLAGS="$CFLAGS -include stdlib.h"

FILES=(
    "$BASE/src/cmp.c"
    "$BASE/src/version.c"
    "$BASE/lib/error.c"
    "$BASE/lib/exitfail.c"
    "$BASE/lib/xstrtol.c"
    "$BASE/lib/xstrtoumax.c"
    "$BASE/lib/cmpbuf.c"
    "$BASE/lib/offtostr.c"
    "$BASE/lib/getopt.c"
    "$BASE/lib/getopt1.c"
    "$BASE/lib/freesoft.c"
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
