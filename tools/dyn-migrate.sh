#!/usr/bin/env bash
# dyn-migrate.sh — convert a HobbyOS user program to a dynamically linked one.
#
#   tools/dyn-migrate.sh <arch> <base> [<base> ...]
#   tools/dyn-migrate.sh <arch> --list <file>     # one base name per line
#   tools/dyn-migrate.sh <arch> --all-simple      # every "one TU + libc" rule
#
# <base> is the program's object base name (obj/<arch>/<base>.o), which is
# src/user/<base>.c for every program in the simple set.
#
# For each base it builds obj/<arch>/dyn/<base>.o with -fPIC and links
# obj/<arch>/DYN/<base>.BIN as ET_DYN + PT_INTERP /LDHOBBY.SO against
# obj/<arch>/libc.so.  Every success is appended to obj/<arch>/dyn_list.h,
# which KERNEL_MODE_DYN_TEST boots (one process per program).
#
# Exit 0 only if every requested program converted; failures are named with
# their compiler/linker diagnostic (never silently skipped).
set -u
ARCH=${1:?usage: dyn-migrate.sh <arch> <base>... | --list FILE | --all-simple}
shift

case "$ARCH" in
intel) TARGET=--target=x86_64-none-elf; EXTRA="-mno-red-zone -mcmodel=large" ;;
arm)   TARGET=--target=aarch64-none-elf; EXTRA="-mcpu=cortex-a53" ;;
*)     echo "dyn-migrate: unknown arch '$ARCH' (intel|arm)" >&2; exit 2 ;;
esac
RINC=$(clang -print-resource-dir)/include
CFLAGS=("$TARGET" -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
        -Isrc/include -Isrc/libc/include -ffreestanding -nostdinc -isystem "$RINC"
        $EXTRA -fPIC)
OUT=obj/$ARCH
mkdir -p "$OUT/dyn" "$OUT/DYN"

bases=()
case "${1:-}" in
--list)
  f=${2:?--list needs a file}
  while IFS= read -r b; do
    b=${b%%#*}                      # drop trailing comments
    b=$(printf '%s' "$b" | tr -d ' \t\r')
    [ -n "$b" ] && bases+=("$b")
  done <"$f" ;;
--all-simple)
  # The Makefile's rule shape "one own .o + libc" is the convertible core.
  python3 - "$OUT" <<'PY' > /tmp/dyn_bases.$$
import re, sys
lines = open('Makefile').read().splitlines()
lean = {'$(OBJ_DIR)/user_libc.o','$(OBJ_DIR)/user_malloc.o','$(OBJ_DIR)/libc_string.o','$(OBJ_DIR)/libc.a'}
seen = []
for ln in lines:
    m = re.match(r'^\$\((\w+_BIN)\):(.*)$', ln)
    if not m:
        continue
    own = [d for d in m.group(2).split() if d.endswith('.o') and d not in lean]
    if len(own) == 1 and own[0].startswith('$(OBJ_DIR)/'):
        base = own[0][len('$(OBJ_DIR)/'):-2]
        if base not in seen:
            seen.append(base)
for b in seen:
    print(b)
PY
  while read -r b; do [ -n "$b" ] && bases+=("$b"); done < /tmp/dyn_bases.$$
  rm -f /tmp/dyn_bases.$$ ;;
*) bases=("$@") ;;
esac

[ -f "$OUT/libc.so" ] || { echo "dyn-migrate: $OUT/libc.so missing (run tools/build_libs.sh $ARCH)" >&2; exit 2; }

# main()-style programs need the dynamic crt0; legacy _start programs must NOT
# link it (two _start definitions).  Build it once per arch.
CRT0_DYN="$OUT/crt0_dyn.o"
if [ ! -f "$CRT0_DYN" ]; then
  clang "${CFLAGS[@]}" -c src/libc/crt0_dyn.c -o "$CRT0_DYN" || { echo "dyn-migrate: crt0_dyn build failed" >&2; exit 2; }
fi

ok=(); bad=()
for entry in "${bases[@]}"; do
  # "name=base1,base2" builds NAME.BIN from several TUs; a bare "base" is the
  # one-TU case.  Output name and list file names stay 8.3-friendly.
  name=${entry%%=*}
  if [ "$name" = "$entry" ]; then list=$entry; else list=${entry#*=}; fi
  IFS=',' read -r -a tus <<<"$list"
  objs=(); cpp=0; missing=0
  for base in "${tus[@]}"; do
    src=""
    for cand in "src/user/$base.c" "src/user/$base.cpp"; do
      [ -f "$cand" ] && src=$cand && break
    done
    if [ -z "$src" ]; then
      src=$(find src -name "$base.c" -o -name "$base.cpp" 2>/dev/null | head -1)
    fi
    if [ -z "$src" ]; then echo "FAIL $name: no source for $base"; missing=1; break; fi
    case "$src" in *.cpp) cpp=1; break ;; esac
    obj="$OUT/dyn/$base.o"
    if ! clang "${CFLAGS[@]}" -c "$src" -o "$obj" 2>/tmp/dyn_cc.$$; then
      echo "FAIL $name: compile $src"; grep -m1 "error:" /tmp/dyn_cc.$$ | sed 's/^/       /'
      missing=1; rm -f /tmp/dyn_cc.$$; break
    fi
    rm -f /tmp/dyn_cc.$$
    objs+=("$obj")
  done
  if [ $cpp -eq 1 ]; then echo "SKIP $name: C++ (needs a PIC libcxx.a first)"; bad+=("$name"); continue; fi
  if [ $missing -eq 1 ]; then bad+=("$name"); continue; fi
  base=$name
  bin="$OUT/DYN/$base.BIN"
  # A TU without _start is a main()-style program: it needs crt0_dyn, or lld
  # emits "cannot find entry symbol _start" and an image whose e_entry is 0
  # (the kernel then refuses it with "e_entry outside every executable
  # segment" — measured on wc/cut/head/tail/touch).
  extra=()
  if ! nm "${objs[@]}" 2>/dev/null | grep -qE '[0-9a-f]+ T _start$'; then
    extra=("$CRT0_DYN")
  fi
  if ! ld.lld -pie --dynamic-linker /LDHOBBY.SO -e _start -z noexecstack -z now \
        -o "$bin" "${extra[@]}" "${objs[@]}" "$OUT/libc.so" 2>/tmp/dyn_ld.$$; then
    echo "FAIL $name: link"; grep -m1 "error:" /tmp/dyn_ld.$$ | sed 's/^/       /'
    bad+=("$name"); rm -f /tmp/dyn_ld.$$; continue
  fi
  rm -f /tmp/dyn_ld.$$
  # A zero entry means the link silently lost its entry symbol.
  if readelf -h "$bin" | grep -q 'Entry point address: *0x0$'; then
    echo "FAIL $name: linked with e_entry = 0"; bad+=("$base"); continue
  fi
  ok+=("$name")
done

# Regenerate the boot list for KERNEL_MODE_DYN_TEST (successes only).
{
  echo "/* generated by tools/dyn-migrate.sh — do not edit */"
  echo "static const char dyn_smoke_names[][16] = {"
  for b in "${ok[@]}"; do
    up=$(printf '%s' "$b" | tr 'a-z' 'A-Z')
    printf '  "%s.BIN",\n' "$up"
  done
  echo "};"
} > "$OUT/dyn_list.h"

echo "dyn-migrate $ARCH: ${#ok[@]} converted, ${#bad[@]} failed"
[ ${#bad[@]} -eq 0 ] || { printf 'failed: %s\n' "${bad[*]}"; exit 1; }
