#!/usr/bin/env python3
"""launch_app.py - open a HobbyOS desktop menu entry over QMP, by index.

    launch_app.py <qmp-sock> <menu-index> [--shot PREFIX]

The Apps menu is a classic press-drag-release menu whose item list is longer
than the screen (DOOM.BIN sits at index 18, below the visible 16), so this
presses the Apps button, walks the highlight down with ArrowDown, and confirms
with Return.  The kernel prints the index -> name map to the serial console
(`[MENU] 18=DOOM.BIN`), which is where the index comes from.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from click_abs import Qmp  # noqa: E402

APPS_BUTTON = (35, 1072)


def main() -> int:
    sock = sys.argv[1]
    index = int(sys.argv[2])
    prefix = sys.argv[3] if len(sys.argv) > 3 else "/tmp/app"

    q = Qmp(sock)
    # clear any open menu, then open the Apps menu
    q.click(900, 400)
    q.click(*APPS_BUTTON)
    q.cmd("screendump", {"filename": f"{prefix}_menu.png", "format": "png"})

    for _ in range(index):
        q.cmd("send-key", {"keys": [{"type": "qcode", "data": "down"}]})
        time.sleep(0.06)
    time.sleep(0.3)
    q.cmd("screendump", {"filename": f"{prefix}_sel.png", "format": "png"})
    q.cmd("send-key", {"keys": [{"type": "qcode", "data": "ret"}]})
    print(f"selected menu index {index}", flush=True)

    for wait in (3, 5, 8):
        time.sleep(wait)
        q.cmd("screendump", {"filename": f"{prefix}_{wait}s.png", "format": "png"})
        print(f"shot {prefix}_{wait}s.png", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
