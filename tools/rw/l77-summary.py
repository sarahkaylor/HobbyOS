#!/usr/bin/env python3
"""L77: reduce a desktop-harness run to the rows this lane's ladder reports.

The guest's serial output interleaves (one row is several uart_puts calls and
another CPU can splice a whole row into the middle of it), so every pattern is
matched ANYWHERE in the line and the fields are pulled out individually.

What it reports, and why each one is here:
  presents / changed / distinct crc   is the client drawing at all
  last crc / last box / last t        the menu evidence (a changing frame whose
                                      diff box is not just the clock)
  res_mib                             the process RSS at the end of the window
  [FUTR] rate / top key               the load plateau's round rate
  [XO] last                           how far the loader got
  'Initializing.'                     the splash text L76 named as the wall
  [PUB] … zero=4096 ver=1             the divergence this lane exists for
  [PBI] / [PBF]                       the publish identity assertion
  [TXT] PROBE / REPLAY / PROBEGET     the probe's own verdict rows
  fault / [UDX] / SIGDUMP             how the process died, if it did
  hole / fr_used / resets             L75's invariants, still watched

Usage: l77-summary.py <serial.log> [--json out.json]
"""
import argparse
import json
import re
import sys

RX = {
    "xsw": re.compile(r"\[XSW\] n=(\d+)\b.*?crc=0x([0-9a-fA-F]+).*?changed=(\d+)"
                      r".*?t=(\d+).*?(?:box=([\d,]+))?"),
    "res_mib": re.compile(r"res_mib=(\d+)"),
    "fr_used": re.compile(r"fr_used=(\d+)"),
    "fr_high": re.compile(r"fr_high=(\d+)"),
    "hole": re.compile(r"\bhole=(\d+)"),
    "resets": re.compile(r"\bresets=(\d+)"),
    "futr": re.compile(r"\[FUTR\] t=(\d+)s ops=(\d+) d=(\d+) rate=(\d+)/s"),
    "futr_key": re.compile(r"(0x[0-9A-Fa-f]+):(\d+)w(\d+)k"),
    "xo": re.compile(r"\[XO\] (\S+)"),
    "pub_zero": re.compile(r"\[PUB[!\]]*\] who=(\S+).*?\bzero=4096\b.*?\bver=1\b"),
    "pub": re.compile(r"\[PUB[!\]]*\] who=(\S+)"),
    "pbi": re.compile(r"\[PBI\] (\S+)"),
    "pbf": re.compile(r"\[PBF\] (\S+)"),
    "probe": re.compile(r"\[TXT\] PROBE off=0x([0-9a-fA-F]+).*?r_nz=(\d+)"),
    "replay": re.compile(r"\[TXT\] REPLAY off=0x([0-9a-fA-F]+)"),
    "probeget": re.compile(r"\[TXT\] PROBEGET off=0x([0-9a-fA-F]+)"),
    "txt_zero": re.compile(r"\[TXT\] ZERO off=0x([0-9a-fA-F]+)"),
    "txt_unv": re.compile(r"\[TXT\] UNVERIFIED off=0x([0-9a-fA-F]+)"),
    "udx": re.compile(r"\[UDX\] rip=0x([0-9a-fA-F]+).*?off=0x([0-9a-fA-F]+)"),
    "fault": re.compile(r"User process fault! Vector: (\d+) RIP: 0x([0-9a-fA-F]+)"),
    "sig": re.compile(r"\[SIGDUMP\] SIG(\w+) code=(\d+)"),
    "init": re.compile(r"Initializing\."),
    "alive": re.compile(r"\[ALIVE-SET\] slot=(\d+) pid=(\d+).*?pc=0x([0-9a-fA-F]+)"),
    "pubs": re.compile(r"\[PUBS\] n=(\d+) exec=(\d+) xzero=(\d+) file_xzero=(\d+)"
                       r".*?xunv=(\d+).*?(?:idc=(\d+) idf=(\d+))?"),
}


def ints(text, name):
    return [int(m.group(1)) for m in RX[name].finditer(text)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--json")
    a = ap.parse_args()
    text = open(a.log, "r", errors="replace").read()

    out = {}

    xsw = []
    for line in text.splitlines():
        m = RX["xsw"].search(line)
        if m:
            bm = re.search(r"box=(-?\d+,-?\d+,-?\d+,-?\d+)", line)
            xsw.append({"n": int(m.group(1)), "crc": m.group(2),
                        "changed": int(m.group(3)), "t": int(m.group(4)),
                        "box": bm.group(1) if bm else None})
    out["xsw_rows"] = len(xsw)
    out["presents"] = max((r["n"] for r in xsw), default=0)
    out["changed_rows"] = sum(1 for r in xsw if r["changed"])
    crcs = {r["crc"] for r in xsw}
    out["distinct_crc"] = len(crcs)
    if xsw:
        last = xsw[-1]
        out["last_crc"] = last["crc"]
        out["last_changed"] = last["changed"]
        out["last_box"] = last["box"]
        out["last_t_us"] = last["t"]

    for k in ("res_mib",):
        v = ints(text, k)
        out[k + "_last"] = v[-1] if v else None
    # The [BLKD] row is one line; a spliced line breaks a field, so only rows
    # where fr_used/fr_high/hole appear together in order are believed -- a
    # bare "hole=0" from a spliced neighbour is not a reading.
    blkd = []
    for line in text.splitlines():
        m = re.search(r"fr_used=(\d+) fr_high=(\d+) hole=(\d+)", line)
        if m:
            blkd.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    out["blkd_rows"] = len(blkd)
    out["fr_used_last"] = blkd[-1][0] if blkd else None
    out["fr_used_max"] = max(b[0] for b in blkd) if blkd else None
    out["fr_high_max"] = max(b[1] for b in blkd) if blkd else None
    out["hole_max"] = max(b[2] for b in blkd) if blkd else None
    out["hole_last"] = blkd[-1][2] if blkd else None
    rst = []
    for line in text.splitlines():
        if "[BLKD]" not in line:
            continue
        m = re.search(r"resets=(\d+)\b", line)
        if m:
            rst.append(int(m.group(1)))
    out["resets_last"] = rst[-1] if rst else None

    futr = [(int(m.group(1)), int(m.group(4))) for m in RX["futr"].finditer(text)]
    out["futr_rows"] = len(futr)
    out["futr_rate"] = futr[-1][1] if futr else None
    out["futr_t"] = futr[-1][0] if futr else None
    # the busiest wait key in the last [FUTR] window, and its round count
    keys = [(m.group(1), int(m.group(2)), int(m.group(3)))
            for m in RX["futr_key"].finditer(text)]
    if keys:
        top = max(keys, key=lambda k: k[2])
        out["futr_key"] = top[0]
        out["futr_w"] = top[1]
        out["futr_k"] = top[2]

    xo = [m.group(1) for m in RX["xo"].finditer(text)]
    out["xo_rows"] = len(xo)
    out["xo_last"] = xo[-1] if xo else None

    out["initializing"] = len(RX["init"].findall(text))
    out["pub_zero_rows"] = len(RX["pub_zero"].findall(text))
    out["pub_zero_who"] = sorted({m.group(1) for m in RX["pub_zero"].finditer(text)})
    pub_who = {}
    for m in RX["pub"].finditer(text):
        pub_who[m.group(1)] = pub_who.get(m.group(1), 0) + 1
    out["pub_who"] = pub_who
    out["pbi"] = len(RX["pbi"].findall(text))
    out["pbf"] = len(RX["pbf"].findall(text))
    out["probe"] = [{"off": m.group(1), "r_nz": int(m.group(2))}
                    for m in RX["probe"].finditer(text)]
    out["replay"] = [m.group(1) for m in RX["replay"].finditer(text)]
    out["probeget"] = [m.group(1) for m in RX["probeget"].finditer(text)]
    out["txt_zero"] = [m.group(1) for m in RX["txt_zero"].finditer(text)]
    out["txt_unverified"] = [m.group(1) for m in RX["txt_unv"].finditer(text)]
    out["udx"] = [(m.group(1), m.group(2)) for m in RX["udx"].finditer(text)]
    out["fault"] = [(int(m.group(1)), m.group(2)) for m in RX["fault"].finditer(text)]
    out["signals"] = [(m.group(1), int(m.group(2))) for m in RX["sig"].finditer(text)]
    out["alive"] = [(int(m.group(1)), int(m.group(2)), m.group(3))
                    for m in RX["alive"].finditer(text)]
    pubs = None
    for m in RX["pubs"].finditer(text):
        pubs = m
    if pubs:
        out["pubs"] = {"n": int(pubs.group(1)), "exec": int(pubs.group(2)),
                       "xzero": int(pubs.group(3)),
                       "file_xzero": int(pubs.group(4)),
                       "xunv": int(pubs.group(5))}
        if pubs.group(6) is not None:
            out["pubs"]["idc"] = int(pubs.group(6))
            out["pubs"]["idf"] = int(pubs.group(7))

    lines = [
        f"presents={out['presents']} xsw_rows={out['xsw_rows']} "
        f"changed={out['changed_rows']} distinct_crc={out['distinct_crc']}",
        f"last: crc=0x{out.get('last_crc','-')} changed={out.get('last_changed','-')} "
        f"box={out.get('last_box','-')} t_us={out.get('last_t_us','-')}",
        f"res_mib={out['res_mib_last']} fr_used={out['fr_used_last']} "
        f"(max {out['fr_used_max']}) fr_high={out['fr_high_max']} "
        f"hole(max)={out['hole_max']} resets={out['resets_last']} "
        f"blkd_rows={out['blkd_rows']}",
        f"futr: rows={out['futr_rows']} rate={out['futr_rate']}/s "
        f"t={out['futr_t']}s key={out.get('futr_key','-')} "
        f"{out.get('futr_w','-')}w{out.get('futr_k','-')}k",
        f"loader: xo_rows={out['xo_rows']} last={out['xo_last']} "
        f"initializing={out['initializing']}",
        f"publish: pub_zero_ver1={out['pub_zero_rows']} "
        f"who={out['pub_zero_who']} pbi={out['pbi']} pbf={out['pbf']}",
        f"probe: rows={len(out['probe'])} probeget={len(out['probeget'])} "
        f"replay={len(out['replay'])} txt_zero={len(out['txt_zero'])} "
        f"unverified={len(out['txt_unverified'])}",
        f"death: fault={out['fault']} udx={out['udx']} signals={out['signals']}",
        f"alive_threads={len(out['alive'])}",
    ]
    if "pubs" in out:
        lines.append(f"pubs={out['pubs']}")
    print("\n".join(lines))

    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
            f.write("\n")


if __name__ == "__main__":
    sys.exit(main())
