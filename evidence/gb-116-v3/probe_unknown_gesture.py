#!/usr/bin/env python3
"""GB-116-V3 clause 1: an unknown gesture must raise, not silently open nothing."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import evidence_rig as R  # noqa: E402


class Rec:
    def __init__(self):
        self.events = []

    def key(self, k, hold=0.0):
        self.events.append(("key", k))

    def click(self, x, y):
        self.events.append(("click", x, y))


for bad in ("auto", "keyz", None):
    q = Rec()
    try:
        R.launch_from_menu(q, 3, gesture=bad)
        print(f"gesture={bad!r}: NO EXCEPTION, events={q.events}")
    except ValueError as e:
        print(f"gesture={bad!r}: ValueError({e})  events_before_raise={q.events}")