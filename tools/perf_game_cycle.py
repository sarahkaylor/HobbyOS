#!/usr/bin/env python3
"""perf_game_cycle.py — game-cycling memory/leak + freeze pass (GB-046).

Boots HobbyOS once under QEMU, cycles the native games (launch, hold, sample),
and asserts two things:

  freeze-watchdog : a game that launches and RUNS keeps producing frames; if a
                    sampled screendump is byte-identical for `freeze_window`
                    consecutive samples the game is flagged FROZEN (lost
                    liveness = a real perf/robustness failure).

  no unbounded growth : the QEMU guest's physical RAM is fixed at boot, so an
                    unbounded rise in the *host* QEMU process RSS across the
                    run indicates a leak (unfreed guest memory accumulating).
                    GB-046-V RED: growth is measured floor->peak over the WHOLE
                    run (the full RSS series), not first->last of one visit.

Freeze comparison operates on a CROPPED playfield (GB-046-V RED): the full
frame includes the desktop clock (top-right, x~1882-1893 y~1063) which ticks
every second, so hashing the whole frame made the watchdog unable to flag a
frozen game.  We hash only the playfield window (default excludes the
top-right clock and the bottom taskbar strip); a static game + ticking clock
now correctly reads as frozen.

Each visit must GENUINELY LAUNCH its game: GB-046-V observed 300
launch_from_menu calls -> only 31 [LAUNCH] lines (most visits screendumped the
desktop and scored a pass).  A visit whose expected game does not appear in
the serial log is now a FAIL (launched=False), not a pass.

The DoD cell "no unbounded growth" (§M4-acceptance) is the memory assertion;
the freeze watchdog is the liveness complement the title names.  Reuses the
shared evidence_rig QMP/boot/menu machinery (GB-040) so it launches each game
by NAME from the [MENU] dump (no hardcoded slots).

Usage:
  ARCH=arm python3 tools/perf_game_cycle.py \\
      --games "XNIBBLER.BIN:8,XCRYPT.BIN:8,NES.BIN" \\
      --cycles 3 --hold 6 --root $(pwd)
Outputs an evidence bundle under evidence/perf_cycle/<ts>/ plus
perf-global-REPORT.md.

Core logic (freeze detection + RSS-trend + PPM crop) is import-testable.
"""

import argparse, hashlib, json, os, re, subprocess, sys, time
from datetime import datetime, timezone

# --- importable core (T1-testable, no QEMU) -----------------------------
class FreezeWatchdog:
    """Flags a duration as frozen when screendump *playfield* hashes stop
    changing.  The caller passes a CROP digest (desktop clock excluded) so a
    static game whose only change is the ticking clock is judged frozen."""
    def __init__(self, window):
        self.window = window
        self._seq = []
        self.frozen = False
        self.runs_seen = 0

    def observe(self, digest):
        self.runs_seen += 1
        if digest is None:
            if self.window <= 1:
                self.frozen = True
            return self.frozen
        self._seq.append(digest)
        # frozen = the last `window` observations are identical and we have
        # enough samples to judge movement.
        if len(self._seq) < self.window:
            return False
        tail = self._seq[-self.window:]
        if len(set(tail)) == 1:
            self.frozen = True
        return self.frozen


def rss_trend_growth(rss_samples, max_growth_mb):
    """Per-visit first->last growth (KB in).  Kept for backward compat; the
    GB-046-V-hardened assertion is rss_floor_peak_growth over the whole run."""
    if not rss_samples:
        return (0.0, True)
    growth_kb = rss_samples[-1] - rss_samples[0]
    ok = growth_kb <= max_growth_mb * 1024.0
    return (round(growth_kb / 1024.0, 2), ok)


def rss_floor_peak_growth(all_rss, max_growth_mb):
    """whole-run floor, peak and growth = peak - floor (KB in).  GB-046-V:
    assert floor->peak over the WHOLE run, not one visit.  Returns
    (floor_mb, peak_mb, growth_mb, ok)."""
    if not all_rss:
        return (0.0, 0.0, 0.0, True)
    floor = min(all_rss)
    peak = max(all_rss)
    growth_kb = peak - floor
    ok = growth_kb <= max_growth_mb * 1024.0
    return (round(floor / 1024.0, 2), round(peak / 1024.0, 2),
            round(growth_kb / 1024.0, 2), ok)


def parse_ppm_header(raw):
    """Parse a P6 PPM's text header.  Returns (w, h, raster_offset) or
    (None, None, None) on a malformed/non-PPM input."""
    pos = 0
    n = len(raw)
    while pos < n and raw[pos] in b" \t\r\n":
        pos += 1
    if raw[pos:pos + 2] != b"P6":
        return (None, None, None)
    pos += 2

    def next_int():
        nonlocal pos
        while pos < n and raw[pos] in b" \t\r\n":
            pos += 1
        start = pos
        while pos < n and raw[pos] not in b" \t\r\n":
            pos += 1
        return int(raw[start:pos]) if start < pos else None

    w = next_int()
    h = next_int()
    mx = next_int()
    if w is None or h is None or mx is None:
        return (None, None, None)
    # PPM spec: exactly one whitespace char separates maxval from the raster.
    pos += 1
    return (w, h, pos)


def crop_sha256(raw, w, h, off, x0, y0, x1, y1):
    """SHA-256 of the playfield [x0,x1)x[y0,y1) of a PPM raster.  Bounds
    clamped; an out-of-range/empty crop yields a deterministic digest."""
    x0 = max(0, min(x0, w)); x1 = max(0, min(x1, w))
    y0 = max(0, min(y0, h)); y1 = max(0, min(y1, h))
    if off is None or x1 <= x0 or y1 <= y0:
        return hashlib.sha256(b"").hexdigest()
    row = w * 3
    hsh = hashlib.sha256()
    for y in range(y0, y1):
        s = off + y * row + x0 * 3
        e = off + y * row + x1 * 3
        if e <= len(raw):
            hsh.update(raw[s:e])
    return hsh.hexdigest()


def crop_sha256_of(path, x0, y0, x1, y1):
    """Crop hash of a PPM file on disk (None on parse/IO failure)."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
        w, h, off = parse_ppm_header(raw)
        return crop_sha256(raw, w, h, off, x0, y0, x1, y1)
    except Exception:
        return None


def flag_freeze_free(observed, window):
    w = FreezeWatchdog(window)
    return [w.observe(d) for d in observed]


def launch_seen(new_serial, spec):
    """True if a fresh `spec` launch appeared in the serial bytes accumulated
    since the launch was issued (the desktop prints `[LAUNCH] <name>` / a
    /GAMES path on a real spawn).  A no-op launch is a GB-046-V FAIL."""
    return (f"[LAUNCH] {spec}" in new_serial or
            f"[LAUNCH] /GAMES/{spec}" in new_serial)


def launched_name(new_serial):
    """The last binary the desktop actually launched in `new_serial`, or None.

    GB-046: the desktop kept launching after 8 visits — the wrong title
    (SYSMON.BIN, MILLIPED.BIN) — because the menu cursor is not where the
    gesture assumed.  A visit that launches the WRONG title is a different
    failure from a no-op and must be reported as such, not as "no-op".
    """
    names = re.findall(r"\[LAUNCH\] (?:/GAMES/)?([A-Za-z0-9_.-]+)", new_serial)
    return names[-1] if names else None


def launch_retry_needed(launched, attempt, retries):
    """Keep retrying while the launch has not appeared and attempts remain.
    `retries` = total attempts allowed; GB-046-V: a persistent no-op (all
    attempts exhausted unfired) still FAILs — retry only recovers transient
    menu no-ops, it does not mask a launch that never happens."""
    return (not launched) and (attempt < retries - 1)


# --- QEMU plumbing (reused from evidence_rig) ---------------------------
from evidence_rig import QMP, boot, find_menu_slot, launch_from_menu

def qemu_rss_kb():
    """Host RSS of the running qemu guest (KB), or None if not found."""
    try:
        out = subprocess.run(["pgrep", "-f", "qemu-system"],
                             capture_output=True, text=True).stdout.split()
        if not out:
            return None
        pid = out[0]
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmRSS:"):
                return int(line.split()[1])   # KB
    except Exception:
        return None
    return None


def sha_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


# --- driver --------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--games", required=True,
                    help="comma list NAME.BIN[:hold_s] to cycle (name-matched in menu)")
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--hold", type=int, default=6, help="default hold seconds per game")
    ap.add_argument("--freeze-window", type=int, default=4,
                    help="consecutive identical frames to call a freeze")
    ap.add_argument("--max-growth-mb", type=int, default=64,
                    help="QEMU RSS growth allowed over the WHOLE run (floor->peak)")
    ap.add_argument("--crop", default="0,0,1820,1010",
                    help="playfield crop x0,y0,x1,y1 (excludes desktop clock+taskbar)")
    ap.add_argument("--launch-retries", type=int, default=3,
                    help="bounded re-attempts per visit if [LAUNCH] <name> "
                         "does not appear (recovers transient menu no-ops; "
                         "a persistent no-op still FAILs the visit)")
    ap.add_argument("--launch-retry-delay", type=float, default=1.5,
                    help="seconds to wait after each launch attempt before "
                         "checking the serial for [LAUNCH]")
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd())
    args = ap.parse_args()
    try:
        x0, y0, x1, y1 = (int(v) for v in args.crop.split(","))
    except ValueError:
        print(f"[perf] FATAL: bad --crop {args.crop!r}")
        sys.exit(1)

    os.chdir(args.root)
    path = f"evidence/perf_cycle/{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"
    os.makedirs(path, exist_ok=True)
    serial_log = os.path.join(path, "serial.log")
    qmp_sock = "/tmp/perf-cycle-qmp"

    qemu_make = ["make", "run", "DESKTOP=hobby", f"ARCH={args.arch}",
                 f"QEMU_ARGS=-display none -qmp unix:{qmp_sock},server,nowait"]
    print(f"[perf] booting {args.arch} once; cycling games; bundle -> {path}")
    q = boot(qemu_make, serial_log, qmp_sock, args.arch)
    serial_log_content = open(serial_log, errors="ignore").read()
    if "not in start-menu dump" in serial_log_content:
        print("[perf] desktop did not come up (no [MENU] dump)")
        sys.exit(1)
    ok = True
    games = [g.strip() for g in args.games.split(",") if g.strip()]
    report = []
    all_rss = []
    base_rss = qemu_rss_kb()
    if base_rss:
        all_rss.append(base_rss)

    for cyc in range(args.cycles):
        for spec in games:
            hold = args.hold
            if ":" in spec:
                spec, hold = spec.split(":")[:2]
                hold = int(hold)
            slot = find_menu_slot(serial_log, spec)
            if slot is None:
                print(f"[perf] FATAL: {spec} not in menu dump")
                ok = False
                continue
            ser_len = os.path.getsize(serial_log) if os.path.exists(serial_log) else 0

            # Bounded launch retry (GB-046 launch-reliability): the desktop
            # menu-click launch path intermittently no-ops (the QMP/desktop
            # input path).  Retry until [LAUNCH] <spec> appears or attempts are
            # exhausted; a persistent no-op still FAILs the visit.
            launched = False
            new_serial = ""
            for attempt in range(args.launch_retries):
                if attempt > 0:
                    # GB-046: ESC is forwarded to the FOCUSED window (the
                    # game), not the desktop — it cannot close a menu.  F4 is
                    # the desktop's close-focused-window key (desktop.c:2200).
                    q.key("f4", 0.4)
                    time.sleep(1.0)
                launch_from_menu(q, slot)
                time.sleep(args.launch_retry_delay)
                new_serial = ""
                try:
                    with open(serial_log, "rb") as f:
                        f.seek(ser_len)
                        new_serial = f.read().decode(errors="ignore")
                except OSError:
                    pass
                if launch_seen(new_serial, spec):
                    launched = True
                    break
                if not launch_retry_needed(launched, attempt, args.launch_retries):
                    break
                print(f"[perf] retry {spec} (cycle {cyc}) attempt "
                      f"{attempt + 1}/{args.launch_retries}: [LAUNCH] not yet in serial")
            time.sleep(1.0)

            wd = FreezeWatchdog(args.freeze_window)
            samples = max(args.freeze_window + 1, max(2, hold))
            for i in range(samples):
                dump = os.path.join(path, f"c{cyc}-{spec}-s{i}.ppm")
                q.screendump(dump)
                r = qemu_rss_kb()
                if r:
                    all_rss.append(r)
                wd.observe(crop_sha256_of(dump, x0, y0, x1, y1))
                time.sleep(1.0)
            frozen = wd.frozen

            if not launched:
                actual = launched_name(new_serial)
                if actual:
                    print(f"[perf] FAIL: {spec} (cycle {cyc}) — the desktop "
                          f"launched {actual} instead (menu cursor drifted)")
                else:
                    print(f"[perf] FAIL: {spec} (cycle {cyc}) never appeared in "
                          f"serial (launch no-op) — new serial bytes "
                          f"[{len(new_serial)}] do not contain [LAUNCH] {spec}")
            else:
                print(f"[perf] cycle={cyc} game={spec} samples={samples} "
                      f"frozen={frozen} launched=True")
            report.append({"cycle": cyc, "game": spec, "slot": slot,
                           "samples": samples, "frozen": frozen,
                           "launched": launched})
            if frozen or not launched:
                ok = False
            # GB-046: close the game's window before the next visit.  The old
            # `esc` went to the focused game (not the desktop) and closed
            # nothing, so windows and processes accumulated (slots 3..11 in the
            # 2026-11-01 serial) and the menu cursor drifted to other titles.
            q.key("f4", 0.4)
            time.sleep(1.0)

    floor_mb, peak_mb, growth_mb, mem_ok = \
        rss_floor_peak_growth(all_rss, args.max_growth_mb)
    print(f"[perf] whole-run RSS: floor={floor_mb} MB peak={peak_mb} MB "
          f"growth={growth_mb} MB (bound {args.max_growth_mb} MB) -> "
          f"{'PASS' if mem_ok else 'FAIL'}")
    if not mem_ok:
        ok = False

    with open(os.path.join(path, "report.json"), "w") as f:
        json.dump({"arch": args.arch, "games": args.games, "cycles": args.cycles,
                   "crop": args.crop, "freeze_window": args.freeze_window,
                   "results": report,
                   "rss_floor_mb": floor_mb, "rss_peak_mb": peak_mb,
                   "rss_full_run_samples": len(all_rss),
                   "rss_growth_mb": growth_mb, "rss_ok": mem_ok,
                   "summary": "PASS" if ok else "FAIL",
                   "captured_at": datetime.now(timezone.utc).isoformat()}, f, indent=2)
    with open("perf-global-REPORT.md", "w") as f:
        f.write("# GB-046 game-cycling perf/leak pass\n\n")
        f.write(f"- arch: {args.arch}, cycles: {args.cycles}, crop: {args.crop}\n")
        f.write(f"- bundle: `{path}` (gitignored)\n\n")
        f.write("| cycle | game | frozen | launched |\n|---|---|---|---|\n")
        for r in report:
            f.write(f"| {r['cycle']} | {r['game']} | {r['frozen']} | {r['launched']} |\n")
        f.write(f"\nWhole-run RSS floor={floor_mb} MB peak={peak_mb} MB "
                f"growth={growth_mb} MB "
                f"({len(all_rss)} samples, bound {args.max_growth_mb} MB)\n")
        f.write(f"\nSummary: **{('PASS' if ok else 'FAIL')}**\n")
    print(f"[perf] summary: {'PASS' if ok else 'FAIL'}")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
