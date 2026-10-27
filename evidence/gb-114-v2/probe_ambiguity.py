#!/usr/bin/env python3
"""GB-114-V2 clause 3: ambiguity still refuses, and #222's real shape still resolves."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "board"))
import merge_pr  # noqa: E402

for name, args in [
    ("branch gb112 + title GB-113 (ambiguous)", ("games/gb112", "GB-113 the wait word", "")),
    ("branch gb112 + title GB-112 (agree)", ("games/gb112", "board: GB-112 fixed", "")),
    ("PR #222's real shape (ticket/NN + title)", ("ticket/222-waitstatus", "abi(t3): GB-113 — quote the raw wait status word", "")),
    ("no id anywhere", ("fix/typo", "spelling in README", "")),
]:
    print(f"  {str(merge_pr.ticket_from(*args)):40} <- {name}")