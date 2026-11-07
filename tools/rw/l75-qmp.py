#!/usr/bin/env python3
"""L75: dump the QEMU-side block-controller facts for a run, over QMP.

Run against the harness's QMP socket while the guest is live (or at the end
of the window).  It answers "did the HOST see a problem" independently of
anything the guest printed:

  * query-status            -- VM run state (a paused/guest-panicked VM would
                               explain a stalled device without a reset)
  * query-block / -stats    -- the drive's own counters: rd/wr operations,
                               failed_rd/failed_wr, flush counts.  A host-side
                               device reset (device_del, block error) shows up
                               here; a guest-side CC write does not.
  * query-cpus-fast         -- did a vCPU stop?
  * info pci                -- BAR addresses per device (the NVMe BAR is what
                               the frame pool must never contain)
  * info mtree (filtered)   -- the 32-bit PCI hole layout: whether the BAR
                               address is RAM or MMIO.

Usage: l75-qmp.py --qmp /path/to/sock [--out file.json] [--wait S]
Prints a compact human summary and (optionally) writes the raw replies.
"""
import argparse
import json
import os
import socket
import sys
import time


class QMP:
    def __init__(self, path, timeout=10.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.f = self.s.makefile("rwb", buffering=0)
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.f.readline()
        if not line:
            raise RuntimeError("qmp: eof")
        return json.loads(line.decode())

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        while True:
            r = self._read()
            if "event" in r:
                continue
            if "error" in r:
                raise RuntimeError("qmp %s: %s" % (name, r["error"]))
            return r.get("return")

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def close(self):
        try:
            self.f.close()
            self.s.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--out")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="retry this long for the socket to appear")
    a = ap.parse_args()

    t0 = time.time()
    while not os.path.exists(a.qmp):
        if time.time() - t0 > a.wait:
            print("QMP: no socket at %s" % a.qmp)
            return 2
        time.sleep(0.5)

    q = QMP(a.qmp)
    out = {}
    out["status"] = q.cmd("query-status")
    out["block"] = q.cmd("query-block")
    out["blockstats"] = q.cmd("query-blockstats")
    try:
        out["cpus"] = q.cmd("query-cpus-fast")
    except RuntimeError as e:
        out["cpus"] = str(e)
    out["pci"] = q.hmp("info pci")
    out["mtree"] = q.hmp("info mtree")
    q.close()

    if a.out:
        with open(a.out, "w") as fh:
            json.dump(out, fh, indent=1)
        print("QMP: wrote %s" % a.out)

    st = out["status"]
    print("QMP status: running=%s paused=%s" % (st.get("running"), st.get("paused")))
    for d in out["block"]:
        dev = d.get("device", "?")
        ins = d.get("inserted") or {}
        print("QMP block %s: file=%s ro=%s" % (dev, ins.get("file"), ins.get("ro")))
    for d in out["blockstats"]:
        stt = d.get("stats", {})
        keys = ("rd_operations", "wr_operations", "failed_rd_operations",
                "failed_wr_operations", "flush_operations",
                "failed_flush_operations", "account_invalid",
                "account_failed")
        print("QMP stats %s: %s" % (
            d.get("device", "?"),
            " ".join("%s=%s" % (k, stt.get(k)) for k in keys)))
    pci = out["pci"]
    if isinstance(pci, str):
        for line in pci.splitlines():
            if "NVMe" in line or "nvme" in line:
                print("QMP pci: %s" % line.strip())
    mt = out["mtree"]
    if isinstance(mt, str):
        for line in mt.splitlines():
            s = line.strip()
            if s.startswith("0000000008") or s.startswith("0000000009") \
               or s.startswith("000000000a") or s.startswith("000000000b"):
                print("QMP mtree: %s" % s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
