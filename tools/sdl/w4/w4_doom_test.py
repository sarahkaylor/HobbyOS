#!/usr/bin/env python3
"""T1 host test for the GB-160 W4 DOOM static-musl port surface (parent GB-041).

The deliverable is a **static-PIE** x86-64 musl ELF (`Type: DYN
(Position-Independent Executable`), no `PT_INTERP`, no dynamic loader, carrying
the DG_* port surface.  That is the class the HobbyOS v2 loader accepts:
`elf_loader.c:124` refuses an `ET_EXEC` linked below the user window, so a
`-no-pie` build is not a valid W4 artifact.  Runtime *playability* in-guest is
the compat gate (GB-033/GB-031) — this is the build/T1 level only.

The gate counts FAILURES: it prints `passed/total` and exits 0 only when every
check passes.  `--artifact PATH` grades an existing ELF instead of building one.
`--self-test` is the falsifier: it grades a synthetic `-no-pie` ET_EXEC and
`/bin/true` and asserts the gate rejects both, so a gate that cannot fail is
itself a test failure.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def grade(elf, quiet=False):
    """Return (checks, failed): a list of (name, ok) and the failed names."""
    checks = []

    def check(name, cond):
        checks.append((name, bool(cond)))
        if not quiet:
            print(f"  {'PASS' if cond else 'FAIL'}: {name}")

    if not os.path.exists(elf):
        check("artifact exists", False)
        return checks, ["artifact exists"]

    check("artifact exists", True)
    head = run(["readelf", "-h", elf]).stdout
    check("ELF64 x86-64 (Machine Advanced Micro Devices X86-64)",
          "Machine:" in head and "X86-64" in head)
    check("loader-accepted class: Type DYN (Position-Independent Executable",
          "DYN (Position-Independent Executable" in head)
    check("not ET_EXEC (the v2 loader refuses it below the window)",
          not re.search(r"Type:\s+EXEC\b", head))
    prog = run(["readelf", "-l", elf]).stdout
    check("no PT_INTERP (static: no dynamic loader dependency)", "INTERP" not in prog)
    desc = run(["file", elf]).stdout
    check("statically linked (static-pie, no interpreter)",
          "static-pie linked" in desc or "statically linked" in desc)
    syms = run(["llvm-nm", elf]).stdout
    for sym in ("main", "doomgeneric_Create", "DG_Init", "DG_DrawFrame"):
        check(f"port surface symbol {sym} present", f" {sym}" in syms)
    return checks, [n for n, ok in checks if not ok]


def self_test():
    """The falsifier: an ET_EXEC and a dynamic EXEC must both be rejected."""
    ok = True

    def say(case, passed, detail):
        nonlocal ok
        ok = ok and passed
        print(f"  {'PASS' if passed else 'FAIL'}: {case} ({detail})")

    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "e.c")
        exe = os.path.join(d, "et_exec")
        open(src, "w").write("int main(void){return 0;}\n")
        r = run(["cc", "-no-pie", "-static", "-o", exe, src])
        if r.returncode != 0:
            say("build a -no-pie ET_EXEC to test the class clause", False, r.stderr.strip()[:80])
        else:
            _, failed = grade(exe, quiet=True)
            say("gate rejects a static ET_EXEC",
                "not ET_EXEC (the v2 loader refuses it below the window)" in failed,
                "NO FAILURES" if not failed else ", ".join(failed[:2]))

        _, failed = grade("/bin/true", quiet=True)
        say("gate rejects a dynamic EXEC (/bin/true)", bool(failed), ", ".join(failed[:2]) or "NO FAILURES")

    print(f"\nW4 gate self-test: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--artifact", help="grade this ELF instead of building one")
    ap.add_argument("--self-test", action="store_true", help="assert the gate rejects bad artifacts")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    with tempfile.TemporaryDirectory() as build_dir:
        if args.artifact:
            elf = args.artifact
        else:
            r = run(["bash", "tools/sdl/w4/build-doom-w4-musl.sh", build_dir])
            elf = os.path.join(build_dir, "DoomW4.elf")
            if r.returncode != 0 or not os.path.exists(elf):
                print(r.stdout[-2000:])
                print(r.stderr[-2000:])
        checks, failed = grade(elf)

    total = len(checks)
    print(f"\nW4 DOOM static-musl port surface: {total - len(failed)}/{total} passed -> "
          f"{'PASS' if not failed else 'FAIL'}")
    for n in failed:
        print(f"  failed: {n}")
    if total < 8:
        print(f"FAIL: expected at least 8 checks, ran {total}")
        return 1
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
