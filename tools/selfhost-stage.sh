#!/usr/bin/env bash
#
# selfhost-stage.sh — stage the in-OS build payload into a FAT16 disk image.
#
# HOST-side tool for the HobbyOS self-hosting program (lane L5, wave 1).  It
# copies the payload that the in-OS toolchain needs (frozen interface F2) into
# /SRC on a FAT16 image, using mtools (mcopy -s for recursive/creates dirs,
# mmd for directories).  It is idempotent: with -f it replaces an existing /SRC.
#
# Usage:
#   tools/selfhost-stage.sh [-f] [image]
#     -f      force: replace any existing /SRC on the image (recursive delete
#             + fresh copy).  Without -f, an image that already has /SRC is
#             left untouched and the script aborts.
#     image   FAT16 image path (default: disk.img).  Must exist and be a
#             formatted FAT filesystem (e.g. created with mkfs.fat -F 16).
#   tools/selfhost-stage.sh --tools [-f] [image]
#     [v3] stage ONLY the in-OS toolchain to /TOOLS (frozen F1 layout:
#     /TOOLS/{CLANG.BIN, LLD.BIN, LLVMAR.BIN, OBJCOPY.BIN, MAKE.BIN,
#     MCOPY.BIN, MDEL.BIN, MMD.BIN, MKFSFAT.BIN} + /TOOLS/RESDIR/include/**)
#     and leave /SRC untouched.  With -f the existing /TOOLS tree is deleted
#     first; without it, files are overwritten in place (-o).  The disk.img
#     recipe invokes this variant; the SELFHOST harness keeps staging /SRC in
#     its own -f run, so the two payloads never clobber each other.
#   tools/selfhost-stage.sh --src [-f] [image]
#     [v4] stage ONLY the BUILD-REQUIRED source subset to /SRC — the kernel
#     sources, shared headers, libc (for the future in-OS libc rebuild), the
#     acceptance user set, Makefile.hobbyos, the limine/EFI boot assets the
#     in-OS S2/S3 disk recipe needs, the carried host build products
#     (libc.a + DESKTOP/CONSOLE/SH for the S5 desktop-path OUT.IMG layout),
#     and SELFHOST.RUN + SELFHOST.SH.  Pre-creates /SRC/obj/arm + /SRC/bin
#     (single-level FAT mkdir; MKDIR.BIN fails on EEXIST so the in-OS
#     recipes never mkdir them).  ~4 MiB, vs 28.7 MiB for the full payload.
#     Source of the LLVM tools: obj/selfhost/dist-staging (override with
#     DIST_STAGING=<path>); the ported tools come from obj/arm/*.bin
#     (override per-tool with the same names as the Makefile vars).
#     Absent staging is a WARNING + skip, not a failure — the M2 install
#     recipe owns producing dist-staging, and trees without it must still
#     build a bootable disk.img.
#
# Payload (frozen F2 shape, refined with measurements from the workstation
# tree on 2026-10-14 — see docs/selfhost/acceptance.md §2; wave-2 additions
# marked [v2]):
#   /SRC/src/**                   615 files, 22.33 MiB   (real source tree, AD-11)
#   /SRC/Makefile.hobbyos         (optional — absent today, warned if missing)
#   /SRC/tools/**                  26 files, 0.37 MiB   (small host-side tools)
#   /SRC/bootloader/BOOTAA64.EFI   (1 file, 308 KiB — the only EFI the in-OS
#                                   disk recipe mounts; other arch EFIs are not
#                                   part of the ARM acceptance)
#   /SRC/limine.conf               (template, copied verbatim from the Makefile
#                                   arm branch, Makefile:1763-1768)
#   /SRC/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include/**
#                                  (C++ include subset; 1681 files, 11.72 MiB;
#                                   AD-9 freeze — DEFAULT OFF (STAGE_LIBCXX=0):
#                                   no acceptance-set TU is C++, re-enable with
#                                   STAGE_LIBCXX=1 when a C++ TU enters the
#                                   in-OS user set)
#   /SRC/SELFHOST.RUN              [v2] the in-OS build driver script
#                                  (src/user/selfhost/SELFHOST.RUN)
#   /SRC/SELFHOST.SH               [v4] the in-OS cwd wrapper the RUN stages
#                                  invoke (`sh /SRC/SELFHOST.SH <target>`)
#   /SELFHOST.BIN                  [v2] the in-OS build driver binary
#                                  (obj/arm/selfhost.bin by default; override
#                                  with SELFHOST_BIN=<path>.  Skipped with a
#                                  WARNING when not built yet)
#   /tmp                           [v2] base dir ensured on the image
#
# Output: a size report (mdir totals | files | bytes) and an informational
# count of payload entries whose basenames exceed FAT 8.3 (informational for
# F8; long names are written for real — LFN is proven, lane L3).
#
# Exits non-zero on any mcopy/mmd failure.
#
# HOST script — bash is fine, but keep it shell-conservative.

set -u

IMAGE="disk.img"
FORCE=0
MODE="payload"   # payload (default) | tools (--tools; /TOOLS only) | src (--src; /SRC build subset)
if [ "${1:-}" = "--tools" ]; then
  MODE="tools"
  shift || true
elif [ "${1:-}" = "--src" ]; then
  MODE="src"
  shift || true
fi
if [ "${1:-}" = "-f" ]; then
  FORCE=1
  IMAGE="${2:-disk.img}"
elif [ -n "${1:-}" ]; then
  IMAGE="$1"
fi

# Payload root = repo root (this script lives in tools/).
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

LIBCXX_INC="${LIBCXX_INC_DIR:-third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include}"
# AD-9 freeze (w4-src, wave 4): STAGE_LIBCXX defaults to OFF (0).  Evidence:
# every TU the S0..S3 in-OS path compiles is C — check/kernel are C+asm, and
# the acceptance user set (HELLO SH LS CAT) plus DESKTOP (C, src/user/
# desktop.c, built with USER_CFLAGS) never touch the libcxx include tree;
# only CXX_LIBCXX_USER_FLAGS programs outside the acceptance set (browser/
# ICU/libcxx tiers) would.  Re-enable with STAGE_LIBCXX=1 when a C++ TU
# enters the in-OS user set (then both modes stage the subset below).
STAGE_LIBCXX="${STAGE_LIBCXX:-0}"
# [v3] Sources for the /TOOLS toolchain staging (--tools): the wave-2 LLVM
# distribution staging, and the ported build tools from obj/arm.
DIST_STAGING="${DIST_STAGING:-obj/selfhost/dist-staging}"
RESDIR_SRC="$DIST_STAGING/usr/local/lib/clang/21/include"
TOOL_SPLIT=" "  # list below

fail() { echo "selfhost-stage: ERROR: $*" >&2; exit 1; }
warn() { echo "selfhost-stage: WARNING: $*" >&2; }

# mmkdir_p — create a directory path on the image one component at a time
# (mtools 4.x mmd does not create parent dirs, and mcopy will not create the
# destination's parent chain either).
mmkdir_p() {
  local p="$1" cur="" comp
  p="${p#::}"
  while [ -n "$p" ]; do
    p="${p#/}"
    comp="${p%%/*}"
    [ -z "$comp" ] && break
    cur="$cur/$comp"
    if ! mdir -i "$IMAGE" "$cur" >/dev/null 2>&1; then
      mmd -i "$IMAGE" "$cur" || { echo "mmd failed: $cur" >&2; return 1; }
    fi
    p="${p#$comp}"
  done
  return 0
}

# ---- [v3] /TOOLS toolchain staging (--tools; frozen F1 layout) ----
# The in-OS build tools on the startup image: /TOOLS/{CLANG.BIN, LLD.BIN,
# LLVMAR.BIN, OBJCOPY.BIN, MAKE.BIN, MCOPY.BIN, MDEL.BIN, MMD.BIN,
# MKFSFAT.BIN} + /TOOLS/RESDIR/include/** (clang builtin headers, staged
# FAITHFULLY with long names — F8 decision: LFN covers them, see
# docs/selfhost/llvm-bootstrap.md §6.2).  The LLVM binaries are staged as the
# raw aarch64-none-elf ELF install-distribution outputs for now; M2 replaces
# them with stripped flat .bin images under the SAME frozen names.  Runs
# standalone (--tools) so the disk.img recipe and the SELFHOST harness stage
# /TOOLS and /SRC independently.
stage_tools() {
  local n=0 total_bytes=0
  echo "selfhost-stage: staging in-OS toolchain to ::/TOOLS (F1 layout)"
  if [ "$FORCE" -eq 1 ] && mdir -i "$IMAGE" ::/TOOLS >/dev/null 2>&1; then
    echo "selfhost-stage: replacing existing ::/TOOLS on $IMAGE"
    mdeltree -i "$IMAGE" ::/TOOLS 2>/dev/null || fail "could not delete existing /TOOLS (mdeltree)"
  fi
  mmkdir_p "::/TOOLS" || fail "mmkdir_p ::/TOOLS"

  # LLVM distribution tools (skip-with-warning when the M2 staging is absent).
  if [ -f "$DIST_STAGING/usr/local/bin/clang-21" ]; then
    mcopy -o -i "$IMAGE" "$DIST_STAGING/usr/local/bin/clang-21"  ::/TOOLS/CLANG.BIN  || fail "mcopy CLANG.BIN"
    mcopy -o -i "$IMAGE" "$DIST_STAGING/usr/local/bin/lld"       ::/TOOLS/LLD.BIN    || fail "mcopy LLD.BIN"
    mcopy -o -i "$IMAGE" "$DIST_STAGING/usr/local/bin/llvm-ar"   ::/TOOLS/LLVMAR.BIN || fail "mcopy LLVMAR.BIN"
    mcopy -o -i "$IMAGE" "$DIST_STAGING/usr/local/bin/llvm-objcopy" ::/TOOLS/OBJCOPY.BIN || fail "mcopy OBJCOPY.BIN"
    n=$((n + 4))
    total_bytes=$((total_bytes +
      $(stat -c %s "$DIST_STAGING/usr/local/bin/clang-21") +
      $(stat -c %s "$DIST_STAGING/usr/local/bin/lld") +
      $(stat -c %s "$DIST_STAGING/usr/local/bin/llvm-ar") +
      $(stat -c %s "$DIST_STAGING/usr/local/bin/llvm-objcopy")))
  else
    warn "/TOOLS toolchain not staged: LLVM distribution staging missing at $DIST_STAGING (build it per docs/selfhost/llvm-bootstrap.md §6 — install-distribution)"
  fi

  # Ported build tools — frozen 8.3 names.  The Makefile's own mcopy lines
  # put MCOPY/MDEL/MMD (and MAKE at ::/MAKE.BIN, MKFSFAT at ::/MKFSFAT.BIN)
  # on the image; these -o copies finish the F1 /TOOLS set, idempotently.
  mmkdir_p "::/TOOLS" || fail "mmkdir_p ::/TOOLS"
  local spec src dst
  for spec in "obj/arm/make.bin MAKE.BIN" "obj/arm/mcopy.bin MCOPY.BIN" \
              "obj/arm/mdel.bin MDEL.BIN"   "obj/arm/mmd.bin MMD.BIN" \
              "obj/arm/mkfsfat.bin MKFSFAT.BIN"; do
    src="${spec% *}"; dst="${spec#* }"
    if [ -f "$src" ]; then
      mcopy -o -i "$IMAGE" "$src" "::/TOOLS/$dst" || fail "mcopy ::/TOOLS/$dst"
      n=$((n + 1))
      total_bytes=$((total_bytes + $(stat -c %s "$src")))
    else
      warn "tool not built ($src) — ::/TOOLS/$dst not staged (make $src)"
    fi
  done

  # Resource dir — faithful long-name tree incl. wrapper subdirs (272 files).
  if [ -d "$RESDIR_SRC" ]; then
    if [ "$FORCE" -eq 1 ] && mdir -i "$IMAGE" ::/TOOLS/RESDIR >/dev/null 2>&1; then
      mdeltree -i "$IMAGE" ::/TOOLS/RESDIR 2>/dev/null || fail "mdeltree ::/TOOLS/RESDIR"
    fi
    mmkdir_p "::/TOOLS/RESDIR" || fail "mmkdir_p ::/TOOLS/RESDIR"
    mcopy -s -o -i "$IMAGE" "$RESDIR_SRC" "::/TOOLS/RESDIR/./" || fail "mcopy RESDIR include tree"
    local cnt; cnt=$(find "$RESDIR_SRC" -type f | wc -l)
    n=$((n + cnt))
    total_bytes=$((total_bytes + $(du -sb "$RESDIR_SRC" | awk '{print $1}')))
  else
    warn "clang resource dir missing at $RESDIR_SRC — /TOOLS/RESDIR not staged (stage the LLVM distribution first)"
  fi

  echo "selfhost-stage: staged $n files, $total_bytes bytes into ::/TOOLS"
  echo "  --- ::/TOOLS (mdir) ---"
  mdir -i "$IMAGE" ::/TOOLS 2>&1 | tail -5
  echo "  --- ::/TOOLS/RESDIR/include count ---"
  mdir -i "$IMAGE" -/ ::/TOOLS/RESDIR/include 2>&1 | tail -3
}

# --tools: stage /TOOLS only and exit before any /SRC logic (the harness's
# own -f run owns /SRC; the two payloads are independent).
if [ "$MODE" = "tools" ]; then
  [ -f "$IMAGE" ] || fail "image not found: $IMAGE (create it, e.g. truncate -s 256M scratch.img && mkfs.fat -F 16 scratch.img)"
  for t in mcopy mmd mdir mdeltree; do
    command -v "$t" >/dev/null 2>&1 || fail "required tool not found: $t"
  done
  stage_tools
  echo "selfhost-stage: done"
  exit 0
fi

for t in mcopy mmd mdir mdeltree; do
  command -v "$t" >/dev/null 2>&1 || fail "required tool not found: $t"
done

[ -f "$IMAGE" ] || fail "image not found: $IMAGE (create it, e.g. truncate -s 256M scratch.img && mkfs.fat -F 16 scratch.img)"

# ---- does /SRC already exist? (mdir returns 0 even on empty, so probe the
# dir).  NOTE: the disk.img recipe pre-creates ::/src (the L3 LFN fixture
# dirs, Makefile 1865-1874) which IS the same FAT dir as /SRC — so --src
# mode must tolerate an existing /SRC and merge (mcopy -o) instead of
# failing or deleting the fixtures.  The default payload mode keeps its
# fail-without--f / delete-with--f contract. ----
if mdir -i "$IMAGE" ::/SRC >/dev/null 2>&1; then
  if [ "$FORCE" -ne 1 ] && [ "$MODE" != "src" ]; then
    fail "image '$IMAGE' already contains /SRC — pass -f to replace it (idempotent)"
  fi
  if [ "$FORCE" -eq 1 ]; then
    echo "selfhost-stage: replacing existing /SRC on $IMAGE"
    mdeltree -i "$IMAGE" ::/SRC 2>/dev/null || fail "could not delete existing /SRC (mdeltree)"
  fi
fi

# ---- payload sources (tolerate absence of the optional ones) ----
[ -d src ] || fail "no src/ at repo root ($REPO_ROOT) — run from the repo root"
[ -d tools ] || warn "tools/ missing — skipping payload tools/**"
[ -f bootloader/BOOTAA64.EFI ] || warn "bootloader/BOOTAA64.EFI missing — skipping"
[ -f Makefile.hobbyos ] || warn "Makefile.hobbyos absent (lands in a later wave) — staging without it"
if [ "$STAGE_LIBCXX" = "1" ]; then
  [ -d "$LIBCXX_INC" ] || warn "libcxx include subset missing — skipping C++ set (STAGE_LIBCXX=0 to silence)"
else
  echo "selfhost-stage: STAGE_LIBCXX=0 — skipping C++ libcxx include subset"
fi

# ---- dry-run 8.3 excess count + file/byte pre-count on the payload ----
# (basename violates 8.3 if the stem is >8 chars or the extension >3 chars)
count_83_excess() {
  find "$@" \( -type f -o -type l \) -printf '%f\n' 2>/dev/null | awk '
    {
      b=$0; e="";
      if (match(b, /\.[^.]*$/)) { e=substr(b, RSTART+1); b=substr(b, 1, RSTART-1) }
      if (length(b) > 8 || length(e) > 3) n++
    }
    END { print n+0 }'
}

payload_files=0
payload_bytes=0
add_payload() {  # add_payload <files> <bytes>
  payload_files=$((payload_files + $1))
  payload_bytes=$((payload_bytes + $2))
}

# ---- stage the tree (payload mode; --src exits earlier, below) ----
if [ "$MODE" != "src" ]; then
  mmd -i "$IMAGE" ::/SRC || fail "mmd ::/SRC"
fi

copy_tree() {  # copy_tree <src-dir> <dest-parent-on-image> — copies the dir ITSELF (dir/<basename>)
  local src="$1" dst="$2"
  [ -d "$src" ] || return 0
  # mcopy -s <dir> <existing-dir/> copies the directory into the target as
  # <target>/<basename>; the ./ guard makes the directory-target explicit.
  mcopy -s -i "$IMAGE" "$src" "$dst/./" || { echo "mcopy failed for $src" >&2; return 1; }
  local cnt bytes
  cnt=$(find "$src" -type f | wc -l)
  bytes=$(du -sb "$src" | awk '{print $1}')
  add_payload "$cnt" "$bytes"
}

# ---- [v4] /SRC BUILD-REQUIRED source subset (--src; frozen F2 flavor) ----
# Stages only what the in-OS build recipe needs: the kernel sources, shared
# headers, libc (headers + crt0 + sources — the future in-OS libc rebuild),
# the acceptance user set + the driver script, Makefile.hobbyos, the
# limine/EFI assets the in-OS S3 OUT.IMG recipe references, the carried host
# build products (libc.a for the S1 link; DESKTOP/CONSOLE/SH + OUTMARK for
# the S5 desktop-path OUT.IMG layout, acceptance.md §8.1/§8.3), and the
# SELFHOST.RUN / SELFHOST.SH stage scripts.  Pre-creates /SRC/obj/arm and
# /SRC/bin: the FAT mkdir is one-level and MKDIR.BIN fails on EEXIST, so the
# in-OS make recipes never mkdir these (L4 sketch option (b)).
stage_src() {
  local cnt
  payload_files=0
  payload_bytes=0
  echo "selfhost-stage: staging BUILD-REQUIRED source subset to ::/SRC"
  # Merge into an existing ::/SRC (the disk recipe's ::/src L3 fixture dirs
  # ARE /SRC); -f (payload mode owner) already deleted it above.
  mmkdir_p "::/SRC" || fail "mmkdir_p ::/SRC"
  mmkdir_p "::/SRC/obj/arm" || fail "mmkdir_p ::/SRC/obj/arm"
  mmkdir_p "::/SRC/bin" || fail "mmkdir_p ::/SRC/bin"

  # Subsets land at /SRC/src/... — the SAME nesting the full payload mode
  # produces (copy_tree src) — so the in-OS recipes' relative src/...
  # references resolve with cwd=/SRC.
  mmkdir_p "::/SRC/src" || fail "mmkdir_p ::/SRC/src"
  for d in kernel include user_include libc; do
    mcopy -s -o -i "$IMAGE" "src/$d" "::/SRC/src/./" || fail "mcopy src/$d"
    add_payload "$(find "src/$d" -type f | wc -l)" "$(du -sb "src/$d" | awk '{print $1}')"
  done

  # User set — the 5 acceptance TUs + user linker script + the driver dir
  # (SELFHOST.RUN/selfhost.c/SELFHOST.SH).  Explicit files, NOT the whole
  # 10.5 MiB src/user (browser/python tiers are not build-required).
  mmkdir_p "::/SRC/src/user" || fail "mmkdir_p ::/SRC/src/user"
  for f in hello.c sh.c sh_aux.c ls.c cat.c linker.ld; do
    mcopy -o -i "$IMAGE" "src/user/$f" "::/SRC/src/user/$f" || fail "mcopy src/user/$f"
    add_payload 1 "$(stat -c %s "src/user/$f")"
  done
  mcopy -s -o -i "$IMAGE" src/user/selfhost "::/SRC/src/user/./" || fail "mcopy src/user/selfhost"
  cnt=$(find src/user/selfhost -type f | wc -l)
  add_payload "$cnt" "$(du -sb src/user/selfhost | awk '{print $1}')"

  # Kernel linker script (repo-root linker.ld; the host link uses
  # -T linker.ld, cwd = the script's dir) + the build driver + boot assets.
  mcopy -o -i "$IMAGE" linker.ld ::/SRC/linker.ld || fail "mcopy linker.ld"
  add_payload 1 "$(stat -c %s linker.ld)"
  mcopy -o -i "$IMAGE" Makefile.hobbyos ::/SRC/Makefile.hobbyos || fail "mcopy Makefile.hobbyos"
  add_payload 1 "$(stat -c %s Makefile.hobbyos)"
  if [ -f bootloader/BOOTAA64.EFI ]; then
    mmkdir_p "::/SRC/bootloader" || fail "mmkdir_p ::/SRC/bootloader"
    mcopy -o -i "$IMAGE" bootloader/BOOTAA64.EFI ::/SRC/bootloader/BOOTAA64.EFI || fail "mcopy BOOTAA64.EFI"
    add_payload 1 "$(stat -c %s bootloader/BOOTAA64.EFI)"
  fi

  # limine.conf template — verbatim arm branch of the disk recipe (Makefile:1763-1768)
  printf 'timeout: 0\ndefault_entry: 1\n\n/HobbyOS (ARM AArch64)\nprotocol: linux\npath: boot():/boot/hobbyos.bin\n' | \
    mcopy -o -i "$IMAGE" - ::/SRC/limine.conf || fail "mcopy limine.conf"
  add_payload 1 136

  # Carried host build products the v1 in-OS recipes reference (WARNING+skip
  # when not built yet; they are disk.img prerequisites, so by recipe time
  # every one below exists).
  for spec in "obj/arm/libc.a ::/SRC/obj/arm/libc.a" \
              "obj/arm/desktop.bin ::/SRC/bin/DESKTOP.BIN" \
              "obj/arm/console.bin ::/SRC/bin/CONSOLE.BIN" \
              "obj/arm/sh.bin ::/SRC/bin/SH.BIN"; do
    src="${spec%% *}"; dst="${spec#* }"
    if [ -f "$src" ]; then
      mcopy -o -i "$IMAGE" "$src" "$dst" || fail "mcopy $dst"
      add_payload 1 "$(stat -c %s "$src")"
    else
      warn "carried build product not built ($src) — $dst not staged (build the tree first)"
    fi
  done
  printf 'OUTMARK:w4-src:staged-by-selfhost-stage-src\n' | \
    mcopy -o -i "$IMAGE" - ::/SRC/bin/OUTMARK.TXT || fail "mcopy OUTMARK.TXT"
  add_payload 1 42

  if [ "$STAGE_LIBCXX" = "1" ] && [ -d "$LIBCXX_INC" ]; then
    # C++ include subset at the frozen path (acceptance.md §2; opt-in —
    # AD-9 freezes the default OFF).
    local_dst="third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx"
    mmkdir_p "::/SRC/$local_dst/include" || fail "mmkdir_p ::/SRC libcxx include"
    mcopy -s -o -i "$IMAGE" "$LIBCXX_INC" "::/SRC/$local_dst/include/" || fail "mcopy libcxx include"
    cnt=$(find "$LIBCXX_INC" -type f | wc -l)
    add_payload "$cnt" "$(du -sb "$LIBCXX_INC" | awk '{print $1}')"
  fi

  echo
  echo "selfhost-stage: staged BUILD-REQUIRED subset into $IMAGE"
  echo "  payload: $payload_files files, $payload_bytes bytes"
  echo "  --- ::/SRC (mdir) ---"
  mdir -i "$IMAGE" -/ ::/SRC 2>&1 | tail -6
}

# --src: stage the build subset and exit before any full-payload logic.
if [ "$MODE" = "src" ]; then
  [ -f "$IMAGE" ] || fail "image not found: $IMAGE (create it, e.g. truncate -s 1G disk.img && mformat -i disk.img ::)"
  for t in mcopy mmd mdir mdeltree; do
    command -v "$t" >/dev/null 2>&1 || fail "required tool not found: $t"
  done
  stage_src
  echo "selfhost-stage: done"
  exit 0
fi

copy_tree src "::/SRC" || exit 1
copy_tree tools "::/SRC" || exit 1

if [ -f Makefile.hobbyos ]; then
  mcopy -i "$IMAGE" Makefile.hobbyos ::/SRC/Makefile.hobbyos || exit 1
  add_payload 1 "$(stat -c %s Makefile.hobbyos)"
fi

if [ -f bootloader/BOOTAA64.EFI ]; then
  mmd -i "$IMAGE" ::/SRC/bootloader 2>/dev/null || true
  mcopy -i "$IMAGE" bootloader/BOOTAA64.EFI ::/SRC/bootloader/BOOTAA64.EFI || exit 1
  add_payload 1 "$(stat -c %s bootloader/BOOTAA64.EFI)"
fi

# limine.conf template — verbatim arm branch of the disk recipe (Makefile:1763-1768)
mmd -i "$IMAGE" ::/SRC 2>/dev/null || true
printf 'timeout: 0\ndefault_entry: 1\n\n/HobbyOS (ARM AArch64)\nprotocol: linux\npath: boot():/boot/hobbyos.bin\n' | \
  mcopy -i "$IMAGE" - ::/SRC/limine.conf || exit 1
add_payload 1 136

if [ "$STAGE_LIBCXX" = "1" ] && [ -d "$LIBCXX_INC" ]; then
  # The include root lands at /SRC/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include
  local_dst="third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx"
  mmkdir_p "::/SRC/$local_dst/include" || exit 1
  # Copy the CONTENTS so the destination keeps the frozen name "include"
  # regardless of the source directory's basename.
  mcopy -s -i "$IMAGE" "$LIBCXX_INC"/* "::/SRC/$local_dst/include/" || exit 1
  add_payload "$(find "$LIBCXX_INC" -type f | wc -l)" "$(du -sb "$LIBCXX_INC" | awk '{print $1}')"
fi

# [v2] base dirs the in-OS build expects on the boot disk (idempotent:
# mmkdir_p skips components that already exist).  /tmp on a real disk.img
# exists already (Makefile recipe); a minimal scratch image may lack it.
mmkdir_p "::/tmp" || warn "could not ensure ::/tmp on the image"

# [v2] in-OS acceptance driver: stage the binary to the disk ROOT (the
# desktop Apps menu + the shell resolve it from there, acceptance §5.3) and
# its command script to /SRC (the driver's default path).  The binary is a
# build product — tolerate a pre-build invocation with a WARNING instead of
# failing, so `stage` alone can still be a first step in a clean checkout.
SELFHOST_BIN="${SELFHOST_BIN:-obj/arm/selfhost.bin}"
if [ -f "$SELFHOST_BIN" ]; then
  # -o: overwrite without prompting — a previous -f run's ::/SELFHOST.BIN is
  # at the disk ROOT (outside /SRC), so it survives the /SRC mdeltree and
  # must be replaced in place.
  mcopy -o -i "$IMAGE" "$SELFHOST_BIN" ::/SELFHOST.BIN || exit 1
  add_payload 1 "$(stat -c %s "$SELFHOST_BIN")"
else
  warn "SELFHOST driver bin not built ($SELFHOST_BIN) — skipping ::/SELFHOST.BIN (make obj/arm/selfhost.bin)"
fi
if [ -f src/user/selfhost/SELFHOST.RUN ]; then
  mcopy -o -i "$IMAGE" src/user/selfhost/SELFHOST.RUN ::/SRC/SELFHOST.RUN || exit 1
  add_payload 1 "$(stat -c %s src/user/selfhost/SELFHOST.RUN)"
else
  warn "SELFHOST.RUN script missing (src/user/selfhost/SELFHOST.RUN) — skipping ::/SRC/SELFHOST.RUN (driver will report SCRIPT-MISSING)"
fi
# [v4] the cwd wrapper SELFHOST.SH must ride alongside SELFHOST.RUN: the
# RUN file's stages invoke it (`sh /SRC/SELFHOST.SH <target>`).  Without it
# every stage fails at sh's "cannot open /SRC/SELFHOST.SH".
if [ -f src/user/selfhost/SELFHOST.SH ]; then
  mcopy -o -i "$IMAGE" src/user/selfhost/SELFHOST.SH ::/SRC/SELFHOST.SH || exit 1
  add_payload 1 "$(stat -c %s src/user/selfhost/SELFHOST.SH)"
else
  warn "SELFHOST.SH wrapper missing (src/user/selfhost/SELFHOST.SH) — skipping ::/SRC/SELFHOST.SH"
fi

# [v4] carried host build products the in-OS v1 recipes reference (same set
# as --src): the S1 user target links /SRC/obj/arm/libc.a, and the S3 disk
# recipe stages the S5 desktop-path products from /SRC/bin.  Keep the
# payload and --src modes consistent so a harness `--stage` re-stage leaves
# the make recipes runnable.  WARNING+skip pre-build.
mmkdir_p "::/SRC/obj/arm" || warn "could not ensure ::/SRC/obj/arm"
mmkdir_p "::/SRC/bin" || warn "could not ensure ::/SRC/bin"
for spec in "obj/arm/libc.a ::/SRC/obj/arm/libc.a" \
            "obj/arm/desktop.bin ::/SRC/bin/DESKTOP.BIN" \
            "obj/arm/console.bin ::/SRC/bin/CONSOLE.BIN" \
            "obj/arm/sh.bin ::/SRC/bin/SH.BIN"; do
  src_spec="${spec%% *}"; dst_spec="${spec#* }"
  if [ -f "$src_spec" ]; then
    mcopy -o -i "$IMAGE" "$src_spec" "$dst_spec" || exit 1
    add_payload 1 "$(stat -c %s "$src_spec")"
  else
    warn "carried build product not built ($src_spec) — $dst_spec not staged (build the tree first)"
  fi
done
printf 'OUTMARK:w4-src:staged-by-selfhost-stage\n' | \
  mcopy -o -i "$IMAGE" - ::/SRC/bin/OUTMARK.TXT || warn "OUTMARK.TXT not staged"
add_payload 1 42

# ---- report ----
echo
echo "selfhost-stage: staged payload into $IMAGE"
echo "  payload: $payload_files files, $payload_bytes bytes"

excess=$(count_83_excess src tools bootloader/BOOTAA64.EFI Makefile.hobbyos "$LIBCXX_INC" 2>/dev/null)
echo "  8.3-oversized basenames in payload: ${excess:-0} (informational for F8; LFN is proven)"

echo "  --- image report (mdir totals) ---"
mdir -i "$IMAGE" -/ ::/SRC 2>&1 | tail -6
echo "  --- full image ---"
mdir -i "$IMAGE" ::/ 2>&1 | tail -3

echo "selfhost-stage: done"
exit 0
