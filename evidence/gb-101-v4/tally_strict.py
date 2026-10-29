import glob, re, os

# Strict tally: only INTACT lines count. The console is shredded by concurrent
# writers, so a lenient regex reads "Vector: 142" out of `Vector: 14` interleaved
# with other output -- that is not evidence of an impossible vector.
DETAIL = re.compile(r"^\[KERNEL\] FATAL kernel-mode fault, halting this CPU: vector=(\d+) rip=0x([0-9a-fA-F]+) .*cr2=0x")
BANNER = re.compile(r"^\[KERNEL\] FATAL: Exception in Kernel Mode! Vector: (\d+) Error Code: 0x[0-9a-fA-F]{16}\r?$")


def classify(vectors):
    imp = [v for v in vectors if v > 255]
    pf = [v for v in vectors if v == 14]
    other = [v for v in vectors if v <= 255 and v != 14]
    return imp, pf, other


print(f"{'wave':10} {'intact':>6} {'impsbl':>7} {'vec14':>6} {'other':>6} {'halt':>4}  detail-vectors")
tot_imp = 0
for f in sorted(glob.glob("evidence/gb-101-v4/wave*.serial.log")):
    lines = open(f, "rb").read().decode("latin-1").splitlines()
    det = [int(m.group(1)) for m in (DETAIL.match(l) for l in lines) if m]
    ban = [int(m.group(1)) for m in (BANNER.match(l) for l in lines) if m]
    imp, pf, other = classify(det + ban)
    tot_imp += len(imp)
    tl = f.replace(".serial.log", ".log")
    halted = "?"
    if os.path.exists(tl):
        mm = re.search(r"halted=(\d)", open(tl, errors="replace").read())
        halted = mm.group(1) if mm else "?"
    print(f"{os.path.basename(f)[:10]:10} {len(det)+len(ban):6d} {len(imp):7d} {len(pf):6d} {len(other):6d} {halted:>4}  {det}")
print()
print("impossible-vector intacts across all waves:", tot_imp)