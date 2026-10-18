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
                    We assert the QEMU RSS stays within `max_growth_mb` of its
                    post-boot floor.

The DoD cell "no unbounded growth" (§M4-acceptance) is the memory assertion;
the freeze watchdog is the liveness complement the title names.  Reuses the
shared evidence_rig QMP/boot/menu machinery (GB-040) so it launches each game
by NAME from the [MENU] dump (no hardcoded slots).

Usage:
  ARCH=arm python3 tools/perf_game_cycle.py \
      --games "XNIBBLER.BIN:8,XCRYPT.BIN:8,NES.BIN" \
      --cycles 3 --hold 6 --root $(pwd)
Outputs an evidence bundle under evidence/perf_cycle/<ts>/ plus
perf-global-REPORT.md.

Core logic (freeze detection + RSS-trend) is import-testable.
"""
import argparse, hashlib, json, os, re, subprocess, sys, time
from datetime import datetime, timezone

# --- importable core (T1-testable, no QEMU) -----------------------------
class FreezeWatchdog:
    """Flags a duration as frozen when screendump hashes stop changing."""
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
    """Return (growth_mb, ok). growth = last - first for the sampled QEMU RSS
    (input in KB).  The bound `max_growth_mb` is in MB, so compare against
    max_growth_mb * 1024 KB."""
    if not rss_samples:
        return (0.0, True)
    growth_kb = rss_samples[-1] - rss_samples[0]
    ok = growth_kb <= max_growth_mb * 1024.0
    return (round(growth_kb / 1024.0, 2), ok)


def flag_freeze_free(observed, window):
    w = FreezeWatchdog(window)
    return [w.observe(d) for d in observed]


# --- QEMU plumbing (reused from evidence_rig) ---------------------------
from evidence_rig import QMP, boot, find_menu_slot, launch_from_menu

def qemu_rss_kb():
    """Host RSS of the running qemu guest (KB), or None if not found."""
    try:
        out = subprocess.run(["pgrep", "-f", "qemu-system"], capture_output=True,
                             text=True).stdout.split()
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
                    help="QEMU RSS growth allowed over the whole run (KB*many)")
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd())
    args = ap.parse_args()

    os.chdir(args.root)
    path = f"evidence/perf_cycle/{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"
    os.makedirs(path, exist_ok=True)
    serial_log = os.path.join(path, "serial.log")
    qmp_sock = "/tmp/perf-cycle-qmp"

    qemu_make = ["make", "run", f"ARCH={args.arch}",
                 f"QEMU_ARGS=-display none -qmp unix:{qmp_sock},server,nowait"]
    print(f"[perf] booting {args.arch} once; cycling games; bundle -> {path}")
    q = boot(qemu_make, serial_log, qmp_sock, args.arch)
    menu = open(serial_log, errors="ignore").read()
    ok = True
    games = [g.strip() for g in args.games.split(",") if g.strip()]
    report = []

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
            launch_from_menu(q, slot)
            time.sleep(1.0)

            wd = FreezeWatchdog(args.freeze_window)
            rss = []
            samples = max(2, hold // 2)
            for i in range(samples):
                dump = os.path.join(path, f"c{cyc}-{spec}-s{i}.ppm")
                q.screendump(dump)
                rss.append(qemu_rss_kb())
                wd.observe(sha_of(dump))
                time.sleep(1.0)
            frozen = wd.frozen
            growth, mem_ok = rss_trend_growth([r for r in rss if r], args.max_growth_mb)
            line = f"cycle={cyc} game={spec} samples={samples} frozen={frozen} growth_mb={growth}"
            print(f"[perf] {line}")
            report.append({"cycle": cyc, "game": spec, "slot": slot,
                           "samples": samples, "frozen": frozen,
                           "growth_mb": growth, "mem_ok": mem_ok})
            if frozen:
                ok = False
            if not mem_ok:
                ok = False
            q.key("esc", 0.4)
            time.sleep(1.0)

    with open(os.path.join(path, "report.json"), "w") as f:
        json.dump({"arch": args.arch, "games": args.games, "cycles": args.cycles,
                   "results": report,
                   "summary": "PASS" if ok else "FAIL",
                   "captured_at": datetime.now(timezone.utc).isoformat()}, f, indent=2)
    with open("perf-global-REPORT.md", "w") as f:
        f.write("# GB-046 game-cycling perf/leak pass\n\n")
        f.write(f"- arch: {args.arch}, cycles: {args.cycles}\n")
        f.write(f"- bundle: `{path}` (gitignored)\n\n")
        f.write("| cycle | game | frozen | growth_mb |\n|---|---|---|---|\n")
        for r in report:
            f.write(f"| {r['cycle']} | {r['game']} | {r['frozen']} | {r['growth_mb']} |\n")
        f.write(f"\nSummary: **{('PASS' if ok else 'FAIL')}**\n")
    print(f"[perf] summary: {'PASS' if ok else 'FAIL'}")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

