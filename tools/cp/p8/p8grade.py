#!/usr/bin/env python3
"""tools/cp/p8/p8grade.py -- the P8/P9 game-boot verdict from the serial log
(lane H2-early, docs/cp-early.md).  boot_p8.py and boot_p9.py call it; the CLI
replays a saved serial (and optionally its timeline.txt) through the same rules.

Verdicts decided here (the watcher's own verdicts -- main-menu, first-run:*,
stall, timeout, qemu-exited -- stay in the boot scripts):

  launcher-done               pfx/probe phase: the launcher printed "[P8] done".
                              PASS.  Never a game-phase verdict.
  early-exit                  game phase: "[P8] end game status=<n>" before the
                              main menu (P8) or before the play script reached
                              the menu (P9).  FAIL (rc 3).  The detail says the
                              status, the ms and whether REDEngineErrorReporter
                              ran (wave 8's H2 bug: the game exits 0 about 2.5 s
                              in, right after it starts the reporter).
  game-timeout                game phase: the launcher's own P8_GAME_S deadline
                              ended the game before the menu.  FAIL.
  launcher-ended-before-game  game phase: "[P8] done" with no "[P8] begin game"
                              (e.g. an NFS mount failed).  FAIL.
  repro                       a -DP8_LAUNCHES=N repro boot (p8launch.c) ended
                              with its "[P8] repro early=<k> n=<N>" row: the
                              detail is k/n.  Not a gate verdict (rc 0).

CLI:  p8grade.py SERIAL [--timeline TIMELINE] [--phase game|pfx]
      prints "verdict: <v>" and "detail: <d>"; exit 0 PASS, 3 early-exit, 1 FAIL.
"""
import re
import sys

BEGIN = "[P8] begin game "
END_RX = re.compile(r"\[P8\] end game status=(\S+)(?: ms=(\d+))?")
DONE = "[P8] done"
REPORTER_RX = re.compile(r"REDEngineErrorR")       # kernel rows (15-char comm), loaddll, census, +process
REPORTER_CMD_RX = re.compile(r'CreateProcessInternalW app L"[^"]*REDEngineErrorReporter\.exe"')
STEAM_RX = re.compile(r'steam_api64\.dll" at')       # the game's first load after the reporter (+loaddll)

PASS = ("launcher-done", "main-menu", "repro")
REPRO_RX = re.compile(r"\[P8\] repro early=(\d+) n=(\d+)")
LAUNCH_RX = re.compile(r"\[P8\] begin launch (\d+)/(\d+) ")
LEND_RX = re.compile(r"\[P8\] end launch (\d+) (?:status=(\S+) ms=(\d+) -> early-exit|alive at (\d+) s -> survived|spawn-failed)")


def launches(text):
    """Per-launch rows of a repro boot: [{i, result, status, ms, reporter, steam}]."""
    out = []
    starts = [(m.start(), int(m.group(1))) for m in LAUNCH_RX.finditer(text)]
    for k, (pos, i) in enumerate(starts):
        seg = text[pos:starts[k + 1][0] if k + 1 < len(starts) else len(text)]
        m = LEND_RX.search(seg)
        cut = seg[:m.start()] if m else seg
        r = {"i": i, "reporter": bool(REPORTER_RX.search(cut) or REPORTER_CMD_RX.search(cut)),
             "steam": bool(STEAM_RX.search(cut)), "status": None, "ms": None}
        if not m:
            r["result"] = "unfinished"
        elif m.group(2) is not None:
            r.update(result="early-exit", status=m.group(2), ms=int(m.group(3)))
        elif m.group(4) is not None:
            r["result"] = "survived"
        else:
            r["result"] = "spawn-failed"
        out.append(r)
    return out


def _after_launch(text):
    i = text.find(BEGIN)
    return None if i < 0 else text[i:]


def game_end(text):
    """None while the game runs; else {status, ms, reporter, steam} from the
    launcher's "[P8] end game" row and what the log shows before it."""
    g = _after_launch(text)
    if g is None:
        return None
    m = END_RX.search(g)
    if not m:
        return None
    before = g[:m.start()]
    return {
        "status": m.group(1),
        "ms": int(m.group(2)) if m.group(2) else None,
        "reporter": bool(REPORTER_RX.search(before) or REPORTER_CMD_RX.search(before)),
        "steam": bool(STEAM_RX.search(before)),
    }


def describe(e):
    rep = "yes" if e["reporter"] else "no"
    steam = "" if not e["steam"] else "; steam_api64 loaded"
    ms = f" after {e['ms']} ms" if e["ms"] is not None else ""
    return f"game status={e['status']}{ms}; REDEngineErrorReporter ran: {rep}{steam}"


def before_menu_exit(text):
    """The verdict when the game process has ended and no menu was seen, or
    None while it runs: (verdict, detail)."""
    e = game_end(text)
    if e is None:
        return None
    if e["status"] == "timeout":
        return "game-timeout", "the launcher's P8_GAME_S deadline ended the game before the menu; " + describe(e)
    return "early-exit", "the game process ended before the main menu: " + describe(e)


def launcher_end_verdict(text, phase):
    """The verdict when the launcher printed "[P8] done" (or the kernel
    halted) and the watcher saw no menu: (verdict, detail)."""
    if phase != "game":
        return "launcher-done", "the launcher finished"
    m = REPRO_RX.search(text)
    if m:
        ls = launches(text)
        rows = " ".join(f"{x['i']}:{x['result']}{'' if x['ms'] is None else '@' + str(x['ms'])}"
                        f"{'+rep' if x['reporter'] else ''}{'+steam' if x['steam'] else ''}" for x in ls)
        return "repro", f"early={m.group(1)} n={m.group(2)}; {rows}"
    r = before_menu_exit(text)
    if r:
        return r
    if _after_launch(text) is None:
        return "launcher-ended-before-game", "the launcher ended without starting the game (see its [P8] rows)"
    return "early-exit", "the launcher ended with the game started and no [P8] end game row"


def rc_of(verdict):
    if verdict in PASS:
        return 0
    return 3 if verdict == "early-exit" else 1


def replay(serial_text, timeline_text="", phase="game"):
    """A saved boot through the same rules: (verdict, detail)."""
    from_tl = re.search(r"main menu OCR at", timeline_text or "")
    if phase == "game" and from_tl:
        return "main-menu", "the timeline has main-menu OCR rows"
    if DONE in serial_text or "System halt" in serial_text:
        return launcher_end_verdict(serial_text, phase)
    if phase == "game":
        r = before_menu_exit(serial_text)
        if r:
            return r
    return "running", "the log ends with the launcher still running (the watcher decided: see the timeline)"


def _unprefix(text):
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    from console_rows import resync
    return "\n".join(resync(text, ["P8"]))


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("serial")
    ap.add_argument("--timeline")
    ap.add_argument("--phase", default="game", choices=["game", "pfx"])
    a = ap.parse_args(argv)
    text = _unprefix(open(a.serial, "rb").read().decode("utf-8", "replace"))
    tl = open(a.timeline).read() if a.timeline else ""
    v, d = replay(text, tl, a.phase)
    print(f"verdict: {v}")
    print(f"detail: {d}")
    return 0 if v == "running" else rc_of(v)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
