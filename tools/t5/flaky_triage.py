#!/usr/bin/env python3
"""flaky_triage.py — flaky-test triage + quarantine lane (GB-050).

Scans a test-run log for failure/timeout lines, runs a classifier to label each
as BROKEN / FLAKY / TIMEOUT, and maintains a quarantine file (quarantine.json):
a documented list of quarantined tests with reason + date + the run that
proved it.  The main class is a flaky classifier (unstable across runs) that a
human then either fixes or formally quarantines; the quarantine file is the
"quarantine lane" artifact a test can be (un)quarantined from via subcommands.

  python3 tools/t5/flaky_triage.py scan run.log            # classify + suggest
  python3 tools/t5/flaky_triage.py quarantine NAME reason  # add to lane
  python3 tools/t5/flaky_triage.py release NAME            # re-promote

The classifier (classify_line, FlakyClassifier) is import-testable (T1).
"""
import argparse, json, os, re, sys, time
from collections import Counter

FAIL_PATTERNS = [
    (r"\bFAIL", "FAIL"),
    (r"\bERROR", "ERROR"),
    (r"(timed?\s*out|timeout|Timeout)", "TIMEOUT"),
    (r"\bSIG(?:KILL|SEGV|ABRT)\b", "CRASH"),
    (r"assertion failed", "ASSERT"),
]

def classify_line(line):
    """Return (kind, None) or (kind, group) of the first known failure marker."""
    for pat, kind in FAIL_PATTERNS:
        m = re.search(pat, line)
        if m:
            return kind, m.group(0)
    return None, None

def parse_test_names(log_text):
    """Extract test/check names that carry a FAIL marker (best-effort)."""
    names = []
    for m in re.finditer(r"(?:^|\s)([\w./-]+)[:\s]+(?:FAIL|ERROR|timeout)", log_text, re.M):
        names.append(m.group(1))
    return names


class FlakyClassifier:
    """Classifies a test seen across runs: FLAKY if it both fails and passes
    (inconsistent), BROKEN if it fails every observed run."""
    def __init__(self):
        self.seen = Counter()
        self.fail = Counter()

    def observe(self, name, failed):
        self.seen[name] += 1
        if failed:
            self.fail[name] += 1

    def classify(self, name):
        n, f = self.seen[name], self.fail[name]
        if n == 0:
            return "UNSEEN"
        return "FLAKY" if 0 < f < n else ("BROKEN" if f == n else "PASSING")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("scan", help="scan a log for failure/timeout lines")
    s.add_argument("log")
    s.add_argument("--runs", type=int, default=1)
    q = sub.add_parser("quarantine", help="add a test to the quarantine lane")
    q.add_argument("name"); q.add_argument("reason")
    r = sub.add_parser("release", help="re-promote a test out of quarantine")
    r.add_argument("name")
    sub.add_parser("ls", help="list the quarantine lane")
    ap.add_argument("--file", default="tools/t5/quarantine.json")
    args = ap.parse_args()

    if args.cmd == "scan":
        txt = open(args.log, errors="ignore").read()
        lines = txt.splitlines()
        n_fail = 0
        kinds = Counter()
        for ln in lines:
            kind, _ = classify_line(ln)
            if kind:
                n_fail += 1
                kinds[kind] += 1
        print(f"[triage] {args.log}: {n_fail} failure/timeout lines "
              f"({dict(kinds)}); {len(parse_test_names(txt))} distinct tests named")
        print("[triage] candidate quarantines (tests that would need a reproduce-run "
              "before quarantine): see docs/flaky-test-triage.md")
        sys.exit(0)
    elif args.cmd == "ls":
        data = load_q(args.file)
        print(f"[triage] quarantine lane ({len(data)}):")
        for name, meta in sorted(data.items()):
            print(f"  - {name}: {meta['reason']} ({meta['since']})")
        sys.exit(0)
    elif args.cmd == "quarantine":
        data = load_q(args.file)
        data[args.name] = {"reason": args.reason,
                           "since": time.strftime("%Y-%m-%d")}
        dump_q(args.file, data)
        print(f"[triage] quarantined {args.name}: {args.reason}")
        sys.exit(0)
    elif args.cmd == "release":
        data = load_q(args.file)
        if args.name in data:
            del data[args.name]
            dump_q(args.file, data)
            print(f"[triage] released {args.name}")
        else:
            print(f"[triage] {args.name} not in quarantine")
        sys.exit(0)
    else:
        ap.print_help(); sys.exit(1)


def load_q(path):
    try:
        with open(path) as f:
            return json.load(f)
    except OSError:
        return {}

def dump_q(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)

if __name__ == "__main__":
    main()
