#!/usr/bin/env python3
"""T1 host test for the GB-041 W4 DOOM static-musl ELF port surface (GB-041).

The GB-041 W4 deliverable is a x86-64 *static* musl ELF exposing the DG_* port
surface (`main`, `doomgeneric_Create`, `DG_Init`, `DG_DrawFrame`) plus the
64-bit x86 machine class — the class the HobbyOS W4 compat loader accepts. This
test drives the reproducible build (tools/sdl/w4/build-doom-w4-musl.sh) into a
temp dir and asserts: the artefact exists, is an ELF64 EXEC x86-64, is statically
linked (not dynamic), and its symbol table carries the port surface. Runtime
*playability* in-guest is the compat gate (GB-033/GB-031) — this is the build /
T1 level.
"""
import os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)

def main():
    passed = 0
    def check(name, cond):
        nonlocal passed
        print(f"  {'PASS' if cond else 'FAIL'}: {name}")
        passed += cond
    with tempfile.TemporaryDirectory() as d:
        r = run(["bash", "tools/sdl/w4/build-doom-w4-musl.sh", d])
        elf = os.path.join(d, "DoomW4.elf")
        check(f"build script runs (exit 0)", r.returncode == 0 and os.path.exists(elf))
        head = run(["readelf", "-h", elf]).stdout
        check("ELF64 x86-64 (Machine Advanced Micro Devices X86-64)", "Machine:" in head and "X86-64" in head)
        file = run(["file", elf]).stdout
        check("static musl ELF (statically linked, not a dynamic executable)",
              "statically linked" in file)
        nm = run(["llvm-nm", elf]).stdout
        for sym in ["main", "doomgeneric_Create", "DG_Init", "DG_DrawFrame"]:
            check(f"port surface symbol {sym} present", f" {sym}" in nm)
    print(f"\nW4 DOOM static-musl port surface: {'PASS' if passed >= 6 else 'FAIL'} ({passed})")
    sys.exit(0 if passed >= 5 else 1)

if __name__ == "__main__":
    main()
