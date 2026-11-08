#!/usr/bin/env python3
"""l86-report.py -- turn obj/l86/run*/l86.jsonl into the report's markdown
tables: one row per click (screen + content coordinates, diff pixels, bbox,
OCR after) plus the keyboard/geometry rows.

Usage: python3 tools/rw/l86-report.py obj/l86/run1/l86.jsonl
"""
import json
import sys


def load(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def short(s, n=90):
    s = " ".join((s or "").split())
    return (s[:n] + "...") if len(s) > n else s


def main():
    recs = load(sys.argv[1])
    clicks = [r for r in recs if r.get("ev") == "click"]
    results = {}
    for r in recs:
        if r.get("ev") == "click_result":
            results[(r.get("stage"), r.get("label"))] = r

    print("| # | stage | click (OCR-located) | screen | content | diff px | bbox | OCR after |")
    print("|---:|---|---|---|---|---:|---|---|")
    for i, c in enumerate(clicks, 1):
        res = results.get((c.get("stage"), c.get("label")), {})
        after = short(res.get("text"), 70)
        px = c.get("pixels")
        print("| %d | %s | %s | (%d,%d) | (%s,%s) | %s | %s | %s |"
              % (i, c.get("stage"), c.get("label"),
                 c["screen"][0], c["screen"][1],
                 c["content"][0], c["content"][1],
                 px if px is not None else "?", c.get("bbox") or "-",
                 after or "-"))

    print()
    print("| event | stage | detail |")
    print("|---|---|---|")
    for r in recs:
        ev = r.get("ev")
        if ev in ("geometry", "modal", "modal_gone", "seed_click", "seed_focus",
                  "seed_typed", "tab", "site_panel", "site_key", "site_double",
                  "generating", "stage", "done", "retry", "no_target",
                  "site_confirm_candidates", "focus"):
            detail = []
            for k in ("note", "text", "bbox", "pixels", "screen",
                      "field_before", "field_after", "ok"):
                if r.get(k) not in (None, "", []):
                    detail.append("%s=%s" % (k, short(str(r[k]), 120)))
            print("| %s | %s | %s |" % (ev, r.get("stage") or "-",
                                        "; ".join(detail) or "-"))


if __name__ == "__main__":
    main()
