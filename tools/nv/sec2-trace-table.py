#!/usr/bin/env python3
"""Turn the kernel's [NV-SEC2T] rows into tables, from the logs themselves.

Every number in the report is read out of the serial logs by this script, so a
claim cannot drift from its evidence.  No third-party modules.

    python3 tools/nv/sec2-trace-table.py evidence/nv/t7a.log [more.log ...]

Emits, per log:
  * one row per trace phase (pre / post / at10ms / at100ms) with ctl, bcr,
    tcfg624, tcfg600 -- the columns the card asks for in the same row;
  * the post_first line (the read nearest the write, its ptimer stamp and the
    measured read cost) and the summary line (bit1_ever / running_ever);
  * the write-lands control and the verdict of every row that has one;
  * the ctl-write-sweep.
Exit status is 1 when a log has no trace rows (so a boot without the
instrument cannot be counted as evidence).
"""
import json
import re
import sys

ROW = re.compile(r"^\[NV-SEC2T\] (\S+) (\S+) ?(.*)$")
KV = re.compile(r"([A-Za-z0-9_]+)=((?:0x)?[0-9A-Fa-f]+)")


def parse_line(line):
    m = ROW.match(line.strip())
    if not m:
        return None
    tag, phase, rest = m.group(1), m.group(2), m.group(3)
    fields = {}
    for k, v in KV.findall(rest):
        fields[k] = int(v, 16) if v.lower().startswith("0x") else v
    # verdict text is everything the key=value scan did not claim
    vm = re.search(r"verdict=([A-Z][A-Z0-9-]+)", rest)
    if vm:
        fields["verdict"] = vm.group(1)
    return {"tag": tag, "phase": phase, "fields": fields, "raw": line.strip()}


def parse_log(path):
    rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            r = parse_line(line)
            if r:
                rows.append(r)
    return rows


def table(headers, data):
    out = ["| " + " | ".join(headers) + " |"]
    out.append("|" + "|".join(["---"] * len(headers)) + "|")
    for d in data:
        out.append("| " + " | ".join(str(x) for x in d) + " |")
    return "\n".join(out)


def h(v):
    return "0x%X" % v if isinstance(v, int) else str(v)


def main(paths):
    rc = 0
    all_rows = {}
    for p in paths:
        rows = parse_log(p)
        all_rows[p] = rows
        boot = p.rsplit("/", 1)[-1].replace(".log", "")
        print("=" * 100)
        print("LOG", p, "trace rows:", len(rows))
        if not rows:
            print("NO TRACE ROWS -- not evidence")
            rc = 1
            continue

        phases = [r for r in rows if r["phase"] in
                  ("pre", "post", "at10ms", "at100ms", "before-reset",
                   "after-reset", "after-bcr")]
        data = []
        for r in phases:
            f = r["fields"]
            data.append([r["tag"], r["phase"], h(f.get("dt_ns")),
                         h(f.get("ctl")), h(f.get("bcr")),
                         h(f.get("tcfg624")), h(f.get("tcfg600")),
                         h(f.get("ent")), h(f.get("alias130")),
                         h(f.get("pc"))])
        print("\n### register trace (one row per phase)")
        print(table(["att", "phase", "dt_ns", "ctl", "bcr", "tcfg624",
                     "tcfg600", "ent", "alias130", "pc"], data))

        print("\n### the read nearest each trigger")
        data = []
        for r in rows:
            if r["phase"] != "post_first":
                continue
            f = r["fields"]
            data.append([r["tag"], h(f.get("ctl")), h(f.get("at_ns")),
                         h(f.get("read_cost_ns")), f.get("bit1"),
                         f.get("running")])
        print(table(["att", "ctl_first", "at_ns", "read_cost_ns", "bit1",
                     "running"], data))

        print("\n### burst (ptimer stamp : ctl) and summaries")
        for r in rows:
            if r["phase"] in ("burst", "summary"):
                print("  " + r["raw"].replace("[NV-SEC2T] ", ""))
        print("\n### write-lands control and other verdict rows")
        for r in rows:
            if "verdict" in r["fields"] and r["phase"] != "summary":
                f = r["fields"]
                print("  %-2s %-14s %s" % (r["tag"], r["phase"],
                                           f.get("verdict")))
        sweep = [r for r in rows if r["phase"] == "ctl-write-sweep"]
        for r in sweep:
            print("  " + r["raw"].replace("[NV-SEC2T] ", ""))

    json.dump(all_rows, open("evidence/nv/sec2-trace.json", "w"), indent=1)
    print("\nwrote evidence/nv/sec2-trace.json")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["evidence/nv/t7a.log"]))
