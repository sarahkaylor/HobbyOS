#!/usr/bin/env python3
"""Host (T1) self-test for perf_game_cycle core logic: freeze-watchdog
detection and the RSS "no unbounded growth" assertion (GB-046). No QEMU."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perf_game_cycle import FreezeWatchdog, rss_trend_growth, flag_freeze_free

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

print("FreezeWatchdog")
# a static scene (same digest repeatedly) must freeze after `window` samples
w = FreezeWatchdog(4)
obs = [w.observe("A") for _ in range(6)]
check(obs[0] is False and obs[1] is False and obs[2] is False,
      "does not flag freeze before `window` identical samples")
check(w.frozen is True and obs[-1] is True,
      "flags freeze once 4 consecutive identical frames are seen")
check(w.runs_seen == 6, "counts observations")

# a changing scene (animated game) must never freeze
w2 = FreezeWatchdog(4)
obs2 = [w2.observe(f"h{i}") for i in range(10)]
check(True not in obs2 and w2.frozen is False,
      "an animated game (changing frames) is never flagged frozen")

# None samples (missing screendump) count toward the window
w3 = FreezeWatchdog(3)
w3.observe("x"); r = w3.observe(None)
check(r is False, "sparse None observed, not yet frozen")

print("rss_trend_growth (\u201cno unbounded growth\u201d)")
g, ok = rss_trend_growth([1000, 1001, 1002], 64)
check(ok is True and round(g, 2) == 0.0, "small RSS growth (2 KB) ok under a 64 MB bound")
g2, ok2 = rss_trend_growth([1000, 1000, 5000], 1)
check(ok2 is False and round(g2, 2) == 3.91, "4000 KB (3.91 MB) growth fails a 1 MB bound")
g3, ok3 = rss_trend_growth([1000, 1000, 5000], 4)
check(ok3 is True and round(g3, 2) == 3.91, "4000 KB (3.91 MB) growth is fine under a 4 MB bound")
g4, ok4 = rss_trend_growth([], 64)
check(ok4 is True and g4 == 0.0, "no samples => pass (nothing measured)")

print("flag_freeze_free")
check(flag_freeze_free(["a", "a", "a", "a"], 3)[-1] is True,
      "flags the static tail frozen")
check(flag_freeze_free(["a", "b", "c", "d"], 3)[-1] is False,
      "does not flag a varying tail")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
