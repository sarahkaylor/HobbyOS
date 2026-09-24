#!/usr/bin/env python3
"""Generate src/user/sed_test_cases.h from GNU sed 4.8 golden outputs.

The oracle is the native GNU sed 4.8 built from the *same sources* this
repo vendors under src/user/sed (third_party/_staging/sed-4.8/sed/sed), so
the in-OS SEDTEST.BIN can demand byte-exact parity for stdout, exit status,
and any files a case rewrites (-i, w) or reads (r).

Every case is run through the oracle with LC_ALL=C in a scratch directory;
argv tokens must not contain whitespace (the HobbyOS argv path splits on
it), which the script asserts.  Output is embedded as C string literals
with octal escapes for anything non-printable.

Usage:
    python3 tools/gen_sed_tests.py [--oracle PATH] [--check]

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
    REPO, "third_party", "_staging", "sed-4.8", "sed", "sed")
OUT_PATH = os.path.join(REPO, "src", "user", "sed_test_cases.h")

# ---------------------------------------------------------------------------
# Case data.  Keys:
#   name    unique id, C identifier fragment
#   args    argv after argv[0]
#   files   dict of file name -> content to create before running
#   stdin   bytes piped to the program (b"" = none; "" means empty stdin)
#   check   (file name, expected content) OR file name — verified after the
#           run; a bare name means "unchanged from what files[] wrote"
#   note    free-form; ends up in the generated table
# ---------------------------------------------------------------------------
L1 = b"alpha\nbeta\ngamma\ndelta\n"          # canonical 4-line input
CASES = [
    dict(name="basic_s", args=["s/alpha/ALPHA/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="s_g", args=["s/a/4/g", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="s_2nd", args=["s/a/Z/2", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="s_p_n", args=["-n", "s/a/ZZ/p", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="amp", args=["s/[0-9]/<&>/", "SEDT.NUM"],
         files={"SEDT.NUM": b"x1 y22 z333\n"}),
    dict(name="backref",
         args=["s/\\(l\\)\\(ph\\)/\\2\\1/", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="empty_reuse", args=["-e", "s/a/1/", "-e", "s//2/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="case_I", args=["s/ALPHA/omega/I", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="addr_line_d", args=["2d", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="addr_range_d", args=["2,3d", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="addr_last_d", args=["$d", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="addr_not", args=["2!d", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="addr_re", args=["/eta/d", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="addr_re_range", args=["/alpha/,/gamma/d", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="print_all_n", args=["-n", "p", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="print_last_n", args=["-n", "$p", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="hold_hg", args=["-n", "1h;3{p;g;p}", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="hold_tac", args=["1!G;h;$!d", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="exchange_x", args=["1x;s/^/1:/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="y_cmd", args=["y/abcd/ABCD/", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="branch_t", args=["s/a/A/;tdone;s/z/Z/;:done", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="multiple_e",
         args=["-e", "s/alpha/1/", "-e", "s/beta/2/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="script_file_f", args=["-f", "SEDT.SCR", "SEDT.IN"],
         files={"SEDT.IN": L1,
                "SEDT.SCR": b"# comment\n2c\\\nSECOND LINE REPLACED\n"}),
    dict(name="append_a", args=["-f", "SEDT.A", "SEDT.IN"],
         files={"SEDT.IN": L1, "SEDT.A": b"1a\\\nAPPENDED\n"}),
    dict(name="insert_i", args=["-f", "SEDT.I", "SEDT.IN"],
         files={"SEDT.IN": L1, "SEDT.I": b"2i\\\nINSERTED\n"}),
    dict(name="quit_status", args=["-n", "2q5", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="quit_plain", args=["2q", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="brace_cond", args=["/beta/{s/b/B/}", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="nested_brace", args=["1,3{/a/d}", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="tab_escape", args=["s/\\t/>T</", "SEDT.TAB"],
         files={"SEDT.TAB": b"a\tb\tc\n"}),
    dict(name="space_hex", args=["s/\\x20/_/g", "SEDT.IN"],
         files={"SEDT.IN": b"one two three\n"}),
    dict(name="dollar_anchor", args=["s/a$/X/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="caret_anchor", args=["s/^/PRE-/" , "SEDT.IN"],
         files={"SEDT.IN": L1}),
    # w/r take a whitespace-separated filename, which the HobbyOS argv
    # splitter cannot express, so these ride in via -f script files.
    dict(name="write_w", args=["-n", "-f", "SEDT.W", "SEDT.IN"],
         files={"SEDT.IN": L1, "SEDT.W": b"1,2w SEDT.OUT\n"}),
    dict(name="read_r", args=["-f", "SEDT.R", "SEDT.IN"],
         files={"SEDT.IN": L1, "SEDT.EXT": b"EXTRA\n",
                "SEDT.R": b"1r SEDT.EXT\n"}),
    dict(name="inplace_i", args=["-i", "s/beta/BETA/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="stdin_mode", args=["s/a/@/g"], stdin=L1),
    dict(name="stdin_no_trailing", args=["s/x/y/"], stdin=b"axxb"),
    dict(name="empty_input", args=["s/a/b/"], stdin=b""),
    dict(name="crlf", args=["s/\\r$/<CR>/", "SEDT.CR"],
         files={"SEDT.CR": b"a\r\nb\r\n"}),
    dict(name="high_bytes", args=["s/caf./X/", "SEDT.HB"],
         files={"SEDT.HB": b"caf\xe9 done\n"}),
    dict(name="y_high_byte", args=["y/\\xe9/E/", "SEDT.HB"],
         files={"SEDT.HB": b"caf\xe9 done\n"}),
    dict(name="long_line", args=["s/x/X/g", "SEDT.LONG"],
         files={"SEDT.LONG": b"x" * 300 + b"END\n"}),
    dict(name="next_n", args=["n;s/gamma/GAMMA/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="N_join", args=["N;s/\\n/+/", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="D_cycle", args=["-n", "N;P;D", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="G_append", args=["G", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="list_l", args=["-n", "2l", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="line_eq", args=["-n", "2=", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="filename_F", args=["-n", "1F", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="zap_z", args=["z", "SEDT.IN"], files={"SEDT.IN": L1}),
    dict(name="repl_newline", args=["s/beta/B1\\nB2/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="two_files", args=["s/a/./", "SEDT.IN", "SEDT2.IN"],
         files={"SEDT.IN": L1, "SEDT2.IN": b"second file\n"}),
    dict(name="dashdash", args=["-e", "s/alpha/AA/", "--", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="bad_script", args=["s/broken", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="nonexistent_file", args=["s/a/b/", "SEDT.NOPE"]),
    dict(name="regex_extended", args=["-E", "s/(al)+(pha)/M/", "SEDT.IN"],
         files={"SEDT.IN": L1}),
    dict(name="word_boundary", args=["s/\\balpha\\b/AB/", "SEDT.IN"],
         files={"SEDT.IN": b"alpha betalpha\n"}),
    dict(name="backref_pattern", args=["s/\\(a.\\)\\1/Y/", "SEDT.BR"],
         files={"SEDT.BR": b"abab cdcd\n"}),
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
                            {"SEDT.OUT"} & set(os.listdir(td))):
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
    w.append(" * Generated by tools/gen_sed_tests.py — do not edit.")
    w.append(" *")
    w.append(" * Golden outputs captured from native GNU sed 4.8 (the same")
    w.append(" * sources this repo vendors) with LC_ALL=C.  Each case lists")
    w.append(" * argv (space-separated, whitespace-free tokens), optional")
    w.append(" * stdin, files to (re)create first, the expected stdout and")
    w.append(" * exit status, and post-run file expectations.  The files and")
    w.append(" * checks fields are NUL-separated name/data runs ("" = none).")
    w.append(" */")
    w.append("#ifndef HOBBYOS_SED_TEST_CASES_H")
    w.append("#define HOBBYOS_SED_TEST_CASES_H 1")
    w.append("")
    w.append("typedef struct {")
    w.append("  const char *name;")
    w.append("  const char *args;")
    w.append("  const char *stdin_data; /* bytes piped in; \"\" = none */")
    w.append("  const char *files;      /* name\\0data pair list, \"\" = none */")
    w.append("  const char *want_stdout;")
    w.append("  int want_status;")
    w.append("  const char *checks;     /* name\\0want\\0 repeated; \"\" = none */")
    w.append("} sed_case_t;")
    w.append("")
    w.append("static const sed_case_t sed_cases[] = {")

    def pairlist(pairs, empty_default='""'):
        """NUL-separated (name, data) run, emitted as adjacent literals with
        explicit \0 so C literal concatenation cannot fuse the entries."""
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
    w.append("#define SED_CASE_COUNT "
             "(sizeof sed_cases / sizeof sed_cases[0])")
    w.append("")
    w.append("#endif /* HOBBYOS_SED_TEST_CASES_H */")
    return "\n".join(w) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle", default=DEFAULT_ORACLE)
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed header is out of date")
    args = ap.parse_args()

    if not os.path.isfile(args.oracle):
        raise SystemExit(f"oracle not found: {args.oracle} "
                         "(build third_party/_staging/sed-4.8 first)")
    results = [run_case(args.oracle, c) for c in CASES]
    header = emit(results)
    if args.check:
        with open(OUT_PATH, "r", encoding="utf-8") as fh:
            if fh.read() != header:
                print("sed_test_cases.h is out of date", file=sys.stderr)
                return 1
        print("sed_test_cases.h is up to date (%d cases)" % len(results))
        return 0
    with open(OUT_PATH, "w", encoding="utf-8") as fh:
        fh.write(header)
    print("wrote %s (%d cases)" % (OUT_PATH, len(results)))
    for r in results:
        print("  %-20s status=%-3d stdout=%dB" %
              (r["name"], r["want_status"], len(r["want_stdout"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
