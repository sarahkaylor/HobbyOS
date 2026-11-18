#!/usr/bin/env python3
"""pfsum.py SERIAL [--phases PHASES] [--hostnet HOSTNET] -- H7-fault's summary
of one game boot (docs/cp-fault.md §2-3).

  * the [PFK]/[PFH]/[PFL]/[PFI] rows summed per phase (phases.txt: "<s since
    boot> <game s> <name>"; a row belongs to the phase its t= falls in):
    faults by kind with time, I/O, max; latency histogram; vm_lock wait/hold;
    zeroing; shootdowns; user trap time by class; the share of the interval's
    wall time the faults took on 8 CPUs;
  * [PFT] top threads per phase; [PFX] long events by kind;
  * [AWAY]/[AWAY-Z] absences with T samples, and the [PFX] events of the same
    thread inside each absence;
  * NFS: [NFSPIPE] stall rows, every [NFSRS] row, the last [NFSRTH] and
    [NFSNET] rows;
  * the host side (hostnet.sh): Udp counter, QEMU socket drop, nfsd socket
    drop and compaction deltas over the boot.
Read-only; prints text."""
import argparse
import collections
import re

KINDS = ("anon", "shobj", "nfs", "ext2", "fat", "img", "cow", "priv", "retry", "err")
EDGES = ["<1", "<2", "<4", "<8", "<16", "<32", "<64", "<128", "<256", "<512",
         "<1m", "<2m", "<4m", "<8m", "<16m", "<32m", ">=32m"]


def load(path):
    return open(path, "rb").read().decode("latin-1").splitlines()


def tail_of(ln, tag):
    i = ln.find(tag)
    return ln[i:] if i >= 0 else None


def phases(path):
    ph = []
    if not path:
        return ph
    try:
        for ln in load(path):
            f = ln.split()
            if len(f) >= 3:
                try:
                    ph.append((float(f[0]), f[2]))
                except ValueError:
                    pass
    except OSError:
        pass
    out = []
    for s, n in ph:  # drop repeats
        if not out or out[-1][1] != n:
            out.append((s, n))
    return out


def phase_of(ph, t_s):
    name = "boot"
    for s, n in ph:
        if t_s >= s:
            name = n
    return name


def ints(s):
    return [int(x) for x in s.split(",") if x != ""]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial")
    ap.add_argument("--phases")
    ap.add_argument("--hostnet")
    a = ap.parse_args()
    lines = load(a.serial)
    ph = phases(a.phases)
    order = ["boot"] + [n for _, n in ph]
    seen = []
    for n in order:
        if n not in seen:
            seen.append(n)
    agg = collections.defaultdict(lambda: collections.defaultdict(float))
    hist = collections.defaultdict(lambda: [0] * 17)
    histf = collections.defaultdict(lambda: [0] * 17)
    top = collections.defaultdict(collections.Counter)
    topf = collections.defaultdict(collections.Counter)
    names = {}
    pfx = []
    for ln in lines:
        r = tail_of(ln, "[PFK] t=")
        if r:
            t = int(re.search(r" t=(\d+)", r).group(1))
            p = phase_of(ph, t / 1000.0)
            g = agg[p]
            g["rows"] += 1
            g["dt"] += int(re.search(r" dt=(\d+)", r).group(1))
            g["n"] += int(re.search(r" n=(\d+)", r).group(1))
            g["us"] += int(re.search(r" us=(\d+)", r).group(1))
            g["kmode"] += int(re.search(r" kmode=(\d+)", r).group(1))
            g["rep"] += int(re.search(r" rep=(\d+)", r).group(1))
            for k in KINDS:
                m = re.search(r" %s=(\d+)/(\d+)/(\d+)(?:/io=(\d+)/rpc=(\d+))?" % k, r)
                if m:
                    g[k + ".n"] += int(m.group(1))
                    g[k + ".us"] += int(m.group(2))
                    g[k + ".max"] = max(g[k + ".max"], int(m.group(3)))
                    if m.group(4):
                        g[k + ".io"] += int(m.group(4))
                        g[k + ".rpc"] += int(m.group(5))
            continue
        r = tail_of(ln, "[PFH] t=")
        if r:
            m = re.search(r" t=(\d+) all=([\d,]+) file=([\d,]+)", r)
            if m:
                p = phase_of(ph, int(m.group(1)) / 1000.0)
                for i, v in enumerate(ints(m.group(2))[:17]):
                    hist[p][i] += v
                for i, v in enumerate(ints(m.group(3))[:17]):
                    histf[p][i] += v
            continue
        r = tail_of(ln, "[PFL] t=")
        if r:
            t = int(re.search(r" t=(\d+)", r).group(1))
            g = agg[phase_of(ph, t / 1000.0)]
            m = re.search(r" lw=(\d+)/(\d+) lh=(\d+)/(\d+) vml=(\d+)/w=(\d+)/(\d+)@(\d+)/h=(\d+)/(\d+)@(\d+)"
                          r" zero=(\d+)/(\d+)/(\d+) fresh=(\d+)/(\d+) shoot=(\d+)/(\d+)/(\d+)/cpus=(\d+)", r)
            if m:
                v = [int(x) for x in m.groups()]
                g["lw"] += v[0]
                g["lw.max"] = max(g["lw.max"], v[1])
                g["lh"] += v[2]
                g["lh.max"] = max(g["lh.max"], v[3])
                g["vml"] += v[4]
                g["vmlw"] += v[5]
                if v[6] > g["vmlw.max"]:
                    g["vmlw.max"], g["vmlw.site"] = v[6], v[7]
                g["vmlh"] += v[8]
                if v[9] > g["vmlh.max"]:
                    g["vmlh.max"], g["vmlh.site"] = v[9], v[10]
                g["zero.n"] += v[11]
                g["zero.us"] += v[12]
                g["zero.max"] = max(g["zero.max"], v[13])
                g["fresh.n"] += v[14]
                g["fresh.us"] += v[15]
                g["shoot.n"] += v[16]
                g["shoot.us"] += v[17]
                g["shoot.max"] = max(g["shoot.max"], v[18])
                g["shoot.cpus"] += v[19]
            continue
        r = tail_of(ln, "[PFI] t=")
        if r:
            t = int(re.search(r" t=(\d+)", r).group(1))
            g = agg[phase_of(ph, t / 1000.0)]
            for c in ("pf", "tmr", "ipi", "oth"):
                m = re.search(r" %s=(\d+)/(\d+)/(\d+)" % c, r)
                if m:
                    g["tr." + c + ".n"] += int(m.group(1))
                    g["tr." + c + ".us"] += int(m.group(2))
                    g["tr." + c + ".max"] = max(g["tr." + c + ".max"], int(m.group(3)))
            continue
        r = tail_of(ln, "[PFT] t=")
        if r:
            m = re.search(r" t=(\d+) top=(.*)", r)
            if m:
                p = phase_of(ph, int(m.group(1)) / 1000.0)
                for e in m.group(2).split(","):
                    f = e.split(":")
                    if len(f) >= 4 and f[0].isdigit() and f[1].isdigit() and f[2].isdigit():
                        # tid:pf_us:irq_us:name
                        top[p][int(f[0])] += int(f[1]) + int(f[2])
                        topf[p][int(f[0])] += int(f[1])
                        names[int(f[0])] = f[3]
                    elif len(f) >= 2 and f[0].isdigit() and f[1].isdigit():
                        top[p][int(f[0])] += int(f[1])  # older tid:us:name rows
                        if len(f) >= 3:
                            names[int(f[0])] = f[2]
            continue
        r = tail_of(ln, "[PFX] ")
        if r:
            m = re.search(r"\[PFX\] (\w) tid=(\d+) at=(\d+) us=(\d+)(?: kind=(\w+))?", r)
            if m:
                pfx.append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                            m.group(5) or "", r))
    print("== faults per phase ([PFK]/[PFL]/[PFI] summed; us = microseconds)")
    for p in seen:
        g = agg.get(p)
        if not g or not g["rows"]:
            continue
        wall = g["dt"] * 1000.0 * 8
        print(f"-- {p}: rows={int(g['rows'])} wall_ms={int(g['dt'])} faults={int(g['n'])} "
              f"fault_us={int(g['us'])} ({100.0 * g['us'] / wall if wall else 0:.2f}% of 8 CPUs) "
              f"kmode={int(g['kmode'])} rep={int(g['rep'])}")
        for k in KINDS:
            n = g[k + ".n"]
            if not n:
                continue
            extra = ""
            if g[k + ".io"] or g[k + ".rpc"]:
                extra = f" io_us={int(g[k + '.io'])} rpc_faults={int(g[k + '.rpc'])}"
            print(f"   {k:6s} n={int(n):8d} us={int(g[k + '.us']):9d} avg={g[k + '.us'] / n:8.1f} "
                  f"max={int(g[k + '.max'])}{extra}")
        print(f"   hist  " + " ".join(f"{e}:{v}" for e, v in zip(EDGES, hist[p]) if v))
        print(f"   hist(file) " + " ".join(f"{e}:{v}" for e, v in zip(EDGES, histf[p]) if v))
        print(f"   vm_lock in faults: wait={int(g['lw'])}us (max {int(g['lw.max'])}) hold={int(g['lh'])}us "
              f"(max {int(g['lh.max'])}); all pairs n={int(g['vml'])} wait={int(g['vmlw'])}us "
              f"max {int(g['vmlw.max'])}@{int(g['vmlw.site'])} hold={int(g['vmlh'])}us max "
              f"{int(g['vmlh.max'])}@{int(g['vmlh.site'])}")
        print(f"   zero n={int(g['zero.n'])} us={int(g['zero.us'])} max={int(g['zero.max'])} "
              f"fresh n={int(g['fresh.n'])} us={int(g['fresh.us'])}; shoot n={int(g['shoot.n'])} "
              f"us={int(g['shoot.us'])} max={int(g['shoot.max'])} cpus={int(g['shoot.cpus'])}")
        print("   user traps: " + " ".join(
            f"{c}={int(g['tr.' + c + '.n'])}/{int(g['tr.' + c + '.us'])}us/max{int(g['tr.' + c + '.max'])}"
            for c in ("pf", "tmr", "ipi", "oth")))
        if top[p]:
            print("   top threads (tid:pf+irq us:pf us:name): " + ", ".join(
                f"{t}:{v}:{topf[p][t]}:{names.get(t, '?')}" for t, v in top[p].most_common(6)))
    print(f"== [PFX] long events (>= 2 ms): {len(pfx)}")
    bykind = collections.Counter()
    for w, tid, at, us, k, _ in pfx:
        bykind[(w, k)] += 1
    for (w, k), n in bykind.most_common():
        us = sorted(e[3] for e in pfx if e[0] == w and e[4] == k)
        print(f"   {w} {k or 'irq'}: n={n} median_us={us[len(us) // 2]} max_us={us[-1]}")
    for e in sorted(pfx, key=lambda e: -e[3])[:8]:
        print("   " + e[5][:230])
    # absences with T samples, and what long events of the same thread fell inside
    aw = []
    for ln in lines:
        r = tail_of(ln, "[AWAY")
        if not r:
            continue
        m = re.search(r"tid=(\d+) .*?ms=(\d+) at=(\d+).*? T=(\d+)", r)
        if m and int(m.group(4)) > 0:
            aw.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), r))
    print(f"== [AWAY]/[AWAY-Z] absences with T>0: {len(aw)}")
    hit = 0
    for tid, ms, at, tn, r in aw:
        inside = [e for e in pfx if e[1] == tid and at <= e[2] <= at + ms + 5]
        if inside:
            hit += 1
        pf_ms = sum(e[3] for e in inside if e[0] == "F") / 1000.0
        if inside or len(aw) <= 40:
            print(f"   tid={tid} ms={ms} at={at} T={tn} long_faults_inside={len(inside)} ({pf_ms:.1f} ms)")
    if aw:
        print(f"   absences with a long fault inside: {hit}/{len(aw)}")
    stalls = [tail_of(ln, "[NFSPIPE] stall") for ln in lines if "[NFSPIPE] stall" in ln]
    rs = [tail_of(ln, "[NFSRS] ") for ln in lines if "[NFSRS] " in ln]
    print(f"== NFS: [NFSPIPE] stall rows={len(stalls)} [NFSRS] rows={len(rs)}")
    for r in rs:
        print("   " + r[:240])
    for tag in ("[NFSRTH] ", "[NFSNET] ", "[NFSRA] "):
        last = [tail_of(ln, tag) for ln in lines if tag in ln]
        if last:
            print("   last " + last[-1][:300])
    hard = [tail_of(ln, "[NFSHARD]") for ln in lines if "[NFSHARD]" in ln]
    print(f"   [NFSHARD] rows={len(hard)}")
    for r in hard[:6]:
        print("   " + r[:200])
    if a.hostnet:
        try:
            hn = load(a.hostnet)
        except OSError:
            hn = []
        U = [ln.split() for ln in hn if ln.startswith("U ")]
        Q = [ln for ln in hn if ln.startswith("Q ")]
        N = [ln for ln in hn if ln.startswith("N ")]
        V = [ln for ln in hn if ln.startswith("V ")]
        print(f"== host (hostnet.sh): samples={len(U)}")
        if len(U) >= 2:
            f = ["InDatagrams", "NoPorts", "InErrors", "OutDatagrams", "RcvbufErrors", "SndbufErrors"]
            d = [int(U[-1][2 + i]) - int(U[0][2 + i]) for i in range(len(f))]
            print("   Udp deltas: " + " ".join(f"{n}={v}" for n, v in zip(f, d)))
            # seconds in which RcvbufErrors rose
            rises = [int(U[i][1]) - int(U[0][1]) for i in range(1, len(U))
                     if int(U[i][6]) > int(U[i - 1][6])]
            print(f"   RcvbufErrors rose in {len(rises)} s (s since sampler start): {rises[:20]}")
        if Q:
            dr = [int(re.search(r"drops=(\d+)", q).group(1)) for q in Q]
            rq = [int(re.search(r"rq_max=(\d+)", q).group(1)) for q in Q]
            print(f"   QEMU UDP sockets: drops first={dr[0]} max={max(dr)} last={dr[-1]}; recv-queue max={max(rq)}")
        if N:
            nd = [int(re.search(r"nfsd_drops=(\d+)", q).group(1)) for q in N]
            print(f"   nfsd :2049 socket drops: first={nd[0]} last={nd[-1]}")
        if len(V) >= 2:
            def kv(s):
                return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", s)}
            v0, v1 = kv(V[0]), kv(V[-1])
            print("   vmstat deltas: " + " ".join(f"{k}={v1[k] - v0.get(k, 0)}" for k in v1))


if __name__ == "__main__":
    main()
