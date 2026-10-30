#!/usr/bin/env python3
"""panic_sink_allowlist_test.py — GB-097: the raw console sink is bannable.

`panic_puts` / `panic_putc` / `panic_print_hex` write COM1 with **no lock**, so
two writers interleave bytes inside one record and both records are destroyed
(GB-097's defect: 113 shredded records in a single intel wave).  They exist
because a lock cannot be taken on a path that may never return — so the rule is
not "take the lock", it is "these symbols belong to NON-RETURNING paths only".

A naming convention nobody checks is a convention that decays.  This test makes
it checkable: every reference to the three symbols must be accounted for by an
explicit (file, expected count) entry below.  A NEW reference anywhere — a new
caller in an allowlisted file included — fails the suite, so a returning caller
cannot be added without someone editing this table and saying why.

The table is deliberately counts, not a file-level yes/no: "mmu.c may use the
raw sink" is what let GB-097 happen.  "mmu.c may use it 7 times, for these
reasons" is a claim that goes stale when someone adds an eighth.

Usage:
  python3 tools/gate/panic_sink_allowlist_test.py          # grade the tree
  python3 tools/gate/panic_sink_allowlist_test.py --self-test
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYMBOLS = re.compile(r"\b(panic_puts2?|panic_putc2?|panic_print_hex2?)\b")

# file -> (expected references, why a non-returning use is legitimate here)
ALLOW = {
    # ---- the sink itself: definitions + the violation reporter ----
    "src/kernel/arch/x64/uart.c": (15, "definitions, panic_enter/leave, violation report"),
    "src/kernel/arch/arm/uart.c": (15, "arm definitions; arm has no COM2 violation channel"),
    "src/host/locksteal_test.c": (1, "host-test shim that stands in for the sink"),

    # ---- non-returning diagnostics: they end in halt/reboot by construction ----
    "src/kernel/arch/x64/trap.c": (172, "fatal dump, stall watchdog, spinlock scream, KPFLOOP"),
    "src/kernel/arch/x64/locks.c": (13, "spinlock scream paths: printed while the lock is wedged"),
    "src/kernel/arch/arm/locks.c": (36, "same, arm"),

    # ---- RETURNING paths: these are the GB-097 residue, not an endorsement ----
    "src/kernel/arch/x64/mmu.c": (7, "FREERACE diagnostic inside v2 free -- RETURNS (GB-097 residue)"),
    "src/kernel/fs.c": (2, "fs diagnostic -- RETURNS (GB-097 residue)"),
    "src/kernel/main.c": (11, "hex/decimal helpers -- RETURN (GB-097 residue)"),
    "src/kernel/process.c": (39, "resume-RETURNED / spawn diagnostics -- RETURN (GB-097 residue)"),
}

RESIDUE = {"src/kernel/arch/x64/mmu.c", "src/kernel/fs.c",
           "src/kernel/main.c", "src/kernel/process.c"}


def scan(root):
    counts = {}
    for base, _dirs, files in os.walk(os.path.join(root, "src")):
        for fn in files:
            if not fn.endswith((".c", ".h")):
                continue
            p = os.path.join(base, fn)
            rel = os.path.relpath(p, root)
            with open(p, encoding="utf-8", errors="replace") as fh:
                n = len(SYMBOLS.findall(fh.read()))
            if n:
                counts[rel] = n
    return counts


def grade(counts, allow):
    problems = []
    for path, n in sorted(counts.items()):
        if path not in allow:
            problems.append(f"{path}: {n} reference(s) and no allowlist entry")
            continue
        want, why = allow[path]
        if n != want:
            problems.append(f"{path}: {n} reference(s), allowlist says {want} ({why})")
    for path in sorted(allow):
        if path not in counts:
            problems.append(f"{path}: allowlisted for {allow[path][0]} reference(s), found none")
    return problems


def main(argv):
    if "--self-test" in argv:
        # a known-good tree, then three known-bad ones; the grader must catch all
        base = {"src/kernel/arch/x64/trap.c": 136}
        allow = {"src/kernel/arch/x64/trap.c": (136, "non-returning")}
        ok = grade(base, allow) == []
        new_file = grade({**base, "src/kernel/newdrv.c": 2}, allow)
        grown = grade({**base, "src/kernel/arch/x64/trap.c": 137}, allow)
        shrunk = grade({"src/kernel/arch/x64/trap.c": 135}, allow)
        cases = [
            ("clean tree grades clean", ok),
            ("a new caller in a new file fails", any("newdrv" in p for p in new_file)),
            ("an 8th call in an allowlisted file fails", grown != []),
            ("a deleted call fails (the table is stale)", shrunk != []),
        ]
        for name, cond in cases:
            print(f"  {'PASS' if cond else 'FAIL'}: {name}")
        ok_all = all(c for _, c in cases)
        print("self-test: " + ("OK" if ok_all else "FAILED"))
        return 0 if ok_all else 1

    counts = scan(ROOT)
    problems = grade(counts, ALLOW)
    total = sum(counts.values())
    residue = sum(n for p, n in counts.items() if p in RESIDUE)
    print(f"panic_sink_allowlist: {total} reference(s) in {len(counts)} file(s); "
          f"{residue} from RETURNING paths (GB-097 residue)")
    for path, n in sorted(counts.items()):
        tag = "RESIDUE" if path in RESIDUE else "ok"
        print(f"  {n:4d}  {path}  [{tag}]")
    if problems:
        for p in problems:
            print("  VIOLATION: " + p)
        print("panic_sink_allowlist: FAIL")
        return 1
    print("panic_sink_allowlist: PASS — every reference is accounted for")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))