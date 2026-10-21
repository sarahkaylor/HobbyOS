"""Print each 'movabsq $print_console' site with the preceding 8 instructions,
so we can see how rdi (the string argument) is formed at each call."""
import re, subprocess, sys

elf = sys.argv[1] if len(sys.argv) > 1 else "obj/intel/shtest.elf"
txt = subprocess.run(["llvm-objdump", "-d", elf], capture_output=True, text=True).stdout
lines = txt.splitlines()
addr_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f ]+)\s*\t(.*)$")
insn = []
for l in lines:
    m = addr_re.match(l)
    if m:
        insn.append((int(m.group(1), 16), m.group(3).strip()))

hits = [i for i, (a, t) in enumerate(insn) if "print_console" in t or "0x10000029e0" in t]
print(f"{len(hits)} sites referencing print_console in {elf}\n")
for i in hits:
    a, t = insn[i]
    print(f"--- site at 0x{a:x}: {t}")
    for j in range(max(0, i - 8), i + 3):
        aa, tt = insn[j]
        mark = ">>" if j == i else "  "
        print(f" {mark} 0x{aa:x}: {tt}")
    print()
