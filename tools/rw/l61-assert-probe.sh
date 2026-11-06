#!/usr/bin/env bash
# M6/L61: a probe build of the game's Mono runtime whose two "unreachable"
# sites trap (#UD) instead of printing and aborting, so the KERNEL's [KREG] row
# carries the registers the message cannot.
#
#   site A  0xf5670  x86-codegen.h:403  default: g_assert (0) in
#                    mono_x86_patch_inline()  -- rdi = code being patched
#   site B  0x1125ee tramp-amd64.c:641  mono_arch_create_specific_trampoline()
#                    far-rel32 assert          -- rax = code, rbx = target-code
#
# Both are `lea <file>,%rdi` (7 bytes); replacing them with `ud2` + 5 nops
# leaves every other byte alone and makes the fault a user #UD, which the
# kernel reports with `[KREG] ... rip/rsp/rax..r15`.
#
# usage: tools/rw/l61-assert-probe.sh [out.so]
#   RW_MONO_SO=$PWD/obj/l61/libmonobdwgc.ud2.so \
#     tools/rw/run-rimworld-desktop.sh --run-timeout 600     # site A (JIT)
#   RW_INTERP=1 RW_MONO_SO=... (site B, when the interpreter is forced)
#
# The shared export is never modified: the harness stages RW_MONO_SO in place
# of /srv/nfs/rimworld/.../libmonobdwgc-2.0.so.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/rw/lane.sh
. "$ROOT/tools/rw/lane.sh"
SRC="${RW_MONO_EXPORT:-$RW_EXPORT/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so}"
OUT="${1:-$ROOT/obj/l61/libmonobdwgc.ud2.so}"
[ -f "$SRC" ] || { echo "missing $SRC"; exit 2; }
mkdir -p "$(dirname "$OUT")"
cp -f "$SRC" "$OUT"
python3 - "$OUT" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
LEA = bytes.fromhex('488d3d')          # lea disp32(%rip),%rdi
for off, name in ((0xf5670, 'x86-codegen.h:403'), (0x1125ee, 'tramp-amd64.c:641')):
    have = bytes(d[off:off + 7])
    if have[:3] != LEA:
        sys.exit('site %s at 0x%x is %s, not a lea: wrong build?' % (name, off, have.hex()))
    print('%-20s 0x%x  %s -> 0f0b9090909090' % (name, off, have.hex()))
    d[off:off + 7] = bytes.fromhex('0f0b9090909090')
open(p, 'wb').write(d)
PY
md5sum "$SRC" "$OUT"
