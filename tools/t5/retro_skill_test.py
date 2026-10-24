#!/usr/bin/env python3
"""Host (T1) validator for GB-055 (retrospective + lessons merged into skills).

Checks: the retrospective exists with the worked/failed/rules structure, and
the hobbyos-gaming skill exists with valid YAML frontmatter (name/description),
so the DoD "lessons merged into skills" is structurally enforced.
"""
import os, re, sys

SKILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                     "skills", "hobbyos-gaming", "SKILL.md")
RETRO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                     "docs", "gaming-retrospective.md")

def check():
    out = []
    # retrospective
    if os.path.isfile(RETRO):
        r = open(RETRO).read()
        ok = all(s in r for s in ("What worked", "What didn't", "Rules to teach"))
        out.append(("ok" if ok else "fail", "retrospective has worked/failed/rules sections"))
    else:
        out.append(("fail", "docs/gaming-retrospective.md missing"))
    # skill frontmatter
    if os.path.isfile(SKILL):
        s = open(SKILL).read()
        fm_ok = s.startswith("---") and re.search(r"(?m)^name:\s*", s) and \
                re.search(r"(?m)^description:\s*", s)
        lessons = any(k in s for k in ("evidence-before-close", "de-magic",
                                       "disk build gotchas", "GB-057", "full-screen"))
        out.append(("ok" if fm_ok and lessons else "fail",
                    "hobbyos-gaming/SKILL.md has valid frontmatter + merges lessons"))
    else:
        out.append(("fail", "skills/hobbyos-gaming/SKILL.md missing"))
    # index lists it
    idx = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "skills", "README.md")
    if os.path.isfile(idx) and "hobbyos-gaming" in open(idx).read():
        out.append(("ok", "skills/README.md indexes hobbyos-gaming"))
    else:
        out.append(("fail", "skills/README.md does not list hobbyos-gaming"))
    return out

def main():
    fails = 0
    for status, msg in check():
        print(f"  {'PASS' if status=='ok' else 'FAIL'}: {msg}")
        if status != "ok":
            fails += 1
    print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
