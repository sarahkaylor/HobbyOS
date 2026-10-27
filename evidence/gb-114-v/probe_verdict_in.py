#!/usr/bin/env python3
"""GB-114-V clause-4 probe: what does groom.verdict_in() read a body as?
Read-only; imports the tool under test from the tree it sits in."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import groom  # noqa: E402

cases = [
    ("real GB-113-V opening", "**GREEN — GB-113-V, verified on this VM (DEEP verifier `wgfx-verify`), code PR #222 @ `d21759c`.**"),
    ("template", "VERDICT: **RED** (GB-112-V) -> GB-112 goes to In Progress"),
    ("plain template", "VERDICT: GREEN"),
    ("Greenfield word", "Greenfield is not a verdict"),
    ("sentence starting Green", "Green eggs and ham"),
    ("sentence starting Green, punctuation", "Green, and then some."),
    ("mid-sentence green", "the build is green today"),
    ("heading GREEN", "## GREEN\n\nsee below"),
    ("fenced code with GREEN", "```\nGREEN\n```"),
    ("word RED at start", "RED herring"),
]

for name, body in cases:
    print(f"{groom.verdict_in(body)!r:8} <- {name}: {body.splitlines()[0][:70]!r}")