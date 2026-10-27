#!/usr/bin/env python3
"""GB-116-V2 clause 6 probe: what does the shared helper do when it is called the way
the rest of the tree calls it?

`tools/t5/acceptance_run.py:200` and `tools/perf_game_cycle.py:281` both call
`launch_from_menu(q, slot)` with no gesture.  The signature default is still
`gesture="auto"` (tools/evidence_rig.py:170), and after a12ef79 the body handles only
"keys" and "click" -- so neither F1 nor the click is sent.

Read-only: a recording stub stands in for the QMP object.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import evidence_rig as R  # noqa: E402

import inspect  # noqa: E402


class Rec:
    def __init__(self):
        self.events = []

    def key(self, k, hold=0.0):
        self.events.append(("key", k))

    def click(self, x, y):
        self.events.append(("click", x, y))


sig = inspect.signature(R.launch_from_menu)
print("signature:", sig)
for label, kwargs in [
    ("launch_from_menu(q, 3)            <- what acceptance_run.py / perf_game_cycle.py do", {}),
    ("launch_from_menu(q, 3, gesture='keys')", {"gesture": "keys"}),
    ("launch_from_menu(q, 3, gesture='click')", {"gesture": "click"}),
]:
    q = Rec()
    R.launch_from_menu(q, 3, **kwargs)
    keys = [e for e in q.events if e[0] == "key" and e[1] == "f1"]
    clicks = [e for e in q.events if e[0] == "click"]
    print(f"\n{label}")
    print(f"  f1={len(keys)}  clicks={len(clicks)}  events={q.events}")