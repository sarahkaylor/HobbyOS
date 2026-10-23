#!/bin/sh
# cc-x86-none-elf.sh — cross C/C++ driver for x86_64-none-elf on macOS.
#
# WHY THIS EXISTS:
#   Apple's clang cannot LINK this GNU/ELF target on a Darwin host.  The
#   Generic-GCC frontend that owns x86_64-*-none-elf* hands final links to
#   /usr/bin/gcc (Apple's shim), which then drives ld64.lld (Mach-O) and
#   rejects ELF flags:  'ld64.lld: unknown argument '-T'' / '-no-pie'.
#   ARM never hits this (no Mach-O lld flavor for aarch64, so clang falls
#   back to ELF ld.lld directly) — which is why the aarch64 path "just works"
#   and x86_64 doesn't.
#
#   This driver mirrors the HobbyOS Makefile's own intel recipe: compile each
#   source with clang, then hand the final link straight to /opt/homebrew/bin/
#   ld.lld with the OS linker script (-T src/user/linker.ld -e _start).  It is
#   a drop-in "cc" for packages whose configure/ninja issue a single compiler
#   command that both compiles and links (ICU's configure conftests, ...).
#
# USAGE:
#   export CC="$(pwd)/tools/cc-x86-none-elf.sh" [CC_CLANG=...] [CC_LINKER=...]
#   bash third_party/icu-78.3/build-target.sh --arch intel
#
set -e

CLANG="${CC_CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
CLANG_HOST="${CC_HOST_CLANG:-/usr/bin/clang}"
LINKER="${CC_LINKER:-/opt/homebrew/bin/ld.lld}"
if [ -n "${CC_ROOT:-}" ]; then ROOT="$CC_ROOT"
else
  ROOT="$(cd "$(dirname "$0")/.." && pwd)"
  [ -f "$ROOT/src/user/linker.ld" ] || ROOT="$HOME/Documents/GitHub/HobbyOS"
fi
LINKER_SCRIPT="${CC_LINKER_SCRIPT:-$ROOT/src/user/linker.ld}"
[ -f "$LINKER_SCRIPT" ] || { echo "cc-x86-none-elf: no linker script at $LINKER_SCRIPT (set CC_ROOT)" >&2; exit 1; }

# --- passthrough guard ------------------------------------------------------
# When this script is installed as PATH 'clang' (symlink) to serve one build
# tree, HOST invocations (no --target) and OTHER-target compiles must fall
# through to the real clang untouched — only x86_64-*-none-elf gets the
# custom link path.
IS_X86=0
PEND=0
for a in "$@"; do
  case "$a" in
    --target=x86_64-*none-elf) IS_X86=1 ;;
    --target=*) : ;;
    --target|-target) PEND=1 ;;
  esac
  [ "$PEND" = 1 ] && { case "$a" in x86_64-*none-elf) IS_X86=1;; esac; PEND=0; }
done
if [ "$IS_X86" = 0 ]; then
  exec "$CLANG" "$@"
fi

# --- first pass: detect compile-only mode (-c/-S/-E) -----------------------
COMPILE=0
for a in "$@"; do [ "$a" = "--" ] && break; case "$a" in -c|-S|-E) COMPILE=1;; esac; done

# --- parse: split into clang args (CARGS), link args (LINKARGS), sources ----
OUT="a.out"
SRCS=""
LINKARGS=""
CARGS=""
want_next=""          # name of the option whose VALUE is the NEXT token
for a in "$@"; do
  [ "$a" = "--" ] && continue
  if [ -n "$want_next" ]; then
    case "$want_next" in
      out) OUT="$a"; CARGS="$CARGS -o $a" ;;
      T)   LINKARGS="$LINKARGS -T $a" ;;
      e)   LINKARGS="$LINKARGS -e $a" ;;
      dep) CARGS="$CARGS $a" ;;   # value of -MT/-MQ/-MF (depfile path)
    esac
    want_next=""; continue
  fi
  case "$a" in
    -fPIE|-fpie|-fPIC|-fpic|-fvisibility=hidden|-fvisibility-inlines-hidden)
      : ;;  # HobbyOS userland is non-PIE / fixed-address; -fPIE also makes
           # clang emit GOTOFF relocs whose SHF_X86_64_LARGE *.lrodata gets
           # dropped by ld.lld under the OS linker script (see WebKit)
    -c|-S|-E) CARGS="$CARGS $a" ;;
    -MT|-MQ|-MF) CARGS="$CARGS $a"; want_next=dep ;;
    -o) want_next=out ;;
    -T) want_next=T ;;
    -e) want_next=e ;;
    -Wl,*) LINKARGS="$LINKARGS $(printf '%s' "${a#-Wl,}" | tr ',' ' ')" ;;
    -) if [ "$COMPILE" = 1 ]; then CARGS="$CARGS -"; else SRCS="$SRCS -"; fi ;;
    *.c|*.cc|*.cpp|*.cxx|*.s|*.S)
      if [ "$COMPILE" = 1 ]; then CARGS="$CARGS $a"; else SRCS="$SRCS $a"; fi ;;
    *.a|*.o|*.obj|-l*|-L*) LINKARGS="$LINKARGS $a" ;;
    *) CARGS="$CARGS $a" ;;
  esac
done

if [ "$COMPILE" = 1 ]; then
  exec $CLANG $CARGS
fi

# Only append the default linker script when the caller didn't pass their own
# -T (a script given both as default and -T overrides the layout: .text base
# collides with .tls_meta -> "section .text load address range overlaps").
case " $LINKARGS " in
  *" -T "*|*-T*) DFLT_T="" ;;
  *) DFLT_T="-T \"$LINKER_SCRIPT\"" ;;
esac

if [ -z "$SRCS" ]; then
  # no sources -> pure link of the given objects/archives
  eval exec $LINKER $DFLT_T -e _start -o "$OUT" $LINKARGS
fi

if [ "$SRCS" = "-" ]; then
  # source on stdin (configure conftests: echo 'int main(){}' | cc -x c - -o x)
  TMPDIR_O="${TMPDIR:-/tmp}/ccx86.$$"
  mkdir -p "$TMPDIR_O"
  trap 'rm -rf "$TMPDIR_O"' EXIT
  $CLANG $CARGS -c - -o "$TMPDIR_O/stdin.o"
  eval exec $LINKER $DFLT_T -e _start -o "$OUT" "$TMPDIR_O/stdin.o" $LINKARGS
fi

TMPDIR_O="${TMPDIR:-/tmp}/ccx86.$$"
mkdir -p "$TMPDIR_O"
trap 'rm -rf "$TMPDIR_O"' EXIT

OBJS=""
for s in $SRCS; do
  o="$TMPDIR_O/$(basename "$s").o"
  # shellcheck disable=SC2086
  $CLANG $CARGS -c "$s" -o "$o"
  OBJS="$OBJS $o"
done

eval exec $LINKER $DFLT_T -e _start -o "$OUT" $OBJS $LINKARGS
