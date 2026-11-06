#!/usr/bin/env python3
"""tools/rw/l62-xsw-follow.py -- timestamp every [XSW] present row as it lands.

The serial log carries no timestamps, and the present counter is the only
frame clock the guest exposes.  This follows the log the harness is writing and
records (host epoch-ms, n=, w=, h=) for each row, so an fps figure is a delta
over a measured wall-clock window instead of "frames / whatever the run lasted".

Usage: l62-xsw-follow.py <serial.log> <out.jsonl>
Stop with SIGTERM/SIGINT; the file is flushed per row.
"""
import json
import os
import re
import signal
import sys
import time

ROW = re.compile(r"\[XSW\] n=(\d+) swap=(\d+) put=(\d+) putok=(\d+)"
                 r"[^\n]*? w=(-?\d+) h=(-?\d+)")
STOP = False


def _stop(_sig, _frm):
    global STOP
    STOP = True


def main():
    log_path, out_path = sys.argv[1], sys.argv[2]
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    out = open(out_path, "w", buffering=1)
    buf = ""
    off = 0
    first_pass = True
    # Rows already present are recorded with t=null: they are before the
    # follower started and must not be given a made-up timestamp.
    while not STOP:
        try:
            size = os.path.getsize(log_path)
        except OSError:
            time.sleep(0.1)
            continue
        if size > off:
            with open(log_path, "rb") as f:
                f.seek(off)
                chunk = f.read(size - off).decode("latin-1")
            off = size
            buf += chunk
            *lines, buf = buf.split("\n")
            now = int(time.time() * 1000)
            for line in lines:
                m = ROW.search(line)
                if m:
                    out.write(json.dumps({
                        "t": None if first_pass else now,
                        "n": int(m.group(1)), "swap": int(m.group(2)),
                        "putok": int(m.group(4)), "w": int(m.group(5)),
                        "h": int(m.group(6))}) + "\n")
            first_pass = False
        elif size < off:
            # The harness truncates the log at every boot (`: > serial.log`);
            # a follower that only grows its offset would go deaf here.
            off = 0
            buf = ""
        time.sleep(0.1)
    out.close()


if __name__ == "__main__":
    main()
