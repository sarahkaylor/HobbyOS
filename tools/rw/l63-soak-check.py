#!/usr/bin/env python3
"""tools/rw/l63-soak-check.py -- the L63 soak verdict.

Two halves, both counted from the serial log:

  1. the L62 tripwire table (block layer, page-in verification, fault branch,
     Player.log NUL runs), unchanged so the two soaks are comparable;
  2. the L63 PUBLISH-TIME PROVENANCE table: every [PUB] row is one page
     installed in a mapping, and an executable page that came from a file
     without verification is a [PUB!] row.  The table names the CALLER, which
     is what §48.3 could not do.

Usage: l63-soak-check.py <serial.log> [Player.log]
"""
import re
import sys


def read(path):
    try:
        return open(path, "rb").read().decode("latin-1")
    except OSError:
        return ""


def count(pat, t):
    return len(re.findall(pat, t))


def rows(pat, t, limit=8, width=240):
    return [m.group(0)[:width] for m in re.finditer(pat, t)][:limit]


def pub_table(s):
    """Aggregate [PUB] / [PUB!] rows per caller."""
    per = {}
    for m in re.finditer(
            r"\[(PUB!?)\] who=(\S+) va=(\S+) off=(\S+) prot=(\S+) priv=(\d) "
            r"src=(\S+) zero=(\d+) ver=(\d)", s):
        alarm, who, va, off, prot, priv, src, zero, ver = m.groups()
        d = per.setdefault(who, {"n": 0, "exec": 0, "xzero": 0, "xunv": 0,
                                 "alarm": 0, "anon_zero": 0, "xzero_ok": 0,
                                 "sample": None})
        d["n"] += 1
        if "X" in prot:
            d["exec"] += 1
            if int(zero) == 4096:
                if src == "zero":
                    d["anon_zero"] += 1
                elif ver == "1":
                    d["xzero_ok"] += 1   # the file really holds zeros here
                else:
                    d["xzero"] += 1      # claimed file bytes, delivered zeros
            if ver == "0" and src != "zero":
                d["xunv"] += 1
        if alarm == "PUB!":
            d["alarm"] += 1
        if d["sample"] is None:
            d["sample"] = m.group(0)
    return per


def main():
    serial = read(sys.argv[1])
    plog = read(sys.argv[2]) if len(sys.argv) > 2 else ""
    s = serial

    txt = re.findall(r"\[TXT\] n=(\d+) bad=(\d+)", s)
    txt_n, txt_bad = (int(txt[-1][0]), int(txt[-1][1])) if txt else (0, 0)
    pubs = re.findall(r"\[PUBS\] n=(\d+) exec=(\d+) xzero=(\d+) "
                      r"file_xzero=(\d+) anon_xzero=(\d+) xunv=(\d+)", s)
    xsw = [int(m.group(1)) for m in re.finditer(r"\[XSW\] n=(\d+)", s)]
    nfs = [int(m.group(1)) for m in re.finditer(r"\[NFSRTT\] periodic .*?bytes=(\d+)", s)]

    nul32 = nul128 = 0
    run = 0
    for ch in plog:
        if ch == "\0":
            run += 1
            if run == 32:
                nul32 += 1
            if run == 128:
                nul128 += 1
        else:
            run = 0

    per = pub_table(s)
    pub_rows = count(r"\[PUB\] who=", s)
    pub_alarm = count(r"\[PUB!\] who=", s)

    verdict = [
        ("zero-byte exec publishes, file said non-zero",
         sum(d["xzero"] for d in per.values()),
         r"\[PUB!\][^\n]*zero=4096[^\n]*"),
        ("unverified exec publishes", sum(d["xunv"] for d in per.values()),
         r"\[PUB!\][^\n]*"),
        ("[UDX] rows (faulting page dumped)", count(r"\[UDX\]", s),
         r"\[UDX\][^\n]*"),
        ("#UD in UnityPlayer text (vector 6, prot=5 kind=7)",
         count(r"fault-pc region=[^\n]*prot=5 kind=7", s),
         r"fault-pc region=[^\n]*prot=5 kind=7[^\n]*"),
    ]
    observed = [
        ("gate refusals (pages NOT published; good)",
         count(r"\[TXT\] (?:ZERO|UNVERIFIED|IMG SUM|IMG UNVERIFIED)", s),
         r"\[TXT\] (?:ZERO|UNVERIFIED|IMG SUM|IMG UNVERIFIED)[^\n]*"),
        ("[TXT] exec pages verified n (bad)", txt_n, r"n/a"),
        ("[ZPFILL] short reads refused", count(r"\[ZPFILL\]", s),
         r"\[ZPFILL\][^\n]*"),
        ("[BLKQ] stalls", count(r"\[BLKQ\] stall", s), r"\[BLKQ\] stall[^\n]*"),
        ("[BLKQ] controller resets", count(r"\[BLKQ\] controller reset", s),
         r"\[BLKQ\] controller reset[^\n]*"),
        ("[BLKSLOW] (>250 ms)", count(r"\[BLKSLOW\]", s), r"\[BLKSLOW\][^\n]*"),
        ("VM stops (KVM internal error)", count(r"KVM internal error", s),
         r"KVM internal error[^\n]*"),
        ("STILL SPINNING", count(r"STILL SPINNING", s), r"STILL SPINNING[^\n]*"),
        ("retry-budget kills [VMK]", count(r"\[VMK\]", s), r"\[VMK\][^\n]*"),
        ("delivered signals [SIGX]", count(r"\[SIGX\]", s), r"\[SIGX\][^\n]*"),
        ("kernel #PF rows", count(r"\[KERNEL\][^\n]*Kernel-mode #PF", s),
         r"\[KERNEL\][^\n]*Kernel-mode #PF[^\n]*"),
        ("non-#PF user exceptions", count(r"\[KERNEL\] User process fault! Vector:", s),
         r"\[KERNEL\] User process fault![^\n]*"),
        ("Player.log mid-file NUL runs >=32 B", nul32, r"n/a"),
    ]

    print("== L63 soak verdict (publish-time provenance + L62 tripwires)")
    print(f"   serial log        {len(s)} bytes, {s.count(chr(10))} lines")
    print(f"   NFS bytes         {max(nfs) if nfs else 0} "
          f"({(max(nfs) if nfs else 0) / 1048576.0:.2f} MiB)")
    print(f"   [TXT] exec pages  n={txt_n} bad={txt_bad}")
    if pubs:
        n, ex, xz, fxz, axz, xu = (int(x) for x in pubs[-1])
        print(f"   [PUBS] totals     n={n} exec={ex} xzero={xz} "
              f"file_xzero={fxz} anon_xzero={axz} xunv={xu}")
    else:
        print("   [PUBS] totals     NO SUMMARY ROW (<4096 publishes?)")
    print(f"   [PUB] rows        {pub_rows} printed, {pub_alarm} alarm rows")
    print("")
    print(f"   {'VERDICT':<46} {'count':>6}")
    print("   " + "-" * 54)
    bad = 0
    fired = []
    for name, n, pat in verdict:
        print(f"   {name:<46} {n:>6}" + ("" if n == 0 else "   <-- FAIL"))
        if n:
            bad += n
            fired.append((name, pat))
    print(f"   {'=> ' + ('PASS' if bad == 0 else 'FAIL'):<46}")
    print("")
    print(f"   {'observed':<46} {'count':>6}")
    print("   " + "-" * 54)
    for name, n, pat in observed:
        print(f"   {name:<46} {n:>6}")
        if n and pat != r"n/a" and not name.startswith("gate refusals") \
                and not name.startswith("[BLK") and not name.startswith("[ZPFILL") \
                and not name.startswith("delivered") and not name.startswith("non-#PF") \
                and not name.startswith("VM stops"):
            fired.append((name, pat))
    if fired:
        print("")
        print("== fired: tuples")
        for name, pat in fired:
            print(f"-- {name}")
            for r in rows(pat, s):
                print("   " + r)
    ref = rows(r"\[TXT\] (?:ZERO|UNVERIFIED|IMG SUM|IMG UNVERIFIED)[^\n]*", s, 20)
    print("")
    print(f"-- gate refusal tuples: {len(ref)}")
    for r in ref:
        print("   " + r)
    print("")
    print(f"   {'caller':<12} {'rows':>6} {'exec':>6} {'exec0bad':>8} "
          f"{'exec0ok':>7} {'unver':>6} {'anon0':>6} {'alarm':>6}")
    print("   " + "-" * 66)
    for who in sorted(per):
        d = per[who]
        print(f"   {who:<12} {d['n']:>6} {d['exec']:>6} {d['xzero']:>8} "
              f"{d['xzero_ok']:>7} {d['xunv']:>6} {d['anon_zero']:>6} "
              f"{d['alarm']:>6}")
    print("")
    samples = [d["sample"] for d in per.values() if d["sample"]]
    print(f"-- provenance rows (first per caller): {len(samples)}")
    for r in samples:
        print("   " + r)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
