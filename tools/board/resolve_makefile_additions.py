#!/usr/bin/env python3
"""Resolve Makefile conflicts where both sides only ADD to a list.

Two shapes recur in this repo and both have a mechanical answer:

* **prerequisite lines** — both sides are one `host_tests: $(A) $(B) …` line and
  each added its own token: the answer is the token UNION on a single line.
* **recipe lines** — both sides are additions of whole `\t$(HOST_RUN) ./$(X)`
  lines: the answer is BOTH lines (they are different tests, not a revision).

Anything else is refused rather than guessed: a resolver that guesses is how a
`<<<<<<<` marker reaches a commit (it happened once, on this very file).
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "Makefile"
src = open(path).read()
BLOCK = re.compile(r"^<<<<<<< HEAD\n(.*?)^=======\n(.*?)^>>>>>>> [^\n]*\n",
                   re.M | re.S)
TOKEN = re.compile(r"\$\(([A-Z0-9_]+)\)")


def union_prereq_line(ours, theirs):
    have = set(TOKEN.findall(ours))
    add = [t for t in TOKEN.findall(theirs) if t not in have]
    return ours.rstrip() + (" " + " ".join("$(%s)" % t for t in dict.fromkeys(add)) if add else "")


def resolve(m):
    ours = m.group(1).strip("\n")
    theirs = m.group(2).strip("\n")
    ol, tl = ours.split("\n"), theirs.split("\n")

    if len(ol) == 1 and len(tl) == 1 and ol[0].startswith("host_tests:"):
        return union_prereq_line(ol[0], tl[0]) + "\n"

    if all(l.startswith("\t") for l in ol) and all(l.startswith("\t") for l in tl):
        merged = list(ol) + [l for l in tl if l not in ol]
        return "\n".join(merged) + "\n"

    sys.stderr.write("REFUSED block:\n--- ours ---\n%s\n--- theirs ---\n%s\n" % (ours[:300], theirs[:300]))
    sys.exit(2)


out, n = BLOCK.subn(resolve, src)
open(path, "w").write(out)
left = out.count("<<<<<<<") + out.count(">>>>>>>")
print("resolved %d block(s); markers left: %d" % (n, left))
sys.exit(1 if left else 0)
