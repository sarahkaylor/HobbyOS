#!/usr/bin/env python3
"""serial_classifier.py - GB-107: a truncated T3/T4 serial log can be told apart from
a guest that merely stopped writing.

Every monitored run records, on one line, the four facts that disambiguate:
    (lines=, bytes=, alive-at-cutoff=, end-marker=)
and the resulting verdict is one of the three distinct outcomes:

    FULL          the guest printed its OWN end marker -> a complete capture
    TRUNCATED     no end marker BUT the guest was alive at cutoff -> the
                  capture was cut mid-stream (distinct from the guest stopping)
    GUEST_STOPPED no end marker and the guest was dead/absent at cutoff -> the
                  guest stopped writing (early death / never wrote)

`classify` is pure and T1-testable.  `capture_record` reads a serial file and the
caller's guest-alive/expected facts to build the record.  A broken classifier must
be caught: the test flips the TRUNCATED/GUEST_STOPPED branch and requires a fail.
"""
import os

FULL = "FULL"
TRUNCATED = "TRUNCATED"
GUEST_STOPPED = "GUEST_STOPPED"


def classify(lines, guest_alive_at_cutoff, end_marker, expected_lines=None):
    """(verdict, reason).  Deterministic: end-marker wins (FULL); otherwise the
    guest-alive-at-cutoff fact tells TRUNCATED from GUEST_STOPPED."""
    if end_marker:
        return FULL, f"{lines} lines, end marker seen: complete capture"
    if guest_alive_at_cutoff:
        return TRUNCATED, (f"{lines} lines (expected {expected_lines or '?'}), guest ALIVE at cutoff, "
                          f"no end marker: capture cut mid-stream")
    return GUEST_STOPPED, (f"{lines} lines, guest not alive at cutoff, no end marker: "
                           f"guest stopped writing")


def capture_record(serial_path, guest_alive_at_cutoff, expected_lines=None, end_marker_text="System halt"):
    """Read a serial file and build the one-line monitored-run record dict."""
    lines = 0
    end_marker = False
    try:
        with open(serial_path, "rb") as fh:
            raw = fh.read()
        lines = raw.count(b"\n") + (1 if raw and not raw.endswith(b"\n") else 0)
        if end_marker_text:
            end_marker = end_marker_text.encode() in raw
    except OSError:
        lines = 0
        end_marker = False
    verdict, reason = classify(lines, guest_alive_at_cutoff, end_marker, expected_lines)
    return {"lines": lines, "bytes": os.path.getsize(serial_path) if os.path.exists(serial_path) else 0,
            "guest_alive_at_cutoff": guest_alive_at_cutoff, "end_marker": end_marker,
            "expected_lines": expected_lines, "verdict": verdict, "reason": reason}


def record_line(rec):
    """The canonical one-line monitored-run record (what the DoD asks a run to emit)."""
    return (f"SERIAL-{rec['verdict']} lines={rec['lines']} bytes={rec['bytes']} "
            f"alive-at-cutoff={int(rec['guest_alive_at_cutoff'])} end-marker={int(rec['end_marker'])} "
            f"(expected {rec['expected_lines'] or '?'}) {rec['reason']}")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("serial")
    ap.add_argument("--alive", type=lambda s: s.lower() in ("1", "true", "yes"))
    ap.add_argument("--expected-lines", type=int)
    ap.add_argument("--end-marker", default="System halt")
    a = ap.parse_args()
    rec = capture_record(a.serial, a.alive, a.expected_lines, a.end_marker)
    print(record_line(rec))
    import sys
    sys.exit(0 if rec["verdict"] == FULL else (2 if rec["verdict"] == TRUNCATED else 3))
