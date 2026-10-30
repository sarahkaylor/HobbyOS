"""GB-136-V3 clause 1: newest-by-time governs, ordinal only breaks ties.

governing() unpacks (ordinal, gid, verdict, ts) — this passes that column order.
"""
import importlib.util

SEL = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb136v3/tools/board/check_verdicts.py"
spec = importlib.util.spec_from_file_location("cv", SEL)
cv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cv)

cases = [
    ("RED re-posted after a GREEN leaves the card RED",
     [(0, "GB-900", "GREEN", "2026-10-30T10:00:00Z"), (1, "GB-900-V", "RED", "2026-10-30T11:00:00Z")],
     "RED"),
    ("GREEN newest supersedes an older RED",
     [(1, "GB-901-V", "RED", "2026-10-30T10:00:00Z"), (2, "GB-901-V2", "GREEN", "2026-10-30T11:00:00Z")],
     "GREEN"),
    ("later RED does NOT supersede an earlier RED",
     [(1, "GB-902-V", "RED", "2026-10-30T10:00:00Z"), (2, "GB-902-V2", "RED", "2026-10-30T11:00:00Z")],
     "RED"),
    ("two cards share one id (#300 GREEN 16:10 vs #301 RED 16:15): newer time wins",
     [(1, "GB-903-V", "GREEN", "2026-10-30T16:10:00Z"), (1, "GB-903-V", "RED", "2026-10-30T16:15:00Z")],
     "RED"),
    ("tie on time -> ordinal breaks it (V2 GREEN beats V RED)",
     [(1, "GB-904-V", "RED", "2026-10-30T10:00:00Z"), (2, "GB-904-V2", "GREEN", "2026-10-30T10:00:00Z")],
     "GREEN"),
]
bad = 0
for name, gates, want in cases:
    gid, got = cv.governing(gates)
    ok = got == want
    bad += 0 if ok else 1
    print(f"  [{'OK  ' if ok else 'FAIL'}] {name}\n         -> governing={gid} verdict={got} (want {want})")
print("failures:", bad)
raise SystemExit(1 if bad else 0)