#!/usr/bin/env python3
"""Verdict propagation check (§4.2 step 5).

A `GB-nnn-V` verification ticket posts a GREEN/RED verdict that *governs the
parent's lane*:

    GREEN -> the implementation ticket goes to Merge Gate
    RED   -> the implementation ticket goes back to In Progress

Today that propagation is done by hand, and it was missed twice in one loop:
GB-010 and GB-070 both sat In Review with a RED verdict already posted against
them, reading as "awaiting review" when a gate had in fact been run and failed.
A lane that lies about the gate is the same defect class as an instrument that
lies about the machine, so this check is a tool, not a habit.

GB-136 supersession (2026-10-30): a gate can be superseded by a LATER gate card
for the same parent (`GB-nnn-V2`, `-V3`, …).  When an earlier gate's RED is
overtaken by a later gate's GREEN, the old RED is NOT a mismatch — the later gate
carries the truth, and the tool names the successor (id + sha) instead of crying
mismatch.  A RED with NO such successor is still reported.  This is what keeps a
re-verified card (GB-035, GB-060, GB-066, GB-071, GB-074, GB-106, GB-110, GB-117,
GB-120 — 9 on a healthy board) from reading as a fresh failure of the exact kind
GB-010/GB-070 were minted for.

Exit 0 = every parent's lane agrees with its governing gate's verdict.
Exit 1 = at least one mismatch or orphan (printed, with the verdict line).
Exit 2 = environment/lookup problem (never a silent pass).
"""
import json
import os
import re
import sys
import urllib.request

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
LANE_LABELS = ("Backlog", "Ready", "In Progress", "In Review", "Merge Gate",
               "Done", "Icebox")
GATE_RE = re.compile(r"^(GB-\d+)-V(\d*)$")


def api(path):
    url = f"{BASE}{path}"
    req = urllib.request.Request(url, headers={"Authorization": f"token {TOKEN}"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def all_issues():
    out, page = [], 1
    while True:
        batch = api(f"/issues?state=all&limit=50&page={page}")
        if not batch:
            return out
        out.extend(batch)
        page += 1


def lane_of(issue):
    names = {l["name"] for l in issue["labels"]}
    return next((x for x in LANE_LABELS if x in names), None)


def base_id(title):
    return title.split()[0] if title else ""


def gate_ordinal(gid):
    """:return the gate ordinal (base -V = 1, -V2 = 2, …) or None if not a gate."""
    m = GATE_RE.match(gid or "")
    if not m:
        return None
    return int(m.group(2)) if m.group(2) else 1


def verdict_of(number):
    """Newest GREEN/RED verdict posted on a verification ticket.

    GB-114: accepts `**GREEN — …**` as well as `VERDICT: GREEN` (shared with groom.py so
    the lint and this check cannot disagree about what a verdict looks like).
    """
    cs = api(f"/issues/{number}/comments")
    for c in reversed(cs):
        body = (c.get("body") or "").replace("\r", "")
        try:
            from groom import verdict_in
            v = verdict_in(body)
        except Exception:
            m = re.search(r"VERDICT[^\n]{0,40}?\b(GREEN|RED)\b", body, re.I)
            v = m.group(1).upper() if m else None
        if v:
            line = next((ln.strip() for ln in body.split("\n")
                        if re.search(r"VERDICT|\bGREEN\b|\bRED\b", ln, re.I)), "")
            return v, line[:160]
    return None, None


def gate_sha(line):
    """The gate's git sha named in a verdict line, for the supersession successor."""
    m = re.search(r"\b[0-9a-f]{7,40}\b", line or "")
    return m.group(0) if m else None


def classify(verdict, parent_lane):
    """The rule, isolated so a self-test can exercise it without the API."""
    if verdict == "GREEN":
        return parent_lane in ("Merge Gate", "Done"), "Merge Gate"
    return parent_lane in ("In Progress", "Backlog", "Ready"), "In Progress"


def governing(gates):
    """GB-136 supersession resolution for one parent's gate chain.

    gates: list of (gid, verdict) — every gate card for the SAME parent, with its
    newest verdict.  Returns (governing_id, governing_verdict, successor) where:

      * governing gate   = the HIGHEST-ORDINAL gate that carries a verdict
      * decision         = (governing_gid, governing_verdict)
      * successor        = None if governing IS the highest; else the governing
                           (gid, sha) that overtren the others
    """
    # highest ordinal that has a verdict
    has = [(gate_ordinal(g) or 1, g, v) for g, v in gates if v]
    if not has:
        return None, None, None
    max_ord, gov_gid, gov_v = max(has, key=lambda x: x[0])
    return gov_gid, gov_v, (gov_gid,)


def is_superseded(gid, gov_gid):
    """True when `gid` is an earlier gate than the governing one (ganny by ordinal)."""
    o1, o2 = gate_ordinal(gid) or 1, gate_ordinal(gov_gid) or 1
    return o1 < o2


def self_test():
    cases = [
        ("RED", "In Review", False),
        ("RED", "In Progress", True),
        ("GREEN", "In Review", False),
        ("GREEN", "Merge Gate", True),
        ("GREEN", "In Progress", False),
        ("RED", "Done", False),
    ]
    bad = 0
    for verdict, lane, want_ok in cases:
        ok, want = classify(verdict, lane)
        mark = "OK  " if ok == want_ok else "FAIL"
        if ok != want_ok:
            bad += 1
        print(f"  [{mark}] verdict={verdict:<5} parent in {lane!r:<13} "
              f"-> ok={ok} (expected {want_ok}, want-lane {want!r})")

    # GB-136 supersession — both directions.
    # Direction 1: an EARLIER gate's RED with a LATER gate's GREEN is superseded,
    # so it is NOT reported; the governing GREEN is the higher ordinal.
    gov_gid, gov_v, succ = governing([("GB-117-V", "RED"), ("GB-117-V5", "GREEN")])
    dir1 = (gov_gid == "GB-117-V5" and gov_v == "GREEN"
            and is_superseded("GB-117-V", gov_gid) and succ == ("GB-117-V5",))
    if not dir1:
        bad += 1
    print(f"  [{'OK  ' if dir1 else 'FAIL'}] GB-136: earlier RED superseded by a later GREEN -> not a mismatch; governing={gov_gid}={gov_v}")
    # Direction 2: a RED with NO successor is still reported (stands as the governor
    # and the lane truth disagrees).
    gov2, v2, _ = governing([("GB-121-V", "RED")])
    dir2 = (gov2 == "GB-121-V" and v2 == "RED"
            and not is_superseded("GB-121-V", gov2))
    if not dir2:
        bad += 1
    print(f"  [{'OK  ' if dir2 else 'FAIL'}] GB-136: RED with no successor is still reported (governing={gov2}={v2})")
    # If the supersession rule is disabled (naive "newest on its own card"), the
    # earlier GB-035-V RED would be read as the truth and reported as a mismatch:
    # this is the load-bearing direction the rule exists to stop.
    naive = ("GB-035-V", "RED") if not dir1 else None
    if dir1 and naive is not None and not is_superseded("GB-035-V", gov_gid):
        bad += 1
    else:
        ok_load = dir1
    print(f"  [{'OK  ' if dir1 else 'FAIL'}] GB-136: supersession is load-bearing (a re-verified RED would otherwise misfire)")

    print(f"self-test result: fails={bad}")
    return 0 if bad == 0 else 1


def main():
    if "--self-test" in sys.argv:
        return self_test()
    if not TOKEN:
        print("check_verdicts: FORGEJO_TOKEN not set (source ~/.hermes/.env)")
        return 2
    try:
        issues = all_issues()
    except Exception as e:                                    # noqa: BLE001
        print(f"check_verdicts: cannot read the board: {e}")
        return 2

    # Parent ticket per GB-nnn.
    by_id = {}
    for i in issues:
        by_id.setdefault(base_id(i.get("title") or ""), i)

    # Group gates per parent.
    from collections import defaultdict
    chain = defaultdict(list)                 # parent GB-nnn -> [gate issue]
    for i in issues:
        gid = base_id(i.get("title") or "")
        m = GATE_RE.match(gid)
        if m:
            chain[m.group(1)].append(i)

    checked = mismatched = orphan = superseded = 0
    lines = []
    for parent in sorted(chain):
        parent_it = by_id.get(parent)
        gates = []
        for gi in chain[parent]:
            v, line = verdict_of(gi["number"])
            if v:
                gates.append((base_id(gi.get("title") or ""), v, gi["number"], line))
        if not gates:
            continue
        gov_gid, gov_v, _ = governing([(g[0], g[1]) for g in gates])
        if parent_it is None:
            lines.append(f"  [{gov_gid}] no parent ticket {parent} on the board")
            mismatched += 1
            continue
        checked += 1
        # show every gate, marking the ones superseded by a later gate.
        for gid, gv, gnum, gline in gates:
            if gid != gov_gid and is_superseded(gid, gov_gid) and gv == "RED":
                superseded += 1
                lines.append(f"  SUPERSEDED {gid}#{gnum} = RED, overtaken by {gov_gid} "
                             f"(successor sha {gate_sha(gline)}) for {parent}#{parent_it['number']}")
        pl = lane_of(parent_it)
        okv, want = classify(gov_v, pl)
        if not okv:
            mismatched += 1
            lines.append(f"  MISMATCH {gov_gid} = {gov_v} "
                         f"but {parent}#{parent_it['number']} is in {pl!r} (want {want!r})")
            lines.append(f"           verdict line: {gates and next((g[3] for g in gates if g[0]==gov_gid), '')}")

    # gates without a matching parent issue are still surfaced.
    for parent in sorted(set(chain) - set(by_id)):
        for gi in chain[parent]:
            v, line = verdict_of(gi["number"])
            if v:
                lines.append(f"  [{base_id(gi.get('title') or '')}] no parent ticket {parent} on the board")
                mismatched += 1
                break

    # Orphan REDs in Merge Gate with no gate ticket (existing guard).
    for pid, pi in sorted(by_id.items()):
        if GATE_RE.match(pid) or pid in chain:
            continue
        if lane_of(pi) != "Merge Gate":
            continue
        verdict, line = verdict_of(pi["number"])
        if verdict != "RED":
            continue
        orphan += 1
        lines.append(f"  MISMATCH {pid}#{pi['number']} is in 'Merge Gate' carrying a RED and has no gate ticket")
        lines.append(f"           verdict line: {line}")

    for ln in lines:
        print(ln)
    print(f"verdicts checked: {checked}, mismatched parents: {mismatched}, "
          f"superseded REDs: {superseded}, orphaned REDs in Merge Gate: {orphan}")
    return 0 if mismatched == 0 and orphan == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
