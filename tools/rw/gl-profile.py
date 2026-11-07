#!/usr/bin/env python3
"""tools/rw/gl-profile.py — what the game asks the GL driver to do.

Runs on text already extracted from an apitrace capture (the trace itself is
hundreds of MB and replaying it is slow):

  apitrace trace -a gl -o out/rw.trace ./RimWorldLinux ...      (mode: gl)
  apitrace dump --grep '^glXCreateContextAttribsARB' out/rw.trace > out/gl-context.txt
  apitrace dump --grep '^glXSwapBuffers|^glDraw|^glClear' out/rw.trace > out/gl-frame-calls.txt

Emits: frames captured, draw calls per frame, clears per frame, the context
request (major/minor/profile/attribs) and a frame-time distribution derived
from the GALLIUM_HUD fps series (1/fps; the HUD samples at ~2 Hz, so this is a
coarse view, not per-frame timing).

Usage: gl-profile.py [out-dir]
"""
import os
import re
import statistics
import sys

DRAW = re.compile(r"^\d+ (glDrawArrays|glDrawElements|glDrawArraysInstanced|"
                  r"glDrawElementsInstanced|glDrawRangeElements|glMultiDrawArrays|"
                  r"glMultiDrawElements)\w*\(")
CLEAR = re.compile(r"^\d+ glClear\(")
SWAP = re.compile(r"^\d+ glXSwapBuffers\(")


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "out"
    calls = os.path.join(d, "gl-frame-calls.txt")
    ctx = os.path.join(d, "gl-context.txt")
    frames, cur = [], {"draw": 0, "clear": 0}
    for line in open(calls, errors="replace"):
        if DRAW.match(line):
            cur["draw"] += 1
        elif CLEAR.match(line):
            cur["clear"] += 1
        elif SWAP.match(line):
            frames.append(cur)
            cur = {"draw": 0, "clear": 0}
    if not frames:
        print("gl-profile: no frames in", calls)
        return 1
    # skip the first ~50 frames: shader compile / level load
    seg = frames[50:-50] if len(frames) > 150 else frames
    draws = [f["draw"] for f in seg]
    clears = [f["clear"] for f in seg]
    print(f"frames captured (glXSwapBuffers): {len(frames)}")
    print(f"draw calls/frame  median={statistics.median(draws)} mean={statistics.fmean(draws):.1f} "
          f"min={min(draws)} max={max(draws)}  (frames {50}..{len(frames)-50})")
    print(f"glClear/frame     median={statistics.median(clears)} min={min(clears)} max={max(clears)}")
    if os.path.exists(ctx):
        seen = set()
        for line in open(ctx, errors="replace"):
            line = line.strip()
            if line and line not in seen:
                seen.add(line)
                print("context request:", line)
    # Frame-time distribution is deliberately NOT derived here: the HUD dump
    # is a ~2 Hz sample of instantaneous fps, not per-frame timing, and it is
    # easy to pair a stale dump with a fresh trace.  fps.py reports it (1/fps)
    # next to the run that produced it.
    return 0


if __name__ == "__main__":
    sys.exit(main())
