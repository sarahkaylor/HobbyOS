#!/usr/bin/env python3
"""abtable.py LOGDIR RUN... -- H7-fault's A/B table (docs/cp-fault.md §5): one
row per game boot from fault-logs (fp.sh / run-hunt.sh outputs):

  fps world / prof-fp-world median and p10 (RUN.summary.txt), audio dropouts
  and [SND] zeros rows (RUN.gamecheck), and from the serial log the boot's
  fault totals ([PFK]/[PFL]: faults, fault ms, anon avg us, vm_lock wait and
  hold inside faults, all-site vm_lock wait, shootdown ms, [PFX] long faults
  with lw >= 1 ms), the NFS pipeline stalls ([NFSNET] stalls=, the last row)
  and classify.py's guest-time hitches over all phases (RUN.class.json); for
  a soak, w_med/w_p10 are the median over its soakN phases.
Then the mean of each column per group (a group = the run name without its
trailing digits).  Read-only."""
import collections
import os
import re
import sys


def rd(p):
    try:
        return open(p, "rb").read().decode("latin-1")
    except OSError:
        return ""


def fps(summary, phase):
    for ln in summary.splitlines():
        f = ln.split()
        if f and f[0] == phase and len(f) >= 5:
            try:
                return float(f[3]), float(f[4])
            except ValueError:
                pass
    return None, None


def row(d, run):
    s = rd(os.path.join(d, run + ".summary.txt"))
    g = rd(os.path.join(d, run + ".gamecheck"))
    ser = rd(os.path.join(d, run + ".serial.txt"))
    r = {"run": run}
    r["w_med"], r["w_p10"] = fps(s, "world")
    r["p_med"], r["p_p10"] = fps(s, "prof-fp-world")
    m = re.search(r"dropouts=(\d+)", g)
    r["drop"] = int(m.group(1)) if m else None
    m = re.search(r"zeros_rows=(\d+)", g)
    r["zeros"] = int(m.group(1)) if m else None
    n = us = an = aus = lw = lh = vw = sh = 0
    for ln in ser.splitlines():
        if "[PFK] t=" in ln:
            n += int(re.search(r" n=(\d+)", ln).group(1))
            us += int(re.search(r" us=(\d+)", ln).group(1))
            m = re.search(r" anon=(\d+)/(\d+)/", ln)
            if m:
                an += int(m.group(1))
                aus += int(m.group(2))
        elif "[PFL] t=" in ln:
            m = re.search(r" lw=(\d+)/\d+ lh=(\d+)/\d+ vml=\d+/w=(\d+)/", ln)
            if m:
                lw += int(m.group(1))
                lh += int(m.group(2))
                vw += int(m.group(3))
            m = re.search(r" shoot=\d+/(\d+)/", ln)
            if m:
                sh += int(m.group(1))
    r["faults"] = n
    r["fault_ms"] = us / 1000.0
    r["anon_avg"] = aus / an if an else None
    r["lw_ms"] = lw / 1000.0
    r["lh_ms"] = lh / 1000.0
    r["vmlw_ms"] = vw / 1000.0
    r["shoot_ms"] = sh / 1000.0
    r["pfx_lw1ms"] = sum(1 for ln in ser.splitlines() if "[PFX] F" in ln and
                         int((re.search(r" lw=(\d+)", ln) or [0, 0])[1] or 0) >= 1000)
    st = [int(m.group(1)) for m in re.finditer(r"\[NFSNET\].*? stalls=(\d+)", ser)]
    r["stalls"] = st[-1] if st else None
    m = re.findall(r"txq_drop=(\d+)", ser)
    r["txdrop"] = int(m[-1]) if m else None
    # classify.py's per-phase guest-time hitches (fps rows > 2.5 s apart), summed
    cj = rd(os.path.join(d, run + ".class.json"))
    hs = [int(x) for x in re.findall(r'"hitches": *(\d+)', cj)]
    r["hitch"] = sum(hs) if hs else None
    # a soak's world: every soakN phase of summary.txt (median of their medians)
    sm = []
    for ln in s.splitlines():
        f = ln.split()
        if f and re.match(r"soak\d+$", f[0]) and len(f) >= 5:
            try:
                sm.append((float(f[3]), float(f[4])))
            except ValueError:
                pass
    if sm and r["w_med"] is None:
        sm.sort()
        r["w_med"] = sm[len(sm) // 2][0]
        r["w_p10"] = sorted(x[1] for x in sm)[len(sm) // 2]
    return r


COLS = ["w_med", "w_p10", "p_med", "p_p10", "drop", "zeros", "faults", "fault_ms", "anon_avg",
        "lw_ms", "lh_ms", "vmlw_ms", "shoot_ms", "pfx_lw1ms", "stalls", "txdrop", "hitch"]


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.1f}"
    return str(v)


def main():
    d = sys.argv[1]
    rows = [row(d, r) for r in sys.argv[2:]]
    print("| run | " + " | ".join(COLS) + " |")
    print("|---" * (len(COLS) + 1) + "|")
    for r in rows:
        print(f"| {r['run']} | " + " | ".join(fmt(r[c]) for c in COLS) + " |")
    grp = collections.OrderedDict()
    for r in rows:
        grp.setdefault(re.sub(r"\d+$", "", r["run"]), []).append(r)
    for k, rs in grp.items():
        out = []
        for c in COLS:
            v = [r[c] for r in rs if r[c] is not None]
            out.append(fmt(sum(v) / len(v)) if v else "-")
        print(f"| **mean {k}** (n={len(rs)}) | " + " | ".join(out) + " |")


if __name__ == "__main__":
    main()
