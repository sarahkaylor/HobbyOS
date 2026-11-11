#!/usr/bin/env bash
# tools/build_libs.sh — build HobbyOS's libc as a position-independent shared
# object (LIBC.SO), and the interpreter (LDHOBBY.SO) from lane D's sources.
#
# Lane C, docs/shared-libs.md §4.  This script is standalone on purpose: the
# coordinator wires it into `make libs` (see the MAKEFILE block in the lane C
# report), it does not read the Makefile and it never writes outside obj/.
#
#   tools/build_libs.sh arm            # obj/arm/libc.so  + evidence
#   tools/build_libs.sh intel          # obj/intel/libc.so + evidence
#   tools/build_libs.sh all            # both, in sequence
#   tools/build_libs.sh gen_map        # regenerate src/libc/libc.map (union
#                                      #   of both arches' libc.a globals)
#   tools/build_libs.sh build_interp <arch>   # obj/<arch>/LDHOBBY.SO (lane D)
#   tools/build_libs.sh print_interp_cmd <arch>  # the command block only
#
# Environment knobs:
#   JOBS=16                 parallel compile jobs (default 16)
#   CC / LD / NM / READELF  tool overrides (default: clang, ld.lld, nm, readelf)
#   HOBBYOS_USE_CCACHE=1    route compiles through ccache when it is installed
#   LIBC_SO_DEBUG=1         keep DWARF in libc.so (default: --strip-debug)
#
# ---------------------------------------------------------------------------
# Compile flags are a copy of the Makefile's userland flags for the same arch
# (Makefile lines ~279-320, both flavours from one source tree):
#
#   intel  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include
#                        -Isrc/user_include/graphics -Isrc/include
#                        -Isrc/libc/include --target=x86_64-none-elf
#                        -ffreestanding -mno-red-zone -nostdinc
#                        -isystem $(clang -print-resource-dir)/include
#                        -mcmodel=large
#   arm    USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include
#                        -Isrc/user_include/graphics -Isrc/include
#                        -Isrc/libc/include --target=aarch64-none-elf
#                        -ffreestanding -mcpu=cortex-a53
#
# ...plus the two flags the DSO needs and the archive does not:
#   -fPIC -ftls-model=initial-exec        (§1.3: initial-exec TLS only)
# and the per-TU flag the archive already carries:
#   src/libc/src/math.c  -ffp-contract=off  (double-double kernels need
#                        strictly-rounded ops; Makefile libc_math.o rule)
#
# -mcmodel=large stays on x86_64 because user images live at 64 GiB; with
# -fPIC it is legal and yields GOT-indirect symbol references
# (R_X86_64_REX_GOTPCRELX), which is exactly what a DSO wants.
#
# Deliberately NOT compiled into libc.so:
#   src/libc/crt0.c          defines _start (the executable owns the entry),
#                            and calls main()/exit() on behalf of a program.
#   anything under src/kernel/** (kernel code is not userland ABI).
# The exclusion is enforced, not assumed: after compiling, every object is
# checked for a defined `_start` and the build aborts naming the TU.
#
# src/user/libc.c + src/user/malloc.c ARE compiled in: they are the syscall
# stubs and the allocator, they are what libc.a ships, and lane D's programs
# need them from LIBC.SO.
# ---------------------------------------------------------------------------
# MAKEFILE WIRING (for the coordinator; this script does not touch the
# Makefile).  Insert next to the existing libc.a rules:
#
#   # --- lane C (docs/shared-libs.md §4): PIC libc + LIBC.SO --------------
#   LIBS_SCRIPT = tools/build_libs.sh
#   .PHONY: libs libs-arm libs-intel libs-fixtures
#   libs: libs-arm libs-intel
#   libs-arm: $(OBJ_DIR-for-arm)          # or just rely on the archive rule
#   libs-arm:
#           $(MAKE) ARCH=arm obj/arm/libc.a
#           $(LIBS_SCRIPT) arm
#   libs-intel:
#           $(MAKE) ARCH=intel obj/intel/libc.a
#           $(LIBS_SCRIPT) intel
#   libs-fixtures:
#           tools/elf-dyn/gen_so_fixtures.sh
#           tools/elf-dyn/gen_so_fixtures.sh --check
#
# (obj/<arch>/libc.a is only a *baseline* for the size/evidence comparison and
# for `gen_map`; the .so build itself does not read it.  `make libs` must not
# become a prerequisite of `all`, so nothing links against LIBC.SO until the
# pilot conversion in §5.)
# ---------------------------------------------------------------------------

set -uo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 1

# Tool defaults must exist on both host families: Linux ships GNU `readelf`
# and `nm`, while macOS has no `readelf` at all and its `nm` is Apple's (no
# `-D`, no `--defined-only`) -- the mac CI failure was
# `tools/build_libs.sh: line 611: readelf: command not found` followed by a
# wall of FAIL gates.  Prefer the llvm-* twins when they are installed;
# they accept every flag this script passes and read the same ELF.
pick_tool() {
  local t
  for t in "$@"; do
    if command -v "$t" >/dev/null 2>&1; then printf '%s' "$t"; return 0; fi
  done
  printf '%s' "$1"
}
# File size in bytes, portable: BSD `stat` has no -c (macOS) and `wc -c`
# pads its count with spaces there.
fsize() { wc -c < "$1" | tr -d ' '; }
CC=${CC:-clang}
LD=${LD:-$(pick_tool ld.lld ld.lld-21)}
# Versioned LLVM names are the norm on Debian/Ubuntu packaging (llvm-nm-21 with
# no bare `llvm-nm`), and GNU `nm` is not a substitute: the evidence gates parse
# llvm-nm's output and count 0 globals against GNU's format.
NM=${NM:-$(pick_tool llvm-nm llvm-nm-21 llvm-nm-20 llvm-nm-19 llvm-nm-18 nm)}
READELF=${READELF:-$(pick_tool llvm-readelf llvm-readelf-21 llvm-readelf-20 llvm-readelf-19 llvm-readelf-18 readelf)}
# A caller (the Makefile) may pass a bare name that is not installed - e.g.
# LLVM_NM=llvm-nm on a distro that only ships llvm-nm-21.  Validate whatever we
# were handed instead of trusting it, or the evidence gates silently read
# nothing and fail with a misleading "0 defined globals".
command -v "$NM" >/dev/null 2>&1 || NM=$(pick_tool llvm-nm llvm-nm-21 llvm-nm-20 llvm-nm-19 llvm-nm-18 nm)
command -v "$READELF" >/dev/null 2>&1 || READELF=$(pick_tool llvm-readelf llvm-readelf-21 llvm-readelf-20 llvm-readelf-19 llvm-readelf-18 readelf)
JOBS=${JOBS:-16}

LIBC_MAP=src/libc/libc.map
LINKER_SO=src/libc/linker_so.ld
CLANG_RESOURCE_INCLUDE=$("$CC" -print-resource-dir 2>/dev/null)/include

# compiler-rt binary128 builtins: libc.a carries exactly these 17 (3 from
# LIBC_BUILTIN_SRCS + 14 from LIBC_X11EXT_BUILTIN_SRCS, Makefile ~1311).  On
# aarch64 `long double` is IEEE quad, so strtod/stdio/math_ext really do call
# them; the DSO must self-contain them or the interpreter would have to
# resolve __divtf3 out of thin air.  x86_64 lowers long double to x87 and the
# objects are simply unreferenced there.
BUILTIN_SRCS=(
  extenddftf2.c trunctfdf2.c trunctfsf2.c
  addtf3.c subtf3.c multf3.c divtf3.c comparetf2.c extendsftf2.c
  floatsitf.c floatunsitf.c floatditf.c floatunditf.c
  fixtfsi.c fixtfdi.c fixunstfsi.c fixunstfdi.c
  # fp_mode.c is not a libc.a member (an archive pulls members lazily, and no
  # libc TU ever pulled the fix* objects that reference these), but a DSO
  # links every object eagerly: __fixtfdi/__fixunstfdi call __fe_getround and
  # __fe_raise_inexact, so the two tiny helpers must come along or ld.lld -z
  # defs rejects the SO.  Same pinned vendor tree, same target flags.
  fp_mode.c
)
BUILTINS_DIR=third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/compiler-rt/lib/builtins

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
die()  { printf 'build_libs: error: %s\n' "$*" >&2; exit 1; }
say()  { printf 'build_libs: %s\n' "$*" >&2; }
note() { printf 'build_libs:   %s\n' "$*" >&2; }

cc_cmd=("$CC")
if [ "${HOBBYOS_USE_CCACHE:-0}" = "1" ]; then
  if command -v ccache >/dev/null 2>&1; then
    cc_cmd=(ccache "$CC")
    note "ccache: on ($(ccache --version | head -1))"
  elif [ -x /usr/lib/ccache/"$(basename "$CC")" ]; then
    cc_cmd=(/usr/lib/ccache/"$(basename "$CC")")
    note "ccache: on (/usr/lib/ccache)"
  else
    note "HOBBYOS_USE_CCACHE=1 but no ccache on this host — compiling directly"
  fi
fi

arch_check() {
  case ${1:-} in
    arm|intel) ;;
    *) die "unknown arch '${1:-}' (expected arm|intel)" ;;
  esac
}

# configure_arch <arch> — sets $TARGET, $PLATFORM_FLAGS, $OBJ_PIC, $SO_OUT
configure_arch() {
  local arch=$1
  arch_check "$arch"
  case $arch in
    arm)
      TARGET=aarch64-none-elf
      # Makefile arm USER_CFLAGS verbatim (no -nostdinc: an aarch64-none-elf
      # triple never picks up host include dirs, so the search is hermetic
      # already — keep the two flavours byte-identical to the Makefile).
      PLATFORM_FLAGS=(
        -O2 -Wall -Wextra -g
        -Isrc/user_include -Isrc/user_include/graphics
        -Isrc/include -Isrc/libc/include
        --target="$TARGET" -ffreestanding -mcpu=cortex-a53
      )
      ;;
    intel)
      TARGET=x86_64-none-elf
      # Makefile intel USER_CFLAGS verbatim, INCLUDING the hermetic include
      # path (-nostdinc + resource-dir -isystem) and -mcmodel=large.
      PLATFORM_FLAGS=(
        -O2 -Wall -Wextra -g
        -Isrc/user_include -Isrc/user_include/graphics
        -Isrc/include -Isrc/libc/include
        --target="$TARGET" -ffreestanding -mno-red-zone
        -nostdinc -isystem "$CLANG_RESOURCE_INCLUDE"
        -mcmodel=large
      )
      ;;
  esac
  PIC_FLAGS=(-fPIC -ftls-model=initial-exec)
  OBJ_PIC=obj/$arch/pic
  SO_OUT=obj/$arch/libc.so
  ARCH=$arch
}

# ---------------------------------------------------------------------------
# job pool: launch <cmd...>; at most $JOBS in flight, failures latched
#
# `wait -n` is bash >= 4.3.  macOS ships bash 3.2 as /bin/bash, where it is
# `wait: -n: invalid option`: the pool reaped nothing, so every job past the
# first $JOBS was orphaned and the run died on `make: *** [obj/<arch>/libc.so]
# Error 1` (the mac CI gate).  Feature-detect once; on old bash the pool
# blocks on the oldest queued pid instead — same cap, same failure latch.
# ---------------------------------------------------------------------------
pool_running=0
pool_failed=0
pool_pids=""            # FIFO of live child pids, only used by the 3.2 path

pool_have_wait_n=0
if [ "${BASH_VERSINFO[0]:-0}" -gt 4 ] ||
   { [ "${BASH_VERSINFO[0]:-0}" -eq 4 ] && [ "${BASH_VERSINFO[1]:-0}" -ge 3 ]; }; then
  pool_have_wait_n=1
fi

# pool_reap: one job leaves the pool; latch a failure.  Always returns 0.
pool_reap() {
  if [ "$pool_have_wait_n" = 1 ]; then
    if ! wait -n; then pool_failed=1; fi
    pool_running=$((pool_running - 1))
    return 0
  fi
  local p=${pool_pids%% *}
  if [ -z "$p" ]; then          # bookkeeping lost: nothing to wait for
    pool_running=0
    return 0
  fi
  case $pool_pids in
    *" "*) pool_pids=${pool_pids#* } ;;
    *)     pool_pids="" ;;
  esac
  if ! wait "$p"; then pool_failed=1; fi
  pool_running=$((pool_running - 1))
  return 0
}

launch() {
  "$@" &
  [ "$pool_have_wait_n" = 1 ] || pool_pids="${pool_pids:+$pool_pids }$!"
  pool_running=$((pool_running + 1))
  if [ "$pool_running" -ge "$JOBS" ]; then
    pool_reap
  fi
}
pool_drain() {
  while [ "$pool_running" -gt 0 ]; do
    pool_reap
  done
}

# ---------------------------------------------------------------------------
# compile
# ---------------------------------------------------------------------------
# compile_tu_inner <src> <out> [extra flags...] — the actual compiler call
#
# Empty arrays are expanded as ${a[@]+"${a[@]}"} on purpose: before bash 4.4
# `set -u` makes `"${a[@]}"` on an empty array a fatal "a[@]: unbound
# variable" *before* the command runs.  macOS /bin/bash is 3.2, where that
# killed all 60 compiles in the mac CI gate with no compiler in the loop.
compile_tu_inner() {
  local src=$1 out=$2
  shift 2
  mkdir -p "$(dirname "$out")"
  local lang_flags=()
  case $src in
    *.cpp) lang_flags=(-std=c++17 -fno-exceptions -fno-rtti -nostdinc++) ;;
  esac
  local ffp=()
  case $src in
    */math.c) ffp=(-ffp-contract=off) ;;
  esac
  "${cc_cmd[@]}" "${PLATFORM_FLAGS[@]}" "${PIC_FLAGS[@]}" \
    ${lang_flags[@]+"${lang_flags[@]}"} ${ffp[@]+"${ffp[@]}"} \
    "$@" -c "$src" -o "$out"
}

# compile_tu <src> <out> [extra flags...] — the pool entry point: one log per
# TU, and a *named* failure.  "A TU that cannot be built -fPIC must be reported
# by name, not silently dropped" (lane C brief), so a failure prints the TU and
# the tail of its log instead of vanishing into interleaved -j output.
compile_tu() {
  local src=$1 out=$2
  shift 2
  local log="$OBJ_PIC/log/$(printf '%s' "$src" | tr '/' '_').log"
  mkdir -p "$OBJ_PIC/log"
  if ! compile_tu_inner "$src" "$out" "$@" >"$log" 2>&1; then
    { printf 'build_libs: TU FAILED: %s\n' "$src"
      printf 'build_libs:   log: %s\n' "$log"
      sed -n '1,40p' "$log" | sed 's/^/build_libs:   | /'
    } >&2
    return 1
  fi
  return 0
}

# obj name: keep the origin visible so two TUs with the same basename can
# never collide (libc/src/<x>.c vs user/<x>.c).
pic_obj_for() {
  local src=$1
  local base
  base=$(basename "${src%.*}")
  case $src in
    src/libc/src/*) echo "$OBJ_PIC/libsrc/$base.o" ;;
    src/user/*)     echo "$OBJ_PIC/usersrc/$base.o" ;;
    *)              echo "$OBJ_PIC/other/$base.o" ;;
  esac
}

# ---------------------------------------------------------------------------
# gen_map: src/libc/libc.map from the *archives*
# ---------------------------------------------------------------------------
# A symbol that must never be exported from a DSO even though the archive
# defines it: crt0.o's entry point (crt0 is not in the SO at all, but the
# archive is the map's source, so filter it here).
MAP_DENY='^(|_start|_init|_fini|__tls_start|__tls_end|__tls_data_end|__tls_align|__tls_meta_align|__tls_size|__tls_tcb_head|_end|end|__bss_start|__init_array_start|__init_array_end|__fini_array_start|__fini_array_end)$'

gen_map() {
  mkdir -p "$(dirname "$LIBC_MAP")"
  local archives=() a tmp
  for a in arm intel; do
    [ -f "obj/$a/libc.a" ] && archives+=("obj/$a/libc.a")
  done
  [ "${#archives[@]}" -gt 0 ] || die "gen_map: no obj/<arch>/libc.a found (build the archive first: make ARCH=<arch> obj/<arch>/libc.a)"

  tmp=$(mktemp)
  # Each arch contributes its own global set (they differ: aarch64 pulls the
  # binary128 compiler-rt builtins, x86_64 does not); the map is the UNION so
  # one committed file serves both flavours.  Sorted with LC_ALL=C so the file
  # is byte-reproducible.
  for a in "${archives[@]}"; do
    "$NM" -g --defined-only "$a" 2>/dev/null | awk 'NF==3 {print $3}'
  done | LC_ALL=C sort -u | grep -Ev "$MAP_DENY" > "$tmp"
  local count
  count=$(wc -l < "$tmp")

  {
    cat <<EOF
/* src/libc/libc.map — the exported symbol set of LIBC.SO (lane C).
 *
 * Generated file: do not hand-edit the list.  Regenerate with
 *
 *     tools/build_libs.sh gen_map           # needs obj/<arch>/libc.a
 *
 * which takes \`nm -g --defined-only\` over BOTH archives (obj/arm/libc.a and
 * obj/intel/libc.a) and unions them, because one map serves both flavours
 * and aarch64 defines the binary128 compiler-rt builtins that x86_64 does
 * not.  This snapshot: ${count} symbols.
 *
 * The block below is an ANONYMOUS version node on purpose: it pins the
 * exported set and hides everything else (\`local: *\`), which is what
 * docs/shared-libs.md §2 freezes, while adding no VERDEF/VERNEED records —
 * §1.4 says v1 has no symbol versioning, and lane D's resolver must never
 * have to be version-aware.  Flipping to real versioning later is a one-line
 * change (name the node, e.g. \`LIBC_1.0 { ... }\`), and would then require
 * lane D to read .gnu.version.
 *
 * Everything not listed — including the linker-script TLS metadata
 * (__tls_start/__tls_data_end/__tls_end/__tls_align/__tls_meta_align, see
 * src/libc/linker_so.ld), the archive's internal helpers and _start — stays
 * local to the DSO.
 */
{
  global:
EOF
    sed 's/^/    /; s/$/;/' "$tmp"
    cat <<'EOF'
  local:
    *;
};
EOF
  } > "$LIBC_MAP"
  rm -f "$tmp"
  say "gen_map: wrote $LIBC_MAP ($count symbols)"
}

# check_map: the archive's global set vs the committed map (drift alarm).
check_map() {
  [ -f "$LIBC_MAP" ] || die "check_map: $LIBC_MAP missing (run: tools/build_libs.sh gen_map)"
  local tmp missing extra
  tmp=$(mktemp)
  for a in arm intel; do
    [ -f "obj/$a/libc.a" ] && "$NM" -g --defined-only "obj/$a/libc.a" 2>/dev/null | awk 'NF==3 {print $3}'
  done | LC_ALL=C sort -u | grep -Ev "$MAP_DENY" > "$tmp"
  missing=$(comm -23 <(LC_ALL=C sort -u "$tmp") <(grep -E '^    [A-Za-z_.].*;$' "$LIBC_MAP" | sed 's/^ *//; s/;$//' | LC_ALL=C sort -u) | wc -l)
  extra=$(comm -13 <(LC_ALL=C sort -u "$tmp") <(grep -E '^    [A-Za-z_.].*;$' "$LIBC_MAP" | sed 's/^ *//; s/;$//' | LC_ALL=C sort -u) | wc -l)
  rm -f "$tmp"
  if [ "$missing" -ne 0 ] || [ "$extra" -ne 0 ]; then
    say "check_map: WARNING — libc.map drifts from the archives: $missing archive symbols unlisted, $extra listed symbols not in any archive (rerun: tools/build_libs.sh gen_map)"
    return 1
  fi
  say "check_map: libc.map matches the archives"
  return 0
}

# ---------------------------------------------------------------------------
# build_libc <arch>
# ---------------------------------------------------------------------------
build_libc() {
  local arch=$1
  configure_arch "$arch"

  [ -f "$LIBC_MAP" ] || die "$LIBC_MAP missing (run: tools/build_libs.sh gen_map)"
  [ -f "$LINKER_SO" ] || die "$LINKER_SO missing"

  local -a srcs=() builtins=() fragments=()
  local f
  # Two glibc-derived TUs are translation-unit *fragments*, not TUs: regex.c
  # #includes regex_internal.c, regcomp.c and regexec.c into one object
  # (libc_regex.o, Makefile line ~1317).  Compiling them standalone fails, so
  # detect the pattern instead of hardcoding a list: any *_*.c that another
  # source in the same directory #includes is a fragment and is skipped (it
  # still reaches the DSO through its parent TU).
  local cand
  for cand in src/libc/src/*.c; do
    local b
    b=$(basename "$cand")
    if grep -lE "^[[:space:]]*#[[:space:]]*include[[:space:]]+\"${b//./\\.}\"" \
         src/libc/src/*.c src/libc/src/*.cpp 2>/dev/null | grep -qv "^${cand}$"; then
      fragments+=("$cand")
    fi
  done
  for f in src/libc/src/*.c src/libc/src/*.cpp src/user/libc.c src/user/malloc.c; do
    [ -f "$f" ] || continue
    case $(basename "$f") in
      crt0.c) continue ;;   # defines _start: never in a DSO
    esac
    local skip=0 frag
    for frag in "${fragments[@]:-}"; do [ "$frag" = "$f" ] && skip=1; done
    [ "$skip" -eq 1 ] && continue
    srcs+=("$f")
  done

  # compiler-rt builtins (see BUILTIN_SRCS comment)
  if [ -d "$BUILTINS_DIR" ]; then
    for f in "${BUILTIN_SRCS[@]}"; do
      [ -f "$BUILTINS_DIR/$f" ] && builtins+=("$BUILTINS_DIR/$f")
    done
  fi
  if [ "${#builtins[@]}" -ne "${#BUILTIN_SRCS[@]}" ]; then
    die "compiler-rt builtins missing under $BUILTINS_DIR — libc.a carries ${#BUILTIN_SRCS[@]} of them and aarch64 libc TUs call them (__divtf3, __extenddftf2, ...). Fetch the pinned vendor tree: bash third_party/libcxx-21.1.8/fetch.sh"
  fi

  say "== $arch: compiling ${#srcs[@]} libc TUs + ${#builtins[@]} compiler-rt builtins (-fPIC, -ftls-model=initial-exec, -j$JOBS)"
  [ "${#fragments[@]}" -gt 0 ] && note "TU fragments (reached through their parent TU, not compiled standalone): ${fragments[*]}"
  [ -f "obj/$arch/libc.a" ] && note "libc.a baseline: $(fsize "obj/$arch/libc.a") bytes"
  rm -rf "$OBJ_PIC"
  mkdir -p "$OBJ_PIC/libsrc" "$OBJ_PIC/usersrc" "$OBJ_PIC/builtins"

  pool_running=0
  pool_failed=0
  local -a objs=()
  for f in "${srcs[@]}"; do
    local o
    o=$(pic_obj_for "$f")
    objs+=("$o")
    launch compile_tu "$f" "$o"
  done
  for f in "${builtins[@]}"; do
    local o="$OBJ_PIC/builtins/$(basename "${f%.c}").o"
    objs+=("$o")
    launch compile_tu "$f" "$o"
  done
  pool_drain
  [ "$pool_failed" -eq 0 ] || die "$arch: PIC compilation failed (see compiler output above)"

  # --- guard: a DSO must not contain a program entry point
  local bad=""
  for o in "${objs[@]}"; do
    if grep -qx '_start' <<<"$("$NM" --defined-only "$o" 2>/dev/null | awk '{print $NF}')"; then
      bad="$bad ${o#$OBJ_PIC/}"
    fi
  done
  [ -z "$bad" ] || die "$arch: these objects define _start and must not be in libc.so:$bad"

  # --- link
  local strip=()
  [ "${LIBC_SO_DEBUG:-0}" = "1" ] || strip=(--strip-debug)
  say "== $arch: linking $SO_OUT"
  # --undefined-version: src/libc/libc.map is the UNION of both arches' global
  # sets (aarch64 defines the binary128 compiler-rt builtins, x86_64 does not),
  # so a handful of listed names are legitimately absent from each link.  lld
  # reports those as errors by default; this flag keeps the single committed
  # map authoritative for both flavours.  Anything genuinely missing shows up
  # in check_map and in the .dynsym count of the evidence file.
  # -z text turns "no TEXTREL" from a post-link assertion into a link-time
  # error: any relocation that would force a write to a PT_LOAD mapped read-only
  # (or R+X) fails the link instead of shipping a self-modifying image.
  # -z separate-loadable-segments: give every PT_LOAD its own page range.
  # Without it lld packs the segments contiguously (p_vaddr == p_offset, no
  # page alignment), so R, R+X and RW segments share pages — and the
  # interpreter maps each segment at page_down(p_vaddr) with its own file
  # offset, which is only sound when a page belongs to one segment.  Measured:
  # the first pilot boot refused LIBC.SO with "PT_LOAD segments share a page".
  "$LD" -shared --soname=LIBC.SO -T "$LINKER_SO" --version-script "$LIBC_MAP" \
    --undefined-version \
    -z relro -z now -z defs -z noexecstack -z text --hash-style=both \
    -z separate-loadable-segments \
    ${strip[@]+"${strip[@]}"} \
    -o "$SO_OUT" "${objs[@]}" || die "$arch: ld.lld -shared failed (see above)"

  [ -f "$SO_OUT" ] || die "$arch: linker produced no $SO_OUT"
  emit_evidence "$arch"
  say "== $arch: done — $SO_OUT ($(fsize "$SO_OUT") bytes)"
}

# ---------------------------------------------------------------------------
# evidence
# ---------------------------------------------------------------------------
emit_evidence() {
  local arch=$1
  configure_arch "$arch"
  local ev="obj/$arch/libc.so.evidence.txt"
  local so=$SO_OUT
  local dyn_defined dyn_total undefs soname textrel
  local readelf_ver
  readelf_ver=$("$READELF" --version 2>/dev/null | head -1)

  dyn_defined=$("$READELF" --dyn-syms -W "$so" | awk '$7!="UND" && $4=="FUNC"||$7!="UND" && $4=="OBJECT"||$7!="UND" && $4=="IFUNC"||$7!="UND" && $4=="TLS"' | grep -c '^ *[0-9]*:' || true)
  dyn_total=$("$READELF" --dyn-syms -W "$so" | grep -cE '^ +[0-9]+: [0-9a-f]+' || true)
  undefs=$("$READELF" --dyn-syms -W "$so" | awk '$7=="UND" {print $8}' | grep -v '^$' | LC_ALL=C sort -u)
  soname=$("$READELF" -d -W "$so" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')
  if grep -q 'TEXTREL' <<<"$("$READELF" -d -W "$so")"; then textrel=YES; else textrel=no; fi

  {
    echo "# libc.so evidence — arch=$arch target=$TARGET"
    echo "# generated by tools/build_libs.sh (lane C, docs/shared-libs.md §4)"
    echo "# host: $(uname -srm)"
    echo "# cc:   $("$CC" --version | head -1)"
    echo "# ld:   $("$LD" --version | head -1)"
    echo "# re:   $readelf_ver"
    echo "# utc:  $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo
    echo "== artifact =="
    echo "path   $so"
    echo "size   $(fsize "$so") bytes"
    echo "sha256 $(sha256sum "$so" | cut -d' ' -f1)"
    if [ -f "obj/$arch/libc.a" ]; then
      local asz ssz
      asz=$(fsize "obj/$arch/libc.a")
      ssz=$(fsize "$so")
      echo "libc.a $asz bytes (archive flavour, unchanged: this script only reads it)"
      echo "ratio  libc.so/libc.a = $((ssz * 100 / asz))%"
    else
      echo "libc.a (not built in this worktree: run make ARCH=$arch obj/$arch/libc.a)"
    fi
    [ "${LIBC_SO_DEBUG:-0}" = "1" ] && echo "debug  DWARF kept (LIBC_SO_DEBUG=1)" || echo "debug  DWARF stripped (--strip-debug; .symtab/.dynsym kept)"
    echo
    echo "== readelf -h =="
    "$READELF" -h -W "$so"
    echo
    echo "== readelf -l =="
    "$READELF" -l -W "$so"
    echo
    echo "-- mapping congruence: LOAD p_vaddr vs p_offset (equal ⇒ lane D can mmap"
    echo "   the whole file at a 16 MiB slot base and every segment lands at p_vaddr)"
    {
      local nload=0 nok=0 moff mva
      while read -r moff mva; do
        nload=$((nload + 1))
        if [ $((moff)) -eq $((mva)) ]; then
          nok=$((nok + 1))
        else
          echo "   MISMATCH: p_offset=$moff p_vaddr=$mva (page-congruent only)"
        fi
      done < <("$READELF" -l -W "$so" | awk '$1=="LOAD"{print $2, $3}')
      echo "   $nok/$nload LOAD segments have p_vaddr == p_offset"
    }
    echo
    echo "== readelf -d =="
    "$READELF" -d -W "$so"
    echo
    echo "== gates =="
    echo "SONAME          ${soname:-<none>}    $([ "$soname" = "LIBC.SO" ] && echo OK || echo FAIL)"
    echo "TEXTREL         $textrel    $([ "$textrel" = no ] && echo OK || echo FAIL)"
    echo "dynsym entries  $dyn_total total, $dyn_defined defined FUNC/OBJECT/TLS"
    echo "map entries     $(grep -cE '^    [A-Za-z_.].*;$' "$LIBC_MAP")"
    echo
    echo "== versioning =="
    echo "(anonymous version node: the export set is pinned, no VERDEF should appear)"
    "$READELF" -V -W "$so" | sed -n '1,40p'
    echo
    echo "== undefined dynamic symbols =="
    if [ -z "$undefs" ]; then echo "(none — libc.so is self-contained)"; else echo "$undefs"; fi
    echo
    echo "== relocations by type (readelf -r) =="
    "$READELF" -r -W "$so" | awk '/R_[A-Z0-9_]+/ {for (i=1;i<=NF;i++) if ($i ~ /^R_[A-Z0-9_]+$/) {print $i; break}}' | LC_ALL=C sort | uniq -c | sort -rn
    echo
    echo "-- full readelf -r --"
    "$READELF" -r -W "$so"
    echo
    echo "== TLS =="
    "$READELF" -l -W "$so" | grep -A1 'TLS' || echo "(no PT_TLS!)"
    echo "-- TLS-related symbols in the image (readelf -sW: nm drops NOTYPE locals that sit at a zero-sized section start) --"
    "$READELF" -sW "$so" | grep -E '__tls_|tp_self|errno_storage' || true
    echo "-- TLS section headers --"
    "$READELF" -S -W "$so" | grep -E '\.tdata|\.tbss|\.tls_meta' || true
    echo "-- initial-exec TLS dynamic relocations (lane D: add module offset) --"
    "$READELF" -rW "$so" | grep -E 'TLS_(TPREL|TPOFF)' || echo "(none)"
    echo
    echo "== dynamic flags worth knowing for the interpreter =="
    "$READELF" -dW "$so" | grep -E 'FLAGS|BIND_NOW|RELACOUNT|JMPREL|PLTRELSZ|TEXTREL' || true
    echo "DF_STATIC_TLS in DT_FLAGS is lld's marker that this DSO uses initial-exec TLS"
    echo "and must land inside the per-thread static block (§1.3)."
    echo
    echo "== relocation types the interpreter must apply to this DSO =="
    "$READELF" -rW "$so" | awk '/R_[A-Z0-9_]+/ {for (i=1;i<=NF;i++) if ($i ~ /^R_[A-Z0-9_]+$/) {print $i; break}}' | LC_ALL=C sort -u
    echo
    echo "== exported dynamic symbols =="
    echo "(--dyn-syms, defined only; also written to obj/$arch/libc.so.map)"
    "$READELF" --dyn-syms -W "$so" | awk 'NR<=3 || $7!="UND"'
  } > "$ev" 2>&1

  # §4 lane C asks for obj/<arch>/libc.so.map as the readelf evidence; keep a
  # compact dynsym listing there.
  {
    echo "# obj/$arch/libc.so.map — defined dynamic symbols of LIBC.SO"
    echo "# regenerate: tools/build_libs.sh $arch"
    "$NM" -D --defined-only "$so" | LC_ALL=C sort -k3
  } > "obj/$arch/libc.so.map" 2>&1

  # --- hard gates: fail the script, do not just print, when a contract breaks
  # (herestrings, not pipes: `set -o pipefail` turns `readelf | grep -q` into a
  # false failure the moment grep -q exits early and readelf takes SIGPIPE)
  local rc=0 elf_l elf_s
  elf_l=$("$READELF" -l -W "$so")
  elf_s=$("$READELF" -sW "$so")
  [ "$soname" = "LIBC.SO" ] || { say "$arch: FAIL soname is '${soname:-<none>}', expected LIBC.SO"; rc=1; }
  [ "$textrel" = no ] || { say "$arch: FAIL TEXTREL present"; rc=1; }
  local nglob
  nglob=$("$NM" -D --defined-only "$so" | awk 'NF==3 && $2 ~ /^[TWDBRVGS]$/' | wc -l)
  [ "$nglob" -ge 400 ] || { say "$arch: FAIL only $nglob defined globals in .dynsym (gate: >= 400)"; rc=1; }
  grep -q 'TLS' <<<"$elf_l" || { say "$arch: FAIL no PT_TLS segment"; rc=1; }
  grep -qE '[[:space:]]__tls_start$' <<<"$elf_s" || { say "$arch: FAIL __tls_start missing from the image"; rc=1; }
  grep -qE '[[:space:]]__tls_end$' <<<"$elf_s" || { say "$arch: FAIL __tls_end missing from the image"; rc=1; }
  grep -qE '[[:space:]]__tls_data_end$' <<<"$elf_s" || { say "$arch: FAIL __tls_data_end missing from the image"; rc=1; }
  grep -qE '[[:space:]]__tls_meta_align$' <<<"$elf_s" || { say "$arch: FAIL __tls_meta_align missing from the image"; rc=1; }
  [ "$rc" -eq 0 ] || die "$arch: evidence gates failed (see $ev)"
  say "$arch: gates OK — SONAME=LIBC.SO, no TEXTREL, $nglob defined globals, PT_TLS + __tls_* present"
}

# ---------------------------------------------------------------------------
# link_probe <arch> — prove LIBC.SO is *linkable*, not just loadable
# ---------------------------------------------------------------------------
# Builds a throwaway ET_DYN executable that imports strlen/malloc/free/strcmp
# from obj/<arch>/libc.so and links it exactly the way the §5 pilot will:
#
#   clang <user flags> -fPIE -c probe.c -o probe.o
#   ld.lld -pie --no-dynamic-linker -e _start -z relro -z now -z defs \
#           -z noexecstack --hash-style=both -o APP.ELF probe.o libc.so
#
# Passing proves three things at once: the .dynsym of LIBC.SO is usable as a
# link input (correct type/size/binding on every exported symbol), the
# DT_SONAME produces a `DT_NEEDED LIBC.SO` entry, and the result is an ET_DYN
# with a real entry point — the shape the kernel + interpreter expect.
link_probe() {
  local arch=$1
  configure_arch "$arch"
  [ -f "$SO_OUT" ] || die "link_probe: $SO_OUT missing (run: tools/build_libs.sh $arch)"
  local dir="obj/$arch/probe"
  mkdir -p "$dir"
  cat > "$dir/link_probe.c" <<'EOF'
/* generated by tools/build_libs.sh link_probe — not part of the tree */
typedef unsigned long hb_size_t;
extern hb_size_t strlen(const char *);
extern void *malloc(hb_size_t);
extern void free(void *);
extern int strcmp(const char *, const char *);
void _start(void) {
  char *p = (char *)malloc(32);
  if (!p)
    for (;;) {
    }
  p[0] = 0;
  if (strlen(p) == 0 && strcmp(p, "") == 0)
    free(p);
  for (;;) {
  }
}
EOF
  "${cc_cmd[@]}" "${PLATFORM_FLAGS[@]}" -fPIE -c "$dir/link_probe.c" -o "$dir/link_probe.o" \
    || die "$arch: link_probe compile failed"
  "$LD" -pie --no-dynamic-linker -e _start -z relro -z now -z defs -z noexecstack \
    --hash-style=both -o "$dir/APP.ELF" "$dir/link_probe.o" "$SO_OUT" \
    || die "$arch: link_probe could not link a program against $SO_OUT"

  local elf_h elf_d rc=0
  elf_h=$("$READELF" -h -W "$dir/APP.ELF")
  elf_d=$("$READELF" -d -W "$dir/APP.ELF")
  grep -q 'NEEDED.*LIBC.SO' <<<"$elf_d" || { say "$arch: FAIL link_probe has no DT_NEEDED LIBC.SO"; rc=1; }
  grep -qE 'Entry point 0x0+$' <<<"$elf_h" && { say "$arch: FAIL link_probe entry point is 0"; rc=1; }
  # Undefined symbols in the *executable* are the imports the interpreter is
  # supposed to resolve out of LIBC.SO; list them so the evidence shows what
  # was actually imported rather than leaving it implicit.
  say "$arch: link_probe imports from LIBC.SO: $("$READELF" --dyn-syms -W "$dir/APP.ELF" | awk '$7=="UND" && $8!="" {printf "%s ", $8}')"
  {
    echo "# link_probe — a program linked against obj/$arch/libc.so"
    echo "# (the §5 pilot shape: ET_DYN executable + DT_NEEDED LIBC.SO)"
    echo "== readelf -h =="; "$READELF" -h -W "$dir/APP.ELF"
    echo; echo "== readelf -d =="; "$READELF" -d -W "$dir/APP.ELF"
    echo; echo "== imports =="; "$READELF" --dyn-syms -W "$dir/APP.ELF"
    echo; echo "== relocations =="; "$READELF" -r -W "$dir/APP.ELF"
  } > "obj/$arch/libc.so.link_probe.txt" 2>&1
  [ "$rc" -eq 0 ] || die "$arch: link_probe gates failed (see obj/$arch/libc.so.link_probe.txt)"
  say "$arch: link_probe OK — APP.ELF links against LIBC.SO, DT_NEEDED LIBC.SO, ET_DYN with entry"
}

# ---------------------------------------------------------------------------
# build_interp <arch> — LDHOBBY.SO from lane D's sources (src/user/ld/)
# ---------------------------------------------------------------------------
# The interpreter is an ET_DYN *static PIE*: the kernel maps it into a
# USER_LIB slot and jumps to interp_base + e_entry, so it may only contain
# R_*_RELATIVE relocations and must not depend on any other module.  Exact
# command block (this function is the executable form of it):
#
#   clang <USER_CFLAGS for arch> -fPIE -c src/user/ld/<each>.c -o obj/<arch>/ld/<each>.o
#   ld.lld -pie --no-dynamic-linker -e _start -z relro -z now -z defs \
#           -z noexecstack --hash-style=both -o obj/<arch>/LDHOBBY.SO <objs>
#
build_interp() {
  local arch=$1 mode=${2:-run}
  configure_arch "$arch"
  local srcdir=src/user/ld
  local -a srcs=()
  local f
  for f in "$srcdir"/*.c; do
    # host_test.c is the HOST-side harness (it includes <sys/syscall.h> and
    # is driven by tools/ldso_host_test.sh); it must never enter the target
    # link.  Same for any *_test.c that lands in the directory later.
    case "$f" in */host_test.c|*_test.c) continue ;; esac
    [ -f "$f" ] && srcs+=("$f")
  done
  if [ "${#srcs[@]}" -eq 0 ]; then
    say "build_interp: lane D sources are not in this tree yet ($srcdir/*.c)."
    say "  expected: ldso.c elf_dyn.c mmap_io.c (docs/shared-libs.md §4 lane D)."
    say "  The command block to run once they exist is:"
    interp_cmd_text "$arch" | sed 's/^/    /' >&2
    die "build_interp: nothing to build"
  fi
  [ -f "$srcdir/ldso.c" ] || say "build_interp: WARNING — $srcdir/ldso.c not found, building what exists"

  local out_dir=obj/$arch/ld
  mkdir -p "$out_dir"
  local -a objs=()
  pool_running=0
  pool_failed=0
  for f in "${srcs[@]}"; do
    local o="$out_dir/$(basename "${f%.c}").o"
    objs+=("$o")
    launch compile_interp_tu "$f" "$o"
  done
  pool_drain
  [ "$pool_failed" -eq 0 ] || die "$arch: interpreter compilation failed"

  local interp=obj/$arch/LDHOBBY.SO
  "$LD" -pie --no-dynamic-linker -e _start -z relro -z now -z defs \
    -z noexecstack --hash-style=both --strip-debug \
    -o "$interp" "${objs[@]}" || die "$arch: LDHOBBY.SO link failed"

  # --- asserts: static PIE, RELATIVE-only, no DT_NEEDED
  local rt needed rc=0
  needed=$("$READELF" -d -W "$interp" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p')
  rt=$("$READELF" -r -W "$interp" | awk '/^R_/ {print $3}' | grep -v '^R_[A-Z0-9_]*_RELATIVE$' | LC_ALL=C sort -u)
  if [ -n "$needed" ]; then say "$arch: FAIL LDHOBBY.SO has DT_NEEDED: $needed"; rc=1; fi
  if [ -n "$rt" ]; then say "$arch: FAIL LDHOBBY.SO has non-RELATIVE dynamic relocations: $rt"; rc=1; fi
  # e_type must be DYN (3).  Match the TYPE FIELD, not readelf's human
  # description: lld labels a -pie output "DYN (Position-Independent
  # Executable file)" and a -shared one "DYN (Shared object file)", and the
  # interpreter is legitimately either.
  # Herestring, not a pipe: same trap as line ~632.  `grep -q` exits on the
  # match and readelf then dies of SIGPIPE, which `set -o pipefail` reports as
  # 141 -- so this assert failed on a correctly linked ET_DYN interpreter
  # (measured on macOS under make, while passing when run by hand: it is a race
  # on readelf's exit, not on the file).
  grep -q 'Type:[[:space:]]*DYN' <<<"$("$READELF" -h -W "$interp")" \
    || { say "$arch: FAIL LDHOBBY.SO is not ET_DYN"; rc=1; }
  [ "$rc" -eq 0 ] || die "$arch: LDHOBBY.SO contract violated"

  {
    echo "# LDHOBBY.SO evidence — arch=$arch target=$TARGET"
    echo "# generated by tools/build_libs.sh build_interp $arch"
    echo "# utc: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "size $(fsize "$interp") bytes"
    echo
    echo "== readelf -h =="; "$READELF" -h -W "$interp"
    echo; echo "== readelf -d =="; "$READELF" -d -W "$interp"
    echo; echo "== readelf -l =="; "$READELF" -l -W "$interp"
    echo; echo "== relocations =="; "$READELF" -r -W "$interp"
    echo; echo "== gates =="
    echo "ET_DYN            OK"
    echo "DT_NEEDED         $( [ -n "$needed" ] && echo "$needed" || echo none )    OK"
    echo "reloc types       $( [ -n "$rt" ] && echo "$rt" || echo 'R_*_RELATIVE only' )"
  } > obj/$arch/LDHOBBY.SO.evidence.txt 2>&1
  say "$arch: LDHOBBY.SO built — $interp ($(fsize "$interp") bytes), RELATIVE-only, no DT_NEEDED"
}

compile_interp_tu() {
  local src=$1 out=$2
  mkdir -p "$(dirname "$out")"
  "${cc_cmd[@]}" "${PLATFORM_FLAGS[@]}" -fPIE -c "$src" -o "$out"
}

interp_cmd_text() {
  local arch=$1
  configure_arch "$arch"
  local flags
  flags=$(printf '%q ' "${PLATFORM_FLAGS[@]}")
  flags=${flags% }
  cat <<EOF
# $arch ($TARGET): ET_DYN static PIE, RELATIVE relocations only, no DT_NEEDED
$CC $flags -fPIE -c src/user/ld/ldso.c    -o obj/$arch/ld/ldso.o
$CC $flags -fPIE -c src/user/ld/elf_dyn.c -o obj/$arch/ld/elf_dyn.o
$CC $flags -fPIE -c src/user/ld/mmap_io.c -o obj/$arch/ld/mmap_io.o
$LD -pie --no-dynamic-linker -e _start -z relro -z now -z defs \\
    -z noexecstack --hash-style=both -o obj/$arch/LDHOBBY.SO \\
    obj/$arch/ld/ldso.o obj/$arch/ld/elf_dyn.o obj/$arch/ld/mmap_io.o
EOF
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
usage() {
  cat <<'EOF' >&2
tools/build_libs.sh — lane C (docs/shared-libs.md §4): PIC libc + LIBC.SO

  tools/build_libs.sh arm|intel   build obj/<arch>/libc.so + evidence
  tools/build_libs.sh all         both arches, arm first
  tools/build_libs.sh gen_map     regenerate src/libc/libc.map from the archives
  tools/build_libs.sh check_map   fail if libc.map drifts from the archives
  tools/build_libs.sh link_probe <arch|all>
                                  link a throwaway ET_DYN program against
                                  obj/<arch>/libc.so (proves it is linkable)
  tools/build_libs.sh build_interp <arch>
                                  build obj/<arch>/LDHOBBY.SO from src/user/ld/
                                  (fails with the command block if lane D's
                                  sources are not in the tree yet)
  tools/build_libs.sh print_interp_cmd <arch>
                                  print that command block only

Environment: JOBS=16, CC, LD, NM, READELF, HOBBYOS_USE_CCACHE=1,
LIBC_SO_DEBUG=1 (keep DWARF in libc.so; default is --strip-debug).

Outputs: obj/<arch>/libc.so, obj/<arch>/libc.so.evidence.txt,
obj/<arch>/libc.so.map, obj/<arch>/pic/ (objects + per-TU logs).
EOF
  exit 2
}

cmd=${1:-}
case $cmd in
  arm|intel)
    build_libc "$cmd"
    ;;
  all)
    build_libc arm
    build_libc intel
    ;;
  gen_map)
    gen_map
    check_map || true
    ;;
  check_map)
    check_map
    ;;
  link_probe)
    shift
    [ $# -ge 1 ] || die "usage: tools/build_libs.sh link_probe <arm|intel|all>"
    if [ "$1" = all ]; then link_probe arm; link_probe intel; else link_probe "$1"; fi
    ;;
  build_interp)
    shift
    [ $# -ge 1 ] || die "usage: tools/build_libs.sh build_interp <arm|intel>"
    build_interp "$1"
    ;;
  print_interp_cmd)
    shift
    [ $# -ge 1 ] || die "usage: tools/build_libs.sh print_interp_cmd <arm|intel>"
    interp_cmd_text "$1"
    ;;
  ""|-h|--help|help)
    usage
    ;;
  *)
    die "unknown command '$cmd' (see --help)"
    ;;
esac
