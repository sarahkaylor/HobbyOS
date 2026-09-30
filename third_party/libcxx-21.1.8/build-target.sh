#!/usr/bin/env bash
# build-target.sh -- build libc++ + libc++abi as ONE static archive for the
# HobbyOS bare-metal targets (browser.md §6 P3.1).
#
# Invoked by the HobbyOS Makefile (rule for $(OBJ_DIR)/libcxx.a); can also be
# run standalone for bring-up:
#
#   build-target.sh --arch arm|intel --objdir obj/arm/libcxx --out obj/arm/libcxx.a
#
# What it does:
#   1. verifies the pinned tree is fetched + overlaid (fetch.sh runs first if
#      the tree is missing);
#   2. generates an incremental makefile in <objdir>/ so objects rebuild only
#      when sources, flags or the site config change;
#   3. compiles every TU in sources.txt with the HobbyOS userland target
#      flags (-fno-exceptions -fno-rtti -std=c++23, C locale, P1 pthreads);
#   4. arch[ives] the objects into <out>.
#
# Exit is non-zero on any compile error; the caller sees the make output.
set -euo pipefail

VENDOR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${VENDOR_DIR}/src/llvm-project-21.1.8.src"
LIB="${SRC}/libcxx"
ABI="${SRC}/libcxxabi"
RT="${SRC}/compiler-rt/lib/builtins"

ARCH=""
OBJDIR=""
OUT=""
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

while [ $# -gt 0 ]; do
  case "$1" in
    --arch)   ARCH="$2"; shift 2 ;;
    --objdir) OBJDIR="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    -j)       JOBS="$2"; shift 2 ;;
    *) echo "usage: $0 --arch arm|intel --objdir DIR --out FILE" >&2; exit 2 ;;
  esac
done
[ -n "$ARCH" ] && [ -n "$OBJDIR" ] && [ -n "$OUT" ] || {
  echo "usage: $0 --arch arm|intel --objdir DIR --out FILE" >&2; exit 2; }

CXX="${CXX:-clang}"

# --- 1. tree present? ------------------------------------------------------
if [ ! -d "$LIB/include/__config_site" ] && [ ! -f "$LIB/include/__config_site" ]; then
  echo "[libcxx] pinned tree not fetched; running fetch.sh"
  "$VENDOR_DIR/fetch.sh"
fi
if [ ! -f "$LIB/include/__config_site" ]; then
  echo "[libcxx] ERROR: $LIB/include/__config_site missing -- run fetch.sh" >&2
  exit 1
fi
if [ ! -f "$LIB/include/__assertion_handler" ]; then
  echo "[libcxx] ERROR: $LIB/include/__assertion_handler missing -- run fetch.sh" >&2
  exit 1
fi

# The committed config/ is the source of truth: re-sync the overlay so a
# config edit always reaches the build (fetch.sh does this too, but its
# marker fast-path would skip the copy).
cp "$VENDOR_DIR/config/__config_site" "$LIB/include/__config_site"
cp "$VENDOR_DIR/config/__assertion_handler" "$LIB/include/__assertion_handler"

# --- 2. target flags (mirror Makefile USER_CFLAGS per arch) ----------------
case "$ARCH" in
  arm)
    TARGET_FLAGS="--target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53"
    ;;
  intel)
    TARGET_FLAGS="--target=x86_64-none-elf -ffreestanding -mno-red-zone"
    ;;
  *)
    echo "[libcxx] ERROR: unknown arch '$ARCH'" >&2
    exit 2
    ;;
esac

REPO_ROOT="$(cd "${VENDOR_DIR}/../.." && pwd)"

# -fno-exceptions -fno-rtti: browser.md §6 P3.1 policy (matches WebKit's own
#   compile configs; "verify the exact needs per-component later").
# -nostdinc++: only the vendored libc++ headers are C++ system headers.
#   C headers resolve from the HobbyOS sysroot (src/libc/include).
# -std=c++23: the language standard libc++ 21 itself builds with; user code
#   may use any standard >= 17 against this archive (headers adapt).
# _LIBCPP_BUILDING_LIBRARY: normal library-build define (system header
#   treatment, non-inline definitions).
# LIBCXX_BUILDING_LIBCXXABI: selects the libc++abi backends in exception.cpp,
#   new_handler.cpp, typeinfo.cpp, stdexcept_default.ipp (the flag the
#   upstream CMake sets via HandleLibCXXABI.cmake for the libc++abi backend).
# -nostdinc blocks the host /usr/include: for --target=x86_64-none-elf the
# clang driver keeps appending the Debian gcc include dirs (arch match), and
# libc++'s __mbstate_t.h happily picks up host glibc's
# <bits/types/mbstate_t.h> if it can.  The compiler's own freestanding
# headers (stdarg.h, stddef.h, ...) stay reachable through the explicit
# -isystem of the resource dir; the HobbyOS sysroot is on -I.
if CLANG_RESOURCE_INCLUDE="$(${CXX} -print-resource-dir 2>/dev/null)/include" && [ -d "${CLANG_RESOURCE_INCLUDE}" ]; then
  :
else
  echo "[libcxx] ERROR: cannot locate the compiler resource include dir" >&2
  exit 1
fi
STDINC="-nostdinc -isystem ${CLANG_RESOURCE_INCLUDE}"

CXXFLAGS_BASE="-O2 -g -std=c++23 -fno-exceptions -fno-rtti -nostdinc++ ${STDINC}
 -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES
 -I${LIB}/include -I${LIB}/src -I${LIB}/src/include -I${ABI}/include
 -I${REPO_ROOT}/src/libc/include -I${REPO_ROOT}/src/include -I${REPO_ROOT}/src/user_include
 ${TARGET_FLAGS}"
CXXFLAGS_ABI="${CXXFLAGS_BASE} -D_LIBCXXABI_BUILDING_LIBRARY -DLIBCXX_BUILDING_LIBCXXABI"

# compiler-rt builtins (the "R" class in sources.txt): freestanding C, no
# HobbyOS headers needed -- clang's own freestanding headers cover
# <stdint.h>/<limits.h> etc. for the bare-metal triples.
CC="${CC:-clang}"
CFLAGS_R="-O2 -g -std=c11 -fno-builtin ${STDINC} -I${RT} ${TARGET_FLAGS}"

mkdir -p "$OBJDIR"

# A header-content stamp: objects depend on it, so editing any sysroot
# header (or the site config) invalidates them all.  The file is only
# rewritten when the hash changes, so a no-op re-run stays incremental.
SYSROOT_HASH="$(
  {
    find "${REPO_ROOT}/src/libc/include" "${REPO_ROOT}/src/include" \
         "${REPO_ROOT}/src/user_include" -name '*.h' -print 2>/dev/null \
      | LC_ALL=C sort | xargs cat 2>/dev/null | sha256sum
    cat "${LIB}/include/__config_site"
    cat "${LIB}/include/__assertion_handler"
  } | sha256sum | cut -d' ' -f1
)"
STAMP="${OBJDIR}/sysroot.stamp"
if [ ! -f "${STAMP}" ] || [ "$(cat "${STAMP}")" != "${SYSROOT_HASH}" ]; then
  printf '%s\n' "${SYSROOT_HASH}" > "${STAMP}"
fi

# --- 3. incremental makefile ----------------------------------------------
# Only rewrite when the content changes: the objects depend on it, so a
# flag/root change forces a full rebuild, but a no-op re-run doesn't.
GEN="${OBJDIR}/Makefile.gen"
GEN_TMP="${OBJDIR}/.Makefile.gen.tmp"
{
  echo "# generated by build-target.sh -- do not edit"
  echo "LIB := ${LIB}"
  echo "ABI := ${ABI}"
  echo "OBJDIR := ${OBJDIR}"
  echo "CXX := ${CXX}"
  echo "CC := ${CC}"
  echo "RT := ${RT}"
  echo "CXXFLAGS_L := $(echo ${CXXFLAGS_BASE} | tr '\n' ' ')"
  echo "CXXFLAGS_A := $(echo ${CXXFLAGS_ABI} | tr '\n' ' ')"
  echo "CFLAGS_R := $(echo ${CFLAGS_R} | tr '\n' ' ')"
  echo
  { printf 'L_SRCS := '; grep '^L ' "${VENDOR_DIR}/sources.txt" | sed 's/^L //' | sed "s|^|${LIB}/|" | paste -sd' ' -; echo; }
  echo
  { printf 'A_SRCS := '; grep '^A ' "${VENDOR_DIR}/sources.txt" | sed 's/^A //' | sed "s|^|${ABI}/|" | paste -sd' ' -; echo; }
  echo
  { printf 'R_SRCS := '; grep '^R ' "${VENDOR_DIR}/sources.txt" | sed 's/^R //' | sed "s|^|${RT}/|" | paste -sd' ' -; echo; }
  echo
  cat <<'EOF'
L_OBJS := $(patsubst $(LIB)/%.cpp,$(OBJDIR)/L/%.o,$(L_SRCS))
A_OBJS := $(patsubst $(ABI)/%.cpp,$(OBJDIR)/A/%.o,$(A_SRCS))
R_OBJS := $(patsubst $(RT)/%.c,$(OBJDIR)/R/%.o,$(R_SRCS))
OBJS := $(L_OBJS) $(A_OBJS) $(R_OBJS)

$(OBJS): $(OBJDIR)/sysroot.stamp

all: $(OBJS)

$(L_OBJS): $(OBJDIR)/L/%.o: $(LIB)/%.cpp $(OBJDIR)/Makefile.gen
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_L) -c $< -o $@

$(A_OBJS): $(OBJDIR)/A/%.o: $(ABI)/%.cpp $(OBJDIR)/Makefile.gen
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_A) -c $< -o $@

$(R_OBJS): $(OBJDIR)/R/%.o: $(RT)/%.c $(OBJDIR)/Makefile.gen
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_R) -c $< -o $@
EOF
} > "$GEN_TMP"
if ! cmp -s "$GEN_TMP" "$GEN"; then
  mv "$GEN_TMP" "$GEN"
else
  rm -f "$GEN_TMP"
fi

echo "[libcxx] building for ${ARCH}: $(grep -c -E '^[LAR] ' "${VENDOR_DIR}/sources.txt") TUs (${JOBS} jobs)"
make -f "$GEN" -j"${JOBS}" all

# --- 4. archive -----------------------------------------------------------
# Object list derived directly from sources.txt (deterministic order).
{
  grep '^L ' "${VENDOR_DIR}/sources.txt" | sed 's/^L //' | sed "s|^|${OBJDIR}/L/|; s|\.cpp$|.o|"
  grep '^A ' "${VENDOR_DIR}/sources.txt" | sed 's/^A //' | sed "s|^|${OBJDIR}/A/|; s|\.cpp$|.o|"
  grep '^R ' "${VENDOR_DIR}/sources.txt" | sed 's/^R //' | sed "s|^|${OBJDIR}/R/|; s|\.c$|.o|"
} > "${OBJDIR}/objects.txt"
rm -f "$OUT"
ar rcs "$OUT" $(cat "${OBJDIR}/objects.txt")

# Sanity: every object listed must exist.
missing=0
while read -r o; do
  [ -f "$o" ] || { echo "[libcxx] ERROR: missing object $o" >&2; missing=1; }
done < "${OBJDIR}/objects.txt"
[ "$missing" -eq 0 ] || exit 1

echo "[libcxx] OK: $(wc -l < "${OBJDIR}/objects.txt") objects -> $OUT"
