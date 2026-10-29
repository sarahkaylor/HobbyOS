#!/usr/bin/env python3
"""GB-126 — assert every x86-64 exception stub pushes the right frame shape.

The trap.c dispatcher declares each exception_<n> stub through either
EXCEPTION_NO_ERR(n) (pushes a dummy 0 then the vector) or EXCEPTION_ERR(n)
(pushes the CPU's error code then the vector).  A NO_ERR stub for an exception
that really pushes an error code skews the trap frame by 8 bytes (the error
code prints as the vector — the GB-125/GB-126 failure class).

This is a host-side, table-driven check so a future exception cannot be added
with the wrong macro in silence.  It parses the actual dispatcher source and
compares every vector 0..255 declared there against the architectural truth.

Architectural error-code truth (Intel SDM, Volume 3A/3B):
  #DE #DB NMI #BP #OF #BR #UD #NM 0  #DF #TS #NP #SS #GP #PF      -> no code
  0   1   2    3   4   5   6   7  8  9   10  11  12  13  14      -> no code
  #MF(16) no; #AC(17) yes; #MC(18) no; #XM(19) no; #CP(21) yes;
  #VC(29) yes; #SX(30) yes.
Vectors 20, 22-28, 31 are reserved; if declared they are used as no-code here.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# True = the vector pushes an error code.
ARCH_HAS_ERROR = {}
for v in range(0, 32):
    ARCH_HAS_ERROR[v] = v in (8, 10, 11, 12, 13, 14, 17, 21, 29, 30)

TRAP_C = ROOT / "src/kernel/arch/x64/trap.c"

def parse_dispatch(text):
    """Return {vector: 'ERR'|'NO_ERR'} from the EXCEPTION_*() invocations."""
    result = {}
    # Split into statements, each like: EXCEPTION_NO_ERR(21);
    pat = re.compile(r"EXCEPTION_(NO_ERR|ERR)\(\s*(\d+)\s*\);")
    for m in pat.finditer(text):
        shape, num = m.group(1), int(m.group(2))
        result[num] = "ERR" if shape == "ERR" else "NO_ERR"
    return result

def main():
    if not TRAP_C.exists():
        print(f"FAIL: {TRAP_C} not found (run from the repo root)")
        return 1
    text = TRAP_C.read_text()
    decl = parse_dispatch(text)
    failures = 0
    checks = 0
    # vectors 0..255 that are actually declared by the dispatcher
    declared = sorted(decl)
    for v in declared:
        want = "ERR" if ARCH_HAS_ERROR.get(v, False) else "NO_ERR"
        got = decl[v]
        checks += 1
        if got != want:
            print(f"  FAIL vector {v:3d}: stub pushes {'an error code' if got=='ERR' else 'a dummy 0'}, "
                  f"architecture needs {'an error code' if want=='ERR' else 'a dummy 0'}")
            failures += 1
        else:
            print(f"  PASS vector {v:3d}: {got} matches architecture")
    # The three vectors this card fixes must be correct (regression guard).
    for v in (21, 29, 30):
        checks += 1
        if decl.get(v) != "ERR":
            print(f"  FAIL vector {v}: GB-126 requires EXCEPTION_ERR")
            failures += 1
    print(f"vector_shape_scan: {checks} checks, {failures} FAIL")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
