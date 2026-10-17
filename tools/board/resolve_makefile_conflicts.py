#!/usr/bin/env python3
"""Resolve an additive Makefile conflict in ONE pass, for every block at once.

Observed shapes, each with one correct answer:
  * one side empty                      -> take the side with content
  * the shared `host_tests:` prereq line -> token union on one line
  * recipe lines (`\t...`)               -> keep both
  * a comment + rule group               -> keep both (different tests)
Everything else is REFUSED rather than guessed, and the caller must follow with
`make -n host_tests` plus a `<<<<<<<` count (both are mandatory here: a
resolver that guesses is how a conflict marker reaches a commit).

Single pass matters: resolving blocks one at a time with nested markers still
present makes the next match span already-resolved text, which is exactly the
trap that turned one clean block into ten markers earlier.
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "Makefile"
src = open(path, encoding="utf-8", errors="surrogateescape").read()
BLOCK = re.compile(r"^<<<<<<< HEAD\n(.*?)^=======\n(.*?)^>>>>>>> [^\n]*\n",
                   re.M | re.S)
TOKEN = re.compile(r"\$\(([A-Z0-9_]+)\)")


def union_prereq(ours, theirs):
    have = set(TOKEN.findall(ours))
    add = [t for t in TOKEN.findall(theirs) if t not in have]
    extra = " " + " ".join("$(%s)" % t for t in dict.fromkeys(add)) if add else ""
    return ours.rstrip() + extra + "\n"


def resolve(m, log):
    ours, theirs = m.group(1).strip("\n"), m.group(2).strip("\n")
    if not ours.strip():
        log.append("one-sided: took theirs")
        return theirs + "\n"
    if not theirs.strip():
        log.append("one-sided: took ours")
        return ours + "\n"

    ol, tl = ours.split("\n"), theirs.split("\n")

    if ol[0].startswith("host_tests:") and tl[0].startswith("host_tests:") and len(ol) == 1 and len(tl) == 1:
        log.append("host_tests prereq: token union")
        return union_prereq(ol[0], tl[0])

    if all(l.startswith("\t") for l in ol) and all(l.startswith("\t") for l in tl):
        log.append("recipe: kept both")
        return "\n".join(list(ol) + [l for l in tl if l not in ol]) + "\n"

    # comment + rule groups (two different tests documented+defined next to
    # each other): keep both.  A block that shares any line is NOT this shape.
    if set(ol) & set(tl) - {""}:
        log.append("REFUSED: sides share lines")
        return None
    if any(l.startswith("host_tests:") for l in ol + tl):
        log.append("REFUSED: touches the prereq line in a mixed block")
        return None
    log.append("additive groups: kept both")
    return ours + "\n\n" + theirs + "\n"


log = []
out, nblocks = BLOCK.subn(lambda m: resolve(m, log) or m.group(0), src)
for line in log:
    print("  " + line)
print("resolved %d block(s)" % nblocks)
if "<<<<<<<" in out or ">>>>>>>" in out:
    print("REFUSED: unresolved markers remain")
    sys.exit(2)
open(path, "w", encoding="utf-8", errors="surrogateescape").write(out)
print("no markers left")
