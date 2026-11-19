#!/usr/bin/env python3
"""lp_check_pfstat.py <serial.log> -- linux_probe_pfstat's console check
(H7-fault, docs/cp-fault.md §2).  The kernel's [PF*] rows, summed over the
boot, must count the probe's faults: anon >= 4096 + 64, cow >= 256, file-backed
(fat/ext2/nfs) >= 8; each [PFK] row's kinds must add up to its n=, each [PFH]
histogram to the matching [PFK] n= (within 2: live counters); frames must have been zeroed
([PFL] zero=), vm_lock pairs counted (vml=) and #PF traps timed ([PFI] pf=).
Exit 0 = pass, 1 = fail, 2 = no run."""
import re
import sys

KINDS = ("anon", "shobj", "nfs", "ext2", "fat", "img", "cow", "priv", "retry", "err")
text = open(sys.argv[1], "rb").read().decode("latin-1")
if "[LP] A1 " not in text:
    print("lp_check_pfstat: no '[LP] A1' row (the probe did not run)")
    sys.exit(2)
lines = text.splitlines()
pfk = [ln[ln.index("[PFK] "):] for ln in lines if "[PFK] t=" in ln]
pfh = {}
for ln in lines:
    if "[PFH] t=" in ln:
        m = re.search(r"\[PFH\] t=(\d+) all=([\d,]+)", ln)
        if m:
            pfh[m.group(1)] = [int(x) for x in m.group(2).split(",")]
tot = {k: 0 for k in KINDS}
bad = []
for ln in pfk:
    t = re.search(r" t=(\d+)", ln).group(1)
    n = int(re.search(r" n=(\d+)", ln).group(1))
    s = 0
    for k in KINDS:
        m = re.search(r" %s=(\d+)/" % k, ln)
        if m:
            tot[k] += int(m.group(1))
            s += int(m.group(1))
    # the emitter reads live per-CPU counters: a fault that ends on another
    # CPU mid-read can move one count across the interval boundary
    if s != n:
        bad.append(f"kinds {s} != n={n} at t={t}")
    if t in pfh and abs(sum(pfh[t]) - n) > 2:
        bad.append(f"histogram {sum(pfh[t])} != n={n} at t={t}")
    if t not in pfh:
        bad.append(f"no [PFH] row for t={t}")


def tot_field(tag, rx):
    v = 0
    for ln in lines:
        if tag in ln:
            m = re.search(rx, ln)
            if m:
                v += int(m.group(1))
    return v


zero = tot_field("[PFL] t=", r" zero=(\d+)/")
vml = tot_field("[PFL] t=", r" vml=(\d+)/")
pft = tot_field("[PFI] t=", r" pf=(\d+)/")
filep = tot["fat"] + tot["ext2"] + tot["nfs"]
checks = [
    ("anon >= 4160", tot["anon"] >= 4160),
    ("cow >= 256", tot["cow"] >= 256),
    ("file >= 8", filep >= 8),
    ("zeroed >= 4096", zero >= 4096),
    ("vml > 0", vml > 0),
    ("#PF traps >= 4096", pft >= 4096),
    ("rows consistent", not bad),
]
for ln in pfk[:3]:
    print("  e.g. " + ln[:220])
for b in bad[:5]:
    print("  inconsistent: " + b)
print("  totals: " + " ".join(f"{k}={v}" for k, v in tot.items() if v) +
      f" zero={zero} vml={vml} pf_traps={pft} rows={len(pfk)}")
ok = bool(pfk) and all(c for _, c in checks)
print(f"lp_check_pfstat: {'PASS' if ok else 'FAIL'} -- " +
      ", ".join(f"{name}:{'ok' if c else 'NO'}" for name, c in checks))
sys.exit(0 if ok else 1)
