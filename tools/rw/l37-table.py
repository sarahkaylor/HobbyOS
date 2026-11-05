#!/usr/bin/env python3
"""tools/rw/l37-table.py — build the L37 fault table from evidence serials.

Each evidence serial carries the [SIGDUMP] instrument: every GPR, the
interrupted RSP/RIP, 48 stack qwords and the region rows.  This prints the
table the launch log quotes, one row per run, plus the full ucontext of the
first fault when --full is passed.  Module offsets use the launcher's
`dlopen ok /RWDSO/UnityPlayer.so ... base=` row, so a rip is named
`UnityPlayer+0x...` and not by an absolute address.
"""
import argparse
import glob
import os
import re

HEX = re.compile(r"(0x[0-9a-fA-F]+)")


def field(line, key):
    m = re.search(r"(?:^|\s)" + key + r"=(0x[0-9a-fA-F]+)", line)
    return int(m.group(1), 16) if m else None


def parse(path):
    base = None
    rec = None
    gpr = {}
    stk = {}
    sigsig = resumes = None
    for line in open(path, "r", errors="replace"):
        if base is None:
            m = re.search(r"dlopen ok /RWDSO/UnityPlayer\.so .*base=(0x[0-9a-fA-F]+)",
                          line)
            if m:
                base = int(m.group(1), 16)
        if rec is None and "[SIGDUMP]" in line:
            rec = {k: field(line, k) for k in
                   ("rip", "rsp", "cs", "vec", "err", "cr2")}
            m = re.search(r"pid=(\d+)", line)
            rec["pid"] = int(m.group(1)) if m else None
            continue
        if rec is None:
            continue
        if "[SGPR]" in line:
            for k, v in re.findall(r"(r[a-z0-9]+)=(0x[0-9a-fA-F]+)", line):
                gpr.setdefault(k, int(v, 16))
        elif "[SLOT-50]" in line and "slot50" not in rec:
            m = re.search(r"val=(0x[0-9a-fA-F]+)\s+==rsp=(\d)", line)
            if m:
                rec["slot50"] = int(m.group(1), 16)
                rec["slot50_is_rsp"] = m.group(2) == "1"
        elif "[STK-80]" in line:
            stk.setdefault("-80", re.findall(r"0x[0-9a-fA-F]{16}", line)[:16])
        elif "[STK+00]" in line:
            stk.setdefault("+00", re.findall(r"0x[0-9a-fA-F]{16}", line)[:16])
        elif "[SIGSIG]" in line and sigsig is None:
            m = re.search(r"trapframe_images=(\d+)", line)
            if m:
                sigsig = int(m.group(1))
        elif "[SRESUME]" in line and resumes is None:
            m = re.search(r"hit=(\d)", line)
            if m:
                resumes = int(m.group(1))
    rec["gpr"] = gpr
    rec["stk"] = stk
    return base, rec, sigsig, resumes


def module(base, rip):
    if base and rip and rip >= base:
        return "UnityPlayer+0x%x" % (rip - base)
    return "0x%x" % rip if rip else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("glob")
    ap.add_argument("--full", action="store_true",
                    help="print the full ucontext + 16 stack qwords per run")
    a = ap.parse_args()

    print("| run | pid | module | rip | cr2 | rw | slot@rsp-0x50 | == rsp | "
          "tf-images | resume-hit |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for path in sorted(glob.glob(a.glob)):
        base, rec, sigsig, resumes = parse(path)
        name = os.path.basename(path).split(".")[0]
        if not rec:
            print("| %s | — | no fault | — | — | — | — | — | %s | %s |" %
                  (name, sigsig, resumes))
            continue
        print("| %s | %s | %s | 0x%x | 0x%x | %s | %s | %s | %s | %s |" % (
            name, rec["pid"], module(base, rec["rip"]), rec["rip"],
            rec["cr2"], "w" if (rec["err"] or 0) & 2 else "r",
            ("0x%x" % rec["slot50"]) if "slot50" in rec else "—",
            {True: "**yes**", False: "no"}.get(rec.get("slot50_is_rsp"), "—"),
            sigsig if sigsig is not None else "—",
            resumes if resumes is not None else "—"))

    if a.full:
        for path in sorted(glob.glob(a.glob)):
            base, rec, _, _ = parse(path)
            if not rec:
                continue
            print("\n### %s\n" % os.path.basename(path))
            print("`rip=%s cr2=0x%x rsp=0x%x cs=0x%x vec=%s err=0x%x`\n" % (
                module(base, rec["rip"]), rec["cr2"], rec["rsp"],
                rec["cs"] or 0, rec["vec"], rec["err"] or 0))
            order = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
                     "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
            print("GPR: " + " ".join("%s=0x%x" % (k, rec["gpr"][k])
                                     for k in order if k in rec["gpr"]))
            for tag in ("-80", "+00"):
                if tag in rec["stk"]:
                    print("\n`[STK%s]`  %s" % (tag, " ".join(rec["stk"][tag])))


if __name__ == "__main__":
    main()
