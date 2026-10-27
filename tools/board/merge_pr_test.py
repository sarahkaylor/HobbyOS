#!/usr/bin/env python3
"""merge_pr_test.py - GB-106: the fail-closed merge guard's rule, tested in isolation
(the DoD: refuse when the GREEN names no sha, and when it names a sha != the head;
today's merges, including #194, must go red under the guard)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from merge_pr import decide, graded_shas_in

fails = 0
def check(cond, msg):
    global fails
    print("  PASS: " + msg if cond else "  FAIL: " + msg)
    if not cond:
        fails += 1

h = "1111111111111111111111111111111111111111"
check(decide(h, h)[0], "graded == head -> allow")
check(not decide(h, "2222222222222222222222222222222222222222")[0], "graded != head -> refuse")
check(not decide(h, None)[0], "GREEN names no sha -> FAILS CLOSED (refuse)")
check(not decide(None, h)[0], "no head supplied -> refuse")
check(40 in [len(s) for s in graded_shas_in("VERDICT GREEN graded sha " + "c" * 40)], "parses a 40-hex graded sha from a VERDICT line")
# GB-110-V2 clause 5: a real verdict body names an ABBREVIATED prefix (7-40 hex)
check("deadbeefe0" in graded_shas_in("## VERDICT: GREEN\n**Tree that would merge:** deadbeefe0\n"),
      "reads an abbreviated (10-char-prefix) sha from a real GREEN body")
check(graded_shas_in("VERDICT GREEN") == [], "a VERDICT with no sha -> empty (fails closed upstream)")

# DoD self-test anchor: today's PR #194 (GB-103) was graded at 7de6dde.. but merged at
# e8c7dc8.. -- the guard must refuse it FOR THE MISMATCH (not for silence).
graded194 = "7de6dde4f3f29f53541b21fd1cd0a290168879a9"
head194 = "e8c7dc89bc0156157930e4a90ba54d1845c0a147"
ok, r = decide(head194, graded194)
check(not ok and graded194 != head194, f"today's #194 incident (graded 7de6dde.., merged e8c7dc8..) -> RED for the mismatch ({r[:40]})")

# GB-109: abbreviated graded shas compare as PREFIXES (>=7 chars); <7 refuses; and the
# vacuous --head flag call is inexpressible (main() has no --head option).
h = "1111111111111111111111111111111111111111"
check(decide(h, h[:7])[0], "graded 7-char prefix of head -> ALLOW")
check(not decide(h, "fffffff")[0], "graded 7-char non-prefix -> REFUSE")
check(not decide(h, h[:6])[0], "graded prefix <7 chars -> REFUSE")
import merge_pr
structural, behavioral = merge_pr.vacuous_head_inexpressible()
check(structural, "parser structurally has no --head option (re-adding it FAILS this probe)")
check(behavioral, "main() rejects a --head argument on the command line")

# GB-110: the graded sha comes from the NEWEST GREEN verdict on the board (Forgejo API);
# the local obj/gate/graded-<pr>.sha file is only a cache and MUST match; a forged file
# (one disagreeing with the board) cannot permit a merge.
import merge_pr as mp
board_sha = "abcdef1234567890abcdef1234567890abcdef12"
_orig_fb = mp.graded_sha_from_board
mp.graded_sha_from_board = lambda pr, env=None: board_sha          # board GREEN names a sha
os.makedirs(os.path.join(mp.TREE, "obj", "gate"), exist_ok=True)
cache = os.path.join(mp.TREE, "obj", "gate", "graded-9.sha")
open(cache, "w").write(board_sha)
check(mp.graded_sha_of(9) == board_sha, "graded sha read from the board-ish GREEN (cache matches)")
open(cache, "w").write("9" * 40)                                    # forged local cache
check(mp.graded_sha_of(9) is None, "a FORGED local cache disagreeing with the board -> FAIL CLOSED")
open(cache, "w").write("f" * 40)
mp.graded_sha_from_board = lambda pr, env=None: None                 # board has no GREEN / unreachable
check(mp.graded_sha_of(9) is None, "no GREEN on the board (or unreachable) -> FAIL CLOSED even with a local file")
os.remove(cache)
mp.graded_sha_from_board = _orig_fb
# GB-110-V clauses 3+4: graded_shas_in must parse the canonical real-green line and the
# sha for THIS head is selected (not blindly shas[0] in a multi-sha / two-GREEN verdict).
check(40 in [len(s) for s in mp.graded_shas_in("**Tree that would merge:** " + "a" * 40)],
      "parses a 40-hex sha on the canonical 'Tree that would merge:' line")
head = board_sha
other = "9999999999999999999999999999999999999999"
_o_head, _o_fj, _o_gb = mp.head_of_pr, mp._forgejo, mp.pr_gb_id
mp.head_of_pr = lambda pr, env=None: head
mp.pr_gb_id = lambda pr, env=None: "110"
def _fk(api, env=None):
    if "comments" in api:
        return [{"body": "## VERDICT: GREEN\n**Tree that would merge:** " + other + "\n**Tree that would merge:** " + head + "\n"}]
    return [{"number": 7, "title": "GB-110-V"}, {"number": 8, "title": "GB-110"}]
mp._forgejo = _fk
sel = mp.graded_sha_from_board(209)
check(sel == head, "selects the sha naming THIS head (not shas[0]) in a multi-sha GREEN (got %s)" % (sel[:8] if sel else None))
mp.head_of_pr, mp._forgejo, mp.pr_gb_id = _o_head, _o_fj, _o_gb

if fails:
    print("MERGE_PR_TEST: FAIL (%d)" % fails); sys.exit(1)
print("MERGE_PR_TEST: PASS - fail-closed guard rule + #194 goes red")
sys.exit(0)
