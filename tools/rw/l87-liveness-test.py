#!/usr/bin/env python3
"""l87-liveness-test.py -- regression test for the L87 stall rule.

The L86 driver called a boot "stalled" when `[XSW] n` had not changed for 150 s.
That is inside the measured normal load: L78 M2 has 215 s with no present while
the def loader keeps opening files, and the menu appears at n~600 after ~650 s.
The death row in L86's own logs is `[L86] STALLED: [XSW] n=390 frozen for 150 s`
at elapsed 568.9 s; the menu arrived 26 s later in L87 boot B (597.7 s).

This test drives the two pure functions the driver now uses with that exact
timeline, plus the boundary.  It needs no QEMU and no screenshot.

    python3 tools/rw/l87-liveness-test.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rw_l86_input import liveness_quiet, is_stalled  # noqa: E402

fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    print("%-58s got=%-6s want=%-6s %s" % (name, got, want,
                                           "PASS" if ok else "FAIL"))
    if not ok:
        fails += 1


# Timeline of the real load, seconds after the launch handed over.
# (t, last present at, last [XO] at) -- presents stop at 150 s, loader opens
# continue until 300 s, menu at 650 s.
def at(t, last_present, last_open):
    return liveness_quiet(t, last_present, last_open)


OLD = 150
NEW = 900

# t=200: presents frozen for 50 s, loader opened a file 0 s ago.
check("old rule at t=200 (presents quiet 50s)", at(200, 150, 200) > OLD, False)
# t=320: presents frozen for 170 s (the old rule fired at 150 s), loader
# opened a file 20 s ago -> the new rule must not fire.
check("old rule fires in the plateau", (320 - 150) > OLD, True)
check("new rule survives the plateau", at(320, 150, 300) > NEW, False)
# t=600: loader has been silent since 300 s, presents since 150 s -> 300 s
# quiet, still inside the 900 s budget (menu lands at 650 s).
check("new rule waits through the end of the load",
      at(600, 150, 300) > NEW, False)
# A genuinely dead guest: both signals silent for >900 s.
check("new rule fires on a real 901 s silence", at(1201, 150, 300) > NEW, True)
# is_stalled is a strict comparison: exactly the budget is not yet a stall.
check("boundary: quiet == budget is not a stall", is_stalled(900, 900), False)
check("boundary: quiet = budget+1 is a stall", is_stalled(901, 900), True)

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
