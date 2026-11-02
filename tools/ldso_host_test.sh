#!/usr/bin/env bash
#
# tools/ldso_host_test.sh — host gate for LDHOBBY.SO (lane D, so-ld).
#
# Builds a real dynamic fixture chain with clang + ld.lld (no dependency on
# lane C's tools/build_libs.sh), compiles src/user/ld/*.c against the POSIX
# shim behind -DHOST_TEST, and runs the assertions in src/user/ld/host_test.c
# inside the fixture directory, which is the loader's "disk root".
#
# Every check prints PASS/FAIL; any FAIL exits nonzero.  `--cross` also builds
# the interpreter itself for both HobbyOS targets with the tree's freestanding
# flags and asserts the resulting LDHOBBY.SO shape (ET_DYN, SONAME, no
# GOT/PLT relocations, no PT_INTERP, no PT_TLS).
#
# Usage: tools/ldso_host_test.sh [--cross] [--keep]
#   --cross  additionally build x86_64-none-elf / aarch64-none-elf LDHOBBY.SO
#   --keep   leave the work directory behind for inspection
#
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CC=${CC:-clang}
LDLLD=${LDLLD:-ld.lld}
READELF=${READELF:-readelf}
DO_CROSS=0
KEEP=0
for arg in "$@"; do
  case "$arg" in
    --cross) DO_CROSS=1 ;;
    --keep) KEEP=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "ldso_host_test: unknown argument: $arg" >&2; exit 2 ;;
  esac
done

export HOBBYOS_USE_CCACHE=1

WORK=${LDSO_TEST_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/ldso_host_test.XXXXXX")}
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$WORK"; }
trap cleanup EXIT

SRC=$WORK/src
FIX=$WORK/fix
OBJ=$WORK/obj
LIBDIR=$WORK/lib
mkdir -p "$SRC" "$FIX" "$OBJ" "$LIBDIR"

fail() { echo "FAIL $*"; exit 1; }
say() { echo "== $*"; }

# Fixture compile flags: freestanding, no libc, no stack protector (the host
# harness installs FS for the TLS checks; a stack protector would read
# FS:0x28 and take the harness down with it).
FIX_CC="$CC -O0 -g -ffreestanding -nostdlib -fno-stack-protector \
        -fno-asynchronous-unwind-tables -fuse-ld=lld -Wl,-z,max-page-size=4096 \
        -ftls-model=initial-exec"

# --- fixtures -------------------------------------------------------------

cat >"$SRC/liba.c" <<'EOF'
/* LIBA.SO: built with --hash-style=sysv (DT_HASH only).  Defines the data
 * object the executable takes a COPY of, and the first definition of the
 * symbols that LIBB also defines. */
extern void app_log(int);
int a_value = 1;
int scope_sym = 100;
int dup_sym = 10;
int a_only(void) { return 9001; }
int a_fn(int x) { return x + a_value; }
int scope_fn(void) { return 100; }
__thread int a_tls = 0x11;
int a_tls_get(void) { return a_tls; }
void a_tls_set(int v) { a_tls = v; }
__attribute__((constructor)) static void a_ctor(void) { app_log(1); }
EOF

cat >"$SRC/libb.c" <<'EOF'
/* LIBB.SO: built with --hash-style=gnu (DT_GNU_HASH only) and a second
 * definition of scope_fn/scope_sym — always out of scope behind LIBA. */
extern void app_log(int);
int b_value = 2;
int scope_sym = 200;
int b_only(void) { return 9002; }
int b_fn(int x) { return x + b_value; }
int scope_fn(void) { return 200; }
__thread int b_tls = 0x22;
int b_tls_get(void) { return b_tls; }
void b_tls_set(int v) { b_tls = v; }
__attribute__((constructor)) static void b_ctor(void) { app_log(2); }
EOF

cat >"$SRC/libe.c" <<'EOF'
/* LIBE.SO: private dependency of LIBD (proves DT_NEEDED is walked for DSOs,
 * not only for the executable). */
extern void app_log(int);
int e_fn(void) { return 41; }
__attribute__((constructor)) static void e_ctor(void) { app_log(4); }
EOF

cat >"$SRC/libd.c" <<'EOF'
/* LIBD.SO: one instance of every relocation kind that a DSO can carry. */
extern void app_log(int);
extern int scope_sym;  /* LIBA (first DSO in scope) */
extern int dup_sym;    /* the executable */
extern int a_fn(int);  /* LIBA */
extern int b_fn(int);  /* LIBB */
extern int e_fn(void); /* LIBE, through LIBD's own DT_NEEDED */
int a_only_impl(void);
extern int a_only(void);

int *d_scope_ptr = &scope_sym;   /* R_*_ABS64 (data)            */
int (*d_fnptr)(void) = &a_only;  /* R_*_ABS64 (function)        */
static int d_self = 5;
int *d_self_ptr = &d_self;       /* R_*_RELATIVE                */

int d_read_scope(void) { return scope_sym; }       /* GLOB_DAT   */
int d_read_dup(void) { return dup_sym; }           /* GLOB_DAT   */
int d_call_a(void) { return a_fn(5); }             /* JUMP_SLOT  */
int d_call_b(void) { return b_fn(5); }             /* JUMP_SLOT  */
int d_call_e(void) { return e_fn(); }              /* JUMP_SLOT  */
int d_self_read(void) { return *d_self_ptr; }      /* RELATIVE   */
__attribute__((constructor)) static void d_ctor(void) { app_log(3); }
EOF

cat >"$SRC/app.c" <<'EOF'
/* APP: ET_DYN executable with PT_INTERP. */
extern int a_fn(int);
extern int b_fn(int);

int dup_sym = 1000;   /* exported: beats LIBA's 10 */
int init_log[8];
int init_n;
void app_log(int id) { if (init_n < 8) init_log[init_n++] = id; }

__thread int app_tls = 0x33;
int app_tls_get(void) { return app_tls; }
void app_tls_set(int v) { app_tls = v; }

int app_sum(int x) { return a_fn(x) + b_fn(x); }

__attribute__((constructor)) static void app_ctor(void) { app_log(0); }
void _start(void) { for (;;) { } }
EOF

cat >"$SRC/app_missing.c" <<'EOF'
/* APP_MISSING: DT_NEEDED LIBNOPE.SO, which is never placed at the disk root. */
extern int nope_fn(void);
int use_nope(void) { return nope_fn(); }
void _start(void) { for (;;) { } }
EOF

cat >"$SRC/libnope.c" <<'EOF'
int nope_fn(void) { return 0; }
EOF

cat >"$SRC/libbad.c" <<'EOF'
/* LIBBAD.SO: a well-formed DSO whose .rela.dyn entry is patched to an
 * unsupported type by this script (see patch_reloc.py below). */
int bad_x = 5;
int *bad_p = &bad_x;
int bad_get(void) { return *bad_p; }
EOF

cat >"$SRC/app_bad.c" <<'EOF'
extern int bad_x;
int use_bad(void) { return bad_x; }
void _start(void) { for (;;) { } }
EOF

cat >"$SRC/libbig.c" <<'EOF'
/* LIBBIG.SO: a 20 MiB .bss segment — larger than one 16 MiB USER_LIB slot, so
 * the loader must refuse it by name instead of mapping across slot bounds. */
char big[20 * 1024 * 1024];
int big_fn(void) { return big[0]; }
EOF

cat >"$SRC/app_big.c" <<'EOF'
extern int big_fn(void);
int use_big(void) { return big_fn(); }
void _start(void) { for (;;) { } }
EOF

cat >"$SRC/libcopy.c" <<'EOF'
/* LIBCOPY.SO: the COPY relocation source for the ET_EXEC fixture. */
int copy_value = 21;
int libcopy_read(void) { return copy_value; }
EOF

cat >"$SRC/app_copy_exec.c" <<'EOF'
/* APP_COPY: a non-PIE ET_EXEC, the only shape that carries a real
 * R_*_COPY relocation (it takes a fixed address for the DSO's data object).
 * Production native programs are ET_DYN/-fPIC and need none; the loader is
 * still required to apply them. */
extern int copy_value;
int copy_read(void) { return copy_value; }
void copy_set(int v) { copy_value = v; }
void _start(void) { for (;;) { } }
EOF

cat >"$SRC/selfpie.c" <<'EOF'
/* SELFPIE: an ET_DYN image with R_*_RELATIVE entries and exported symbols,
 * used to prove hb_ldso_self_relocate() applies them. */
int self_val = 42;
int *self_ptr = &self_val;
int self_get(void) { return *self_ptr; }
void _start(void) { for (;;) { } }
EOF

say "fixtures -> $FIX"
$FIX_CC -fPIC -shared -Wl,-soname,LIBA.SO -Wl,--hash-style=sysv -o "$FIX/LIBA.SO" "$SRC/liba.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBA.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBB.SO -Wl,--hash-style=gnu -o "$FIX/LIBB.SO" "$SRC/libb.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBB.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBE.SO -o "$FIX/LIBE.SO" "$SRC/libe.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBE.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBD.SO -o "$FIX/LIBD.SO" "$SRC/libd.c" "$FIX/LIBE.SO" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBD.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBNOPE.SO -o "$LIBDIR/LIBNOPE.SO" "$SRC/libnope.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBNOPE.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBBAD.SO -o "$FIX/LIBBAD.SO" "$SRC/libbad.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBBAD.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBCOPY.SO -o "$FIX/LIBCOPY.SO" "$SRC/libcopy.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBCOPY.SO"; }
$FIX_CC -fPIC -shared -Wl,-soname,LIBBIG.SO -o "$FIX/LIBBIG.SO" "$SRC/libbig.c" 2>"$OBJ/err" || { cat "$OBJ/err"; fail "build LIBBIG.SO"; }

# The executable: ET_DYN + PT_INTERP naming LDHOBBY.SO.  -fno-pic is a
# deliberate choice: it is what produces the COPY relocation under test.
$FIX_CC -fPIC -pie -Wl,--export-dynamic \
  -Wl,--no-dynamic-linker -Wl,-e,_start -Wl,-rpath-link,"$FIX" \
  -o "$FIX/APP" "$SRC/app.c" "$FIX/LIBA.SO" "$FIX/LIBB.SO" "$FIX/LIBD.SO" \
  2>"$OBJ/err" || { cat "$OBJ/err"; fail "build APP"; }

$FIX_CC -fno-pic -mcmodel=large -no-pie -Wl,--export-dynamic \
  -Wl,--no-dynamic-linker -Wl,-e,_start -Wl,-rpath-link,"$FIX" \
  -o "$FIX/APP_COPY" "$SRC/app_copy_exec.c" "$FIX/LIBCOPY.SO" \
  2>"$OBJ/err" || { cat "$OBJ/err"; fail "build APP_COPY"; }

$FIX_CC -fPIC -pie -Wl,--export-dynamic -Wl,--no-dynamic-linker \
  -Wl,-e,_start -Wl,-rpath-link,"$FIX" \
  -o "$FIX/APP_BIG" "$SRC/app_big.c" "$FIX/LIBBIG.SO" \
  2>"$OBJ/err" || { cat "$OBJ/err"; fail "build APP_BIG"; }

$FIX_CC -fPIC -pie -Wl,--export-dynamic \
  -Wl,--no-dynamic-linker -Wl,-e,_start -Wl,-rpath-link,"$LIBDIR" \
  -o "$FIX/APP_MISSING" "$SRC/app_missing.c" "$LIBDIR/LIBNOPE.SO" \
  2>"$OBJ/err" || { cat "$OBJ/err"; fail "build APP_MISSING"; }

$FIX_CC -fPIC -pie -Wl,--export-dynamic \
  -Wl,--no-dynamic-linker -Wl,-e,_start -Wl,-rpath-link,"$FIX" \
  -o "$FIX/APP_BADRELOC" "$SRC/app_bad.c" "$FIX/LIBBAD.SO" \
  2>"$OBJ/err" || { cat "$OBJ/err"; fail "build APP_BADRELOC"; }

$FIX_CC -fPIC -pie -Wl,--export-dynamic -Wl,--no-dynamic-linker \
  -Wl,-e,_start -o "$FIX/SELFPIE" "$SRC/selfpie.c" \
  2>"$OBJ/err" || { cat "$OBJ/err"; fail "build SELFPIE"; }

# --- shape assertions on the fixtures themselves -------------------------

say "fixture shape"
have() { $READELF -d "$1" | grep -q "$2"; }
have "$FIX/LIBA.SO" "HASH" && ! $READELF -d "$FIX/LIBA.SO" | grep -q "GNU_HASH" \
  || fail "LIBA.SO is not sysv-hash-only"
have "$FIX/LIBB.SO" "GNU_HASH" && ! $READELF -d "$FIX/LIBB.SO" | grep -q "(HASH)" \
  || fail "LIBB.SO is not gnu-hash-only"
$READELF -d "$FIX/LIBD.SO" | grep -q "NEEDED.*LIBE.SO" \
  || fail "LIBD.SO does not carry a DT_NEEDED for LIBE.SO"
$READELF -h "$FIX/APP" | grep -q "Type:.*DYN" || fail "APP is not ET_DYN"
$READELF -Wr "$FIX/APP_COPY" | grep -q "R_X86_64_COPY" || fail "APP_COPY has no COPY relocation"
$READELF -h "$FIX/APP_COPY" | grep -q "Type:.*EXEC" || fail "APP_COPY is not ET_EXEC"
echo "PASS fixture: sysv-only, gnu-only, DSO DT_NEEDED, ET_DYN, COPY present"

# --- patched fixture: unsupported relocation type ------------------------

say "patch LIBBAD.SO -> unsupported relocation type"
python3 - "$FIX/LIBBAD.SO" <<'PY' || fail "patch LIBBAD.SO"
import struct, sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
e_shoff, = struct.unpack_from("<Q", data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
if e_shoff == 0 or e_shnum == 0:
    sys.exit("no section headers")
shstr_off, = struct.unpack_from("<Q", data, e_shoff + e_shstrndx * e_shentsize + 0x18)
def sh(i):
    b = e_shoff + i * e_shentsize
    return (struct.unpack_from("<I", data, b)[0],
            struct.unpack_from("<Q", data, b + 0x18)[0],
            struct.unpack_from("<Q", data, b + 0x20)[0])
def nm(off):
    end = data.index(b"\0", shstr_off + off)
    return data[shstr_off + off:end].decode()
target = None
for i in range(e_shnum):
    name, off, size = sh(i)
    if nm(name) in (".rela.dyn", ".rela.plt") and size >= 24:
        target = (nm(name), off)
        break
if not target:
    sys.exit("no dynamic RELA table")
name, off = target
# ELF64 Rela: r_offset (8), r_info (8; high 32 = sym, low 32 = type), addend.
struct.pack_into("<I", data, off + 8, 0xDEAD)
open(path, "wb").write(data)
print("patched %s entry 0 -> type 0xdead" % name)
PY

# --- build the loader under test -----------------------------------------

say "compile LDHOBBY.SO sources (host shim)"
CFLAGS_COMMON="-O1 -g -Wall -Wextra -std=c11 -DHOST_TEST -I$ROOT/src/include -I$ROOT/src/user/ld"
for f in ldso elf_dyn strtab mmap_io; do
  $CC $CFLAGS_COMMON -c "$ROOT/src/user/ld/$f.c" -o "$OBJ/$f.o" 2>"$OBJ/err" \
    || { cat "$OBJ/err"; fail "compile $f.c"; }
done
$CC $CFLAGS_COMMON -c "$ROOT/src/user/ld/host_test.c" -o "$OBJ/host_test.o" 2>"$OBJ/err" \
  || { cat "$OBJ/err"; fail "compile host_test.c"; }
$CC -o "$WORK/ldso_host_test" "$OBJ/ldso.o" "$OBJ/elf_dyn.o" "$OBJ/strtab.o" \
  "$OBJ/mmap_io.o" "$OBJ/host_test.o" 2>"$OBJ/err" \
  || { cat "$OBJ/err"; fail "link host harness"; }

# --- run ------------------------------------------------------------------

say "host assertions (fixture dir: $FIX)"
"$WORK/ldso_host_test" "$FIX"
rc=$?
if [ "$rc" -ne 0 ]; then
  KEEP=1 # keep the fixtures and objects for debugging
  fail "host assertions (rc=$rc, work dir kept at $WORK)"
fi

# --- optional: the real interpreter for both targets ---------------------

if [ "$DO_CROSS" = 1 ]; then
  say "cross-build LDHOBBY.SO (x86_64-none-elf, aarch64-none-elf)"
  RES_INC=$($CC -print-resource-dir)/include
  for arch in x86_64 aarch64; do
    for f in ldso elf_dyn strtab mmap_io; do
      $CC -c -O2 -Wall -Wextra -I"$ROOT/src/include" -I"$ROOT/src/user/ld" \
        --target=$arch-none-elf -ffreestanding -nostdinc -isystem "$RES_INC" \
        -fPIC -fno-stack-protector -fno-builtin -fvisibility=hidden \
        "$ROOT/src/user/ld/$f.c" -o "$OBJ/$f.$arch.o" 2>"$OBJ/err" \
        || { cat "$OBJ/err"; fail "cross compile $f.c ($arch)"; }
    done
    $LDLLD -shared -soname LDHOBBY.SO -e _start -z max-page-size=4096 \
      --no-undefined -o "$OBJ/LDHOBBY.$arch.SO" \
      "$OBJ/ldso.$arch.o" "$OBJ/elf_dyn.$arch.o" "$OBJ/strtab.$arch.o" \
      "$OBJ/mmap_io.$arch.o" 2>"$OBJ/err" \
      || { cat "$OBJ/err"; fail "link LDHOBBY.SO ($arch)"; }

    $READELF -h "$OBJ/LDHOBBY.$arch.SO" | grep -q "Type:.*DYN" \
      || fail "LDHOBBY.SO ($arch) is not ET_DYN"
    $READELF -d "$OBJ/LDHOBBY.$arch.SO" | grep -q "SONAME.*LDHOBBY.SO" \
      || fail "LDHOBBY.SO ($arch) has no SONAME"
    if $READELF -Wr "$OBJ/LDHOBBY.$arch.SO" | grep -q "GLOB_DAT\|JUMP_SLOT"; then
      fail "LDHOBBY.SO ($arch) needs GOT/PLT relocations"
    fi
    if $READELF -l "$OBJ/LDHOBBY.$arch.SO" | grep -q "INTERP\|TLS"; then
      fail "LDHOBBY.SO ($arch) carries PT_INTERP/PT_TLS"
    fi
    echo "PASS cross: LDHOBBY.$arch.SO is ET_DYN, SONAME, no GOT/PLT, no INTERP/TLS"
  done
fi

echo "LDSO HOST TEST: ALL PASS"
exit 0
