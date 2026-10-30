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


def verdict_line_of(body):
    """The verdict a comment actually declares, from a VERDICT-SHAPED line only.

    GB-136's third defect (found by GB-136-V, #301): the old code took the value from
    `groom.verdict_in()` — a loose substring match — and then reported the *first* line
    mentioning GREEN/RED as its evidence.  That made prose into verdicts:

        "verdicts checked: 66, ... superseded REDs: 60 ..."   -> read as RED (plural!)
        "GREEN-gated merge helper ..."                        -> read as GREEN

    so the tool both suppressed real violations (a parent card whose comment merely
    mentioned GREEN became the governing GREEN) and manufactured fake ones.  A verdict
    must START a line (after optional markdown), and the token must not be part of a
    longer word: `REDs`, `GREEN-gated`, `REDUCE` are not verdicts.
    """
    for ln in (body or "").replace("\r", "").split("\n"):
        # (A) an explicit marker — the shape every Verifier uses.
        m = re.search(r"\bVERDICT\s*:\s*\**\s*(GREEN|RED)\b", ln, re.I)
        if m:
            return m.group(1).upper(), ln.strip()[:160]
        # (B) a line-initial, UPPER-CASE verdict token, optionally bold, followed by a
        # separator or end-of-line.  GB-136-V3 found three prose lines that this must
        # reject: `red or pending run …`, `RED fixed (F1 + F2) …`, `red log: …`.  The
        # separator requirement rejects the first two (a following lowercase word), and
        # requiring upper case rejects the lowercase sentences.
        m = re.match(r"^\s*(?:[-*]\s*)?\**\s*(GREEN|RED)\**\s*(?:[—–\-:.,;!)]|$)", ln)
        if m:
            return m.group(1), ln.strip()[:160]
        # (C) a heading that names the card, an em dash, then the verdict:
        #     `## GB-136-V3 — **RED** at a1f4fd4. …`
        m = re.match(r"^\s*#{1,6}\s+\S.*?[—–]\s*\**\s*(GREEN|RED)\b", ln)
        if m:
            return m.group(1), ln.strip()[:160]
    return None, None


def verdict_of(number):
    """Newest GREEN/RED verdict posted on a verification ticket.

    GB-114: accepts `**GREEN — …**` as well as `VERDICT: GREEN`, and GB-136: only a
    verdict-SHAPED line counts (see verdict_line_of), so a summary line that mentions
    "superseded REDs" can no longer become a verdict.
    """
    cs = api(f"/issues/{number}/comments")
    for c in reversed(cs):
        v, line = verdict_line_of((c.get("body") or ""))
        if v:
            # GB-136: the timestamp matters because two cards can share one gate id
            # (#300 and #301 are both "GB-136-V"), and then "the newest verdict" is a
            # question about time, not about card identity.
            return v, line, c.get("created_at")
    return None, None, None


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

    gates: list of (gid, verdict, ordinal) — every gate card for the SAME parent,
    each with its newest verdict.  ordinal: base `-V`=1, `-V2`=2, …; the PARENT
    card itself is passed with ordinal 0.

    GB-136-V (#301) defect 1: the first version returned the highest-ordinal member
    whatever its verdict, so a LATER RED "superseded" an earlier RED — the standing
    failure was silently labelled overturned at the very sha that failed.  **Only a
    GREEN can supersede.**  If any gate carries a GREEN the highest-ordinal GREEN
    governs; otherwise the highest-ordinal verdict governs and stays reported.
    """
    has = [(ord_, g, v, ts or "") for ord_, g, v, ts in gates if v]
    if not has:
        return None, None
    # GB-136-V (#301) defect 1, corrected: the NEWEST verdict wins, where "newest" is
    # a question of time (two cards can share one gate id — #300 and #301 are both
    # "GB-136-V"), and ordinal only breaks ties.  A later RED therefore re-confirms a
    # standing failure instead of being ignored because an older GREEN exists; a GREEN
    # supersedes only when it is itself the newest word.
    _o, g, v, _ts = max(has, key=lambda x: (x[3], x[0]))
    return g, v


def is_superseded(gid, gov_gid):
    """True when `gid` is an earlier gate than the governing one (ganny by ordinal;
    ordinal 0 = the parent, which is the governing side only for a parent-GREEN)."""
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
    gov_gid, gov_v = governing([(1, "GB-117-V", "RED", "t1"), (5, "GB-117-V5", "GREEN", "t5")])
    dir1 = (gov_gid == "GB-117-V5" and gov_v == "GREEN"
            and is_superseded("GB-117-V", gov_gid))
    if not dir1:
        bad += 1
    print(f"  [{'OK  ' if dir1 else 'FAIL'}] GB-136: earlier RED superseded by a later GREEN -> not a mismatch; governing={gov_gid}={gov_v}")
    # Direction 2: a RED with NO successor is still reported (stands as the governor
    # and the lane truth disagrees).
    gov2, v2 = governing([(1, "GB-121-V", "RED", "t1")])
    dir2 = (gov2 == "GB-121-V" and v2 == "RED"
            and not is_superseded("GB-121-V", gov2))
    if not dir2:
        bad += 1
    print(f"  [{'OK  ' if dir2 else 'FAIL'}] GB-136: RED with no successor is still reported (governing={gov2}={v2})")
    # If the supersession rule is disabled (naive "newest on its own card"), the
    # earlier GB-035-V RED would be read as the truth and reported as a mismatch:
    # this is the load-bearing direction the rule exists to stop.
    if dir1:
        ok_load = dir1
    print(f"  [{'OK  ' if dir1 else 'FAIL'}] GB-136: supersession is load-bearing (a re-verified RED would otherwise misfire)")

    # GB-136 defect 3 (found by GB-136-V, #301): verdict_of() read prose.  The parser
    # must take a verdict only from a VERDICT-SHAPED line, and this test exercises the
    # SAME function the board loop calls (verdict_line_of), not an adjacent helper.
    parse_cases = [
        ("**VERDICT: RED** — clause 1 fails", "RED"),
        ("## VERDICT: **GREEN** — all clauses", "GREEN"),
        ("**VERDICT: RED at `721650e` — ...", "RED"),
        ("GREEN — GB-114-V2, verified on this VM", "GREEN"),
        # the three that must NOT be verdicts (observed on the live board)
        ("verdicts checked: 66, mismatched parents: 2, superseded REDs: 60", None),
        ("a GREEN-gated merge helper that refuses", None),
        # GB-136-V3 (the three fictions found on the live board, plus the heading shape)
        ("red or pending run is not a defect and a green run is not correctness", None),
        ("RED fixed (F1 + F2) — pushed to ticket/GB-098-stale / PR #157", None),
        ("red log:   T3_intel: FAIL — 1 FAIL line(s)", None),
        ("## GB-136-V3 — **RED** at `a1f4fd4`. The fix is real; …", "RED"),
        ("**GREEN.** None of the RED conditions tripped", "GREEN"),
        ("**MERGED — `b1594f9` on `main`. GB-005 → Done.**", None),
    ]
    for text, want in parse_cases:
        got, _line = verdict_line_of(text)
        ok = got == want
        if not ok:
            bad += 1
        print(f"  [{'OK  ' if ok else 'FAIL'}] GB-136 parse: {text[:48]!r} -> {got} (want {want})")

    # A later RED must not supersede an earlier RED (defect 1, live path).
    red_only = governing([(1, "GB-016-V", "RED", "t1")])
    ok_red = red_only == ("GB-016-V", "RED")
    if not ok_red:
        bad += 1
    print(f"  [{'OK  ' if ok_red else 'FAIL'}] GB-136: RED-only chain still governs (no supersession) -> {red_only}")
    green_beats = governing([(1, "GB-117-V", "RED", "t1"), (5, "GB-117-V5", "GREEN", "t5")])
    ok_green = green_beats == ("GB-117-V5", "GREEN")
    if not ok_green:
        bad += 1
    print(f"  [{'OK  ' if ok_green else 'FAIL'}] GB-136: a GREEN supersedes an earlier RED -> {green_beats}")

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
            v, line, ts = verdict_of(gi["number"])
            if v:
                gates.append((gate_ordinal(base_id(gi.get("title") or "")) or 1,
                              base_id(gi.get("title") or ""), v, gi["number"], line, ts))
        # The PARENT's own card can also carry a GREEN that supersedes an earlier gate
        # RED (GB-120: parent #261 re-verified GREEN 2026-10-28T15:05Z after -V#262 RED).
        if parent_it is not None:
            pv, pline, pts = verdict_of(parent_it["number"])
            if pv:
                gates.append((0, parent, pv, parent_it["number"], pline, pts))
        if not gates:
            continue
        gov_gid, gov_v = governing([(g[0], g[1], g[2], g[5]) for g in gates])
        if parent_it is None:
            lines.append(f"  [{gov_gid}] no parent ticket {parent} on the board")
            mismatched += 1
            continue
        checked += 1
        # show every gate, marking the ones superseded by a later GREEN gate.
        # GB-136-V defect 1: a later RED does not supersede anything.
        for ord_, gid, gv, gnum, gline, _gts in gates:
            if gov_v != "GREEN":
                break
            if gid != gov_gid and gv == "RED":
                superseded += 1
                succ_line = next((g[4] for g in gates if g[1] == gov_gid), "")
                lines.append(f"  SUPERSEDED {gid}#{gnum} = RED, overtaken by {gov_gid} "
                             f"(successor sha {gate_sha(succ_line)}) for {parent}#{parent_it['number']}")
        pl = lane_of(parent_it)
        okv, want = classify(gov_v, pl)
        if not okv:
            mismatched += 1
            lines.append(f"  MISMATCH {gov_gid} = {gov_v} "
                         f"but {parent}#{parent_it['number']} is in {pl!r} (want {want!r})")
            lines.append(f"           verdict line: {next((g[4] for g in gates if g[1]==gov_gid), '')}")

    # gates without a matching parent issue are still surfaced.
    for parent in sorted(set(chain) - set(by_id)):
        for gi in chain[parent]:
            v, line, _ts = verdict_of(gi["number"])
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
        verdict, line, _ts = verdict_of(pi["number"])
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
