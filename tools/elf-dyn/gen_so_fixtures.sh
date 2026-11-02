#!/usr/bin/env bash
# tools/elf-dyn/gen_so_fixtures.sh — build the dynamic-linking fixture chain
# and its golden readelf dumps.
#
# Lane C, docs/shared-libs.md §4 lane C.3.  The fixtures are the *contract
# evidence* for lane D's interpreter: instead of guessing what a relocation
# looks like, they pin one module per relocation shape and freeze the
# readelf output next to the sources.  A later regression is then a diff.
#
#   LIBDEP.SO   data + function, no dependencies, one local pointer
#   LIBUSE.SO   imports both from LIBDEP (GLOB_DAT + JUMP_SLOT), DT_NEEDED
#               on LIBDEP.SO, and defines __thread TLS (.tdata + .tbss)
#   APP.BIN     ET_DYN executable (e_entry = _start), DT_NEEDED on LIBUSE.SO,
#               calls into it and reads its data
#
# Covered relocation types (asserted below, per target, not assumed):
#   R_*_RELATIVE    every module (local pointers)
#   R_*_GLOB_DAT    LIBUSE.SO (dep_counter), APP.BIN (use_localp)
#   R_*_JUMP_SLOT   LIBUSE.SO (dep_bump),  APP.BIN (use_call)
#   TLS TPREL/TPOFF LIBUSE.SO (use_tls, use_tls_init) + DT_FLAGS STATIC_TLS
#   DT_NEEDED       LIBUSE.SO -> LIBDEP.SO, APP.BIN -> LIBUSE.SO
#
# Usage:
#   tools/elf-dyn/gen_so_fixtures.sh            # rebuild + refresh golden/
#   tools/elf-dyn/gen_so_fixtures.sh --check    # rebuild + diff against golden/
#   tools/elf-dyn/gen_so_fixtures.sh --arch arm # one target only
#
# Outputs (gitignored build dir):   obj/fixtures/<arch>/{LIBDEP.SO,LIBUSE.SO,APP.BIN}
# Golden dumps (committed):         tools/elf-dyn/golden/<arch>/<module>.dyn.txt
#
# Flags: plain -fPIC, NOT the Makefile's -mcmodel=large.  These modules are
# interesting only for their relocation shapes, and the small PIC model emits
# the classic PLT (JUMP_SLOT) + GOT (GLOB_DAT) pattern on x86_64, whereas
# -mcmodel=large turns function imports into GOT loads as well (that is why
# libc.so's x86_64 build has 140 GLOB_DAT and 1 JUMP_SLOT — see
# obj/intel/libc.so.evidence.txt).  Both shapes are legal for lane D; the
# fixtures pin the PLT one so a JUMP_SLOT is always exercised.

set -uo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-clang}
LD=${LD:-ld.lld}
READELF=${READELF:-readelf}
JOBS=${JOBS:-8}
SRC=tools/elf-dyn/src
GOLD=tools/elf-dyn/golden

mode=regen
arches=()
while [ $# -gt 0 ]; do
  case $1 in
    --check) mode=check ;;
    --arch)  shift; arches+=("$1") ;;
    -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "gen_so_fixtures: unknown argument '$1'" >&2; exit 2 ;;
  esac
  shift
done
[ ${#arches[@]} -gt 0 ] || arches=(arm intel)

die()  { printf 'gen_so_fixtures: error: %s\n' "$*" >&2; exit 1; }
say()  { printf 'gen_so_fixtures: %s\n' "$*" >&2; }

platform_flags() {
  case $1 in
    arm)
      echo "-O2 -g --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -ftls-model=initial-exec"
      ;;
    intel)
      echo "-O2 -g --target=x86_64-none-elf -ffreestanding -mno-red-zone -ftls-model=initial-exec"
      ;;
    *) die "unknown arch '$1' (expected arm|intel)" ;;
  esac
}

# One module: compile <src> and link with the DSO/PIE shape asked for.
#   build_module <arch> <outdir> <cfile> <outfile> <linkmode> [deps...]
#   linkmode: dso | pie
build_module() {
  local arch=$1 outdir=$2 cf=$3 out=$4 linkmode=$5
  shift 5
  local obj="$outdir/$(basename "${cf%.c}").o"
  local flags
  flags=$(platform_flags "$arch")
  # shellcheck disable=SC2086
  "$CC" $flags -fPIC -c "$SRC/$cf" -o "$obj" || die "compile failed: $cf ($arch)"

  local -a common=(-z relro -z now -z defs -z noexecstack --hash-style=both)
  case $linkmode in
    dso)
      local soname
      soname=$(basename "$out")   # LIBDEP.SO / LIBUSE.SO: 8.3 names, as on disk
      "$LD" -shared --soname="$soname" "${common[@]}" -o "$outdir/$out" "$obj" "$@" \
        || die "link failed: $out ($arch)"
      ;;
    pie)
      "$LD" -pie --no-dynamic-linker -e _start "${common[@]}" \
        -o "$outdir/$out" "$obj" "$@" || die "link failed: $out ($arch)"
      ;;
    *) die "bad linkmode $linkmode" ;;
  esac
}

# dump_module <outdir> <module> <goldendir> — readelf -r/-d/--dyn-syms, plus
# -h/-l (a few lines) so the file type, entry point and segment flags are in
# the diff too.
dump_module() {
  local outdir=$1 mod=$2 gdir=$3
  {
    echo "### readelf -h (identity) — $mod"
    "$READELF" -h -W "$outdir/$mod" | grep -E 'Class|Type|Machine|Entry point'
    echo
    echo "### readelf -l (program headers) — $mod"
    "$READELF" -l -W "$outdir/$mod" | sed -n '/Program Headers/,/Section to Segment/p' | grep -v '^$'
    echo
    echo "### readelf -d (dynamic section) — $mod"
    "$READELF" -d -W "$outdir/$mod"
    echo
    echo "### readelf -r (relocations) — $mod"
    "$READELF" -r -W "$outdir/$mod"
    echo
    echo "### readelf --dyn-syms (dynamic symbols) — $mod"
    "$READELF" --dyn-syms -W "$outdir/$mod"
  } > "$gdir/$mod.dyn.txt"
}

# validate <arch> <outdir> — assert the fixture chain really contains the
# shapes lane D must support; fail loudly by name if one is missing.
validate() {
  local arch=$1 outdir=$2 rc=0
  local relocs
  relocs() { "$READELF" -rW "$outdir/$1" | awk '/R_[A-Z0-9_]+/ {for (i=1;i<=NF;i++) if ($i ~ /^R_[A-Z0-9_]+$/) {print $i; break}}' | LC_ALL=C sort -u; }
  dyn() { "$READELF" -dW "$outdir/$1"; }

  case $arch in
    arm) PAT_RELATIVE=$'R_AARCH64_RELATIVE'; PAT_GLOB=$'R_AARCH64_GLOB_DAT'
         PAT_JUMP=$'R_AARCH64_JUMP_SLOT'; PAT_TLS=$'R_AARCH64_TLS_TPREL64' ;;
    intel) PAT_RELATIVE=$'R_X86_64_RELATIVE'; PAT_GLOB=$'R_X86_64_GLOB_DAT'
           PAT_JUMP=$'R_X86_64_JUMP_SLOT'; PAT_TLS=$'R_X86_64_TPOFF64' ;;
  esac

  local m types
  for m in LIBDEP.SO LIBUSE.SO APP.BIN; do
    [ -f "$outdir/$m" ] || { say "FAIL $arch: $m was not produced"; rc=1; continue; }
    types=$(relocs "$m")
    grep -qx "$PAT_RELATIVE" <<<"$types" \
      || { say "FAIL $arch: $m has no $PAT_RELATIVE (local pointer)"; rc=1; }
  done

  types=$(relocs LIBUSE.SO)
  grep -qx "$PAT_GLOB" <<<"$types" || { say "FAIL $arch: LIBUSE.SO has no $PAT_GLOB (data import)"; rc=1; }
  grep -qx "$PAT_JUMP" <<<"$types" || { say "FAIL $arch: LIBUSE.SO has no $PAT_JUMP (function import)"; rc=1; }
  grep -qx "$PAT_TLS" <<<"$types" || { say "FAIL $arch: LIBUSE.SO has no $PAT_TLS (initial-exec TLS)"; rc=1; }
  dyn LIBUSE.SO | grep -q 'STATIC_TLS' || { say "FAIL $arch: LIBUSE.SO DT_FLAGS lacks STATIC_TLS"; rc=1; }

  types=$(relocs APP.BIN)
  grep -qx "$PAT_JUMP" <<<"$types" || { say "FAIL $arch: APP.BIN has no $PAT_JUMP (function import)"; rc=1; }
  grep -qx "$PAT_GLOB" <<<"$types" || { say "FAIL $arch: APP.BIN has no $PAT_GLOB (data import)"; rc=1; }

  dyn LIBUSE.SO | grep -q 'NEEDED.*LIBDEP.SO' \
    || { say "FAIL $arch: LIBUSE.SO has no DT_NEEDED LIBDEP.SO"; rc=1; }
  dyn APP.BIN | grep -q 'NEEDED.*LIBUSE.SO' \
    || { say "FAIL $arch: APP.BIN has no DT_NEEDED LIBUSE.SO"; rc=1; }
  dyn APP.BIN | grep -q 'NEEDED.*LIBDEP.SO' && say "note: APP.BIN also carries DT_NEEDED LIBDEP.SO (transitive, harmless)"

  # the executable must be ET_DYN with a real entry point and no PT_INTERP
  case $($READELF -h -W "$outdir/APP.BIN" | sed -n 's/.*Type: *\([A-Z]*\).*/\1/p') in
    DYN) ;;
    *) say "FAIL $arch: APP.BIN is not ET_DYN (readelf Type is not DYN)"; rc=1 ;;
  esac
  "$READELF" -h -W "$outdir/APP.BIN" | grep -qE 'Entry point 0x0+$' \
    && { say "FAIL $arch: APP.BIN entry point is 0"; rc=1; }
  "$READELF" -l -W "$outdir/APP.BIN" | grep -q 'INTERP' \
    && { say "FAIL $arch: APP.BIN unexpectedly has PT_INTERP (fixtures are entered by lane D, not the kernel)"; rc=1; }

  [ "$rc" -eq 0 ] || die "$arch: fixture contract violated"
  say "$arch: fixture contract OK"
}

# ---------------------------------------------------------------------------
rc_all=0
for arch in "${arches[@]}"; do
  outdir=obj/fixtures/$arch
  rm -rf "$outdir"
  mkdir -p "$outdir" "$GOLD/$arch"
  say "== $arch: building LIBDEP.SO / LIBUSE.SO / APP.BIN"
  build_module "$arch" "$outdir" dep.c LIBDEP.SO dso
  build_module "$arch" "$outdir" use.c LIBUSE.SO dso "$outdir/LIBDEP.SO"
  build_module "$arch" "$outdir" app.c APP.BIN  pie "$outdir/LIBUSE.SO"
  validate "$arch" "$outdir"

  if [ "$mode" = check ]; then
    local_fail=0
    for m in LIBDEP.SO LIBUSE.SO APP.BIN; do
      tdir=$(mktemp -d)
      dump_module "$outdir" "$m" "$tdir"
      if ! diff -u "$GOLD/$arch/$m.dyn.txt" "$tdir/$m.dyn.txt" > "$tdir/diff.txt"; then
        say "DIFF $arch/$m.dyn.txt:"
        sed -n '1,40p' "$tdir/diff.txt" >&2
        local_fail=1
      fi
      rm -rf "$tdir"
    done
    [ "$local_fail" -eq 0 ] && say "$arch: golden dumps match ($GOLD/$arch)" || rc_all=1
  else
    for m in LIBDEP.SO LIBUSE.SO APP.BIN; do
      dump_module "$outdir" "$m" "$GOLD/$arch"
    done
    say "$arch: wrote $GOLD/$arch/{LIBDEP.SO,LIBUSE.SO,APP.BIN}.dyn.txt"
  fi
  say "$arch: modules in $outdir"
done

exit "$rc_all"
