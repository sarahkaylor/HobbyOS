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
#                                  (C++ acceptance set only; 1681 files,
#                                   11.72 MiB; skip with STAGE_LIBCXX=0 — kept
#                                   at its default until the M3 AD-9 freeze)
#   /SRC/SELFHOST.RUN              [v2] the in-OS build driver script
#                                  (src/user/selfhost/SELFHOST.RUN)
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
MODE="payload"   # payload (default) | tools (--tools; /TOOLS only)
if [ "${1:-}" = "--tools" ]; then
  MODE="tools"
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
STAGE_LIBCXX="${STAGE_LIBCXX:-1}"
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

# ---- does /SRC already exist? (mdir returns 0 even on empty, so probe the dir) ----
if mdir -i "$IMAGE" ::/SRC >/dev/null 2>&1; then
  if [ "$FORCE" -ne 1 ]; then
    fail "image '$IMAGE' already contains /SRC — pass -f to replace it (idempotent)"
  fi
  echo "selfhost-stage: replacing existing /SRC on $IMAGE"
  mdeltree -i "$IMAGE" ::/SRC 2>/dev/null || fail "could not delete existing /SRC (mdeltree)"
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

# ---- stage the tree ----
mmd -i "$IMAGE" ::/SRC || fail "mmd ::/SRC"

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
