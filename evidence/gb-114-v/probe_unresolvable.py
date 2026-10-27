#!/usr/bin/env python3
"""GB-114-V clause 6 probe: the distinct 'cannot resolve this PR to a board ticket'
line, demonstrated on the production (--merge) path.

No PR is merged and no network write happens: pr_gb_id is stubbed to None, which is
the branch under test, and the guard exits before any merge call.  head_of_pr is
stubbed so the run does not depend on an unresolvable PR number.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import merge_pr  # noqa: E402

merge_pr.head_of_pr = lambda pr, env=None: "0123456789abcdef0123456789abcdef01234567"
merge_pr.pr_gb_id = lambda pr, env=None: None
merge_pr.merge_pr_github = lambda *a, **k: (False, "merge_pr_github must not be reached")

sys.argv = ["merge_pr.py", "--pr", "9999", "--merge"]
try:
    merge_pr.main()
except SystemExit as e:
    print(f"[probe] exit status = {e.code}")