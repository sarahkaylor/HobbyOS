#!/usr/bin/env python3
"""GB-057 taxonomy v2: split the intel T3 FAIL lines into console artifacts vs real failures.

A FAIL line carries its own tag ([SEDTEST] FAIL ...).  Anything *else* bracketed in
the message body — another test's tag, [WAITPID], [KERNEL], [ALIVE-SET] — means the
console glued a foreign record into the middle of this line, so the message is
evidence about the console, not about the program under test.
"""
import collections, re, sys

PATH = sys.argv[1] if len(sys.argv) > 1 else "/home/sarah/Documents/GitHub/HobbyOS/obj/t3_intel_fresh.log"
lines = [l.replace("\r", "") for l in open(PATH, errors="replace")]
fails = [l for l in lines if "FAIL" in l and "PASS" not in l]

TAG = re.compile(r"^\s*\[([A-Za-z0-9_]+)\]")
ANY_TAG = re.compile(r"\[([A-Za-z0-9_-]{3,})\]")
RECORD = re.compile(r"v2 loader:")
SPLIT_HEAD = re.compile(r"^\s*:")

cats = collections.Counter()
by_tag = collections.defaultdict(collections.Counter)
examples = collections.defaultdict(list)

for l in fails:
    m = TAG.match(l)
    own = m.group(1) if m else None
    body = l[m.end():] if m else l
    others = [t for t in ANY_TAG.findall(body) if t != own]
    t = l.strip()
    if others or RECORD.search(body):
        c = "ARTIFACT_FOREIGN"      # another record glued into this line
    elif SPLIT_HEAD.match(l):
        c = "ARTIFACT_SPLIT"        # tail of a message whose head went elsewhere
    elif len(t) < 20:
        c = "ARTIFACT_FRAGMENT"
    elif re.search(r"\brc>0\b|rc != 0|non-zero", t):
        c = "REAL_RC"
    else:
        c = "REAL_ASSERT"
    cats[c] += 1
    tag = own or "(untagged)"
    by_tag[tag][c] += 1
    if len(examples[c]) < 4:
        examples[c].append((tag, t[:100], others))

print(f"log: {PATH}  (PARTIAL — the tier wedged and was stopped)")
print(f"FAIL lines: {len(fails)}\n")
for c, n in cats.most_common():
    print(f"{c:18s} {n:4d}")
art = sum(v for k, v in cats.items() if k.startswith("ARTIFACT"))
print(f"\nartifacts {art} / real {len(fails)-art}\n")
for c in ("ARTIFACT_FOREIGN", "ARTIFACT_SPLIT", "REAL_ASSERT", "REAL_RC"):
    print(f"--- {c}")
    for tag, e, others in examples[c]:
        print(f"   [{tag}] {e}" + (f"   <foreign: {','.join(others)}>" if others else ""))
print("\n--- by tag (artifacts / real):")
for tag, c in sorted(by_tag.items(), key=lambda kv: -sum(kv[1].values())):
    a = c["ARTIFACT_FOREIGN"] + c["ARTIFACT_SPLIT"] + c["ARTIFACT_FRAGMENT"]
    r = c["REAL_RC"] + c["REAL_ASSERT"]
    print(f"  {tag:12s} total={sum(c.values()):3d}  artifacts={a:3d}  real={r:3d}")
