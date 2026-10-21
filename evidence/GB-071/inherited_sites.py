"""Show the print_console sites where rdi is NOT set in the preceding window —
those are the candidates for the faulting call (rdi inherited)."""
import re, subprocess, sys

elf = sys.argv[1]
pc = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
txt = subprocess.run(["llvm-objdump", "-d", elf], capture_output=True, text=True).stdout
addr_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f ]+)\s*\t(.*)$")
insn = []
for l in txt.splitlines():
    m = addr_re.match(l)
    if m:
        insn.append((int(m.group(1), 16), m.group(3).strip()))

sites = [i for i, (a, t) in enumerate(insn) if "0x10000029e0" in t]
for i in sites:
    window = [t for _, t in insn[max(0, i - 10):i]]
    if any(re.match(r"movabsq\s+\$0x1[0-9a-f]+, %rdi", t) for t in window):
        continue
    a = insn[i][0]
    print(f"=== site 0x{a:x} (rdi not literal-set nearby)")
    for j in range(max(0, i - 14), min(len(insn), i + 4)):
        aa, tt = insn[j]
        mark = ">>" if j == i else "  "
        print(f" {mark} 0x{aa:x}: {tt}")
    print()
