#!/usr/bin/env python3
"""tools/board/pr_sync_test.py — GB-237 split-aware PR -> card mapping test.

The mapping is the only thing that decides which card an open PR moves in
WORK POLICY §2.  The old timer regex

    re.findall(r"\\bGB-(\\d+)(?:-(V\\d*))?\\b", title)

collapsed `GB-146-A` to `GB-146`; PR #410 (the GB-146-A PR) therefore moved the
umbrella #345 instead of the split card #563.  This test pins the five required
mappings, the no-id negative, and the falsifier: if the old collapse ever comes
back, the test fails.

Evidence markers: 'PR_SYNC_TEST: PASS — 5 mappings + no-id negative'
                  'PR_SYNC_CLI: PASS — --apply and --digest-only accepted'
"""
import contextlib
import io
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import pr_sync  # noqa: E402

fails = 0


def ok(cond, name):
    global fails
    print(("  PASS: " if cond else "  FAIL: ") + name)
    if not cond:
        fails += 1


CASES = [
    ("GB-146-A-V2: [BADVEC] 8-byte watch on &tf->vector", "GB-146-A"),
    ("GB-146-A: [BADVEC] — name the writer of the frame", "GB-146-A"),
    ("GB-146-B-LOCKDIAG: the wedge's lock owner", "GB-146-B"),
    ("GB-047-V2: Verify GB-047 at PR #312", "GB-047-V2"),
    ("GB-146: the full 63-program wave crashes the kernel", "GB-146"),
]

for title, want in CASES:
    got = pr_sync.ticket_of(title)
    ok(got == want, "%s -> %s (got %s)" % (title.split(":")[0], want, got))

# The no-id negative: nothing to map, so nothing can be moved.
ok(pr_sync.ticket_of("docs: clarify the build instructions") is None,
   "no GB id -> None (nothing is moved)")
ok(pr_sync.ticket_of("") is None, "empty title -> None")

# Falsifier: the old regex's output is reproduced here on the same title, and
# the new mapping must differ from it.  If this assertion ever becomes false,
# the split-aware rule has regressed to the bug GB-237 was minted for.
OLD_RE = re.compile(r"\bGB-(\d+)(?:-(V\d*))?\b")
old = OLD_RE.findall("GB-146-A: [BADVEC] — name the writer of the frame")
ok(old and "GB-" + old[0][0] == "GB-146",
   "old regex still shows the defect (GB-146-A -> GB-146)")
ok(pr_sync.ticket_of("GB-146-A: [BADVEC] — name the writer of the frame") != "GB-146",
   "falsifier: new mapping does NOT collapse GB-146-A to GB-146")

# The split letter must be a whole token: GB-1460 is a different card, and a
# tag name must not be read as a split (GB-146-B-LOCKDIAG is B, not L).
ok(pr_sync.ticket_of("GB-1460: unrelated card") == "GB-1460",
   "GB-1460 is not read as GB-146")
ok(pr_sync.ticket_of("GB-146-LOCKDIAG: report") == "GB-146",
   "a non-split tag (GB-146-LOCKDIAG) is not read as split L")

# CLI wiring (GB-237 bounce).  The deployed timer runs
#   ExecStart=... /opt/swarm-lease/pr_sync.py --apply
# and the first head accepted only --digest-only, so `--apply` exited 2 with
# "unrecognized arguments: --apply" and the versioned copy could not replace
# the stopgap.  These call main() with sync() stubbed: the CLI is under test,
# not the network, so no token and no board/GitHub call is needed.
_orig_sync = pr_sync.sync
_orig_tokens = (pr_sync.TOKEN, pr_sync.GH_TOKEN)
seen = []


def _fake_sync(digest_only=False):
    seen.append(digest_only)
    return 0


pr_sync.sync = _fake_sync
pr_sync.TOKEN = pr_sync.GH_TOKEN = "test-token"


def _main(argv):
    """main(), turning argparse's exit into a value so a FAIL prints cleanly."""
    try:
        return pr_sync.main(argv)
    except SystemExit as e:
        return "SystemExit(%r)" % (e.code,)


try:
    rc = _main(["--apply"])
    ok(rc == 0 and seen == [False],
       "--apply accepted; apply is the default (rc=%r, digest_only=%r)" % (rc, seen))
    rc = _main(["--digest-only"])
    ok(rc == 0 and seen == [False, True],
       "--digest-only accepted and sets digest_only (rc=%r, digest_only=%r)" % (rc, seen))
    rc = _main([])
    ok(rc == 0 and seen == [False, True, False],
       "no flag is still apply (rc=%r)" % (rc,))
finally:
    pr_sync.sync = _orig_sync
    pr_sync.TOKEN, pr_sync.GH_TOKEN = _orig_tokens

# Negative: an unknown flag still exits 2, so a typo cannot silently apply.
# argparse prints its usage to stderr; that is the expected output here, so it
# is swallowed to keep the test's evidence block readable.
try:
    with contextlib.redirect_stderr(io.StringIO()):
        pr_sync.main(["--not-a-flag"])
except SystemExit as e:
    ok(e.code == 2, "unknown flag exits 2 (got %r)" % (e.code,))
else:
    ok(False, "unknown flag exits 2 (it did not exit)")

print("pr_sync_test: " + ("ALL PASSED" if fails == 0 else "FAILED"))
print("PR_SYNC_TEST: PASS — 5 mappings + no-id negative" if fails == 0
      else "PR_SYNC_TEST: FAILED")
print("PR_SYNC_CLI: PASS — --apply and --digest-only accepted" if fails == 0
      else "PR_SYNC_CLI: FAILED")
sys.exit(0 if fails == 0 else 1)
