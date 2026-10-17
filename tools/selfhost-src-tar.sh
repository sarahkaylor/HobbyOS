#!/usr/bin/env bash
#
# selfhost-src-tar.sh — assemble SRC.TAR, the in-OS source payload.
#
# HOST-side tool for the HobbyOS in-OS kernel build (docs/selfhost/
# inos-build-kernel.md).  Produces a single uncompressed ustar tarball
# that the disk image carries at ::/SRC.TAR; inside HobbyOS the rooted
# script /BUILD_KERNEL.SH extracts it with the in-OS /TOOLS/TAR.BIN and
# then builds the kernel from it with /TOOLS/MAKE.BIN + Makefile.hobbyos.
#
# Usage:
#   tools/selfhost-src-tar.sh [out.tar]      (default: obj/selfhost/SRC.TAR)
#
# Contents, exactly the paths the in-OS build expects after extraction
# into /SRC (frozen F2 flavor, selfhost.md §7.3):
#   src/**                     the real source tree (AD-11: real names)
#   tools/**                   repo tools (small; useful in-OS)
#   Makefile.hobbyos           the in-OS build driver (F3 targets)
#   linker.ld                  kernel linker script (repo root)
#   bootloader/BOOTAA64.EFI    bootloader for the in-OS-assembled OUT.IMG
#   limine.conf                ARM boot config (= bootloader/limine-arm.conf)
#   bin/{DESKTOP,CONSOLE,SH}.BIN + OUTMARK.TXT
#                              carried host build products the S3 OUT.IMG
#                              layout references (§8.1)
#   obj/arm/libc.a             carried libc archive (the S1 link)
#   build_kernel.sh            the in-OS build script (also staged at
#                              the disk root as /BUILD_KERNEL.SH)
#
# The tarball is created with --format=ustar (the in-OS reader covers
# ustar + the GNU 'L' long-name extension); --hard-dereference is set so
# no hardlink entries can appear.  A self-check re-reads the tarball with
# the HOST tar and byte-compares a sample against the source files — a
# build that cannot be re-read is never written out silently.
#
# HOST script — bash is fine, but keep it shell-conservative.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT="${1:-obj/selfhost/SRC.TAR}"
TREE="obj/selfhost/srctar-tree"

fail() { echo "selfhost-src-tar: ERROR: $*" >&2; exit 1; }
warn() { echo "selfhost-src-tar: WARNING: $*" >&2; }

# GNU stat -c%s vs BSD/macOS stat -f%z (same helper as the sibling
# selfhost-stage.sh / third_party/icu-78.3 build-host.sh).
size_of() { if [ "$(uname -s)" = "Darwin" ]; then stat -f%z "$1"; else stat -c%s "$1"; fi; }

for t in tar mcopy; do
  command -v "$t" >/dev/null 2>&1 || fail "required tool not found: $t"
done

[ -d src ] || fail "no src/ at repo root ($REPO_ROOT)"
[ -f Makefile.hobbyos ] || fail "Makefile.hobbyos missing — the in-OS driver"
[ -f linker.ld ] || fail "linker.ld missing (repo-root kernel linker script)"
[ -f bootloader/limine-arm.conf ] || fail "bootloader/limine-arm.conf missing"
[ -f src/user/selfhost/build_kernel.sh ] || fail "src/user/selfhost/build_kernel.sh missing"

echo "selfhost-src-tar: assembling $OUT"

# ---- stage the tree (out of the repo tree, gitignored obj/) ----------------
rm -rf "$TREE"
mkdir -p "$TREE/bin" "$TREE/obj/arm" "$TREE/bootloader"

cp -a src "$TREE/src" || fail "cp src"
cp -a tools "$TREE/tools" || fail "cp tools"
cp Makefile.hobbyos linker.ld "$TREE/" || fail "cp Makefile.hobbyos/linker.ld"
cp bootloader/BOOTAA64.EFI "$TREE/bootloader/BOOTAA64.EFI" || fail "cp BOOTAA64.EFI"
cp bootloader/limine-arm.conf "$TREE/limine.conf" || fail "cp limine.conf"
cp src/user/selfhost/build_kernel.sh "$TREE/build_kernel.sh" || fail "cp build_kernel.sh"

# Carried host build products (same set as tools/selfhost-stage.sh --src;
# WARNING+skip before the tree is built — the disk recipe builds them
# first, so by recipe time every one below exists).
for spec in "obj/arm/libc.a obj/arm/libc.a" \
            "obj/arm/desktop.bin bin/DESKTOP.BIN" \
            "obj/arm/console.bin bin/CONSOLE.BIN" \
            "obj/arm/sh.bin bin/SH.BIN"; do
  src_file="${spec%% *}"; dst_file="${spec#* }"
  if [ -f "$src_file" ]; then
    cp "$src_file" "$TREE/$dst_file" || fail "cp $src_file"
  else
    warn "carried build product not built ($src_file) — $dst_file not in SRC.TAR (build the tree first)"
  fi
done
printf 'OUTMARK:inos-build-kernel:staged-by-src-tar\n' > "$TREE/bin/OUTMARK.TXT"

# ---- single-pass ustar create ---------------------------------------------
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# GNU tar vs bsdtar/macOS: --hard-dereference/--sort=name are GNU-only and
# bsdtar rejects them; neither matters for correctness here (the repo has no
# hard links to dereference, and --sort is cosmetic for a tarball our custom
# in-OS reader re-reads).  Both accept --format=ustar --owner/--group/
# --numeric-owner, which ARE load-bearing (the in-OS reader expects ustar +
# uid/gid 0).
if tar --version 2>&1 | grep -q "GNU tar"; then
  TAR_CREATE_ARGS=(--format=ustar --hard-dereference --numeric-owner --owner=0 --group=0 --sort=name)
else
  TAR_CREATE_ARGS=(--format=ustar --numeric-owner --owner=0 --group=0)
fi
tar "${TAR_CREATE_ARGS[@]}" -cf "$OUT" -C "$TREE" \
    src tools Makefile.hobbyos linker.ld bootloader limine.conf bin obj build_kernel.sh \
  || fail "tar create failed"

# ---- self-check: re-read with the host tar and compare ---------------------
n_tar=$(tar -tf "$OUT" | wc -l)
n_tree=$(find "$TREE" -mindepth 1 | wc -l)
[ "$n_tar" -eq "$n_tree" ] || fail "entry count mismatch: tar=$n_tar tree=$n_tree"

check_member() {  # check_member <member> <source-file>
  tar -xOf "$OUT" "$1" 2>/dev/null | cmp -s - "$2" \
    || fail "self-check failed: member $1 != $2"
}
check_member src/kernel/main.c src/kernel/main.c
check_member Makefile.hobbyos Makefile.hobbyos
check_member linker.ld linker.ld
check_member limine.conf bootloader/limine-arm.conf
check_member build_kernel.sh src/user/selfhost/build_kernel.sh
if [ -f obj/arm/libc.a ]; then
  check_member obj/arm/libc.a obj/arm/libc.a
fi
if [ -f obj/arm/desktop.bin ]; then
  check_member bin/DESKTOP.BIN obj/arm/desktop.bin
fi

size=$(size_of "$OUT")
echo "selfhost-src-tar: done — $OUT"
echo "  entries: $n_tar (files+dirs), size: $size bytes"
echo "  spot-checked members: src/kernel/main.c Makefile.hobbyos linker.ld limine.conf build_kernel.sh"
