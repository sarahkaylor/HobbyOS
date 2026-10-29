import glob, re, os

DETAIL = re.compile(r"FATAL kernel-mode fault, halting this CPU: vector=(\d+)")
BANNER = re.compile(r"FATAL: Exception in Kernel Mode! Vector: (\d+)")

rows = []
for f in sorted(glob.glob("evidence/gb-101-v4/wave*.serial.log")):
    txt = open(f, "rb").read().decode("latin-1")
    det = [int(v) for v in DETAIL.findall(txt)]
    ban = [int(v) for v in BANNER.findall(txt)]
    allv = det + ban
    impossible = [v for v in allv if v > 255]
    pf = [v for v in allv if v == 14]
    other = [v for v in allv if v <= 255 and v != 14]
    tl = f.replace(".serial.log", ".log")
    halted, summary = "?", ""
    if os.path.exists(tl):
        t = open(tl, errors="replace").read()
        m = re.search(r"T3_intel: \d+ console lines.*?halted=\d", t)
        if m:
            summary = m.group(0)
            h = re.search(r"halted=(\d)", m.group(0))
            halted = h.group(1) if h else "?"
    rows.append((os.path.basename(f), len(allv), len(impossible), len(pf), len(other), halted, summary))

print(f"{'serial':28} {'fatal':>6} {'impsbl':>7} {'vec14':>6} {'other':>6} {'halt':>5}")
for r in rows:
    print(f"{r[0]:28} {r[1]:6d} {r[2]:7d} {r[3]:6d} {r[4]:6d} {r[5]:>5}")
print()
for r in rows:
    if r[6]:
        print(r[0], "::", r[6])
print()
print("impossible-vector (this patch's business):",
      [r[2] for r in rows], "total", sum(r[2] for r in rows))
print("vector 14 (GB-122's defect, counted not blamed):",
      [r[3] for r in rows], "total", sum(r[3] for r in rows))