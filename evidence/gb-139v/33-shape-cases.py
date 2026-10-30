"""GB-139-V clauses 1-2: the two house shapes, and the seven regression cases."""
import importlib.util

SEL = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb139v/tools/board/check_verdicts.py"
spec = importlib.util.spec_from_file_location("cv", SEL)
cv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cv)

print("=== clause 1: the two house shapes ===")
house = [
    ("**VERDICT — GREEN (GB-060-V2).** the gate re-ran clean", "GREEN"),
    ("**GB-101-V6 — GREEN at `1dd7160`**", "GREEN"),
]
bad = 0
for text, want in house:
    got, line = cv.verdict_line_of(text)
    ok = got == want
    bad += 0 if ok else 1
    print(f"  [{'OK  ' if ok else 'FAIL'}] {text[:60]!r} -> {got} (want {want})")

print("\n=== clause 2: the four earlier gates must not regress ===")
regress = [
    ("red or pending run is not a defect and a green run is not correctness", None),
    ("RED fixed (F1 + F2) — pushed to ticket/GB-098-stale / PR #157", None),
    ("red log:   T3_intel: FAIL — 1 FAIL line(s); 1262:SOMETEST: FAIL injected genuine red   rc=1", None),
    ("The string `VERDICT: GREEN` in prose is documentation, not a verdict", None),
    ("REDs and GREEN-gated helpers are not verdicts", None),
    ("## GB-136-V3 — **RED**", "RED"),
    ("**VERDICT: RED**", "RED"),
    ("GREEN — GB-114-V2 verified at 899d2a8", "GREEN"),
]
for text, want in regress:
    got, line = cv.verdict_line_of(text)
    ok = got == want
    bad += 0 if ok else 1
    print(f"  [{'OK  ' if ok else 'FAIL'}] {text[:64]!r:<68} -> {got} (want {want})")

print("\nfailures:", bad)
raise SystemExit(1 if bad else 0)