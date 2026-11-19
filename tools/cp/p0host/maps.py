#!/usr/bin/env python3
"""tools/cp/p0host/maps.py <proc_maps.txt> — VA layout facts from a /proc/<pid>/maps snapshot.

Prints: totals; every mapping that starts below 8 GiB (HobbyOS's x64 identity-mapped
range, docs/cyberpunk.md gap #2) merged into runs of adjacent same-name regions; the
Cyberpunk2077.exe image range; the 15 largest single mappings; the highest address;
the largest file-backed mapping of a game-dir file.
"""
import sys, collections

GAME = "/mnt/fast/games/steamapps/common/Cyberpunk 2077/"
rows = []
for l in open(sys.argv[1]):
    f = l.split(None, 5)
    a, b = (int(x, 16) for x in f[0].split("-"))
    rows.append((a, b, f[1], f[5].strip() if len(f) > 5 else ""))
rows = [r for r in rows if r[3] != "[vsyscall]"]
tot = sum(b - a for a, b, _, _ in rows)
res = sum(b - a for a, b, p, _ in rows if p.startswith("---"))
print(f"mappings={len(rows)} total_mapped={tot} ({tot/2**30:.1f} GiB, of which PROT_NONE reservations "
      f"{res/2**30:.1f} GiB) lowest=0x{rows[0][0]:x} highest_end=0x{max(b for _,b,_,_ in rows):x} (excl. [vsyscall])")
G8 = 8 << 30
print("\n# mappings starting below 8 GiB (0x200000000), adjacent same-name/perm runs merged")
low = [r for r in rows if r[0] < G8]
runs = []
for a, b, p, n in low:
    if runs and runs[-1][1] == a and runs[-1][3] == n and runs[-1][2] == p:
        runs[-1] = (runs[-1][0], b, p, n, runs[-1][4] + 1)
    else:
        runs.append((a, b, p, n, 1))
for a, b, p, n, c in runs:
    nm = n.replace(GAME, "<game>/") if n else "[anon]"
    print(f"0x{a:012x}-0x{b:012x} {(b-a)/1024:>12.0f} KiB {p} x{c} {nm}")
lowc = sum(min(b, G8) - a for a, b, _, _ in low)
lowr = sum(min(b, G8) - a for a, b, p, _ in low if p.startswith("---"))
print(f"# below-8GiB: {len(low)} mappings covering {lowc/2**20:.1f} MiB of [0, 8 GiB) "
      f"({lowr/2**20:.1f} MiB PROT_NONE reservations); below 4 GiB: "
      f"{sum(min(b, 4<<30) - a for a, b, _, _ in rows if a < 4<<30)/2**20:.1f} MiB")
for fixed in (0x7ffe0000, 0x140000000):
    hit = [r for r in rows if r[0] <= fixed < r[1]]
    print(f"# fixed address 0x{fixed:x}: " + (f"0x{hit[0][0]:x}-0x{hit[0][1]:x} {hit[0][2]} {hit[0][3] or '[anon]'}" if hit else "unmapped"))
print("\n# [0, 8 GiB) in 512 MiB bands: accessible MiB / PROT_NONE MiB / free MiB, notable names")
B = 512 << 20
for i in range(16):
    lo, hi = i * B, (i + 1) * B
    acc = res = 0; names = collections.Counter()
    for a, b, p, n in rows:
        o = max(0, min(b, hi) - max(a, lo))
        if not o: continue
        if p.startswith("---"): res += o
        else: acc += o
        if n: names[n.split("/")[-1]] += o
    top = ", ".join(f"{k}" for k, _ in names.most_common(4))
    print(f"  0x{lo:09x}-0x{hi:09x} {acc/2**20:7.1f} {res/2**20:7.1f} {(B-acc-res)/2**20:7.1f}  {top}")

ex = [r for r in rows if r[3].endswith("Cyberpunk2077.exe")]
if ex:
    print(f"\n# Cyberpunk2077.exe image: 0x{ex[0][0]:x}-0x{max(b for _,b,_,_ in ex):x} ({len(ex)} mappings)")
print("\n# 15 largest single mappings")
for a, b, p, n in sorted(rows, key=lambda r: r[0] - r[1])[:15]:
    print(f"0x{a:012x} {(b-a)/2**20:>10.1f} MiB {p} {n.replace(GAME,'<game>/') or '[anon]'}")
# by backing: what HobbyOS would have to back (vm.h: VM_OBJ_MAX_BYTES caps SHARED objects,
# i.e. memfd / MAP_SHARED anon / file / NFS objects, at 64 MiB each; the user window is 32 GiB)
def kind(p, n):
    if n.startswith("/dev/dri/"): return "dri (GPU BO mmaps)"
    if n.startswith("/memfd:"): return "memfd " + ("shared" if p[3] == "s" else "private")
    if n.startswith(GAME): return "game-dir file"
    if n.startswith("/"): return "other file " + ("shared" if p[3] == "s" else "private")
    if n.startswith("["): return n
    return "anon " + ("PROT_NONE" if p.startswith("---") else ("shared" if p[3] == "s" else "private"))
by = collections.defaultdict(lambda: [0, 0, 0])
for a, b, p, n in rows:
    k = kind(p, n); by[k][0] += 1; by[k][1] += b - a; by[k][2] = max(by[k][2], b - a)
print("\n# by backing: count, total, largest single mapping")
for k, (c, t, m) in sorted(by.items(), key=lambda x: -x[1][1]):
    print(f"  {k:<28} {c:>5} {t/2**20:>12.1f} MiB  largest {m/2**20:>10.1f} MiB")
sh = [r for r in rows if r[2][3] == "s"]
if sh:
    a, b, p, n = max(sh, key=lambda r: r[1] - r[0])
    print(f"# largest MAP_SHARED mapping: {(b-a)/2**20:.1f} MiB {p} {n or '[anon]'}; "
          f"shared mappings > 64 MiB: {sum(1 for r in sh if r[1]-r[0] > 64<<20)}")

gf = [r for r in rows if r[3].startswith(GAME)]
if gf:
    a, b, p, n = max(gf, key=lambda r: r[1] - r[0])
    per = collections.Counter()
    for a2, b2, _, n2 in gf: per[n2] += b2 - a2
    print(f"\n# game-dir file mappings: {len(gf)} regions over {len(per)} files; largest region "
          f"{(b-a)/2**20:.1f} MiB of {n.replace(GAME,'<game>/')}")
    for n2, s in per.most_common(10):
        print(f"  {s/2**20:>8.1f} MiB {n2.replace(GAME,'<game>/')}")
