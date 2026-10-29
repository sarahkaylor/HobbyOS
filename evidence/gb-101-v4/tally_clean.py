import glob, re, os

# Only a WHOLE clean line counts as evidence of a vector. The console is shredded
# by concurrent writers, so "1879291950" or "142" can be digits of two lines run
# together -- requiring the full expected tail is what separates a torn frame from
# a shredded printout.
DETAIL = re.compile(
    r"^\[KERNEL\] FATAL kernel-mode fault, halting this CPU: vector=(\d+) "
    r"rip=0x[0-9a-fA-F]{16} cr2=0x[0-9a-fA-F]{16} pid=\d+ err=\d+ "
    r"cr3=0x[0-9a-fA-F]{16} pd480=0x[0-9a-fA-F]{16}\r?$")
BANNER = re.compile(
    r"^\[KERNEL\] FATAL: Exception in Kernel Mode! Vector: (\d+) "
    r"Error Code: 0x[0-9a-fA-F]{16}\r?$")

tot_imp = 0
print(f"{'wave':12} {'clean':>6} {'impsbl':>7} {'vec14':>6} {'other':>6} {'halt':>4}  vectors")
for f in sorted(glob.glob("evidence/gb-101-v4/wave*.serial.log")):
    raw = open(f, "rb").read().decode("latin-1").splitlines()
    det = [int(m.group(1)) for m in (DETAIL.match(l) for l in raw) if m]
    ban = [int(m.group(1)) for m in (BANNER.match(l) for l in raw) if m]
    allv = det + ban
    imp = [v for v in allv if v > 255]
    pf = [v for v in allv if v == 14]
    other = [v for v in allv if v <= 255 and v != 14]
    tot_imp += len(imp)
    tl = f.replace(".serial.log", ".log")
    halted = "?"
    if os.path.exists(tl):
        mm = re.search(r"halted=(\d)", open(tl, errors="replace").read())
        halted = mm.group(1) if mm else "?"
    print(f"{os.path.basename(f)[:12]:12} {len(allv):6d} {len(imp):7d} {len(pf):6d} {len(other):6d} {halted:>4}  {allv}")

print()
print("impossible-vector on CLEAN lines, all waves:", tot_imp)