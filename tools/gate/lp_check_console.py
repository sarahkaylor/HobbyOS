#!/usr/bin/env python3
"""lp_check_console.py -- grade linux_probe_console's rows on the host side.

The probe (src/user/linux_probe_console.c) cannot see the console, so the
verdict is taken from the serial log here.  Every "[CSTR]" (kernel) and "[LPC]"
(user) row carries a payload that is a pure function of its numbers; this
script regenerates it and counts:

  interleaved  lines on which a [CSTR]/[LPC] tag appears anywhere but column 0
               (another writer's bytes in front of it, or two rows fused)
  torn         lines that start with the tag but do not parse, or whose payload
               or length is not the one the numbers define (bytes inside the
               row came from somebody else, or bytes are missing)
  missing      rows the probe says it printed (its "[LP] console totals" row)
               that are not present intact

PASS = 0 interleaved, 0 torn, 0 missing, and kernel rows seen from every CPU
the probe reported (at least --min-cpus).

Usage: lp_check_console.py <serial.log> [--min-cpus N]
       lp_check_console.py --self-test
Exit 0 = PASS, 1 = FAIL, 2 = no probe run in the log.
"""
import re
import sys

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
TAG = re.compile(r"\[(CSTR|LPC)\] ")
KROW = re.compile(r"\[CSTR\] c=(\d+) k=(\d) s=(\d+) p=([A-Za-z0-9]*) e=(\d+)")
UROW = re.compile(r"\[LPC\] w=(\d) s=(\d+) p=([A-Za-z0-9]*) e=(\d+)")
TOTALS = re.compile(r"\[LP\] console totals kernel=(\d+) user=(\d+) cpus=0x([0-9a-f]+) ncpus=(\d+)")


def kpayload(cpu, seq):
    n = 40 + (seq * 7 + cpu * 13) % 160
    return "".join(ALPHA[(seq + i * 7 + cpu) % 62] for i in range(n))


def ulen(w, s):
    if w == 1:
        return 600 + (s * 37) % 1200
    if w == 2:
        return 60 + (s * 11) % 80
    return 30 + (s * 13 + w) % 90


def upayload(w, s):
    return "".join(ALPHA[(s + i * 17 + w * 5) % 62] for i in range(ulen(w, s)))


# tools/t3_tier.sh appends its own "\n=== T3 guest sample: ... ===\n" to the
# console log when it sees a stall marker, wherever the guest's byte stream
# happens to be; that is the host writing into the file, not the console.
T3_NOTE = re.compile(r"\n=== T3 guest sample[^\n]*===\n")


def check(text, min_cpus=0, show=8):
    text = T3_NOTE.sub("", text.replace("\r", ""))
    lines = text.split("\n")
    inter, torn = [], []
    kseen, useen, cpus = set(), set(), set()
    totals = None
    for ln in lines:
        m = TOTALS.search(ln)
        if m:
            totals = m
        hits = [h.start() for h in TAG.finditer(ln)]
        if not hits:
            continue
        if hits[0] != 0 or len(hits) > 1:
            inter.append(ln)
            continue
        if ln.startswith("[CSTR] "):
            m = KROW.fullmatch(ln)
            ok = bool(m)
            if ok:
                c, k, s, p, e = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4), int(m.group(5))
                ok = p == kpayload(c, s) and e == len(p) and k < 5
            if ok:
                kseen.add(s)
                cpus.add(c)
            else:
                torn.append(ln)
        else:
            m = UROW.fullmatch(ln)
            ok = bool(m)
            if ok:
                w, s, p, e = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))
                ok = w < 4 and p == upayload(w, s) and e == len(p)
            if ok:
                useen.add((w, s))
            else:
                torn.append(ln)
    res = {"interleaved": len(inter), "torn": len(torn), "kernel_ok": len(kseen),
           "user_ok": len(useen), "cpus": sorted(cpus), "examples": (inter + torn)[:show]}
    if totals:
        kt, ut = int(totals.group(1)), int(totals.group(2))
        res["kernel_want"], res["user_want"] = kt, ut
        res["probe_ncpus"] = int(totals.group(4))
        res["missing"] = max(0, kt - len(kseen)) + max(0, ut - len(useen))
    return res


def verdict(res, min_cpus):
    if "kernel_want" not in res:
        return 2, "no '[LP] console totals' row (the probe did not finish)"
    need = max(min_cpus, res["probe_ncpus"])
    bad = []
    if res["interleaved"]:
        bad.append(f"{res['interleaved']} interleaved line(s)")
    if res["torn"]:
        bad.append(f"{res['torn']} torn row(s)")
    if res["missing"]:
        bad.append(f"{res['missing']} missing row(s)")
    if len(res["cpus"]) < need:
        bad.append(f"kernel rows from {len(res['cpus'])} CPU(s), want {need}")
    return (1 if bad else 0), ("; ".join(bad) if bad else "clean")


def self_test():
    good = []
    for s in range(3):
        good.append(f"[CSTR] c=1 k={s % 5} s={s} p={kpayload(1, s)} e={len(kpayload(1, s))}")
    for s in range(2):
        good.append(f"[LPC] w=1 s={s} p={upayload(1, s)} e={len(upayload(1, s))}")
    tot = "[LP] console totals kernel=3 user=2 cpus=0x2 ncpus=1 w0=0 w1=2 w2=0 w3=0 secs=1.0"
    r = check("\n".join(good + [tot]) + "\n")
    assert verdict(r, 1)[0] == 0, r
    # a kernel row welded behind another writer's text
    bad = list(good)
    bad[1] = "recover_us=" + bad[1]
    r = check("\n".join(bad + [tot]))
    assert r["interleaved"] == 1 and r["missing"] == 1 and verdict(r, 1)[0] == 1, r
    # two rows alternating characters (the integ7 shape)
    a, b = good[0], good[2]
    mix = "".join(x + y for x, y in zip(a, b))
    r = check("\n".join([mix, good[1]] + good[3:] + [tot]))
    assert verdict(r, 1)[0] == 1 and r["missing"] == 2, r
    # a row cut short
    cut = list(good)
    cut[3] = cut[3][:-20]
    r = check("\n".join(cut + [tot]))
    assert r["torn"] == 1 and verdict(r, 1)[0] == 1, r
    # not enough CPUs
    r = check("\n".join(good + [tot.replace("ncpus=1", "ncpus=8")]))
    assert verdict(r, 1)[0] == 1, r
    # the tier's own note dropped into the middle of a row is not a tear
    note = list(good)
    note[0] = note[0][:20] + "\n=== T3 guest sample: SKIPPED -- no monitor ===\n" + note[0][20:]
    r = check("\n".join(note + [tot]))
    assert verdict(r, 1)[0] == 0, r
    print("lp_check_console self-test: PASS")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    min_cpus = 0
    if "--min-cpus" in argv:
        min_cpus = int(argv[argv.index("--min-cpus") + 1])
    path = [a for a in argv[1:] if not a.startswith("--") and not a.isdigit()][0]
    text = open(path, "rb").read().decode("latin-1")
    res = check(text, min_cpus)
    rc, why = verdict(res, min_cpus)
    print(f"lp_check_console: kernel rows intact {res['kernel_ok']}/{res.get('kernel_want', '?')}, "
          f"user rows intact {res['user_ok']}/{res.get('user_want', '?')}, "
          f"interleaved lines {res['interleaved']}, torn rows {res['torn']}, "
          f"missing {res.get('missing', '?')}, kernel-row CPUs {res['cpus']}")
    for ex in res["examples"]:
        print("  e.g. " + ex[:200])
    print(f"lp_check_console: {'PASS' if rc == 0 else 'FAIL'} -- {why}")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
