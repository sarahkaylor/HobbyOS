#!/bin/bash
# tac_parity.sh — byte-exact acceptance test for the HobbyOS ported tac.
#
# Runs our host-built tac (tac_host, argv[0] forced to "tac" via exec -a so
# error messages are comparable) against a GNU tac reference on the same
# inputs and compares stdout bytes, exit codes and stderr text across
# plain reversal, -b/-s/-r variants, multi-file and '-' operand plumbing,
# partial-line edge cases, the 8192-byte read-size steps and the giant
# single-record buffer-growth path.
#
# Reference selection:
#   $2 = explicit path, else $TAC_REF, else the first of these that exists:
#        /usr/bin/tac — whatever tac the host ships (here a Rust/clap-style
#        build, not classic GNU coreutils); compared in LOOSE mode because
#        its diagnostics, --help/--version text and option-parsing quirks
#        (dash-leading values, "--" as a value, leading "+" in regexes)
#        all diverge.  The strictest bar is `make tac_parity_strict`, which
#        builds textutils-2.1's original tac as the reference instead
#        (stdout, stderr and the exit code all compare byte-for-byte).
#
# Comparison levels (strict mode):
#   FULL  — stdout + stderr + exit code, all byte-exact.
#   OUTRC — stdout + exit code byte-exact; stderr NOT compared.  Used only
#           for the unknown-option cases, where the *sysroot* getopt prints
#           glibc-style quoted diagnostics ("invalid option -- 'x'") while
#           2.1's own getopt prints them unquoted ("invalid option -- x").
#           That wording difference is sysroot-wide (the same one the
#           cut/head/tail/tsort corpora sidestep); everything the port
#           itself prints still compares byte-for-byte.
# In loose mode stderr is never compared, exit codes compare as
# success/failure classes, and the cases tagged `modern` are skipped with
# a note (they are the documented 2.1-vs-modern divergences: --help and
# --version text).
set -u

TAC_HOST="${1:-./obj/tac_host}"
REF="${3:-${2:-${TAC_REF:-$(command -v tac || echo /usr/bin/tac)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0
N_OUTRC=0
N_SKIPPED=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict:
# stdout + stderr + exit codes compare byte-for-byte) vs a modern
# coreutils tac (loose: diagnostics were rewritten after 2.1).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# --- fixtures --------------------------------------------------------------
printf ''                                  > "$TMP/empty.txt"
printf 'one\n'                             > "$TMP/oneline.txt"
printf 'one two three'                     > "$TMP/oneline_nonl.txt"
printf 'one\ntwo\nthree\n'                 > "$TMP/three.txt"
printf 'one\ntwo'                          > "$TMP/two_nonl.txt"
printf '\n\n\n'                            > "$TMP/newlines.txt"
printf '\n\na\n\nb\n\n'                    > "$TMP/blanky.txt"
printf 'wXXvXXu'                           > "$TMP/sepxx.txt"
printf 'a,b,,c,'                           > "$TMP/commas.txt"
printf 'aaaa'                              > "$TMP/overlap_aa.txt"
printf 'rule1\n-----\nrule2\n-----\n'      > "$TMP/rules.txt"
printf 'x1y22z333*'                        > "$TMP/digits.txt"
printf 'abc[def]ghi\nklm[n]op\n'           > "$TMP/brackets.txt"
printf 'a+b+c\n'                           > "$TMP/pluses.txt"
printf 'nosep\n'                           > "$TMP/plain.txt"
python3 - <<'PY' > "$TMP/big8k.txt"
# 90 lines x 200 chars: > 8192, so recurring reads step through the
# 8192-byte boundary logic in tac_seekable with lines straddling blocks.
line = "L" + "x" * 198 + "\n"
import sys
for i in range(90):
    sys.stdout.write("%03d" % i + line[3:])
PY
python3 - <<'PY' > "$TMP/giant.txt"
# one record of 20000 bytes with no separator: forces read_size doubling
# and the xrealloc buffer-growth path.
import sys
sys.stdout.write("G" * 20000)
PY
python3 - <<'PY' > "$TMP/giant2.txt"
# one 20000-byte record followed by a short one after a separator, so the
# grown buffer must still track match_start/past_end adjustments.
import sys
sys.stdout.write("A" * 20000 + "\nB\nC\n")
PY
python3 - <<'PY' > "$TMP/manylines.txt"
import sys
sys.stdout.write("".join("line%04d\n" % i for i in range(5000)))
PY
python3 - <<'PY' > "$TMP/binjunk.bin"
import sys
sys.stdout.buffer.write(b"\x01\x02 x\n\xff\xfe y\ny \x03\x04\n")
sys.stdout.buffer.write(b"a\x00b c\n")
sys.stdout.buffer.write(b"\x7f\x80 \x81\x82")
PY
# default stdin fixture (rewritten per case with set_stdin)
printf 'alpha\nbeta\ngamma\n' > "$TMP/in.txt"
set_stdin() { printf '%b' "$1" > "$TMP/in.txt"; }

# --- harness ---------------------------------------------------------------

# Run one case for one implementation.  $1 = args string (word-split);
# $2 = input spec: "-" (stdin), "file:NAME" (FILE operand) or
# "file:-" (literal "-" operand with stdin).  Results land in
# $TMP/{out,err,rc}.
run_one() {
    local args_str="$1" spec="$2" impl="$3"
    local args=()
    if [ -n "$args_str" ]; then
        read -r -a args <<< "$args_str"
        # Resolve file:NAME tokens (used for multi-file argc cases) to the
        # real fixture paths.  Anchored substitution: only a token that
        # STARTS with "file:" is rewritten.
        local resolved=() tok
        for tok in "${args[@]}"; do
            resolved+=("${tok/#file:/$TMP/}")
        done
        args=("${resolved[@]}")
    fi
    local cmd
    if [ "$impl" = hb ]; then
        cmd=(bash -c 'exec -a tac "$0" "$@"' "$TAC_HOST")
    else
        cmd=(bash -c 'exec -a tac "$0" "$@"' "$REF")
    fi
    case "$spec" in
        "-")
            "${cmd[@]}" "${args[@]}" <"$TMP/in.txt" >"$TMP/out" 2>"$TMP/err"
            ;;
        "file:-")
            "${cmd[@]}" "${args[@]}" - <"$TMP/in.txt" >"$TMP/out" 2>"$TMP/err"
            ;;
        file:*)
            "${cmd[@]}" "${args[@]}" "$TMP/${spec#file:}" >"$TMP/out" 2>"$TMP/err"
            ;;
    esac
    echo "$?" > "$TMP/rc"
}

# cmp_case <level> <flags> <args string> <input spec>
#   flags: "" or "modern" (skip in loose mode: documented divergence)
cmp_case() {
    local level="$1" flags="$2" args_str="$3" spec="$4"
    if [ "$REF_MODE" = loose ] && [ "$flags" = modern ]; then
        N_SKIPPED=$((N_SKIPPED + 1))
        return
    fi
    N=$((N + 1))
    if [ "$level" = OUTRC ]; then
        N_OUTRC=$((N_OUTRC + 1))
    fi

    run_one "$args_str" "$spec" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$spec" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")

    local desc="tac $args_str [$spec]"

    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: $desc"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi

    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: $desc"
            fail=1
        fi
    fi

    if [ "$level" = FULL ] && [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs: $desc"
        echo "  hb:  $(tr '\n' '|' < "$TMP/eh")"
        echo "  ref: $(tr '\n' '|' < "$TMP/err")"
        fail=1
    fi
}

# --- corpus: basic reversal ------------------------------------------------
cmp_case FULL "" "" "file:three.txt"
set_stdin 'alpha\nbeta\ngamma\n'
cmp_case FULL "" "" "-"
cmp_case FULL "" "" "file:oneline.txt"
cmp_case FULL "" "" "file:oneline_nonl.txt"
cmp_case FULL "" "" "file:two_nonl.txt"
cmp_case FULL "" "" "file:empty.txt"
set_stdin ''
cmp_case FULL "" "" "-"
cmp_case FULL "" "" "file:newlines.txt"
cmp_case FULL "" "" "file:blanky.txt"
set_stdin '\n\n'
cmp_case FULL "" "" "-"

# --- corpus: -b (separator before) -----------------------------------------
cmp_case FULL "" "-b" "file:three.txt"
cmp_case FULL "" "-b" "file:oneline_nonl.txt"
cmp_case FULL "" "-b" "file:two_nonl.txt"
cmp_case FULL "" "-b" "file:newlines.txt"
set_stdin 'alpha\nbeta\ngamma\n'
cmp_case FULL "" "-b" "-"
cmp_case FULL "" "--before" "file:blanky.txt"

# --- corpus: -s separator (non-regex) --------------------------------------
cmp_case FULL "" "-s XX" "file:sepxx.txt"
cmp_case FULL "" "-s ," "file:commas.txt"
cmp_case FULL "" "-s ," "file:three.txt"
cmp_case FULL "" "-s ," "file:plain.txt"
cmp_case FULL "" "-s aa" "file:overlap_aa.txt"
cmp_case FULL modern "-s -----" "file:rules.txt"
cmp_case FULL "" "-b -s XX" "file:sepxx.txt"
cmp_case FULL "" "-b -s ," "file:commas.txt"
cmp_case FULL modern "-b -s -----" "file:rules.txt"
cmp_case FULL modern "--separator=" "file:three.txt"
cmp_case FULL "" "--separator=XX" "file:sepxx.txt"
cmp_case FULL "" "-s ab" "file:plain.txt"
set_stdin 'A--B--C'
cmp_case FULL modern "-s --" "-"
cmp_case FULL "" "-s ," "file:three.txt"

# --- corpus: -r regex separators -------------------------------------------
cmp_case FULL "" "-r -s [0-9]" "file:digits.txt"
cmp_case FULL "" "-r -s [0-9][0-9]" "file:digits.txt"
cmp_case FULL modern "-r -s + " "file:pluses.txt"
cmp_case FULL "" "-r -s \\[.*\\]" "file:brackets.txt"
cmp_case FULL "" "-r -s x*" "file:plain.txt"
cmp_case FULL "" "-r -s q" "file:plain.txt"
cmp_case FULL "" "-r" "file:three.txt"
cmp_case FULL "" "-r -b -s [ab]" "file:plain.txt"
cmp_case FULL "" "-r -s ." "file:oneline_nonl.txt"
set_stdin 'p1q22r333s'
cmp_case FULL "" "-r -s [0-9]+" "-"
cmp_case FULL "" "-r -s p\\|q" "file:pq.txt"
echo "note: 'tac -r -s .\\|NL' (byte-reverse) exercised via the '[0-9]+' and 'x*' regex cases"
cmp_case FULL "" "--regex --separator=[0-9]" "file:digits.txt"
cmp_case FULL "" "-r -s (" "file:three.txt"
# --- corpus: capacity / buffer growth --------------------------------------
cmp_case FULL "" "" "file:big8k.txt"
cmp_case FULL "" "" "file:giant.txt"
cmp_case FULL "" "" "file:giant2.txt"
cmp_case FULL "" "-b" "file:giant2.txt"
cmp_case FULL "" "-s BB" "file:giant.txt"
cmp_case FULL "" "-r -s B+" "file:giant2.txt"
cmp_case FULL "" "" "file:manylines.txt"
set_stdin 'edge\n\n'
cmp_case FULL "" "" "-"

# --- corpus: multi-file and '-' plumbing -----------------------------------
cmp_case FULL "" "file:three.txt file:oneline.txt" "-"
cmp_case FULL "" "file:oneline.txt file:three.txt" "-"
cmp_case FULL "" "file:three.txt file:empty.txt file:blanky.txt" "-"
cmp_case FULL "" "file:- file:three.txt" "-"
cmp_case FULL "" "- file:three.txt -" "-"
set_stdin 'x\ny\n'
cmp_case FULL "" "-" "file:-"
cmp_case FULL "" "--" "file:three.txt"
cmp_case FULL "" "" "file:nonexistent_xyz.txt"
cmp_case FULL "" "file:nonexistent_xyz.txt file:three.txt" "-"
cmp_case FULL "" "file:three.txt file:nonexistent_xyz.txt" "-"
cmp_case FULL "" "file:big8k.txt file:three.txt" "-"

# --- corpus: binary transparency -------------------------------------------
cmp_case FULL "" "" "file:binjunk.bin"
cmp_case FULL "" "-b" "file:binjunk.bin"
set_stdin '\x01\x02\n\x03'
cmp_case FULL "" "" "-"

# --- corpus: options -------------------------------------------------------
set_stdin ''
cmp_case FULL modern "--help" "-"
cmp_case FULL modern "--version" "-"
cmp_case OUTRC "" "-x" "-"
cmp_case OUTRC "" "--bogus" "-"
cmp_case OUTRC modern "-h" "-"

if [ "$fail" = 0 ]; then
    echo "tac parity: PASS ($N cases vs $REF; $N_OUTRC stdout+rc only; $N_SKIPPED modern-divergence skips)"
    exit 0
fi
echo "tac parity: FAIL (of $N cases)"
exit 1
