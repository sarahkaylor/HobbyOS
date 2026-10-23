#!/usr/bin/env python3
"""Probe: does a SECOND navigation to a heavy page repaint, and how long is
the blank window?

Story: example.com -> Wikipedia/Cat -> example.com -> Wikipedia/Cat again.
The 2nd Cat load in lane s5 painted an EMPTY frame (`frame view=... nonZero=0`)
and the screenshot 2.5 s later was still blank.  This probe watches the frame
receipts for up to 180 s after each load-ok and records when a NON-EMPTY frame
actually lands, so the bug can be stated as fact (permanent vs. slow).
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wk_lane import Lane  # noqa: E402

SEQ = ["https://example.com",
       "https://en.wikipedia.org/wiki/Cat",
       "https://example.com",
       "https://en.wikipedia.org/wiki/Cat"]


def frames_since(L, since):
    return re.findall(r"\[WIN\] frame view=\d+x\d+ nonZero=(\d+) checksum=0x([0-9a-f]+)",
                      L.text()[since:])


def main():
    L = Lane("probe-repaint", stage_certs=True)
    out = []
    try:
        L.launch_browser()
        for n, url in enumerate(SEQ, 1):
            rec = {"n": n, "url": url}
            t0 = time.time()
            m, doc, failed = L.navigate(url, timeout=600)
            rec["load_ok_secs"] = round(time.time() - t0, 1)
            rec["failed"] = failed
            rec["doc"] = (doc or {}).get("raw")
            # watch for a non-empty frame for up to 180 s
            t1 = time.time()
            painted = None
            while time.time() - t1 < 180:
                fs = frames_since(L, m)
                nonempty = [f for f in fs if int(f[0]) > 0]
                if nonempty:
                    painted = round(time.time() - t1, 1)
                    break
                time.sleep(1.0)
            rec["nonempty_frame_after_s"] = painted
            rec["frames"] = frames_since(L, m)[-4:]
            L.shot("seq%d" % n)
            print(rec, flush=True)
            out.append(rec)
            if failed:
                break
    finally:
        L.report(os.path.join(L.dir, "report.json"), out)
        L.quit()


if __name__ == "__main__":
    main()
