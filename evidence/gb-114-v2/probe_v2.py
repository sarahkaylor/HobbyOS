#!/usr/bin/env python3
"""GB-114-V2 probes — clauses 1 and 2, plus a few shapes beyond the card's list."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import groom  # noqa: E402
import merge_pr  # noqa: E402

print("== clause 1: prose must NOT be a verdict ==")
must_be_none = [
    ("the exact falsifier", "Green light to merge at 86f8473c0ffee - the diff is small."),
    ("Green eggs and ham", "Green eggs and ham"),
    ("RED herring", "RED herring"),
    ("Greenfield is not a verdict", "Greenfield is not a verdict"),
    ("mid-line green", "the build is green today"),
]
for name, body in must_be_none:
    got = groom.verdict_in(body)
    print(f"  {str(got):5}  <- {name}: {body[:60]!r}")

print("\n== clause 2: real verdict shapes must still resolve ==")
must_resolve = [
    ("GB-113-V opening", "**GREEN — GB-113-V, verified on this VM (DEEP verifier `wgfx-verify`), code PR #222 @ `d21759c`.**", "GREEN"),
    ("my own RED opening", "RED — GB-114-V, verified on this VM (DEEP verifier `wgfx-verify`), code PR #223 @ `86f8473`.", "RED"),
    ("## VERDICT: **RED** (GB-112-V)", "## VERDICT: **RED** (GB-112-V) -> GB-112 goes to In Progress", "RED"),
    ("## VERDICT: **GREEN** (GB-113-V)", "## VERDICT: **GREEN** (GB-113-V) -> Merge Gate", "GREEN"),
    ("bare **GREEN**", "**GREEN**", "GREEN"),
]
for name, body, want in must_resolve:
    got = groom.verdict_in(body)
    print(f"  {str(got):5}  <- {name} (want {want})")

print("\n== beyond the card's list (reported, not required) ==")
extra = [
    ("bold marker then parenthetical", "**GREEN** (GB-113-V) — Merge Gate"),
    ("bold marker, em dash inside bold", "**GREEN — GB-113-V**\nbody"),
    ("heading, no delimiter", "## GREEN\n\nsee below"),
    ("marker with colon", "GREEN: GB-113-V"),
    ("lowercase template", "verdict: green"),
]
for name, body in extra:
    print(f"  {str(groom.verdict_in(body)):5}  <- {name}: {body.splitlines()[0]!r}")

print("\n== the merge-authorising sentence, end to end ==")
body = "Green light to merge at 86f8473c0ffee - the diff is small."
print(f"  verdict_in            -> {groom.verdict_in(body)!r}")
print(f"  graded_shas_in        -> {merge_pr.graded_shas_in(body)!r}")