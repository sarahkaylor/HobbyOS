#!/usr/bin/env python3
"""L75: reduce a run's serial log to the numbers the lane's ladder reports.

The guest's serial output interleaves (one row is several uart_puts calls and
another CPU can splice a whole row into the middle of it), so every pattern
here is matched ANYWHERE in the line and the fields are pulled out
individually -- a start-of-line parser reports phantom zeros.

Usage: l75-summary.py <serial.log> [--json out.json]
"""
import argparse
import json
import re
import sys

FIELDS = [
    # (name, regex)
    ("fr_used", re.compile(r"fr_used=(\d+)")),
    ("reqs", re.compile(r"\breqs=(\d+)")),
    ("resets", re.compile(r"\bresets=(\d+)")),
]


def last_of(text, name, rx):
    v = None
    for m in rx.finditer(text):
        v = int(m.group(1))
    return v


def all_of(text, rx):
    return [int(m.group(1)) for m in rx.finditer(text)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--json")
    a = ap.parse_args()
    text = open(a.log, "r", errors="replace").read()

    out = {}
    out["controller_resets"] = len(re.findall(r"\[BLKQ\] controller reset #", text))
    out["reinit_rows"] = len(re.findall(r"\[BLKQ\] reinit cap=", text))
    out["reinit_ok"] = len(re.findall(r"\[BLKQ\] reinit ok", text))
    out["reinit_fail"] = len(re.findall(r"\[BLKQ\] reset: gave up", text))
    out["ns_rows"] = len(re.findall(r"\[BLKQ\] ns nsid=1", text))
    out["flush_rows"] = len(re.findall(r"\[BLKC\] post-reset flush dropped=", text))
    out["mmio_change_rows"] = len(re.findall(r"\[BLKQ\] mmio cc=", text))
    out["device_off"] = len(re.findall(r"\[BLKQ\] device off cid=", text))
    out["stalls"] = len(re.findall(r"\[BLKQ\] stall cid=", text))
    out["untrusted"] = len(re.findall(r"untrusted completion re-read", text))
    out["txt_zero"] = len(re.findall(r"\[TXT\] ZERO", text))
    out["img_sum"] = len(re.findall(r"\[TXT\] IMG SUM", text))
    out["eio"] = len(re.findall(r"\[EIO\] pid=", text))
    out["faults"] = len(re.findall(r"memory fault VA=", text))
    out["kernel_kills"] = len(re.findall(r"Kernel-mode #PF", text))
    out["presents"] = (all_of(text, re.compile(r"\[XSW\] n=(\d+)")) or [0])[-1]
    out["present_rows"] = len(re.findall(r"\[XSW\] n=", text))
    out["menus"] = len(re.findall(r"\[XO\] ", text))
    out["alive_set"] = len(re.findall(r"\[ALIVE-SET\]", text))
    # a page published with EVERY byte zero is a zero page; on an exec page
    # that is the L69/L73 failure, on an anon page it is normal demand zero.
    out["pub_zero4096"] = len(re.findall(r"zero=4096", text))
    out["pub_zero_exec"] = len(re.findall(r"prot=RX[^\n]*zero=4096", text))
    out["idlestuck"] = len(re.findall(r"\[IDLESTUCK\]", text))

    # the last frame-pool / device row
    for name, rx in FIELDS:
        out[name] = last_of(text, name, rx)

    # the selfcheck row (fields may be spliced, so pull each one)
    sc = re.search(r"\[BLKQ\] selfcheck", text)
    out["selfcheck"] = None
    if sc:
        tail = text[sc.start():sc.start() + 400]
        d = {}
        for k in ("cache1", "cache2", "raw1", "raw2", "reinit_rc", "ms", "match"):
            m = re.search(k + r"=0x([0-9A-Fa-f]+)|" + k + r"=(-?\d+)", tail)
            if m:
                d[k] = int(m.group(1), 16) if m.group(1) else int(m.group(2))
        out["selfcheck"] = d

    # the block-stress probe: the highest counter seen in any [BKS] row
    bks = {}
    for k in ("reads", "write_verify", "bad", "mismatch"):
        vals = all_of(text, re.compile(k + r"=(\d+)"))
        bks[k] = max(vals) if vals else 0
    out["bks_rows"] = len(re.findall(r"\[BKS\] stress", text))
    out["bks"] = bks

    # QEMU trace: every CC write, and whether any was not ours
    out["qemu_cc_writes"] = len(re.findall(r"pci_nvme_mmio_cfg", text))
    out["qemu_stopped"] = len(re.findall(r"pci_nvme_mmio_stopped", text))
    out["qemu_flr"] = len(re.findall(r"pci_nvme_pci_reset", text))

    print("L75 run summary: %s" % a.log)
    for k in ("controller_resets", "reinit_rows", "reinit_ok", "reinit_fail",
              "ns_rows", "flush_rows", "device_off", "stalls", "untrusted",
              "txt_zero", "img_sum", "eio", "faults", "kernel_kills",
              "presents", "present_rows", "menus", "alive_set", "idlestuck",
              "pub_zero4096", "pub_zero_exec",
              "fr_used", "reqs", "mmio_change_rows", "bks_rows"):
        print("  %-18s %s" % (k + ":", out.get(k)))
    print("  %-18s %s" % ("selfcheck:", out["selfcheck"]))
    print("  %-18s %s" % ("bks:", out["bks"]))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
        print("  wrote %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
