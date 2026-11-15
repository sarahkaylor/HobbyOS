#!/usr/bin/env bash
# tools/cp/exec/exec_gate.sh — linux_probe_exec with glibc 2.31 children staged
# (cp/exec, docs/cp-exec.md).  The plain probe (t3_linux_probe.sh exec) execs
# musl copies of itself; this boot also stages, on the FAT16 disk:
#   /XHS.ELF   glibc -static-pie child (R_X86_64_IRELATIVE)
#   /XHD.ELF   glibc dynamic child, PT_INTERP /lib64/ld-linux-x86-64.so.2
#   /lib64/ld-linux-x86-64.so.2, /LIB64/libc.so.6   the 2.31 runtime
# and runs the probe with argv[1]=glibc, which makes those rows mandatory.
# Every boot under flock /tmp/cp-qemu.lock.  Env: GLIBC_SYSROOT (default
# ~/cp/sysroot-glibc231), LP_TIMEOUT (default 420), CPDATA (optional).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
SYSR="${GLIBC_SYSROOT:-$HOME/cp/sysroot-glibc231}"
D="$ROOT/obj/exec-gate"; rm -rf "$D"; mkdir -p "$D"
src="$ROOT/tools/cp/exec/exec_child.c"
clang --sysroot="$SYSR" -fuse-ld=lld -O1 -static-pie -fPIE -o "$D/xhs" "$src" || { echo "exec_gate: static-pie build failed"; exit 2; }
clang --sysroot="$SYSR" -fuse-ld=lld -O1 -fPIE -pie \
  -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 -o "$D/xhd" "$src" || { echo "exec_gate: dynamic build failed"; exit 2; }
make -C "$ROOT" ARCH=intel obj/intel/probe_exec.elf >/dev/null || { echo "exec_gate: probe build failed"; exit 2; }
LDSO="$(readlink -f "$SYSR/lib64/ld-linux-x86-64.so.2")"
LIBC="$(readlink -f "$SYSR/lib/x86_64-linux-gnu/libc.so.6")"
printf 'PROBE.ELF glibc\n' > "$D/wave.cfg"
STAGE="mmd -i \$ROOT/disk.img ::/lib64 ::/LIB64 2>/dev/null; \
mcopy -o -i \$ROOT/disk.img $ROOT/obj/intel/probe_exec.elf ::PROBE.ELF && \
mcopy -o -i \$ROOT/disk.img $D/xhs ::XHS.ELF && mcopy -o -i \$ROOT/disk.img $D/xhd ::XHD.ELF && \
mcopy -o -i \$ROOT/disk.img $LDSO ::/lib64/ld-linux-x86-64.so.2 && \
mcopy -o -i \$ROOT/disk.img $LIBC ::/LIB64/libc.so.6 && \
mcopy -o -i \$ROOT/disk.img $D/wave.cfg ::WAVE.CFG"
T3_TIMEOUT="${LP_TIMEOUT:-420}" T3_STAGE="$STAGE" T3_MIN_LINES=1 TOOLS_SKIP=1 \
  flock /tmp/cp-qemu.lock bash "$ROOT/tools/t3_tier.sh" intel > "$D/tier.log" 2>&1
CONSOLE="$(sed -n 's/^T3_LOG=//p' "$D/tier.log" | tail -1)"
[ -r "$CONSOLE" ] || { echo "exec_gate: FAIL — no console"; tail -20 "$D/tier.log"; exit 1; }
cp "$CONSOLE" "$D/console.log"
grep -a '^\[LP\]\|^\[EXEC\]' "$D/console.log"
if grep -aq '^\[LP\] done rc=0' "$D/console.log" && \
   grep -aq '^\[LP\] ok   execve glibc static-pie' "$D/console.log" && \
   grep -aq '^\[LP\] ok   execve glibc dynamic' "$D/console.log"; then
  echo "exec_gate: PASS"; exit 0
fi
echo "exec_gate: FAIL"; exit 1
