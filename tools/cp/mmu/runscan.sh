#!/bin/bash
# tools/cp/mmu/runscan.sh intel|arm [extra -D...]
#
# Run castscan.py (docs/cp-address-space.md Appendix A.1) over every kernel TU
# for one arch and print one row per int<->pointer cast site:
#   file:line <TAB> I2P|P2I <TAB> C|implicit <TAB> type <TAB> TU
# Needs clang 21 (linuxci).  The flags are the kernel CFLAGS from the Makefile
# (intel / arm branches) minus warnings and debug info.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
ARCH=${1:?usage: runscan.sh intel|arm [extra flags]}; shift
EXTRA="$*"
if [ "$ARCH" = intel ]; then
  CF="-O2 -Isrc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -fstack-protector-strong -mstack-protector-guard=global"
  AD=src/kernel/arch/x64
else
  CF="-O2 -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -fstack-protector-strong -mstack-protector-guard=global"
  AD=src/kernel/arch/arm
fi
ls src/kernel/*.c $AD/*.c | xargs -P "${SCAN_JOBS:-10}" -I{} sh -c \
  "clang $CF $EXTRA -fsyntax-only -w -Xclang -ast-dump -fno-color-diagnostics {} 2>/dev/null | python3 tools/cp/mmu/castscan.py src/ {}"
