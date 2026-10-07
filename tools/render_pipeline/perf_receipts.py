#!/usr/bin/env python3
"""
perf_receipts.py — wall-clock rendering receipts from a HobbyOS WK-5 serial log.

Parses the [WIN] perf/telemetry markers the fork's WK5WindowDriver emits and
produces (a) a machine-readable JSON receipt and (b) a compact human table.

Extracted markers (all byte-compatible with the driver at the 2026-10-07
frozen tip; unknown [WIN] lines are counted and reported, never fatal):

  [WIN] frame-times render=<N>ms blit=<N>ms total=<N>ms
  [WIN] viewport apply begin <W>x<H>
  [WIN] viewport apply end ms=<N>
  [WIN] reblit cached view=<W>x<H> rect=<W>x<H>+<dx> at <x>,<y>
  [WIN] load-ok url=<url> ms=<N>
  [WIN] frame view=<W>x<H> nonZero=<n> checksum=0x<h>
  [WIN] geom x=<x> y=<y> w=<w> h=<h>
  [WIN] painted rect=<W>x<H>+<dx> at <x>,<y>
  [WIN] close ok / [WIN] exit rc=<n> / [WIN] exit rc=0
  [WIN] stable-repaint (settle budget=<n>)
  [WIN] render-scale=<s> (marker=<n>)

Usage:
  perf_receipts.py <serial.log> [<serial2.log> ...] [--repeat N] [--json OUT]
      --repeat N   re-run the extraction N times over the same inputs and
                   report aggregate stats (min/median/max) for the per-run
                   perf numbers (used by run_rp.sh to shake out run-to-run
                   variance of a single receipt).
      --json OUT   also write the JSON receipt to OUT.

Exit codes: 0 = parsed ok (even with missing optional markers), 1 = usage,
2 = no input readable / nothing parseable.

This is part of the render-pipeline verification & measurement harness
(tools/render_pipeline). See docs/browser/render-pipeline-baselines.md.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import sys

# ---- marker patterns (kept as literal-ish regexes so the receipts table
# ---- shows the exact driver spelling for each) ---------------------------
RE_FRAME_TIMES = re.compile(
    r"\[WIN\] frame-times render=(\d+)ms blit=(\d+)ms total=(\d+)ms")
RE_VIEWPORT_BEGIN = re.compile(r"\[WIN\] viewport apply begin (\d+)x(\d+)")
RE_VIEWPORT_END = re.compile(r"\[WIN\] viewport apply end ms=(\d+)")
RE_REBLIT = re.compile(
    r"\[WIN\] reblit cached view=(\d+)x(\d+) rect=(\d+)x(\d+)\+(\d+) at (\d+),(\d+)")
RE_LOAD_OK = re.compile(r"\[WIN\] load-ok url=(\S+) ms=(\d+)")
RE_LOAD_START = re.compile(r"\[WIN\] load start (?:fixture|url)=(\S+)\s")
RE_LOAD_FAIL = re.compile(r"\[WIN\] load open-fail")
RE_FRAME = re.compile(
    r"\[WIN\] frame view=(\d+)x(\d+) nonZero=(\d+) checksum=0x([0-9a-fA-F]+)")
RE_GEOM = re.compile(r"\[WIN\] geom x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
# painted rect: driver current format "WxH+dx at x,y", legacy "WxH at x,y"
RE_PAINTED = re.compile(
    r"\[WIN\] painted rect=(\d+)x(\d+)(?:\+(-?\d+))? at (-?\d+),(-?\d+)")
RE_CLOSE_OK = re.compile(r"\[WIN\] close ok")
RE_EXIT = re.compile(r"\[WIN\] exit rc=(\d+)")
RE_STABLE = re.compile(r"\[WIN\] stable-repaint \(settle budget=(\d+)\)")
RE_RENDER_SCALE = re.compile(r"\[WIN\] render-scale=(\S+)\s*\(marker=(\d+)\)")


def parse_log(path: str) -> dict:
    """Parse one serial log into a receipt dict (never raises on content)."""
    with open(path, errors="ignore") as fh:
        text = fh.read()

    lines = text.splitlines()
    rec = {
        "log": path,
        "size_bytes": os.path.getsize(path),
        "lines": len(lines),
        "win_marker_total": 0,
        "win_marker_unknown": [],
        "boot": None,
        "created": None,
        "page": None,
        "geom": {},
        "geoms": [],
        "loads": [],
        "frames": [],
        "frame_times": [],
        "viewport_applies": [],
        "reblits": [],
        "painted": [],
        "close_ok": False,
        "exit_rc": None,
        "stable_repaints": [],
        "render_scale": None,
        "unknown_examples": [],
    }

    unknown = 0
    for ln in lines:
        if "[WIN]" not in ln:
            continue
        rec["win_marker_total"] += 1

        m = RE_FRAME_TIMES.search(ln)
        if m:
            rec["frame_times"].append(
                {"render": int(m.group(1)), "blit": int(m.group(2)),
                 "total": int(m.group(3))})
            continue
        m = RE_VIEWPORT_BEGIN.search(ln)
        if m:
            rec["viewport_applies"].append(
                {"w": int(m.group(1)), "h": int(m.group(2)), "begin_ms": None,
                 "end_ms": None})
            continue
        m = RE_VIEWPORT_END.search(ln)
        if m:
            if rec["viewport_applies"] and \
                    rec["viewport_applies"][-1]["end_ms"] is None:
                rec["viewport_applies"][-1]["end_ms"] = int(m.group(1))
            else:
                rec["viewport_applies"].append(
                    {"w": None, "h": None, "begin_ms": None,
                     "end_ms": int(m.group(1))})
            continue
        m = RE_REBLIT.search(ln)
        if m:
            rec["reblits"].append(
                {"view_w": int(m.group(1)), "view_h": int(m.group(2)),
                 "rect_w": int(m.group(3)), "rect_h": int(m.group(4)),
                 "dx": int(m.group(5)), "x": int(m.group(6)), "y": int(m.group(7))})
            continue
        m = RE_LOAD_OK.search(ln)
        if m:
            rec["loads"].append({"url": m.group(1), "ms": int(m.group(2)),
                                 "kind": "ok"})
            continue
        m = RE_LOAD_FAIL.search(ln)
        if m:
            rec["loads"].append({"url": None, "ms": None, "kind": "open-fail"})
            continue
        m = RE_LOAD_START.search(ln)
        if m:
            rec["loads"].append({"url": m.group(1), "ms": None, "kind": "start"})
            continue
        m = RE_FRAME.search(ln)
        if m:
            rec["frames"].append(
                {"view_w": int(m.group(1)), "view_h": int(m.group(2)),
                 "nonzero": int(m.group(3)),
                 "checksum": "0x%08x" % int(m.group(4), 16)})
            continue
        m = RE_GEOM.search(ln)
        if m:
            g = {"x": int(m.group(1)), "y": int(m.group(2)),
                 "w": int(m.group(3)), "h": int(m.group(4))}
            if not rec["geom"]:
                rec["geom"] = g
            rec["geoms"].append(g)
            continue
        m = RE_PAINTED.search(ln)
        if m:
            rec["painted"].append(
                {"w": int(m.group(1)), "h": int(m.group(2)),
                 "dx": int(m.group(3)) if m.group(3) else 0,
                 "x": int(m.group(4)), "y": int(m.group(5))})
            continue
        m = RE_CLOSE_OK.search(ln)
        if m:
            rec["close_ok"] = True
            continue
        m = RE_EXIT.search(ln)
        if m:
            rec["exit_rc"] = int(m.group(1))
            continue
        m = RE_STABLE.search(ln)
        if m:
            rec["stable_repaints"].append(int(m.group(1)))
            continue
        m = RE_RENDER_SCALE.search(ln)
        if m:
            rec["render_scale"] = m.group(1)
            continue
        m = re.search(r"\[WIN\] BOOT mode=(\S+) start=(\S+)", ln)
        if m:
            rec["boot"] = {"mode": m.group(1), "start": m.group(2)}
            continue
        m = re.search(r"\[WIN\] created pref=(\d+)x(\d+)", ln)
        if m:
            rec["created"] = {"pref_w": int(m.group(1)), "pref_h": int(m.group(2))}
            continue
        m = re.search(r"\[WIN\] page id=\S+ viewport=(\d+)x(\d+)", ln)
        if m:
            rec["page"] = {"viewport_w": int(m.group(1)), "viewport_h": int(m.group(2))}
            continue
        m = re.search(r"\[WIN\] (init webprocess=ok|nav name=\S+|url-prompt"
                      r"|url-entry=\S+|key=\S+|click .*|link -> .*"
                      r"|wheel .*|scroll .*|net .*|doc title=.*|load start .*"
                      r"|load diag .*|load-heartbeat .*|load-ok pending-frag=\S+)", ln)
        if m:
            rec.setdefault("info", []).append(m.group(1).split()[0])
            continue
        # everything else — counted, non-fatal
        unknown += 1
        if len(rec["unknown_examples"]) < 5:
            rec["unknown_examples"].append(ln.strip()[:120])

    rec["win_marker_unknown"] = unknown
    rec["unknown_count"] = unknown

    # Derive per-load render scenes: pair each load-ok with frame-times that
    # followed it (until the next load start / close), for the scene table.
    scenes = []
    current = None
    order = [(ln.get("kind", ""), ln.get("url"), ln.get("ms"))
             for ln in rec["loads"]]
    # simpler: rebuild scene boundaries from the ordered loads list,
    # then bucket frame-times by position.
    load_oks = [l for l in rec["loads"] if l["kind"] == "ok"]
    fti = 0
    for lidx, l in enumerate(rec["loads"]):
        if l["kind"] != "ok":
            continue
        scene = {"url": l["url"], "load_ms": l["ms"],
                 "frames": [], "frame_times": []}
        scenes.append(scene)
    # frame-times between load events: attach each frame-time to the most
    # recent load-ok scene (crude but stable; fine for receipts).
    last_ok = None
    for l in rec["loads"]:
        if l["kind"] == "ok":
            last_ok = l["url"]
    for ft in rec["frame_times"]:
        pass  # bucketing below by count/position instead
    # assign frame-times to scenes in order (ok loads only)
    ok_scene = [s for s in scenes]
    pos = 0
    for ft in rec["frame_times"]:
        if pos < len(ok_scene):
            ok_scene[pos]["frame_times"].append(ft)
        else:
            pos = len(ok_scene) - 1
            if pos >= 0:
                ok_scene[pos]["frame_times"].append(ft)
        pos = max(pos + 1, 0)
        if pos > len(ok_scene):
            pos = len(ok_scene)
    for s in ok_scene:
        if s["frame_times"]:
            s["render_avg_ms"] = round(statistics.mean(
                f["render"] for f in s["frame_times"]), 1)
            s["blit_avg_ms"] = round(statistics.mean(
                f["blit"] for f in s["frame_times"]), 1)
            s["total_avg_ms"] = round(statistics.mean(
                f["total"] for f in s["frame_times"]), 1)
    rec["scenes"] = ok_scene
    return rec


def aggregate(parses: list[dict]) -> dict:
    """Aggregate stats for --repeat N across repeated parses."""
    def stats_for(key, subkey):
        vals = []
        for p in parses:
            vals += [x[subkey] for x in p[key]]
        if not vals:
            return None
        return {"n": len(vals), "min": min(vals), "median": round(
            statistics.median(vals), 1), "max": max(vals),
            "mean": round(statistics.mean(vals), 1)}

    return {
        "n_parses": len(parses),
        "frame_times_render_ms": stats_for("frame_times", "render"),
        "frame_times_blit_ms": stats_for("frame_times", "blit"),
        "frame_times_total_ms": stats_for("frame_times", "total"),
        "viewport_apply_end_ms": stats_for(
            [p for p in parses for _ in [0]], None) and None or None,
    }


def agg(key):
    """small helper: pull a flat list of ints across parses for one key."""
    def _inner(parses, key, sub):
        vals = []
        for p in parses:
            vals += [x[sub] for x in p[key] if x.get(sub) is not None]
        return vals
    return _inner


def fmt_table(recs: list[dict]) -> str:
    out = []
    for rec in recs:
        out.append(f"== {rec['log']}  {rec['size_bytes']} B, "
                   f"{rec['lines']} lines, {rec['win_marker_total']} [WIN] "
                   f"lines ({rec['unknown_count']} unknown)")
        if rec["boot"]:
            out.append(f"  BOOT      mode={rec['boot']['mode']} "
                       f"start={rec['boot']['start']}")
        if rec["geom"]:
            g = rec["geom"]
            out.append(f"  GEOM      x={g['x']} y={g['y']} w={g['w']} h={g['h']} "
                       f"(content rect; {len(rec['geoms'])} geom events)")
        if rec["render_scale"]:
            out.append(f"  RENDER-SCALE {rec['render_scale']}")
        out.append(f"  LOADS     {len(rec['loads'])} events "
                   f"({sum(1 for l in rec['loads'] if l['kind']=='ok')} ok, "
                   f"{sum(1 for l in rec['loads'] if l['kind']=='open-fail')} open-fail)")
        for l in rec["loads"]:
            if l["kind"] == "ok":
                out.append(f"    load-ok {l['url']} ms={l['ms']}")
            elif l["kind"] == "open-fail":
                out.append(f"    load open-fail")
        out.append(f"  FRAMES    {len(rec['frames'])}")
        if rec["frame_times"]:
            ft = rec["frame_times"]
            r_avg = statistics.mean(f["render"] for f in ft)
            b_avg = statistics.mean(f["blit"] for f in ft)
            t_avg = statistics.mean(f["total"] for f in ft)
            out.append(f"  FRAME-TIMES n={len(ft)} "
                       f"render avg={r_avg:.1f}ms blit avg={b_avg:.1f}ms "
                       f"total avg={t_avg:.1f}ms "
                       f"(min total={min(f['total'] for f in ft)}ms, "
                       f"max total={max(f['total'] for f in ft)}ms)")
        if rec["viewport_applies"]:
            vals = [v["end_ms"] for v in rec["viewport_applies"]
                    if v.get("end_ms") is not None]
            if vals:
                out.append(f"  VIEWPORT-APPLY n={len(vals)} "
                           f"end avg={statistics.mean(vals):.1f}ms "
                           f"(min={min(vals)}ms max={max(vals)}ms)")
        if rec["reblits"]:
            out.append(f"  REBLIT-CACHED n={len(rec['reblits'])}")
        out.append(f"  CLOSE     ok={rec['close_ok']} exit_rc={rec['exit_rc']}")
        if rec["stable_repaints"]:
            out.append(f"  STABLE-REPAINTS n={len(rec['stable_repaints'])}")
        if rec["unknown_count"]:
            out.append(f"  UNKNOWN   {rec['unknown_count']} unmatched [WIN] lines")
            for ex in rec["unknown_examples"]:
                out.append(f"    e.g. {ex}")
    return "\n".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="perf_receipts.py",
        description="Extract [WIN] rendering receipts from a WK-5 serial log.")
    ap.add_argument("logs", nargs="+", help="serial log(s) to parse")
    ap.add_argument("--repeat", type=int, default=1,
                    help="re-run extraction N times over the same inputs and "
                         "report aggregate stats (default 1)")
    ap.add_argument("--json", metavar="OUT", default=None,
                    help="also write the JSON receipt to OUT")
    args = ap.parse_args(argv)

    if not args.logs:
        ap.error("no serial logs given")
    readable = [p for p in args.logs if os.path.isfile(p)]
    if not readable:
        print("no readable input logs", file=sys.stderr)
        return 2

    parses = []
    for _ in range(max(1, args.repeat)):
        for p in readable:
            parses.append(parse_log(p))

    table = fmt_table(parses[:len(readable)])
    print(table)

    result = {
        "tool": "perf_receipts.py",
        "repeat": max(1, args.repeat),
        "inputs": readable,
        "receipts": [{
            "log": r["log"], "boot": r["boot"], "created": r["created"],
            "page": r["page"], "geom": r["geom"], "loads": r["loads"],
            "frames": r["frames"], "frame_times": r["frame_times"],
            "viewport_applies": r["viewport_applies"], "reblits": r["reblits"],
            "painted": r["painted"], "close_ok": r["close_ok"],
            "exit_rc": r["exit_rc"], "stable_repaints": r["stable_repaints"],
            "render_scale": r["render_scale"], "scenes": r["scenes"],
            "win_marker_total": r["win_marker_total"],
            "unknown_count": r["unknown_count"],
        } for r in parses],
    }
    # aggregated perf numbers across all parses (repeat>1)
    if len(parses) > 1:
        def flat(key, sub):
            return [x[sub] for r in parses for x in r[key]
                    if x.get(sub) is not None]
        agg_ft_r = flat("frame_times", "render")
        agg_ft_b = flat("frame_times", "blit")
        agg_ft_t = flat("frame_times", "total")
        agg_va = flat("viewport_applies", "end_ms")
        agg_lm = [l["ms"] for r in parses for l in r["loads"]
                  if l["kind"] == "ok" and l.get("ms") is not None]
        def stat(vals):
            if not vals:
                return None
            return {"n": len(vals), "min": min(vals),
                    "median": round(statistics.median(vals), 1),
                    "max": max(vals), "mean": round(statistics.mean(vals), 1)}
        result["aggregate"] = {
            "frame_times_render_ms": stat(agg_ft_r),
            "frame_times_blit_ms": stat(agg_ft_b),
            "frame_times_total_ms": stat(agg_ft_t),
            "viewport_apply_end_ms": stat(agg_va),
            "load_ok_ms": stat(agg_lm),
        }

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(result, fh, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
