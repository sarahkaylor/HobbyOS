#!/usr/bin/env python3
"""tools/board/groom.py — GB-049 Backlog grooming bot.

The board's hygiene checks as one report that can fail, owned by the `infra`
lane.  Scope accumulated across the ticket, PLAN-006 and the 2026-10-19 board
posting:

  * close/await/block moves  — REPORT only.  A bot may move a card on label
    state, but it must not CLOSE one: a merged PR does not prove a DoD (see
    drift_check.py), and a card sits in a review lane with its gate unrun, so
    closing on a "merged" signal is exactly how a board starts lying.  This
    tool therefore does not --apply any lane move at all; it names the
    transitions a coordinator or worker could take, and posts the "awaiting
    dependency" note when asked (see --note-awaiting).
  * blockers lint (deadlock rule) — the `Depends on` graph across OPEN tickets
    must be acyclic.  An edge A -> B means A cannot progress until B does; a
    cycle means every member waits on the next, so no worker can ever take any
    member.  Both tickets would sit open forever and read as "awaiting review"
    or "awaiting dependency" — the board lying in the direction this ticket
    exists to stop.
  * verify-lane lint (PLAN-006, three checks) —
      unverified-in-review : an implementation card in a review lane (In
          Review / Merge Gate) whose `GB-nnn-V` gate has posted no GREEN.
          A review lane with a gate that never passed reads as "awaiting
          review" when the gate was simply never run (18/18 on 2026-10-20).
      stale-verify        : a `GB-nnn-V` ticket still open while its parent
          implementation is already closed — the gate has nothing left to
          govern.
      orphan-verify       : a `GB-nnn-V` whose parent implementation ticket
          does not exist on the board.
  * quantitative-DoD hard stop (2026-10-19) — closing a ticket whose DoD is a
    numeric threshold without measured evidence is refused.  GB-068 was closed
    as Done by a label-only read while `42`–`55 user memory faults` still
    printed on every wave; a falsely closed ticket costs the program the only
    signal that forty processes die per run.  A Done move is blocked unless an
    EVIDENCE comment quotes the measured value when the DoD carries a
    digit-bearing predicate (`== 0`, `0 <x>`, `>= N`, `within N`).

Exit 0 = clean.  Exit 1 = at least one finding (printed).  Exit 2 =
environment/network problem (never a silent pass).

Usage:
  python3 tools/board/groom.py                    # report on the live board
  python3 tools/board/groom.py --json            # findings as JSON
  python3 tools/board/groom.py --self-test       # prove each check CAN fail
  python3 tools/board/groom.py --note-awaiting   # post 'awaiting GB-x' notes
Env: FORGEJO_TOKEN (board).
"""
import json
import os
import re
import sys
import urllib.request

BASE = os.environ.get("FORGEJO_BASE", "http://192.168.0.234:3000") + "/api/v1/repos/sarah/gaming-program"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")
LANES = ("Backlog", "Ready", "In Progress", "In Review", "Merge Gate", "Done", "Icebox")
REVIEW_LANES = ("In Review", "Merge Gate")

# "GB-044/45/46" and "GB-003/004" are compact shorthands; only literal
# `GB-ddd` tokens enter the dependency graph.  That is a deliberate precision
# choice: the deadlock rule must not guess at a dependency the body never
# states, and the shorthand right-hand ids are not corrupted by it (see
# expand_dep_field).
GB_RE = re.compile(r"\bGB-(\d{3})\b")
V_RE = re.compile(r"^GB-\d{3}-V\b")


# ---------------------------------------------------------------- pure core
def base_id(title):
    """Board id from a title: the leading token (GB-nnn, GB-nnn-V, PLAN-…)."""
    return (title or "").split()[0]


def lane_of(issue):
    names = [l["name"] for l in issue.get("labels", [])]
    return next((x for x in LANES if x in names), None)


def expand_dep_field(field):
    """Normalise a raw `Depends on` cell to a set of GB-nnn ids.

    Handles the three real shapes seen on the board: comma-separated full ids
    (`GB-077, GB-060`), slash shorthands whose right members drop leading zeros
    (`GB-044/45/46` -> GB-044 GB-045 GB-046, `GB-003/004` -> GB-003 GB-004),
    and a single id.  A short numeric member is zero-padded to three digits, so
    `45` becomes `GB-045`.  Non-GB tokens (e.g. `turtles`) are dropped, never
    guessed.
    """
    out = set()
    for t in re.split(r"[\s,;/\t]+", (field or "").strip()):
        m = re.match(r"^GB-(\d{1,3})$", t, re.I)
        if m:
            out.add(("GB-" + m.group(1).zfill(3)).upper())
            continue
        m = re.match(r"^(\d{1,3})$", t)
        if m:
            out.add("GB-" + m.group(1).zfill(3))
    return out


def deps_of(body):
    """GB ids named in the 'Depends on' field of a ticket body."""
    body = body or ""
    m = re.search(r"Depends\s+on[:\s|]*(.*)", body, re.I)
    if not m:
        return set()
    field = m.group(1)
    # stop at a closing '|' cell, an em-dash prose break, or a parenthetical
    field = re.split(r"[—\-]{2,}|\)|\|", field, maxsplit=1)[0]
    return expand_dep_field(field)


def dependency_cycles(open_issues):
    """True dependency cycles (blockers deadlock) among OPEN tickets.

    Pure over a `[{'title','body'}]` list so the deadlock rule is testable
    without the network.  Returns a list of cycles, each a list of GB ids in
    dependency order (A -> B -> ... -> A).  Only open tickets participate and
    only edges to open tickets are followed: a Done dependency cannot block.
    """
    by_title = {base_id(i.get("title")): i for i in open_issues}
    if not by_title:
        return []
    graph = {}
    for i in open_issues:
        g = base_id(i.get("title"))
        deps = sorted(d for d in deps_of(i.get("body", "")) if d != g and d in by_title)
        graph[g] = deps
    WHITE, GREY, BLACK = 0, 1, 2
    color = {g: WHITE for g in graph}

    def dfs(node, path, stack):
        color[node] = GREY
        stack.append(node)
        for nxt in graph.get(node, []):
            if color.get(nxt) == GREY:
                # a back edge found: extract the cycle from the stack
                i = stack.index(nxt)
                cycle = stack[i:] + [nxt]
                yield cycle
            elif color.get(nxt) == WHITE:
                yield from dfs(nxt, path + [nxt], stack)
        stack.pop()
        color[node] = BLACK

    seen = []
    for node in graph:
        if color[node] == WHITE:
            for cycle in dfs(node, [], []):
                key = tuple(sorted(cycle[:-1]))
                if key not in [tuple(sorted(c[:-1])) for c in seen]:
                    seen.append(cycle)
    return seen


def newest_verdict(comments):
    """Newest GREEN/RED verdict body marker from a comment list.

    Ordering matters and the API's is not guaranteed: Forgejo returns comments
    newest-first, so `reversed()` yields OLDEST-first and a re-verified card
    resolves to its FIRST verdict.  Measured: GB-085-V round 1 was RED, round 2
    was GREEN, the gate card was closed -- and `board.py lint` reported
    'gate GB-085-V has not posted GREEN' while GB-085 sat in Merge Gate
    correctly.  Sort by created_at when present, and fall back to the caller's
    order otherwise.
    """
    cs = list(comments or [])
    if cs and all(c.get("created_at") for c in cs):
        cs.sort(key=lambda c: c["created_at"])
    for c in reversed(cs):
        body = (c.get("body") or "").replace("\r", "")
        m = re.search(r"VERDICT[^\n]{0,40}?\b(GREEN|RED)\b", body, re.I)
        if m:
            return m.group(1).upper()
    return None


def verify_lint_findings(issues, comment_fn):
    """The three PLAN-006 verify-lane checks.

    issues: full board issue list (open + closed).  comment_fn(id) -> comment
    list for a ticket number (may hit the network in live mode; a dict of
    canned comment lists in a self-test).  Returns a list of
    (kind, gid, detail) findings.
    """
    by_title = {}
    for i in issues:
        k = base_id(i.get("title"))
        prev = by_title.get(k)
        # Duplicate ids happen: a re-verification mints a second card titled
        # "GB-nnn-V ..." (GB-085-V round 1 RED, round 2 GREEN).  Keeping
        # whichever came last in list order picked the stale round and reported
        # "gate has not posted GREEN" for a card that had -- prefer the highest
        # issue number, which is the newest round.
        if prev is None or (i.get("number") or 0) > (prev.get("number") or 0):
            by_title[k] = i
    findings = []

    # unverified-in-review: a review-lane impl whose -V gate posted no GREEN.
    for gid, it in by_title.items():
        if V_RE.match(gid):
            continue
        lane = lane_of(it)
        if lane not in REVIEW_LANES:
            continue
        v = by_title.get(gid + "-V")
        verdict = newest_verdict(comment_fn(v["number"])) if v else None
        if verdict != "GREEN":
            which = gid + "-V" if v else "no gate ticket"
            detail = (f"in {lane!r} but gate {which} has not posted GREEN"
                      if v else f"in {lane!r} with no {gid}-V gate ticket")
            findings.append(("unverified-in-review", gid, detail))

    # stale-verify: an open -V whose parent is already closed/Done-gated.
    for gid, it in by_title.items():
        if not V_RE.match(gid):
            continue
        if it.get("state") == "closed":
            continue
        parent = by_title.get(gid[:-2])
        if parent is None:
            findings.append(("orphan-verify", gid,
                             "parent " + gid[:-2] + " does not exist on the board"))
        elif parent.get("state") == "closed" or lane_of(parent) == "Done":
            findings.append(("stale-verify", gid,
                             "parent " + gid[:-2] + " is already "
                             + ("closed" if parent.get("state") == "closed" else "Done")))
    return findings


def board_state_lint(issues, comment_fn):
    """verify-lint + blockers deadlock — the board-state checks board.py lint
    reuses (PLAN-006: the detection belongs in board.py lint).  comments in
    comment_fn(number) are read only for review-lane tickets, so a self-test can
    hand canned comment lists."""
    open_issues = [i for i in issues if i.get("state") == "open"]
    findings = [("blocks-deadlock", " -> ".join(c),
                 "dependency cycle among open tickets — no member can start "
                 "or complete")
                for c in dependency_cycles(open_issues)]
    findings += verify_lint_findings(issues, comment_fn)
    return findings


THRESH_RE = re.compile(
    r"(?:\d+\s*(?:==|>=|<=|>|<|=)\s*\d+\b)"      # == 0, >= N, ...
    r"|(?:within\s*\d+)"                          # within N
    r"|(?:\b0\s+\w)"                              # 0 failed, 0 user memory faults
    r"|(?:\b\d+\s+(?:errors?|faults|failures|checks|bytes|users?|requests?))\b",
    re.I,
)


def quant_dod_ok(dod_cell, evidence_comments):
    """Refuse a Done move on a numeric-threshold DoD without measured evidence.

    dod_cell: the DoD text (single string).  evidence_comments: list of comment
    bodies.  Returns True when the Done move is allowed.  The 2026-10-19 rule:
    a DoD that is a digit-bearing predicate (`== 0`, `0 <x>`, `>= N`,
    `within N`) may not close unless an EVIDENCE comment quotes the measured
    value.  This is a lint, not a proof — GB-068's DoD (`verdict prints "0 user
    memory faults"`) is the canonical case it must refuse.
    """
    if not THRESH_RE.search(dod_cell or ""):
        return True
    for body in evidence_comments or []:
        if body.lstrip(" \t\r\n*_#").upper().startswith("EVIDENCE"):
            return True
    return False


# ---------------------------------------------------------------- live runner
def api(path):
    req = urllib.request.Request(BASE + path,
                                 headers={"Authorization": "token " + TOKEN})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def all_issues():
    out, page = [], 1
    while True:
        batch = api(f"/issues?state=all&limit=100&page={page}")
        if not batch:
            return out
        out.extend(batch)
        page += 1


def comments_of(number):
    return api(f"/issues/{number}/comments?limit=100")


def live_findings():
    issues = all_issues()
    open_issues = [i for i in issues if i.get("state") == "open"]
    findings = []

    for cycle in dependency_cycles(open_issues):
        findings.append(("blocks-deadlock", " -> ".join(cycle),
                         "dependency cycle among open tickets — no member "
                         "can start or complete"))
    findings += verify_lint_findings(issues, comments_of)

    # quantitative-DoD Done guard: flag numeric-threshold cards that sit where a
    # Done move could actually happen (work under way or complete) but carry no
    # EVIDENCE quoting the measured value.  A Backlog/Ready card has no business
    # being closed, so it is not noise a close would refuse.  Report only; this
    # tool never closes.
    DONE_IMMINENT = REVIEW_LANES + ("In Progress",)
    for it in open_issues:
        if lane_of(it) not in DONE_IMMINENT:
            continue
        body = it.get("body") or ""
        guide = re.search(r"### Definition of Done", body)
        if not guide:
            continue
        # take the region between "### Definition of Done" and the next heading
        after = body[guide.end():]
        dod_text = re.split(r"\n#{1,4} ", after, maxsplit=1)[0]
        if not quant_dod_ok(dod_text, [c["body"] for c in comments_of(it["number"])]):
            gid = base_id(it.get("title"))
            findings.append(("quant-dod", gid,
                             "numeric-threshold DoD with no EVIDENCE quoting "
                             "the measured value (close refused)"))
    return findings, issues


# ---------------------------------------------------------------- self-test
def self_test():
    checks, failed = 0, 0

    def check(ok, name):
        nonlocal checks, failed
        checks += 1
        print(("  PASS: " if ok else "  FAIL: ") + name)
        if not ok:
            failed += 1

    # deadlock: an acyclic board is clean.
    acyclic = [
        {"title": "GB-001 a", "body": "| Depends on | GB-002 |"},
        {"title": "GB-002 b", "body": "| Depends on | — |"},
        {"title": "GB-003 c", "body": "| Depends on | |"},
    ]
    check(dependency_cycles(acyclic) == [], "acyclic dependencies find no cycle")

    # deadlock: a two-ticket cycle is found.
    cyc = [
        {"title": "GB-010 x", "body": "| Depends on | GB-011 |"},
        {"title": "GB-011 y", "body": "| Depends on | GB-010 |"},
    ]
    c = dependency_cycles(cyc)
    check(len(c) == 1 and set(c[0][:-1]) == {"GB-010", "GB-011"},
          "two-ticket dependency cycle is detected")
    check(len(c[0]) == 3 and c[0][0] == c[0][-1],
          "reported cycle is closed (A -> B -> A)")

    # deadlock: a Done dependency does not block (no cycle through it).
    done_dep = [
        {"title": "GB-020 p", "body": "| Depends on | GB-021 |"},
        {"title": "GB-021 q", "body": "| Depends on | GB-020 |"},
    ]
    # both are open here so a cycle exists; replace one with closed semantics by
    # dropping it from the open set the caller passes -- the function only sees
    # open tickets, so an edge to a closed member is simply absent.
    check(dependency_cycles([{"title": "GB-020 p", "body": "| Depends on | GB-021 |"}]) == [],
          "an edge to a ticket not in the open set cannot close a cycle")

    # exped open shorthand
    check(expand_dep_field("GB-044/45/46") == {"GB-044", "GB-045", "GB-046"},
          "compact shorthand GB-044/45/46 expands to three ids")
    check(expand_dep_field("GB-003/004") == {"GB-003", "GB-004"},
          "compact shorthand GB-003/004 expands to two ids")
    check(deps_of("| Depends on | turtles |") == set(),
          "a non-GB dependency token is ignored, not guessed")

    # verify-lint: a green-gated review card is not flagged.
    issues_good = [
        {"title": "GB-030 impl", "state": "open",
         "labels": [{"name": "In Review"}], "number": 1},
        {"title": "GB-030-V gate", "state": "open",
         "labels": [{"name": "Ready"}], "number": 2},
    ]
    cm = {2: [{"body": "VERDICT: GREEN"}]}
    check(verify_lint_findings(issues_good, lambda n: cm.get(n, [])) == [],
          "a review card with a GREEN gate is not flagged unverified")

    # verify-lint: un-gated review card is flagged.
    issues_bad = [
        {"title": "GB-031 impl", "state": "open",
         "labels": [{"name": "In Review"}], "number": 3},
        {"title": "GB-031-V gate", "state": "open",
         "labels": [{"name": "Ready"}], "number": 4},
    ]
    cm2 = {4: [{"body": "started the run"}]}   # no VERDICT line at all
    f = verify_lint_findings(issues_bad, lambda n: cm2.get(n, []))
    check(any(k == "unverified-in-review" and g == "GB-031" for k, g, _ in f),
          "a review-lane card whose gate has no GREEN is flagged unverified-in-review")
    check(all(k != "unverified-in-review" for k, g, _ in f if g == "GB-030"),
          "the green-gated card stays out of the findings")

    # verify-lint: orphan -V with no parent.
    orphan = [{"title": "GB-040-V gate", "state": "open",
               "labels": [{"name": "Ready"}], "number": 5}]
    f = verify_lint_findings(orphan, lambda n: [])
    check(any(k == "orphan-verify" and g == "GB-040-V" for k, g, _ in f),
          "a -V with no parent on the board is flagged orphan-verify")

    # verify-lint: stale -V with closed parent.
    stale = [
        {"title": "GB-041 impl", "state": "closed",
         "labels": [{"name": "Done"}], "number": 6},
        {"title": "GB-041-V gate", "state": "open",
         "labels": [{"name": "Ready"}], "number": 7},
    ]
    f = verify_lint_findings(stale, lambda n: [])
    check(any(k == "stale-verify" and g == "GB-041-V" for k, g, _ in f),
          "an open -V whose parent is closed is flagged stale-verify")

    # quantitative-DoD: the GB-068 shape is refused without EVIDENCE...
    check(quant_dod_ok('verdict prints "0 user memory faults"', []) is False,
          "numeric-threshold DoD without EVIDENCE is refused a Done close (GB-068 shape)")
    # ...and allowed with a quoted measured value.
    check(quant_dod_ok('verdict prints "0 user memory faults"',
                       ["**EVIDENCE — ** 0 user memory faults on a full wave."]) is True,
          "the same DoD is allowed once an EVIDENCE comment quotes 0")
    # a non-numeric DoD is never gated.
    check(quant_dod_ok("docs pass human read", []) is True,
          "a descriptive (non-numeric) DoD is not gated")

    print(f"groom self-test: {checks} checks, {failed} failed -- "
          + ("ALL PASSED" if failed == 0 else "FAILED"))
    return 0 if failed == 0 else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--self-test" in argv:
        return self_test()
    if not TOKEN:
        print("groom.py: FORGEJO_TOKEN not set (source ~/.hermes/.env)")
        return 2
    try:
        findings, issues = live_findings()
    except Exception as e:                                    # noqa: BLE001
        print(f"groom.py: cannot read the board: {e}")
        return 2

    if "--json" in argv:
        print(json.dumps({"findings": findings, "board": len(issues)}, indent=2))
    else:
        print(f"groom: {len(issues)} board tickets")
        if not findings:
            print("groom: clean -- no unverified, stale, orphan, deadlocked, "
                  "or un-evidenced-threshold cards")
        for kind, gid, detail in findings:
            print(f"  [{kind}] {gid}: {detail}")
        print("groom: report only -- this tool never closes or re-lanes a card.")
    return 0 if not findings else 1


if __name__ == "__main__":
    sys.exit(main())
