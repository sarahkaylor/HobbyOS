#!/usr/bin/env python3
"""Which verdict reader is this?  Prints verdict_in()'s answer for a body that is a
sentence of English beginning with 'Green', plus the sha the guard would take from it.
Run in any tree; it reports the tree it ran in."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.join(HERE, "..", "..", "tools", "board")
sys.path.insert(0, BOARD)
import groom  # noqa: E402
import merge_pr  # noqa: E402

tree = subprocess.run(["git", "-C", HERE, "log", "--oneline", "-1"], capture_output=True, text=True).stdout.strip()
body = "Green light to merge at 86f8473c0ffee — the diff is small."
print(f"tree: {tree}")
print(f"body: {body}")
print(f"  groom.verdict_in  -> {groom.verdict_in(body)!r}")
print(f"  graded_shas_in    -> {merge_pr.graded_shas_in(body)!r}")
print(f"  verdict_in('Green eggs and ham')      -> {groom.verdict_in('Green eggs and ham')!r}")
print(f"  verdict_in('Greenfield is not...')    -> {groom.verdict_in('Greenfield is not a verdict')!r}")
print(f"  verdict_in('**GREEN — GB-113-V ...**')-> {groom.verdict_in('**GREEN — GB-113-V, verified on this VM, code PR #222 @ d21759c.**')!r}")