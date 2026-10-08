#!/usr/bin/env python3
"""
resp_close_probe.py — WM close responsiveness under heavy load (the
documented single-threaded starvation limitation, render-pipeline plan §2).

Loads a fixture page (TALL by default, load ~7-9 s under TCG) and issues a
close (F4 → ESC [ D, or the titlebar-X path) at a chosen point in the load /
settle timeline, then asserts "[WIN] close ok" + "[WIN] exit rc=0" arrive
within the grace window and records the wall-clock close latency.

Scenarios:
  midload     F4 sent as soon as 'load start' is observed (page mid-load)
  midsettle   F4 sent during the post-load settle-repaint burst
  starvation  F4 sent the moment 'load-ok' appears (heaviest setSize/paint
              moment for TALL) and again ~0.4 s later if the first is still
              pending — this is the documented starvation worst case
  titlebar    close via the window's titlebar X button (WM click path)
  gentlesettle  baseline control: load + settle, then F4 (no load activity)

The honest baseline is latency: close-action → '[WIN] close ok' → 'exit rc=0',
and whether each arrived inside --grace.  A starved close (timeout) is
recorded as FAIL with the timeout value — that is the documented limitation
being baselined, not a probe defect.

Usage:
  resp_close_probe.py --wp-elf PATH [--os-tree PATH] [--disk PATH]
      [--scenario midload] [--grace 30] [--evdir DIR] [--instance ID]
      [--keep] [--timeout S]
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


def close_latency(serial_log, action_t0, grace):
    """Watch serial for '[WIN] close ok' then '[WIN] exit rc=0'. Returns
    dict with latencies (s) and ok flags, polling until grace or success."""
    t = R.serial_text(serial_log)
    start_len = len(t)
    deadline = time.time() + grace
    saw_close = "[WIN] close ok" in t
    saw_exit = re.search(r"\[WIN\] exit rc=(\d+)", t) is not None
    exit_rc = None
    t_close = t_exit = None
    if saw_close:
        t_close = 0.0
    if saw_exit:
        t_exit = 0.0
    while time.time() < deadline and not (saw_close and saw_exit):
        time.sleep(0.15)
        t = R.serial_text(serial_log)
        if not saw_close and "[WIN] close ok" in t:
            saw_close = True
            t_close = round(time.time() - action_t0, 2)
        m = re.search(r"\[WIN\] exit rc=(\d+)", t)
        if not saw_exit and m:
            saw_exit = True
            t_exit = round(time.time() - action_t0, 2)
            exit_rc = int(m.group(1))
    if not saw_exit:
        exit_rc = None
    return {
        "close_ok": saw_close,
        "exit_rc0": saw_exit and exit_rc == 0,
        "exit_rc": exit_rc,
        "close_ok_latency_s": t_close,
        "exit_latency_s": t_exit,
        "within_grace": bool(saw_close and saw_exit and exit_rc == 0),
        "serial_bytes_since_action": len(R.serial_text(serial_log)) - start_len,
    }


def run_scenario(args, evdir, qmp, serial_log, wp_sha):
    results = {"scenario": args.scenario, "wp_sha256": wp_sha,
               "grace_s": args.grace, "steps": [], "closes": [],
               "t0": time.time()}
    s = serial_log
    url = args.url or "tall.htm"

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

    wait("[WIN] BOOT", 120, "webproc-boot")
    wait("[WIN] created", 90, "win-created")
    wait("[WIN] geom", 120, "win-geom")
    time.sleep(0.5)

    # ---- open the URL prompt and kick off the load ----------------------
    qmp.key("f2", pause=0.5)
    wait("[WIN] url-prompt", 20, "url-prompt")
    type_url(qmp, url)
    wait("[WIN] url-entry=" + url, 30, "url-entry")
    wait("[WIN] load start", 60, "load-start")

    t_load_start = time.time()
    load_start_wall = t_load_start

    def do_close(method, t0):
        if method == "f4":
            qmp.key("f4", pause=0.2)
        elif method == "titlebar":
            info = R.parse_win_log(s)
            gx, gy, gw, gh = info["geom"]
            wx = gx
            wy = gy - R.CHROME_T
            x_btn = wx + gw + R.CHROME_R - 18 + 8
            y_btn = wy + 2 + 8
            qmp.click(min(x_btn, R.SCREEN_W - 4), max(y_btn, 4), pause=0.3)
        return close_latency(s, t0, args.grace)

    # ---- scenario-specific close timing ---------------------------------
    if args.scenario == "gentlesettle":
        wait("[WIN] load-ok", 150, "load-ok")
        R.wait_marker(s, "[WIN] frame-times", 20)
        time.sleep(1.5)
        t0 = time.time()
        res = do_close("f4", t0)
        results["closes"].append({"method": "f4", "point": "after-settle",
                                  "load.ms": _last_load_ms(s), **res})

    elif args.scenario == "midload":
        # load just started; close while the page is still loading
        time.sleep(0.4)
        t0 = time.time()
        res = do_close("f4", t0)
        results["closes"].append({"method": "f4", "point": "mid-load",
                                  "load.ms": _last_load_ms(s), **res})

    elif args.scenario == "midsettle":
        wait("[WIN] load-ok", 150, "load-ok")
        R.wait_marker(s, "[WIN] frame-times", 20)
        time.sleep(0.3)          # inside the settle repaint window
        t0 = time.time()
        res = do_close("f4", t0)
        results["closes"].append({"method": "f4", "point": "mid-settle",
                                  "load.ms": _last_load_ms(s), **res})

    elif args.scenario == "starvation":
        # closing exactly at the heaviest moment
        wait("[WIN] load-ok", 150, "load-ok")
        t0 = time.time()
        res = do_close("f4", t0)
        results["closes"].append({"method": "f4", "point": "at-load-ok",
                                  "load.ms": _last_load_ms(s), **res})
        if not res["within_grace"]:
            time.sleep(1.0)
            t1 = time.time()
            res2 = do_close("f4", t1)
            results["closes"].append({"method": "f4", "point": "retry",
                                      "load.ms": _last_load_ms(s), **res2})

    elif args.scenario == "titlebar":
        wait("[WIN] load-ok", 150, "load-ok")
        time.sleep(0.5)
        t0 = time.time()
        res = do_close("titlebar", t0)
        results["closes"].append({"method": "titlebar-X",
                                  "point": "after-load",
                                  "load.ms": _last_load_ms(s), **res})

    else:
        return None, f"unknown scenario {args.scenario}"

    results["load_start_wall_s"] = round(load_start_wall - results.get(
        "t0", load_start_wall), 2)
    # verdict: every close within grace with exit rc=0
    all_ok = all(c["within_grace"] for c in results["closes"])
    results["verdict"] = "PASS" if all_ok else \
        ("FAIL-STARVED" if not all_ok else "FAIL")
    results["summary"] = {
        "closes": len(results["closes"]),
        "all_within_grace": all_ok,
        "max_close_latency_s": max(
            (c["close_ok_latency_s"] or args.grace) for c in results["closes"]),
        "load_ms": _last_load_ms(s),
    }
    return results, None


def _last_load_ms(serial_log):
    ms = None
    for m in re.finditer(r"\[WIN\] load-ok url=(\S+) ms=(\d+)",
                         R.serial_text(serial_log)):
        ms = int(m.group(2))
    return ms


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="resp_close_probe.py")
    ap.add_argument("--wp-elf", required=True)
    ap.add_argument("--os-tree", default="/home/sarah/hobbyos-scratch/rp-h-os")
    ap.add_argument("--disk", default=None)
    ap.add_argument("--url", default="tall.htm")
    ap.add_argument("--scenario", default="midload",
                    choices=["midload", "midsettle", "starvation",
                             "titlebar", "gentlesettle"])
    ap.add_argument("--grace", type=float, default=30.0)
    ap.add_argument("--fixtures-root",
                    default="/home/sarah/webkit-hobbyos/HobbyOS/continuation/wk5/fixtures")
    ap.add_argument("--evdir", default=None)
    ap.add_argument("--instance", default="rp-h")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem-mb", type=int, default=4096)
    args = ap.parse_args(argv)

    if not os.path.isfile(args.wp_elf):
        print(f"missing --wp-elf: {args.wp_elf}", file=sys.stderr)
        return 2
    wp_sha = R.sha256_file(args.wp_elf)

    evdir = args.evdir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "evidence", f"resp-close-{args.scenario}-{int(time.time())}")
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
                        need_console=False, fixtures=fixtures)
        disk_sha = R.sha256_file(disk)

    print(f"[probe] wp_elf sha256={wp_sha}", flush=True)
    qemu_pid = R.boot_qemu(disk, qmp_sock, serial_log, pidfile,
                           smp=args.smp, mem_mb=args.mem_mb)
    report = {"tool": "resp_close_probe.py", "scenario": args.scenario,
              "wp_elf": args.wp_elf, "wp_sha256": wp_sha,
              "disk": disk, "disk_sha256": disk_sha,
              "os_tree": args.os_tree, "instance": args.instance,
              "grace_s": args.grace, "qemu_pid": qemu_pid}
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
    return rc


if __name__ == "__main__":
    sys.exit(main())
