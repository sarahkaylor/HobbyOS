#!/bin/bash
# nl_parity.sh — byte-exact acceptance test for the HobbyOS ported nl.
#
# Runs our host-built nl (nl_host, argv[0] forced to "nl" via exec -a so
# error messages are comparable) against a GNU nl reference on the same
# inputs and compares stdout bytes, exit codes and stderr text across
# -b/-h/-f numbering styles (t / a / n / p REGEX), -i, -l, -v, -w,
# -n ln/rn/rz, -s, -d two-character section delimiters, logical-page
# sections, blank-line joining, stdin mode, multi-file reads, files with
# no final newline / CR / control bytes and error paths.
#
# Reference selection:
#   $2 = explicit path, else $NL_REF, else the first of these that exists:
#        /usr/bin/nl — tests the port against a modern nl; the
#        --help/--version text legitimately differs (2.1 vs coreutils) and
#        is not compared (help/version are also no-ops in the port, whose
#        GETOPT_HELP_OPTION_DECL carries braces like cut_gnu.c/cut.c's
#        sys2.h-derived macros; the 2.1 reference prints its help text).
#        The strictest bar is `make nl_parity_strict`, which builds
#        textutils-2.1's ORIGINAL nl (see build_tu21_nl_ref.sh) as the
#        reference; then stdout, exit codes AND stderr text compare
#        byte-for-byte.
#
# Two documented 2.1-vs-modern differences are gated:
#   * cmp_case_21 cases run only when the reference IS 2.1: the main one
#     is that 2.1 prints its unnumbered-line field with
#     puts (print_no_line_fmt), i.e. it emits a newline after the field,
#     while modern coreutils (>= 4.5) uses fputs.  Other entries there
#     cover option values 2.1 rejects but modern nl accepts (-i 0, -l 0,
#     -v past INT_MAX), numbering differences across logical pages, and
#     reads where modern nl stops at the first file it cannot open while
#     2.1 keeps going.  The port follows its 2.1 lineage in all of these.
#   * one loose-only case: with an invalid -b p REGEX the diagnostic text
#     comes from the regex engine, not from nl (2.1's engine prints
#     "Unmatched [ or [^", the glibc-lineage engine prints "Invalid
#     regular expression"); stdout and exit code still compare.
set -u

NL_HOST="${1:-./obj/nl_host}"
REF="${3:-${2:-${NL_REF:-$(command -v nl || echo /usr/bin/nl)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0
SKIPPED_21=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs a modern nl (loose:
# diagnostics were rewritten in 2003-era coreutils and several option
# values/degenerate inputs were relaxed, so those cases are skipped and
# error paths compare only whether the run succeeded).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# Fixtures
printf 'a\n\nb\n\n\nc\n' > "$TMP/small.txt"
printf 'p\nq\nr\ns\nt\n' > "$TMP/allnl.txt"
printf '  lead\n\t\ntrail  \n\n   x   \n' > "$TMP/mixed.txt"
printf '\n\n' > "$TMP/blank2.txt"
printf 'one\ntwo\nthree-no-final-newline' > "$TMP/noeol.txt"
printf 'a\r\nb\r\n\r\nc\r\n' > "$TMP/cr.txt"
printf '\\:\\:\\:\nh1\nh2\n\\:\\:\nb1\n\nb3\n\\:\nf1\n' > "$TMP/sec.txt"
printf '\\:\\:\\:\nh1\nh2\n\\:\\:\nb1\n\nb3\n\\:\nf1\n\\:\\:\\:\nh3\n\\:\\:\nb4\n\\:\nf5\n' > "$TMP/sec2.txt"
printf 'xyxyxy\nh1\nh2\nxyxy\nb1\n\nb3\nxy\nf1\n' > "$TMP/secxy.txt"
printf 'alpha\nbeta\ngamma1\n\n1234\ndelta\n' > "$TMP/re.txt"
# chosen so that a pattern applied with its leading 'p' still attached
# (i.e. a build_type_arg that forgot optarg++ like 2.1 does) numbers a
# different line set than the correct pattern does
printf 'pp\nxx\naa\np2\n1z\naa2\n' > "$TMP/re2.txt"
printf '' > "$TMP/empty.txt"
python3 - <<'PY' > "$TMP/big.txt"
for i in range(300):
    print("line %d" % i if i % 4 else "")
PY
python3 - <<'PY' > "$TMP/ctl.txt"
import sys
sys.stdout.buffer.write(b"ab\x00cd\nef\x00\n\n\x7f ctl\x01\n")
PY

# Run a case for one implementation.  $1 = args string (word-split, with
# __SP__ / __EMPTY__ / __BSL__ tokens standing in for a quoted " ", ""
# and a backslash argument); $2 = input spec: "-" (stdin, no file
# operand), "file:-" (stdin via a "-" operand), "file:<name>" (one input
# file) or "files:<n1,n2,...>" (several input files); $3 = hb | ref.
run_one() {
    local args_str="$1" spec="$2" impl="$3"
    local args=() operands=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
        args[i]="${args[i]//__BSL__/\\}"
    done
    case "$spec" in
        "-") ;;
        "file:-") operands=("-") ;;
        "file:"*) operands=("$TMP/${spec#file:}") ;;
        "files:"*)
            local fl=()
            IFS=',' read -r -a fl <<< "${spec#files:}"
            local f
            for f in "${fl[@]}"; do operands+=("$TMP/$f"); done
            ;;
    esac
    local cmd
    if [ "$impl" = hb ]; then
        cmd=(bash -c 'exec -a nl "$0" "$@"' "$NL_HOST")
    else
        cmd=(bash -c 'exec -a nl "$0" "$@"' "$REF")
    fi
    if [ "$spec" = "-" ]; then
        "${cmd[@]}" "${args[@]}" <"$TMP/small.txt" >"$TMP/out" 2>"$TMP/err"
    else
        "${cmd[@]}" "${args[@]}" "${operands[@]}" </dev/null >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" > "$TMP/rc"
}

cmp_two() {  # $1 = args string; $2 = input spec
    local args_str="$1" spec="$2"
    N=$((N+1))
    run_one "$args_str" "$spec" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$spec" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: nl $args_str ($spec)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: nl $args_str ($spec)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && [ "$rc_hb" != "0" ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): nl $args_str ($spec)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# Cases valid against both the 2.1 original and modern nl.
cmp_case() { cmp_two "$1" "$2"; }

# Cases where textutils-2.1 legitimately differs from modern nl (see the
# header): compare only when the reference IS the 2.1 build, otherwise
# count them and print one consolidated note at the end.
cmp_case_21() {
    if [ "$REF_MODE" = strict ]; then
        cmp_two "$1" "$2"
    else
        SKIPPED_21=$((SKIPPED_21+1))
    fi
}

# --- default numbering and -b styles: t (default), a, n ---
cmp_case "-bt" "file:allnl.txt"
cmp_case "--body-numbering=t" "file:allnl.txt"
cmp_case "-ba" "file:allnl.txt"
cmp_case "--body-numbering=a" "file:allnl.txt"
cmp_case "-ba" "file:small.txt"
cmp_case "-ba" "file:mixed.txt"
cmp_case "-ba" "file:blank2.txt"
cmp_case "-ba" "file:noeol.txt"
cmp_case "-ba" "file:cr.txt"
cmp_case "-bt" "file:empty.txt"
cmp_case "-ba" "file:big.txt"
cmp_case "-ba" "file:ctl.txt"
cmp_case_21 "" "file:small.txt"
cmp_case_21 "" "file:mixed.txt"
cmp_case_21 "" "file:blank2.txt"
cmp_case_21 "" "file:noeol.txt"
cmp_case_21 "" "file:cr.txt"
cmp_case_21 "-bn" "file:small.txt"
cmp_case_21 "--body-numbering=n" "file:allnl.txt"
cmp_case_21 "-bpz" "file:re.txt"

# --- -b p REGEX (BRE via the sysroot/glibc-lineage engine) ---
cmp_case_21 "-bp^a" "file:re.txt"
cmp_case_21 "-b p[0-9]" "file:re.txt"
cmp_case_21 "-b p[a-c]" "file:re.txt"
cmp_case_21 "-b p..." "file:small.txt"
cmp_case_21 "-b p$" "file:allnl.txt"
# The next block pins the pattern plumbing hard: each case would number a
# different line set if the 'p' style character were left on the front of
# the pattern (2.1 advances optarg past it), and the \{2\} pair pins the
# inherited syntax (neither implementation calls re_set_syntax, so the
# engine default applies — with file re2.txt, "x\{2\}" is a LITERAL 'x'
# followed by literal "{2}" and must number nothing; a port that switched
# to POSIX-basic syntax would number line "xx").
cmp_case_21 "-b p[0-9]" "file:re2.txt"
cmp_case_21 "-b p__BSL__(.__BSL__)__BSL__1" "file:re2.txt"
cmp_case_21 "-b pa__BSL__|1" "file:re2.txt"
cmp_case_21 "-b px__BSL__{2__BSL__}" "file:re2.txt"
cmp_case_21 "-h p^h" "file:sec.txt"
cmp_case "-b pa__BSL__" "file:small.txt"       # trailing backslash: engine error
# invalid regex: loose-only (engine diagnostics differ between 2.1's
# engine and glibc/sysroot's; see the header comment)
if [ "$REF_MODE" = strict ]; then
    echo "note: skipping 1 invalid-regex case in strict mode (2.1 engine: 'Unmatched [ or [^', glibc-lineage engine: 'Invalid regular expression'; stdout and exit code still agree)"
else
    cmp_case "-b p[" "file:small.txt"
fi

# --- -i increment (including 0 and out-of-range starts) ---
cmp_case "-i2 -ba" "file:small.txt"
cmp_case "-i10 -ba" "file:allnl.txt"
cmp_case "-i 7 -ba" "file:allnl.txt"
cmp_case_21 "-i0 -ba" "file:small.txt"         # 2.1 rejects, modern accepts

# --- -v starting line number ---
cmp_case "-v10 -ba" "file:allnl.txt"
cmp_case "-v0 -ba" "file:allnl.txt"
cmp_case "-v-5 -ba" "file:allnl.txt"
cmp_case_21 "-v2147483648 -ba" "file:allnl.txt" # past INT_MAX: 2.1 rejects

# --- -w widths ---
cmp_case "-w3 -ba" "file:allnl.txt"
cmp_case "-w1 -ba" "file:allnl.txt"
cmp_case "-w12 -ba" "file:allnl.txt"
cmp_case "-w3 -ba" "file:small.txt"

# --- -n number formats ---
cmp_case "-nln -ba" "file:allnl.txt"
cmp_case "-nrn -ba" "file:allnl.txt"
cmp_case "-nrz -ba" "file:allnl.txt"
cmp_case "-nrz -w3 -ba" "file:allnl.txt"
cmp_case "-nln -w1 -ba" "file:allnl.txt"
cmp_case "--number-width=3 --number-format=rz -ba" "file:allnl.txt"

# --- -s separator (one char, empty, two chars, long option) ---
cmp_case "-s: -ba" "file:small.txt"
cmp_case "-s __SP__ -ba" "file:allnl.txt"
cmp_case "-s __EMPTY__ -ba" "file:allnl.txt"
cmp_case "-s XY -ba -w2" "file:allnl.txt"
cmp_case "--number-separator=| -ba" "file:big.txt"

# --- everything at once ---
cmp_case "-i3 -v10 -w3 -s: -nrz -ba" "file:big.txt"
cmp_case_21 "-i2" "file:small.txt"

# --- -l blank-line joining (only affects the 'a' style) ---
cmp_case_21 "-ba -l2" "file:small.txt"
cmp_case_21 "-ba -l3" "file:small.txt"
cmp_case_21 "-ba -l2 -i5" "file:small.txt"
cmp_case_21 "--join-blank-lines=2 -ba" "file:small.txt"
cmp_case_21 "-ba -l2" "file:blank2.txt"
cmp_case_21 "-l0 -ba" "file:small.txt"         # 2.1 rejects, modern accepts

# --- logical pages: sections, -h/-f styles, -p, -d ---
cmp_case_21 "" "file:sec2.txt"
cmp_case_21 "-ba" "file:sec2.txt"
cmp_case_21 "-h a" "file:sec2.txt"
cmp_case_21 "-hpn" "file:sec2.txt"
cmp_case_21 "-f pn" "file:sec2.txt"
cmp_case_21 "-h a -f a -ba" "file:sec2.txt"
cmp_case_21 "-ha -fa -ba -w3" "file:sec2.txt"
cmp_case_21 "--header-numbering=a --footer-numbering=a -ba" "file:sec2.txt"
cmp_case_21 "-p" "file:sec2.txt"
cmp_case_21 "-p -v5 -ba" "file:sec2.txt"
cmp_case_21 "-p -i2 -ba" "file:sec2.txt"
cmp_case_21 "-b pa" "file:sec2.txt"
cmp_case_21 "-h a -b a -fa" "file:sec2.txt"
cmp_case_21 "-d :x" "file:sec2.txt"            # two-char custom delimiter
cmp_case_21 "-d :x -ba" "file:sec2.txt"
cmp_case_21 "-d xy" "file:secxy.txt"
cmp_case_21 "-d xy -ba" "file:secxy.txt"
cmp_case_21 "--section-delimiter=xy" "file:secxy.txt"
cmp_case_21 "-d x" "file:secxy.txt"            # single char: no section match
cmp_case_21 "-d :" "file:sec2.txt"             # single char: no section match
cmp_case_21 "-d __EMPTY__" "file:sec2.txt"     # empty delimiter

# --- stdin mode (no file operand, and an explicit "-") ---
cmp_case "-ba -w3 -s: -" "test"
cmp_case "-ba -nrz -w3" "file:-"
cmp_case_21 "-bt" "test"
cmp_case_21 "" "file:-"

# --- multi-file reads ---
cmp_case "-ba" "file:nonexistent_nl_xyz.txt"   # single missing file (rc 1)
cmp_case "-ba" "files:small.txt,allnl.txt"
# 2.1 keeps numbering the remaining files after one it cannot open;
# modern nl stops at the first failure, so this compares only vs 2.1.
cmp_case_21 "-ba" "files:small.txt,nonexistent_nl_xyz.txt,allnl.txt"

# --- error paths (nl's own diagnostics, plus getopt's) ---
cmp_case "-i x" "file:allnl.txt"
cmp_case "-i __EMPTY__" "file:allnl.txt"
cmp_case "-l x" "file:allnl.txt"
cmp_case "-v x" "file:allnl.txt"
cmp_case "-w x" "file:allnl.txt"
cmp_case "-w 1k" "file:allnl.txt"
cmp_case "-n l" "file:allnl.txt"
cmp_case "-n xy" "file:allnl.txt"
# 2.1 only looks at the first two characters of -n's argument, so "-n rnx"
# is accepted as "rn"; modern nl rejects the trailing junk, hence strict-only.
cmp_case_21 "-n rnx" "file:allnl.txt"
cmp_case "-h q" "file:allnl.txt"
cmp_case "-f q" "file:allnl.txt"
cmp_case "-b q" "file:allnl.txt"
cmp_case "-Z" "file:allnl.txt"
cmp_case "-b" "file:allnl.txt"
cmp_case "--bogus-option" "file:allnl.txt"
cmp_case "--body-numbering=q" "file:allnl.txt"

if [ "$fail" = 0 ]; then
    [ "$SKIPPED_21" != 0 ] && echo "note: skipped $SKIPPED_21 textutils-2.1-only cases in loose mode (puts(print_no_line_fmt) extra newline for unnumbered fields, -i 0 / -l 0 / -v past INT_MAX, logical-page numbering, aborted-on-missing-file; see the comments on cmp_case_21)"
    echo "nl parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "nl parity: FAIL (of $N cases)"
exit 1
