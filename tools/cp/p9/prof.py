#!/usr/bin/env python3
"""tools/cp/p9/prof.py PROF.txt HOBBYOS_ELF -- what the guest's vCPUs were doing
(lane C9-play, docs/cp-p9.md §5), from boot_p9.py's `prof` samples (QMP
'info registers -a': rip, CPL and HLT of every vCPU, ~10 Hz).

  idle   HLT=1 (the vCPU is halted in the kernel's idle loop)
  kernel CPL=0, symbolized with `nm -n` of the kernel image
  user   CPL=3, bucketed by address: the game's exe image (0x140000000..),
         Windows PE DLLs (0x6fff_xxxx_xxxx / 0x7fff_xxxx_xxxx: Wine builtins,
         vkd3d-proton, DXVK), and Linux .so / unix-side code (0x10_xxxx_xxxx:
         glibc, Wine's unix libs, Mesa Venus, libX11)."""
import bisect
import collections
import subprocess
import sys

path, elf = sys.argv[1], sys.argv[2]
syms = []
for ln in subprocess.run(["nm", "-n", elf], capture_output=True, text=True).stdout.splitlines():
    f = ln.split()
    if len(f) == 3 and f[1] in "tTwW":
        syms.append((int(f[0], 16), f[2]))
addrs = [a for a, _ in syms]


def ksym(rip):
    i = bisect.bisect_right(addrs, rip) - 1
    return syms[i][1] if i >= 0 else "?"


def ubucket(rip):
    if 0x140000000 <= rip < 0x150000000:
        return "user: game exe (Cyberpunk2077.exe image)"
    if rip >= 0x6F0000000000:
        return "user: PE DLLs (Wine builtins, vkd3d-proton, DXVK, game DLLs)"
    if 0x1000000000 <= rip < 0x2000000000:
        return "user: unix .so (glibc, Wine unix side, Mesa Venus, X11)"
    return f"user: other ({rip >> 32:#x}xxxxxxxx)"


tot = 0
cls = collections.Counter()
kern = collections.Counter()
user = collections.Counter()
percpu = collections.defaultdict(collections.Counter)
chains = collections.defaultdict(collections.Counter)
for ln in open(path):
    f = ln.split()
    if len(f) < 5:
        continue
    _, cpu, rip, cpl, hlt = f[:5]
    if len(f) > 5 and f[5].startswith("k="):
        ws = [int(w, 16) for w in f[5][2:].split(",") if w]
        names = []
        for w in ws:
            nm = ksym(w)
            if nm not in names:
                names.append(nm)
        chains[ksym(int(rip, 16))][" < ".join(names[:5])] += 1
    rip = int(rip, 16)
    tot += 1
    if hlt == "1":
        c = "idle (HLT)"
    elif cpl == "0":
        c = "kernel"
        kern[ksym(rip)] += 1
    else:
        c = "user"
        user[ubucket(rip)] += 1
    cls[c] += 1
    percpu[cpu][c] += 1
print(f"== {path}: {tot} vCPU samples")
for c, n in cls.most_common():
    print(f"   {c:<12} {n:6d}  {100.0 * n / max(tot, 1):5.1f} %")
print("== per vCPU (idle / kernel / user %)")
for cpu in sorted(percpu, key=int):
    t = sum(percpu[cpu].values())
    print(f"   cpu{cpu}: " + " / ".join(f"{100.0 * percpu[cpu][c] / t:4.0f}" for c in ("idle (HLT)", "kernel", "user")))
print("== kernel, top symbols (% of all samples)")
for s, n in kern.most_common(25):
    print(f"   {100.0 * n / tot:5.1f} %  {s}")
print("== user buckets (% of all samples)")
for s, n in user.most_common():
    print(f"   {100.0 * n / tot:5.1f} %  {s}")
if chains:
    print("== kernel stack words (rough callers, innermost first) for the top leaves")
    for leaf, _ in kern.most_common(4):
        c = chains.get(leaf)
        if not c:
            continue
        print(f"   {leaf}:")
        for ch, n in c.most_common(8):
            print(f"      {n:5d}  {ch}")
