#!/bin/bash
# =============================================================================
# verify_artifacts.sh — prove a pulled-back HobbyOS artifact set is really an
# x64 build before anyone boots it.
#
# The dangerous failure mode is a SILENT wrong-arch artifact: $(TARGET)
# (hobbyos.elf) is one shared root file linked from whichever ARCH was built
# last, so an "intel" run in a tree that last built ARM yields an AArch64 ELF
# that boots to a dead black screen (or an OVMF boot-option loop) with no OS
# output.  The x64 kernel is deliberately ELF32/Intel-80386 (multiboot1, entry
# 0x70000030) — so "ELF32/80386" is CORRECT here and "AArch64" is the bug.
#
# Usage: verify_artifacts.sh <artifact-dir> [boot.log]
# Exit:  0 = all checks pass, 1 = an artifact is wrong/missing.
# =============================================================================

DIR="${1:-}"
BOOTLOG="${2:-}"
[ -n "$DIR" ] || { echo "usage: $0 <artifact-dir> [boot.log]" >&2; exit 2; }
[ -d "$DIR" ] || { echo "verify: no such dir: $DIR" >&2; exit 1; }

FAIL=0
pass() { printf '    ok   %s\n' "$*"; }
bad()  { printf '    FAIL %s\n' "$*"; FAIL=1; }
info() { printf '    info %s\n' "$*"; }

# ---- find an ELF reader (Homebrew llvm is keg-only: not on PATH) ------------
READELF=""
for c in llvm-readelf /opt/homebrew/opt/llvm/bin/llvm-readelf /usr/bin/llvm-readelf readelf; do
  if command -v "$c" >/dev/null 2>&1; then READELF="$c"; break; fi
done

ELF="$DIR/hobbyos.elf"
IMG="$DIR/disk.img"

# ---- 1. the kernel ----------------------------------------------------------
if [ ! -f "$ELF" ]; then
  bad "hobbyos.elf missing from $DIR"
else
  ELF_SIZE=$(wc -c < "$ELF" | tr -d ' ')
  if [ "$ELF_SIZE" -lt 500000 ]; then
    bad "hobbyos.elf is only ${ELF_SIZE} bytes — truncated or a stub"
  else
    pass "hobbyos.elf present ($(( ELF_SIZE / 1024 )) KiB)"
  fi

  if [ -n "$READELF" ]; then
    HDR="$("$READELF" -h "$ELF" 2>/dev/null)"
    MACHINE="$(printf '%s\n' "$HDR" | sed -n 's/ *Machine: *//p' | head -1)"
    CLASS="$(printf '%s\n' "$HDR" | sed -n 's/ *Class: *//p' | head -1)"
    TYPE="$(printf '%s\n' "$HDR" | sed -n 's/ *Type: *//p' | head -1)"
    ENTRY="$(printf '%s\n' "$HDR" | sed -n 's/ *Entry point address: *//p' | head -1)"
    case "$MACHINE" in
      *80386*|*i386*)
        case "$CLASS" in
          *ELF32*) pass "kernel is ELF32 / $MACHINE (multiboot1 x64 kernel — expected)" ;;
          *)       bad  "kernel is $CLASS / $MACHINE — x64 kernel must be ELF32" ;;
        esac ;;
      *AArch64*|*aarch64*)
        bad "kernel is $MACHINE — this is the ARM kernel! Stale-arch artifact: the shared hobbyos.elf was linked by an ARCH=arm build. Rebuild in a fresh tree (rm hobbyos.elf)." ;;
      *X86-64*|*x86-64*)
        bad "kernel is $MACHINE (ELF64) — HobbyOS x64 is multiboot1 ELF32; this ELF is not from linker_x64.ld" ;;
      "")
        bad "could not read the ELF header from hobbyos.elf" ;;
      *)
        bad "kernel arch is '$MACHINE' — not an x64 HobbyOS kernel" ;;
    esac
    info "type=$TYPE entry=${ENTRY:-?}"
    if [ -n "$ENTRY" ] && [ "$ENTRY" != "0x70000030" ]; then
      info "entry is $ENTRY (expected 0x70000030 for the x64 kernel)"
    fi
  else
    info "no llvm-readelf/readelf on PATH — relying on the raw magic checks below"
  fi

  # ---- 2. multiboot1 header in the first 8 KiB (Limine/multiboot contract) --
  MB_OK=0
  if command -v python3 >/dev/null 2>&1; then
    MB_OK=$(python3 - "$ELF" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read(8192)
# multiboot1: magic 0x1BADB002 (LE) at a 4-byte aligned offset < 8192
magic = b'\x02\xb0\xad\x1b'
print(1 if data.find(magic) != -1 else 0)
PY
)
  fi
  if [ "$MB_OK" = "1" ]; then
    pass "multiboot1 header found in the first 8 KiB"
  elif command -v python3 >/dev/null 2>&1; then
    bad "no multiboot1 magic (0x1BADB002) in the first 8 KiB — Limine cannot load this kernel"
  else
    info "python3 unavailable — skipped the multiboot1 magic check"
  fi
fi

# ---- 3. the disk image ------------------------------------------------------
if [ ! -f "$IMG" ]; then
  bad "disk.img missing from $DIR"
else
  IMG_MB=$(( $(wc -c < "$IMG") / 1048576 ))
  if [ "$IMG_MB" -lt 60 ]; then
    bad "disk.img is only ${IMG_MB} MiB — the FAT16 image should be ~1000 MiB"
  else
    pass "disk.img present (${IMG_MB} MiB)"
  fi

  # Prefer mtools (authoritative FAT listing); fall back to a raw-string probe.
  if command -v mdir >/dev/null 2>&1; then
    if mdir -i "$IMG" ::/boot 2>/dev/null | grep -qi 'hobbyos'; then
      pass "FAT /boot/hobbyos.elf present in the image (mdir)"
    else
      bad "no /boot/hobbyos.elf inside disk.img (mdir ::/boot)"
    fi
    # mdir prints 8.3 names with a space instead of the dot ("ICEWM    BIN"),
    # so match the stem only.
    if mdir -i "$IMG" ::/ 2>/dev/null | grep -qiE 'icewm|xhobby'; then
      pass "X11/icewm binaries staged at the FAT root"
    else
      info "no ICEWM/XHOBBY at the FAT root (x11-staged may have been skipped)"
    fi
  else
    if LC_ALL=C grep -qai 'limine.conf' "$IMG"; then
      pass "limine.conf string found in disk.img (raw probe; mtools not installed)"
    else
      info "mtools not installed and no limine.conf string found — image contents unverified"
    fi
  fi
fi

# ---- 4. boot log (when the builder ran the KVM boot check) ------------------
if [ -n "$BOOTLOG" ] && [ -f "$BOOTLOG" ]; then
  if grep -q 'Mode: DESKTOP' "$BOOTLOG"; then
    pass "boot log shows 'Mode: DESKTOP (icewm) - Launching X session'"
  else
    bad "boot log has no DESKTOP mode banner — guest did not reach the desktop"
  fi
  for m in ICEWM.BIN XHOBBY.BIN; do
    if grep -q "$m" "$BOOTLOG"; then pass "boot log: $m launched"; else bad "boot log: $m never launched"; fi
  done
  if grep -qiE 'kernel panic|PANIC:|FATAL' "$BOOTLOG"; then
    bad "boot log contains a panic/fatal fault"
    grep -iE 'PANIC|FATAL' "$BOOTLOG" | head -5 | sed 's/^/         /'
  else
    pass "no kernel panic / fatal fault in the boot log"
  fi
else
  info "no boot log supplied — boot behaviour unverified"
fi

if [ "$FAIL" = 0 ]; then
  echo "    => artifacts verified as a good x64 build"
  exit 0
fi
echo "    => verification FAILED"
exit 1
