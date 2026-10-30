"""GB-139-V clause 6, narrowed: of the lines that LOOK like verdict declarations,
which shapes does the graded parser miss?  Prose that mentions GREEN is supposed
to be unhandled; a declaration that is missed is the finding."""
import importlib.util
import json
import re

SEL = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb139v/tools/board/check_verdicts.py"
spec = importlib.util.spec_from_file_location("cv", SEL)
cv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cv)

bodies = json.load(open("/home/sarah/.hermes/cache/scratch/board_comments.json"))
TOKEN = re.compile(r"\b(GREEN|RED)\b")

DECL = re.compile(r"^\s*(?:[-*]\s*)?(?:#{1,6}\s*)?\**\s*"
                  r"(?:VERDICT|GB-\d+|VERDICT\s*:)", re.I)


def skeleton(line):
    s = line.strip()
    s = TOKEN.sub("<V>", s)
    s = re.sub(r"`[^`]*`", "<code>", s)
    s = re.sub(r"[0-9a-f]{7,40}", "<sha>", s)
    s = re.sub(r"GB-\d+(?:-V\d*)?", "<GB>", s)
    s = re.sub(r"#\d+", "<num>", s)
    s = re.sub(r"\s+", " ", s)
    return s[:80]


shapes = {}
for body in bodies:
    for line in (body or "").replace("\r", "").split("\n"):
        if not TOKEN.search(line) or not DECL.match(line):
            continue
        got, _ = cv.verdict_line_of(line)
        k = skeleton(line)
        r = shapes.setdefault(k, {"n": 0, "parsed": 0, "sample": line.strip()[:130]})
        r["n"] += 1
        r["parsed"] += 1 if got else 0

tot = sum(r["n"] for r in shapes.values())
par = sum(r["parsed"] for r in shapes.values())
multi = {k: v for k, v in shapes.items() if v["n"] >= 2}
print(f"declaration-shaped lines: {tot} in {len(shapes)} distinct shapes (n>=2: {len(multi)})")
print(f"parsed: {par} ({100.0*par/max(tot,1):.1f}%)")
print(f"shapes n>=2 parsed: {sum(1 for v in multi.values() if v['parsed'])}/{len(multi)}")

print("\n--- unparsed DECLARATION shapes (n>=2) — the eighth gate's blind spots ---")
for k, v in sorted(multi.items(), key=lambda x: -x[1]["n"]):
    if not v["parsed"]:
        print(f"  n={v['n']:3d}  {k}")
        print(f"         e.g. {v['sample']}")

print("\n--- unparsed DECLARATION shapes (n==1) ---")
ones = [(k, v) for k, v in shapes.items() if v["n"] == 1 and not v["parsed"]]
print(f"  count: {len(ones)}")
for k, v in ones[:12]:
    print(f"    {k}")
    print(f"      e.g. {v['sample']}")