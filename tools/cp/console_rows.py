#!/usr/bin/env python3
"""tools/cp/console_rows.py -- find graded rows anywhere in a serial line
(C0-console, docs/cp-console.md §4: the graders' second line of defence).

Since C0-console the kernel writes every row whole, but a grade must not hinge
on that: a row that lands behind another writer's bytes ("recover_us=[P5] end
fscheck ...") or two rows fused on one line must still be found.  resync()
splits each serial line at every occurrence of a known row tag ("[P5] ",
"[VK] ", ...) and returns the rows, each starting at its tag; text in front of
the first tag stays a row of its own.  XSESSION's relay prefix "[X] " is
dropped in front of a tag or at the start of a line.

Only the tags a grader names are split on, and a tag must be followed by a
space, so payloads that merely mention a tag-like word are not cut: the rows a
grader looks for are its own protocol's rows, none of which embeds another.

CLI (shell graders):
    python3 tools/cp/console_rows.py P2,WP < serial.log > serial.txt
    python3 tools/cp/console_rows.py --self-test
"""
import re
import sys

_XPFX = re.compile(r"\[X\] (?=\[)")


def resync(text, tags):
    """Rows of `text`, split at every "[<tag>] " of `tags`, "[X] " removed."""
    pat = re.compile(r"\[(?:%s)\] " % "|".join(re.escape(t) for t in tags))
    rows = []
    for ln in text.replace("\r", "").split("\n"):
        ln = _XPFX.sub("", ln)
        if ln.startswith("[X] "):
            ln = ln[4:]
        starts = [m.start() for m in pat.finditer(ln)]
        if not starts:
            rows.append(ln)
            continue
        if starts[0] > 0:
            rows.append(ln[:starts[0]])
        for i, s in enumerate(starts):
            rows.append(ln[s:starts[i + 1] if i + 1 < len(starts) else len(ln)])
    return rows


def self_test():
    t = ("[P5] begin d3d11 : wine x\n"
         "recover_us=[P5] end fscheck status=0 ms=5\n"
         "[X] [D3D11] done rc=0\n"
         "[X] hello\n"
         "mapped=[P5] mount ok[PUB] who=fault va=0x1\n"
         "[P5] end d3d11 status=0 ms=9[P5] end d3d12 status=0 ms=1\n"
         "plain kernel row\r\n")
    r = resync(t, ["P5", "D3D11"])
    assert "[P5] end fscheck status=0 ms=5" in r, r
    assert "recover_us=" in r, r
    assert "[D3D11] done rc=0" in r, r
    assert "hello" in r, r
    assert "[P5] mount ok[PUB] who=fault va=0x1" in r, r  # an unknown tag is not split
    assert "[P5] end d3d11 status=0 ms=9" in r and "[P5] end d3d12 status=0 ms=1" in r, r
    assert "plain kernel row" in r, r
    print("console_rows self-test: PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        sys.exit(self_test())
    tags = [t for t in (sys.argv[1] if len(sys.argv) > 1 else "").split(",") if t]
    if not tags:
        sys.exit("usage: console_rows.py TAG[,TAG...] < serial > rows")
    data = sys.stdin.buffer.read().decode("utf-8", "replace")
    sys.stdout.write("\n".join(resync(data, tags)) + "\n")
