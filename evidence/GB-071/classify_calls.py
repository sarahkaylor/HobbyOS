"""Classify each print_console call site by how rdi is formed in the ~10 instrs before it."""
import re, subprocess, sys

elf = sys.argv[1]
txt = subprocess.run(["llvm-objdump", "-d", elf], capture_output=True, text=True).stdout
addr_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f ]+)\s*\t(.*)$")
insn = []
for l in txt.splitlines():
    m = addr_re.match(l)
    if m:
        insn.append((int(m.group(1), 16), m.group(3).strip()))

sites = [i for i, (a, t) in enumerate(insn) if "0x10000029e0" in t]
cat = {"literal": 0, "from_memory": 0, "no_rdi_set": 0, "other": 0}
detail = []
for i in sites:
    window = [t for _, t in insn[max(0, i - 10):i]]
    if any(re.match(r"movabsq\s+\$0x1[0-9a-f]+, %rdi", t) for t in window):
        cat["literal"] += 1
    elif any(re.match(r"(movq|leaq)\s+.*, %rdi", t) for t in window):
        cat["from_memory"] += 1
        detail.append((hex(insn[i][0]), [t for t in window if "%rdi" in t]))
    elif any("%rdi" in t for t in window):
        cat["other"] += 1
    else:
        cat["no_rdi_set"] += 1

print(elf, "call sites:", len(sites), cat)
for d in detail:
    print("  from_memory site", d[0], "->", d[1][-2:])
