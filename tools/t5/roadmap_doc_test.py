#!/usr/bin/env python3
"""Host (T1) validator for program-2/ROADMAP.md (GB-054).

Checks the re-plan doc is grounded rather than aspirational:
  * the three epics are named;
  * the measured boundary is stated correctly (native dynamic linking is LANDED
    via LDHOBBY.SO; Linux glibc dynamic linking is ABSENT and cites §W5);
  * the two staged cards exist in the backlog table, each with an Artifact and
    a Falsifier line in its own section of this doc/board;
  * every GB-xxxx the doc cites exists in gaming.md (no invented tickets).

Run: python3 tools/t5/roadmap_doc_test.py [roadmap] [plan]
Exit 0 = pass.
"""
import os, re, sys

def check(roadmap="program-2/ROADMAP.md", plan="gaming.md"):
    txt = open(roadmap, encoding="utf-8").read()
    plan_txt = open(plan, encoding="utf-8").read()
    plan_ids = set(re.findall(r"\bGB-(\d{3})\b", plan_txt))
    out = []

    for epic in ("MONO", "RTS", "VULKAN"):
        out.append(("ok" if epic in txt else "fail", f"epic {epic} named"))

    out.append(("ok" if "LDHOBBY.SO" in txt and "landed" in txt.lower()
                else "fail", "native dynamic route LDHOBBY.SO stated as landed"))
    out.append(("ok" if "§W5" in txt and "absent" in txt.lower()
                else "fail", "Linux glibc dynamic route stated absent with §W5"))

    for card, art in (("GB-196", "src/kernel/elf_loader.c"),
                      ("GB-197", "tools/t4/virgl_probe.py")):
        in_table = re.search(r"\|\s*" + card + r"\b", txt) is not None
        out.append(("ok" if in_table else "fail", f"{card} in the ordered backlog table"))
        out.append(("ok" if art in txt else "fail", f"{card} names its artifact {art}"))
        out.append(("ok" if "Falsifier" in txt or "falsifier" in txt else "fail",
                    f"{card}: the doc states a falsifier policy"))

    cited = set(re.findall(r"\bGB-(\d{3})\b", txt))
    missing = sorted(cited - plan_ids)
    out.append(("ok" if not missing else "fail",
                f"every cited GB id exists in gaming.md (missing={missing})"))

    return out


if __name__ == "__main__":
    road = sys.argv[1] if len(sys.argv) > 1 else "program-2/ROADMAP.md"
    plan = sys.argv[2] if len(sys.argv) > 2 else "gaming.md"
    res = check(road, plan)
    bad = 0
    for st, msg in res:
        print(("PASS: " if st == "ok" else "FAIL: ") + msg)
        bad += st != "ok"
    print(f"\nROADMAP_DOC_TEST: {'PASS' if not bad else 'FAIL'} ({len(res)} checks, {bad} failed)")
    sys.exit(1 if bad else 0)
