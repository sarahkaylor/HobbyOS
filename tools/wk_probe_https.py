#!/usr/bin/env python3
"""Probe: does HTTPS work at all once /CERTS/CA.PEM exists on the disk?

Boots a lane whose disk has a CA bundle staged, then navigates to a few real
HTTPS sites in sequence and records the driver's own fetch receipts:

  [WIN] net fetch url=... ok=.. status=.. bytes=.. effective=.. err=..
  [WIN] load-ok url=... ms=...
  [WIN] doc title=...

Used to separate "TLS/CA plumbing broken" from "page-rendering broken" before
writing the multi-site scenario suite.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wk_lane import Lane  # noqa: E402

URLS = [
    "https://example.com",
    "https://en.wikipedia.org/wiki/Cat",
    "https://news.ycombinator.com",
    "https://www.google.com",
]


def main():
    L = Lane("probe-https", stage_certs=True)
    out = {"boot_secs": round(L.boot_secs, 1), "urls": []}
    try:
        L.launch_browser()
        for url in URLS:
            rec = {"url": url}
            try:
                m, doc, failed = L.navigate(url, timeout=420)
                seg = L.text()[m:]
                fetch = re.findall(r"\[WIN\] net fetch url=(\S+) ok=(\d+) status=(\d+) "
                                   r"bytes=(\d+) effective=(\S*) err=(.*)", seg)
                rec["fetch"] = fetch[-1] if fetch else None
                rec["failed"] = failed
                rec["doc"] = doc
                rec["inputs"] = L.inputs()
                rec["buttons"] = L.buttons()
                rec["links"] = re.findall(r"\[WIN\] links (.*)", seg)[-1:] if \
                    re.findall(r"\[WIN\] links (.*)", seg) else []
                L.shot("nav-" + re.sub(r"[^a-z0-9]+", "-", url.lower())[:40])
            except Exception as e:
                rec["error"] = str(e)
                L.shot("nav-err-" + re.sub(r"[^a-z0-9]+", "-", url.lower())[:30])
            out["urls"].append(rec)
            print(json.dumps(rec, indent=2)[:1200], flush=True)
    finally:
        L.report(os.path.join(L.dir, "report.json"), out)
        L.quit()


if __name__ == "__main__":
    main()
