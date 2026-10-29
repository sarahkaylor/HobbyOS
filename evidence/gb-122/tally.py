import glob, re, os

rows = []
for f in sorted(glob.glob("evidence/gb-122/fix-run*.serial.log")):
    txt = open(f, "rb").read().decode("latin-1")
    fat = re.findall(r"FATAL kernel-mode fault, halting this CPU: vector=(\d+) rip=0x([0-9A-Fa-f]+) cr2=0x([0-9A-Fa-f]+)", txt)
    stack_top = [c for v, r, c in fat if 0x17FFFFF000 <= int(c, 16) < 0x1800000000]
    text_log = f.replace(".serial.log", ".log")
    verdict = ""
    if os.path.exists(text_log):
        v = open(text_log, errors="replace").read()
        m = re.search(r"T3_intel: \d+ console lines.*?halted=\d", v)
        verdict = m.group(0) if m else ""
        h = re.search(r"halted=(\d)", verdict)
        halted = h.group(1) if h else "?"
    else:
        halted = "?"
    rows.append((os.path.basename(f), len(fat), len(stack_top), halted, verdict))

print(f"{'serial':32} {'fatals':>6} {'top-stk':>7} {'halted':>6}")
for name, n, s, h, _ in rows:
    print(f"{name:32} {n:6d} {s:7d} {h:>6}")
print()
for name, n, s, h, v in rows:
    if v:
        print(name, "::", v)