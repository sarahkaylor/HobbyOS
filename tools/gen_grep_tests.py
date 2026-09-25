#!/usr/bin/env python3
"""Generate src/user/grep_test_cases.h from GNU grep 2.5.4 golden outputs.

The oracle is the native GNU grep 2.5.4 built from the *same sources* this
repo vendors under src/user/grep (third_party/_staging/grep-2.5.4/src/grep),
so the in-OS GREPTEST.BIN can demand byte-exact parity for stdout, exit
status, and any file the case rewrites (grep itself only reads).

Every case is run through the oracle with LC_ALL=C in a scratch directory;
argv tokens must not contain whitespace (the HobbyOS argv path splits on
it), which the script asserts.  Output is embedded as C string literals
with octal escapes for anything non-printable.

Usage:
    python3 tools/gen_grep_tests.py [--oracle PATH] [--check]

--check re-runs the generator and fails if the committed header differs.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ORACLE = os.path.join(
    REPO, "third_party", "_staging", "grep-2.5.4", "src", "grep")
OUT_PATH = os.path.join(REPO, "src", "user", "grep_test_cases.h")

# ---------------------------------------------------------------------------
# Case data.  Keys:
#   name    unique id, C identifier fragment
#   args    argv after argv[0]
#   files   dict of file name -> content to create before running
#   stdin   bytes piped to the program (b"" = none; "" means empty stdin)
#   note    free-form; ends up in the generated table
# The post-run file expectations are collected automatically (the fixtures,
# which grep only reads).
# ---------------------------------------------------------------------------
TXT = b"alpha\nbeta\ngamma\ndelta\n"          # canonical 4-line input
CASES = [
    dict(name="basic_match", args=["beta", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="no_match", args=["zzz", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="count_c", args=["-c", "a", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="count_c_neg", args=["-c", "zzz", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="invert_v", args=["-v", "eta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="ignore_case", args=["-i", "ALPHA", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="word_w_miss", args=["-w", "alph", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="word_w_hit", args=["-w", "alpha", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="line_x", args=["-x", "beta", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="line_x_miss", args=["-x", "bet", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="fixed_F_miss", args=["-F", "b.ta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="fixed_F_hit", args=["-F", "eta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="fixed_F_dot", args=["-F", "a.c", "GRT.FD"],
         files={"GRT.FD": b"abc\na.c\n"}),
    dict(name="extended_E", args=["-E", "(al)+(pha)", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="extended_E_alt", args=["-E", "bet|gam", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="ERE_class", args=["-E", "b[ae]t+", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="ERE_interval", args=["-E", "a{1,2}lpha", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="basic_G", args=["-G", "be.a", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="bre_backref", args=["\\(ab\\)\\1", "GRT.BR"],
         files={"GRT.BR": b"abab cdcd\n"}),
    dict(name="bre_alt", args=["alpha\\|gamma", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="bre_word_b", args=["\\balpha\\b", "GRT.WB"],
         files={"GRT.WB": b"alpha betalpha\n"}),
    dict(name="bre_plus", args=["[0-9]\\+", "GRT.NUM"],
         files={"GRT.NUM": b"x1 y22 z333\n"}),
    dict(name="anchors_caret", args=["^alpha", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="anchors_dollar", args=["eta$", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="anchors_both_miss", args=["^beta$", "GRT.FN"],
         files={"GRT.FN": b"beta is here\n"}),
    dict(name="anchors_both_hit", args=["^beta$", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="dot_match", args=["g.m.a", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="star_match", args=["ga*mm", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="bracket", args=["[ab]eta", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="bracket_neg", args=["[^a]elta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="bracket_range", args=["[a-c]a", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="context_A", args=["-A", "1", "beta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="context_B", args=["-B", "1", "gamma", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="context_C", args=["-C", "1", "beta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="context_num_both", args=["-1", "beta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="context_num_before", args=["-2", "gamma", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="byte_offset", args=["-b", "beta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="line_number", args=["-n", "gamma", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="only_matching", args=["-o", "[a-z]*a", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="only_matching_a", args=["-o", "a", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="files_with_matches", args=["-l", "beta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="no_filename", args=["-h", "beta", "GRT.IN", "GRT2.IN"],
         files={"GRT.IN": TXT, "GRT2.IN": b"beta second\n"}),
    dict(name="with_filename", args=["-H", "beta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="multi_files", args=["beta", "GRT.IN", "GRT2.IN"],
         files={"GRT.IN": TXT, "GRT2.IN": b"beta second\n"}),
    dict(name="stdin_match", args=["beta"], stdin=TXT),
    dict(name="stdin_count", args=["-c", "a"], stdin=TXT),
    dict(name="stdin_empty", args=["a"], stdin=b""),
    dict(name="stdin_no_trailing_nl", args=["beta"], stdin=b"alpha\nbeta"),
    dict(name="stdin_stdin_dash", args=["-c", "a", "-"], stdin=TXT),
    dict(name="empty_match_via_star", args=["a*", "GRT.IN"],
         files={"GRT.IN": b"ax\nb\n\n"}),
    dict(name="pattern_file", args=["-f", "GRT.PAT", "GRT.IN"],
         files={"GRT.IN": TXT, "GRT.PAT": b"beta\ngamma\n"}),
    dict(name="pattern_file_F", args=["-F", "-f", "GRT.PAT", "GRT.IN"],
         files={"GRT.IN": TXT, "GRT.PAT": b"beta\ngamma\n"}),
    dict(name="multiple_e", args=["-e", "alpha", "-e", "delta", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="dashdash", args=["beta", "--", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="missing_file", args=["beta", "GRT.NOPE"]),
    dict(name="bad_regex", args=["-E", "(", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="quiet_q", args=["-q", "beta", "GRT.IN"], files={"GRT.IN": TXT}),
    dict(name="quiet_q_miss", args=["-q", "zzz", "GRT.IN"],
         files={"GRT.IN": TXT}),
    dict(name="silent_s", args=["-s", "beta", "GRT.NOPE"]),
    dict(name="high_bytes", args=["caf.", "GRT.HB"],
         files={"GRT.HB": b"caf\xe9 done\n"}),
    dict(name="high_bytes_F", args=["-F", "caf\xe9", "GRT.HB"],
         files={"GRT.HB": b"caf\xe9 done\n"}),
    # A literal TAB in argv would be split by the HobbyOS argv path, so the
    # tab pattern rides in through a pattern file.
    dict(name="tab_via_file", args=["-f", "GRT.TPAT", "GRT.TAB"],
         files={"GRT.TPAT": b"\t\n", "GRT.TAB": b"a\tb\nc\n"}),
    dict(name="crlf_count", args=["-c", "a", "GRT.CR"],
         files={"GRT.CR": b"a\r\nb\r\n"}),
    dict(name="long_line", args=["-c", "x", "GRT.LNG"],
         files={"GRT.LNG": b"x" * 100 + b"\n"}),
    dict(name="long_line_o", args=["-o", "x", "GRT.LNG"],
         files={"GRT.LNG": b"x" * 100 + b"\n"}),
]


def c_literal(data: bytes) -> str:
    """Byte-exact C string literal: printable ASCII as-is, else \\NNN."""
    out = ['"']
    for b in data:
        if b in (0x22, 0x5C):                      # " and backslash
            out.append("\\" + chr(b))
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x09:
            out.append("\\t")
        elif b == 0x0D:
            out.append("\\r")
        else:
            out.append("\\%03o" % b)
    out.append('"')
    return "".join(out)


def run_case(oracle: str, case: dict) -> dict:
    for a in case["args"]:
        if any(c in a for c in " \t\r\n"):
            raise SystemExit(
                f"case {case['name']}: argv token contains whitespace: {a!r} "
                "(the HobbyOS argv path splits on it)")
    with tempfile.TemporaryDirectory() as td:
        for fname, data in case.get("files", {}).items():
            with open(os.path.join(td, fname), "wb") as fh:
                fh.write(data)
        env = dict(os.environ, LC_ALL="C")
        proc = subprocess.run(
            [oracle] + case["args"], cwd=td, env=env,
            input=case.get("stdin", b""),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if b"\0" in proc.stdout:
            raise SystemExit(f"case {case['name']}: oracle stdout has NUL")
        res = dict(case)
        res["want_stdout"] = proc.stdout
        res["want_status"] = proc.returncode
        # post-run file verification (auto: everything it wrote/rewrote)
        checks = []
        for fname in sorted(set(case.get("files", {})) |
                            {"GRT.OUT"} & set(os.listdir(td))):
            path = os.path.join(td, fname)
            if os.path.isfile(path):
                with open(path, "rb") as fh:
                    checks.append((fname, fh.read()))
        res["checks"] = checks
        res["stderr"] = proc.stderr[:200]
        return res


def emit(results: list[dict]) -> str:
    w = []
    w.append("/*")
    w.append(" * Generated by tools/gen_grep_tests.py — do not edit.")
    w.append(" *")
    w.append(" * Golden outputs captured from native GNU grep 2.5.4 (the same")
    w.append(" * sources this repo vendors) with LC_ALL=C.  Each case lists")
    w.append(" * argv (space-separated, whitespace-free tokens), optional")
    w.append(" * stdin, files to (re)create first, the expected stdout and")
    w.append(" * exit status, and post-run file expectations.  The files and")
    w.append(" * checks fields are NUL-separated name/data runs (\"\" = none).")
    w.append(" */")
    w.append("#ifndef HOBBYOS_GREP_TEST_CASES_H")
    w.append("#define HOBBYOS_GREP_TEST_CASES_H 1")
    w.append("")
    w.append("typedef struct {")
    w.append("  const char *name;")
    w.append("  const char *args;")
    w.append("  const char *stdin_data; /* bytes piped in; \\\"\\\" = none */")
    w.append("  const char *files;      /* name\\0data pair list, \\\"\\\" = none */")
    w.append("  const char *want_stdout;")
    w.append("  int want_status;")
    w.append("  const char *checks;     /* name\\0want\\0 repeated; \\\"\\\" = none */")
    w.append("} grep_case_t;")
    w.append("")
    w.append("static const grep_case_t grep_cases[] = {")

    def pairlist(pairs, empty_default='""'):
        """NUL-separated (name, data) run, emitted as adjacent literals with
        explicit \\0 so C literal concatenation cannot fuse the entries."""
        if not pairs:
            return empty_default
        parts = []
        for name, data in pairs:
            parts.append('%s "\\0" %s "\\0"'
                         % (c_literal(name.encode()), c_literal(data)))
        return "\n    ".join(parts)

    for r in results:
        stdin_lit = c_literal(r.get("stdin", b""))
        w.append("  {")
        w.append(f"    {c_literal(r['name'].encode())},")
        w.append(f"    {c_literal(' '.join(r['args']).encode())},")
        w.append(f"    {stdin_lit},")
        w.append(f"    {pairlist(sorted(r.get('files', {}).items()))},")
        w.append(f"    {c_literal(r['want_stdout'])},")
        w.append(f"    {r['want_status']},")
        checks_lit = pairlist(r["checks"]) if r["checks"] else c_literal(b"")
        w.append(f"    {checks_lit},")
        w.append("  },")
    w.append("};")
    w.append("")
    w.append("#define GREP_CASE_COUNT "
             "(sizeof grep_cases / sizeof grep_cases[0])")
    w.append("")
    w.append("#endif /* HOBBYOS_GREP_TEST_CASES_H */")
    return "\n".join(w) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle", default=DEFAULT_ORACLE)
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed header is out of date")
    args = ap.parse_args()

    if not os.path.isfile(args.oracle):
        raise SystemExit(f"oracle not found: {args.oracle} "
                         "(build third_party/_staging/grep-2.5.4 first)")
    results = [run_case(args.oracle, c) for c in CASES]
    header = emit(results)
    if args.check:
        with open(OUT_PATH, "r", encoding="utf-8") as fh:
            if fh.read() != header:
                print("grep_test_cases.h is out of date", file=sys.stderr)
                return 1
        print("grep_test_cases.h is up to date (%d cases)" % len(results))
        return 0
    with open(OUT_PATH, "w", encoding="utf-8") as fh:
        fh.write(header)
    print("wrote %s (%d cases)" % (OUT_PATH, len(results)))
    for r in results:
        print("  %-24s status=%-3d stdout=%dB" %
              (r["name"], r["want_status"], len(r["want_stdout"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
