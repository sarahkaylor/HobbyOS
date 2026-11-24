#!/bin/bash
# tools/nv/el0_wave.sh -- boot ONE program (the WP-EL0 DRM-ABI probe) under
# MODE=test and capture the serial log.
#
# Why this exists: /dev/nvshim's DRM face was proved only by kernel unit tests,
# which call the handlers with kernel pointers.  This runs the same ABI from
# ring 3 with a real syscall, a real user VA and a real fd.  The vehicle is a
# static-PIE x86_64 Linux-ABI ELF (NVABI.ELF) because /dev/nvshim's requests are
# reachable only through the compat ioctl(16) route.
#
# Usage: tools/nv/el0_wave.sh [--skip-build] [--timeout SECS] [--out FILE]
# Env:   ARCH (intel), NVABI_ELF, T3-style overrides unused.
# Exit:  0 = the program printed "[EL0] RESULT PASS", 1 = it failed or never ran,
#        2 = environment/build failure (never a code verdict).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ARCH="${ARCH:-intel}"
SKIP_BUILD=0
TO=240
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
  --skip-build) SKIP_BUILD=1; shift ;;
  --timeout) TO="$2"; shift 2 ;;
  --out) OUT="$2"; shift 2 ;;
  -h | --help) sed -n '2,15p' "$0"; exit 0 ;;
  *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

say() { echo "[el0] $*"; }
ELF="${NVABI_ELF:-$ROOT/obj/$ARCH/nvabi_probe.elf}"
[ -n "$OUT" ] || OUT="$ROOT/evidence/nv/el0-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$OUT")"

case ":$PATH:" in
*:/usr/sbin:* | *:/sbin:*) ;;
*) PATH="/usr/sbin:/sbin:$PATH"; export PATH ;;
esac
for t in mcopy mkfs.fat qemu-system-x86_64 readelf; do
  command -v "$t" >/dev/null 2>&1 || { say "ENVIRONMENT: missing $t"; exit 2; }
done

if [ "$SKIP_BUILD" != "1" ]; then
  say "building $ELF"
  make ARCH="$ARCH" TOOLS_SKIP=1 BROWSER_SKIP=1 nvabi_elf >"$ROOT/obj/el0_elf.log" 2>&1 ||
    { say "BUILD FAILED (nvabi_elf)"; tail -20 "$ROOT/obj/el0_elf.log"; exit 2; }
  say "building kernel + disk (MODE=test)"
  make -j"$(nproc 2>/dev/null || echo 8)" ARCH="$ARCH" MODE=test LO_ON_DISK= \
    TOOLS_SKIP=1 BROWSER_SKIP=1 hobbyos.elf disk.img >"$ROOT/obj/el0_build.log" 2>&1 ||
    { say "BUILD FAILED (kernel/disk)"; tail -30 "$ROOT/obj/el0_build.log"; exit 2; }
fi
[ -f "$ELF" ] || { say "ENVIRONMENT: $ELF absent"; exit 2; }
[ -f disk.img ] || { say "ENVIRONMENT: disk.img absent"; exit 2; }

# One program per boot, so every console row is attributable to it.  The stage
# runs through the Makefile's own RUN_STAGE hook, i.e. AFTER `run`'s prerequisite
# build and BEFORE QEMU -- staging from here would be undone, because `run`
# depends on disk.img and re-runs its dd+mkfs recipe whenever the kernel was
# relinked (measured: the first attempt staged, then watched the whole default
# wave run because mkfs had wiped the manifest).
printf 'NVABI.ELF\n' >/tmp/el0-wave.cfg
STAGE="mcopy -o -i disk.img '$ELF' ::/NVABI.ELF && mcopy -o -i disk.img /tmp/el0-wave.cfg ::/WAVE.CFG"
say "staged /WAVE.CFG = NVABI.ELF ($(stat -c%s "$ELF" 2>/dev/null || echo '?') B)"

KVM=""
[ -e /dev/kvm ] && KVM="-enable-kvm"
say "booting (deadline ${TO}s, log $OUT)"
( timeout "$TO" make ARCH="$ARCH" MODE=test TOOLS_SKIP=1 BROWSER_SKIP=1 \
    RUN_STAGE="$STAGE" run QEMU_ARGS="-display none $KVM" ) >"$OUT" 2>&1
rc=$?
if [ "$rc" = "124" ]; then
  say "TIMEOUT after ${TO}s (a hang is a defect, not a pass)"
fi

say "--- console rows ---"
grep -a '^\[EL0\]' "$OUT" || echo "   (no [EL0] rows at all: the program never ran)"
say "--- kernel rows for this client ---"
grep -a '^\[NVSHIM\]' "$OUT" | head -40
say "--- loader rows ---"
grep -aE '^\[ARRIVED\]|^\[WAVE-TRY\]|v2 loader' "$OUT" | tail -5

if grep -aq '^\[EL0\] RESULT PASS' "$OUT"; then
  say "PASS"
  exit 0
fi
say "FAIL"
exit 1
