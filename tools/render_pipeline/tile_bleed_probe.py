#!/usr/bin/env python3
"""
tile_bleed_probe.py — issue #1 probe: does the browser draw outside its
window when the WM re-tiles?

Brings up a desktop with the browser (WEBPROC.BIN from the given WebProcess
ELF) plus CONSOLE.BIN on the disk, loads a fixture page, then triggers a WM
re-tile by launching the console (second window — the WM's update_layout()
goes 1x1 -> 2x1, halving the browser window) and captures QMP screendumps
+ serial at every step.  Per capture pair it computes the pixels that
CHANGED outside the browser content rect (`[WIN] geom` rect, verified
against desktop.c/window.c), after masking the known legitimate overlay
regions (taskbar, window chrome, the second window's rect, the Apps-menu
rect, the cursor box).  Any remaining changed pixels are "unattributed" and
reported with bounds + color samples; persistence across the settle capture
flags the strongest bleed signal.

Scenarios:
  gentle    TALL loaded + settled, then console launched once (bare re-tile)
  load      console launched ~1.5 s into the TALL load (re-tile while loading)
  settle    console launched during the post-load settle-repaint burst
            (wheel-scroll first to force repaints, then re-tile mid-burst)
  openclose console opened and closed twice during the TALL load
  perf      no console; load HOME then TALL, collect frame receipts
            (back-to-back scene timings for the baseline table)

Baseline guidance (docs/browser/render-pipeline.md): "no bleed observed in
scenario X" is a valid, valuable baseline if the measurement is precise.

Usage:
  tile_bleed_probe.py --wp-elf PATH [--os-tree PATH] [--disk PATH]
      [--scenario gentle] [--evdir DIR] [--instance ID] [--keep]
      [--smp N] [--mem-mb N] [--timeout S]

Exit codes: 0 = probe completed (bleed or no-bleed is reported in the JSON),
1 = driver/step failure, 2 = usage/input error.  report.json is written to
<evdir>/report.json.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rp_lib as R  # noqa: E402

QCODES = {".": "dot", "-": "minus", "/": "slash", "_": "minus"}


def type_url(qmp: R.Qmp, url: str, pause: float = 0.25):
    for ch in url:
        qmp.key(QCODES.get(ch, ch), pause=pause)
    qmp.key("ret", pause=0.6)


def wallclock():
    return time.time()


# ------------------------------------------------------------- analysis --
def overlay_rects(geom, num_windows, cursor, menu_open=False,
                  menu_items=0):
    """Legitimate-overlay rects for the change census, in screen coords.
    geom: (x,y,w,h) content rect of the browser.  Returns list of
    (name, (x0,y0,x1,y1))."""
    rects = []
    rects.append(("taskbar", (0, R.TASKBAR_Y, R.SCREEN_W, R.SCREEN_H)))
    if geom:
        gx, gy, gw, gh = geom
        # browser window rect = content rect expanded by the chrome band
        wx, wy = gx - R.CHROME_L, gy - R.CHROME_T
        ww = gw + R.CHROME_L + R.CHROME_R
        wh = gh + R.CHROME_T + R.CHROME_B
        # chrome bands: title bar (full width, 34px), bottom, left, right —
        # all WM-owned window chrome, legitimate changes on focus/re-tile
        rects.append(("browser-chrome", (wx, wy, wx + ww, gy)))        # title bar
        rects.append(("browser-chrome", (wx, gy + gh, wx + ww, wy + wh)))  # bottom
        rects.append(("browser-chrome", (wx, gy, gx, gy + gh)))        # left
        rects.append(("browser-chrome", (gx + gw, gy, wx + ww, gy + gh)))  # right
    if num_windows >= 2:
        # second window rect (console at index 1) — its whole rect is
        # legitimate output (WM chrome + console terminal content).
        r = R.tile_rect(1, num_windows)
        if r:
            x, y, w, h = r
            rects.append(("second-window", (x, y, x + w, y + h)))
    if menu_open:
        n = min(max(menu_items, 1), R.START_MENU_VISIBLE)
        h = n * 20 + 8
        y0 = R.TASKBAR_Y - h
        rects.append(("apps-menu", (4, y0, 4 + R.START_MENU_W, R.TASKBAR_Y)))
    if cursor:
        cx, cy = cursor
        rects.append(("cursor", (cx - 10, cy - 6, cx + 14, cy + 16)))
    return rects


def point_in_rect(px, py, rect):
    x0, y0, x1, y1 = rect
    return x0 <= px < x1 and y0 <= py < y1


def analyze_pair(path_a, path_b, geom, num_windows, cursor,
                 menu_open=False, menu_items=0):
    """Changed-pixel census A->B with overlay masking.

    Returns dict with counts + unattributed detail.  geom is the browser
    content rect ((x,y,w,h) or None); content mask = that rect.
    """
    wa, ha, a = R.read_ppm(path_a)
    wb, hb, b = R.read_ppm(path_b)
    if (wa, ha) != (wb, hb):
        raise ValueError(f"capture geometry mismatch {wa}x{ha} vs {wb}x{hb}")
    overlays = overlay_rects(geom, num_windows, cursor, menu_open, menu_items)

    n_changed = n_content = n_masked = n_unatt = 0
    u_min_x, u_min_y = 10 ** 9, 10 ** 9
    u_max_x, u_max_y = -1, -1
    u_samples = []
    u_colors = {}
    import struct
    ita = struct.iter_unpack("<BBB", a)
    itb = struct.iter_unpack("<BBB", b)
    W = wa
    for j, (pa, pb) in enumerate(zip(ita, itb)):
        if pa == pb:
            continue
        n_changed += 1
        px, py = j % W, j // W
        if geom and (geom[0] <= px < geom[0] + geom[2] and
                     geom[1] <= py < geom[1] + geom[3]):
            n_content += 1
            continue
        masked = False
        for _name, rect in overlays:
            if point_in_rect(px, py, rect):
                masked = True
                break
        if masked:
            n_masked += 1
            continue
        n_unatt += 1
        if px < u_min_x:
            u_min_x = px
        if py < u_min_y:
            u_min_y = py
        if px > u_max_x:
            u_max_x = px
        if py > u_max_y:
            u_max_y = py
        rgb = (pb[0], pb[1], pb[2])  # the NEW color (what appeared there)
        key = rgb
        u_colors[key] = u_colors.get(key, 0) + 1
        if len(u_samples) < 8:
            u_samples.append({"x": px, "y": py, "rgb": list(rgb)})
    return {
        "changed": n_changed,
        "in_content": n_content,
        "masked": n_masked,
        "unattributed": n_unatt,
        "unattributed_bbox": ([u_min_x, u_min_y, u_max_x, u_max_y]
                              if n_unatt else None),
        "unattributed_top_colors": sorted(
            u_colors.items(), key=lambda kv: -kv[1])[:6],
        "unattributed_samples": u_samples,
    }


# ------------------------------------------------------------- scenario --
def wait_settle(serial_log, stable_needle, timeout):
    """Wait for the content to settle: `stable_needle` marker or the given
    timeout, whichever first.  Returns (settled, bool)."""
    return R.wait_marker(serial_log, stable_needle, timeout)


def run_scenario(args, evdir, qmp, serial_log, wp_sha):
    results = {"scenario": args.scenario, "wp_sha256": wp_sha,
               "steps": [], "captures": []}
    qmp = qmp
    s = serial_log

    # --- boot / menu / launch browser -----------------------------------
    ok, _ = R.wait_marker(s, "Desktop starting", 120)
    results["steps"].append({"step": "desktop-boot", "ok": ok})
    if not ok:
        return None, "desktop never booted"
    time.sleep(2)
    menu = R.menu_map(s)
    if not menu:
        return None, "no [MENU] mapping"
    if not R.launch_from_menu(qmp, menu, "WEBPROC.BIN"):
        return None, "WEBPROC.BIN not in the Apps menu"

    def wait(needle, timeout, name):
        ok, _ = R.wait_marker(s, needle, timeout)
        results["steps"].append({"step": name, "ok": ok})
        return ok

    ok = wait("[WIN] BOOT", 120, "webproc-boot")
    ok = wait("[WIN] created", 90, "win-created")
    ok = wait("[WIN] geom", 120, "win-geom") and ok
    time.sleep(0.5)

    def geom_now():
        info = R.parse_win_log(s)
        return info["geom"]

    nwin = {"v": 1}   # window count (1 until the console opens)

    def shot(label):
        ppm = os.path.join(evdir, label + ".ppm")
        qmp.shot(ppm)
        results["captures"].append({
            "label": label, "ppm": ppm,
            "wallclock_since_run": round(wallclock() - results.get("t0", wallclock()), 2),
            "geom": geom_now(), "nwin": nwin["v"],
        })
        return ppm

    results["t0"] = wallclock()
    shot("00-boot")

    # ---- load a page (the probe target) ---------------------------------
    def load_page(url):
        qmp.key("f2", pause=0.5)
        ok = wait("[WIN] url-prompt", 20, f"url-prompt-{url}")
        type_url(qmp, url)
        ok = wait("[WIN] url-entry=" + url, 30, f"url-entry-{url}") and ok
        mark = len(R.serial_text(s))
        ok = wait("[WIN] load-ok", 120, f"load-ok-{url}") and ok
        t = R.serial_text(s)
        ms = _slice_load_ms(s, mark)
        results.setdefault("load_debug", []).append({
            "url": url, "mark": mark, "total": len(t),
            "slice_tail_has_loadok": "[WIN] load-ok" in t[mark:],
            "slice_head": t[mark:mark + 80].replace("\r", "<CR>"),
        })
        return ok, ms

    if args.scenario == "perf":
        out = {"scenario": "perf", "runs": []}
        for url in ("home.htm", "tall.htm"):
            t = wallclock()
            okk, load_ms = load_page(url)
            # let it settle: a few settle repaints / budget marker
            R.wait_marker(s, "[WIN] frame-times", 30)
            time.sleep(1.0)
            shot(f"10-{url}")
            info = R.parse_win_log(s)
            out["runs"].append({"url": url, "load_ok": okk,
                                "load_ms": load_ms,
                                "frames": info["frames"],
                                "frame_times": info["frame_times"],
                                "reblits": info["reblits"],
                                "viewport_apply": info["viewport_apply"],
                                "elapsed_s": round(wallclock() - t, 1)})
            time.sleep(0.5)
        results["verdict"] = "OK" if all(r["load_ok"] for r in out["runs"]) \
            else "FAIL"
        results["summary"] = {
            "scenario": "perf",
            "loads_ok": sum(1 for r in out["runs"] if r["load_ok"]),
            "loads_total": len(out["runs"]),
            "runs": out["runs"],
        }
        results["perf"] = out
        return results, None

    # --- the bleed scenarios load TALL --------------------------------
    def load_tall_async():
        # F2 URL entry but do NOT wait for load-ok (caller times the
        # re-tile against the load).
        qmp.key("f2", pause=0.5)
        wait("[WIN] url-prompt", 20, "url-prompt-tall")
        type_url(qmp, "tall.htm")
        wait("[WIN] url-entry=tall.htm", 30, "url-entry-tall")
        wait("[WIN] load start", 30, "load-start-tall")

    def menu_now():
        # the WM draws the Apps menu at (4, TASKBAR_Y - h) sized 220 x h
        mm = R.menu_map(s, timeout=2)
        return len(mm) if mm else 16

    def launch_console():
        menu = R.menu_map(s)
        if not R.launch_from_menu(qmp, menu, "CONSOLE.BIN"):
            return False
        nwin["v"] = 2
        return True

    events = []
    n = args.scenario
    if n == "gentle":
        okk = load_page("tall.htm")
        R.wait_marker(s, "[WIN] frame-times", 30)
        time.sleep(1.2)
        shot("20-settled-before-console")
        t = wallclock()
        launch_console()
        events.append(("console-launched", wallclock() - t))
        # burst of captures through the re-tile
        for i, d in enumerate((0.0, 0.25, 0.6, 1.2, 2.5, 5.0)):
            time.sleep(d)
            shot(f"2{i + 1}-retile")
        wait_settle(s, "[WIN] geom", 30)
        time.sleep(1.5)
        shot("30-settled-after-console")

    elif n == "load":
        load_tall_async()
        time.sleep(1.5)                      # browser mid-load, pre-load-ok
        shot("20-midload-before-console")
        t = wallclock()
        launch_console()
        events.append(("console-launched", wallclock() - t))
        for i, d in enumerate((0.0, 0.3, 0.8, 1.6, 3.0)):
            time.sleep(d)
            shot(f"2{i + 1}-retile-duringload")
        wait("[WIN] load-ok", 150, "load-ok-after-retile")
        time.sleep(1.5)
        shot("30-settled-after-console")

    elif n == "settle":
        okk = load_page("tall.htm")
        R.wait_marker(s, "[WIN] frame-times", 30)
        # force a settle-repaint burst with a wheel scroll
        qmp.wheel("down", 1)
        time.sleep(0.6)
        shot("20-settlerepaint-before-console")
        t = wallclock()
        launch_console()
        events.append(("console-launched", wallclock() - t))
        for i, d in enumerate((0.0, 0.25, 0.7, 1.5, 3.0)):
            time.sleep(d)
            shot(f"2{i + 1}-retile-settle")
        wait_settle(s, "[WIN] geom", 30)
        time.sleep(1.5)
        shot("30-settled-after-console")

    elif n == "openclose":
        load_tall_async()
        time.sleep(1.0)
        shot("20-openclose-pre")
        okc = launch_console()
        time.sleep(1.5)
        shot("21-console-open")
        qmp.key("f4", pause=1.0)             # close the (focused) console
        time.sleep(1.5)
        shot("22-console-closed")
        okc2 = launch_console()
        time.sleep(1.5)
        shot("23-console-open-2")
        qmp.key("f4", pause=1.0)
        time.sleep(1.0)
        shot("24-console-closed-2")
        wait("[WIN] load-ok", 150, "load-ok-after-openclose")
        time.sleep(1.5)
        shot("30-settled-after-openclose")
    else:
        return None, f"unknown scenario {n}"

    results["events"] = events
    results["steps"].append({"step": "console-launch-ok", "ok": True})

    # ---- close the browser (clean exit) ------------------------------
    # The console has focus after launch (WM sets focused_window on spawn);
    # click the browser's title bar to focus it first, then F4.
    qmp.click(50, 17, pause=0.3)          # browser title bar (not the X)
    qmp.key("f4", pause=1.0)
    ok = wait("[WIN] close ok", 20, "close-ok")
    ok = wait("[WIN] exit rc=0", 20, "exit-rc0") and ok
    results["steps"].append({"step": "browser-close", "ok": ok})

    # ---- analysis: every capture pair ----------------------------------
    caps = results["captures"]
    pairs = []
    final = caps[-1]["ppm"] if caps else None
    for i in range(1, len(caps)):
        a, b = caps[i - 1], caps[i]
        # overlay state is the TO frame's (post-transition): browser content
        # rect + window count as they were when capture B was taken.
        an = analyze_pair(a["ppm"], b["ppm"], b["geom"], b["nwin"],
                          cursor=None, menu_open=False)
        an["from"] = a["label"]
        an["to"] = b["label"]
        pairs.append(an)
    # persistent residual: every capture vs the final settle capture
    residuals = []
    if final and len(caps) > 1:
        last = caps[-1]
        for c in caps[:-1]:
            an = analyze_pair(c["ppm"], final, last["geom"], last["nwin"],
                              cursor=None, menu_open=False)
            an["from"] = c["label"]
            an["to"] = final
            residuals.append(an)
    results["pairs"] = pairs
    results["persistent_residuals"] = residuals
    n_bad = sum(1 for p in pairs if p["unattributed"] > 0)
    n_res = sum(1 for p in residuals if p["unattributed"] > 0)
    results["verdict"] = ("BLEED-INDICATOR" if (n_bad or n_res)
                          else "NO-BLEED-OBSERVED")
    results["summary"] = {
        "pairs_with_unattributed_changes": n_bad,
        "pairs_total": len(pairs),
        "captures_with_persistent_residual": n_res,
        "unattributed_total_pixels": sum(p["unattributed"] for p in pairs),
    }
    return results, None


def _last_load_ms(serial_log, url):
    t = R.serial_text(serial_log)
    ms = None
    for m in re.finditer(r"\[WIN\] load-ok url=(\S+) ms=(\d+)", t):
        ms = int(m.group(2))
    return ms


def _slice_load_ms(serial_log, since_len: int):
    """Return the ms= of the newest load-ok marker in the serial slice that
    starts at byte offset since_len (per-run load receipt)."""
    t = R.serial_text(serial_log)
    ms = None
    for m in re.finditer(r"\[WIN\] load-ok url=(\S+) ms=(\d+)",
                         t[since_len:]):
        ms = int(m.group(2))
    return ms


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="tile_bleed_probe.py")
    ap.add_argument("--wp-elf", required=True)
    ap.add_argument("--os-tree", default="/home/sarah/hobbyos-scratch/rp-h-os")
    ap.add_argument("--disk", default=None,
                    help="reuse a preassembled disk instead of assembling")
    ap.add_argument("--fixtures-root",
                    default="/home/sarah/webkit-hobbyos/HobbyOS/continuation/wk5/fixtures")
    ap.add_argument("--scenario", default="gentle",
                    choices=["gentle", "load", "settle", "openclose", "perf"])
    ap.add_argument("--evdir", default=None)
    ap.add_argument("--instance", default="rp-h")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem-mb", type=int, default=4096)
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args(argv)

    if not os.path.isfile(args.wp_elf):
        print(f"missing --wp-elf: {args.wp_elf}", file=sys.stderr)
        return 2
    wp_sha = R.sha256_file(args.wp_elf)

    evdir = args.evdir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "evidence", f"tile-bleed-{args.scenario}-{int(time.time())}")
    os.makedirs(evdir, exist_ok=True)
    qmp_sock = f"/tmp/rp-{args.instance}-qmp.sock"
    serial_log = f"/tmp/rp-{args.instance}-serial.log"
    pidfile = os.path.join(evdir, "qemu.pid")

    disk = args.disk
    disk_sha = None
    if disk is None:
        fixtures = {}
        root = args.fixtures_root
        for name in ("HOME.HTM", "TALL.HTM"):
            p = os.path.join(root, name)
            if os.path.isfile(p):
                fixtures[name] = p
        disk = os.path.join(evdir, "disk-rp.img")
        print(f"[probe] assembling disk -> {disk}", flush=True)
        R.assemble_disk(evdir, args.os_tree, args.wp_elf, disk,
                        need_console=(args.scenario != "perf"),
                        fixtures=fixtures)
        disk_sha = R.sha256_file(disk)

    print(f"[probe] wp_elf sha256={wp_sha}", flush=True)
    print(f"[probe] booting {disk} (smp {args.smp}, {args.mem_mb}M)",
          flush=True)
    qemu_pid = R.boot_qemu(disk, qmp_sock, serial_log, pidfile,
                           smp=args.smp, mem_mb=args.mem_mb)
    report = {"tool": "tile_bleed_probe.py", "scenario": args.scenario,
              "wp_elf": args.wp_elf, "wp_sha256": wp_sha,
              "disk": disk, "disk_sha256": disk_sha,
              "os_tree": args.os_tree, "instance": args.instance,
              "qemu_pid": qemu_pid}
    try:
        qmp = R.Qmp(qmp_sock)
        results, err = run_scenario(args, evdir, qmp, serial_log, wp_sha)
    except Exception as exc:
        results, err = None, f"driver exception: {exc}"
    finally:
        if not args.keep:
            R.kill_exact(pidfile)
    report["serial_log"] = serial_log
    report["serial_tail"] = R.serial_text(serial_log)[-4000:]
    if results:
        report.update(results)
        report["status"] = "done"
        rc = 0
    else:
        report["status"] = "error"
        report["error"] = err
        rc = 1
    with open(os.path.join(evdir, "report.json"), "w") as fh:
        json.dump(report, fh, indent=2)
    print(json.dumps({"status": report.get("status"),
                      "scenario": args.scenario,
                      "verdict": report.get("verdict"),
                      "summary": report.get("summary"),
                      "wp_sha256": wp_sha[:16],
                      "evdir": evdir}, indent=2))
    if results and results.get("summary"):
        sm = results["summary"]
        print(f"[probe] pairs={sm.get('pairs_total')} "
              f"with_unattributed={sm.get('pairs_with_unattributed_changes')} "
              f"persistent_residual={sm.get('captures_with_persistent_residual')} "
              f"unattributed_px={sm.get('unattributed_total_pixels')}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
