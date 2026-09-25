#!/bin/bash
# comm_parity.sh — byte-exact acceptance test for the HobbyOS ported comm.
#
# Runs our host-built comm (comm_host, argv[0] forced to "comm" via
# exec -a so error messages and usage text are comparable) against a GNU
# comm reference on the same inputs and compares stdout bytes, exit codes
# and stderr text across the default 3-column merge, -1/-2/-3 in every
# combination, duplicate runs, empty/one-empty files, identical files,
# missing final newlines, prefix-length ties, long lines, high bytes and
# embedded NULs, the two "-" (stdin) forms, and the error paths (missing
# file, missing/extra operand, unknown option).
#
# both sides run with LC_ALL=C: comm's line comparison is
# (HAVE_SETLOCALE && hard_LC_COLLATE) ? xmemcoll : memcmp, and the C
# locale is the one path the port implements (see src/user/comm_gnu.c) --
# pinning it keeps the reference on the same memcmp branch.
#
# Reference selection:
#   $2 = explicit path, else $COMM_REF, else the first of these that exists:
#        /usr/bin/comm (GNU coreutils) — tests the port against a modern
#        comm; the --help/--version text legitimately differs (2.1 vs
#        coreutils) and is not compared.  The strictest bar is
#        `make comm_parity_strict` which builds textutils-2.1's original
#        comm as the reference instead.
set -u

COMM_HOST="${1:-./obj/comm_host}"
REF="${3:-${2:-${COMM_REF:-$(command -v comm || echo /usr/bin/comm)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Pin the C locale for both sides (see the header comment).
export LC_ALL=C
export LANG=C

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs modern coreutils
# (loose: diagnostics/exit-code tightening were rewritten in later
# releases, so error paths compare only whether the run succeeded, not
# the wording).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# --- fixtures ---------------------------------------------------------
printf 'apple\nbanana\ncherry\ndate\n' > "$TMP/l1.txt"
printf 'banana\ncherry\nfig\ngrape\n' > "$TMP/r1.txt"
printf 'a\na\nb\nc\nc\nc\n' > "$TMP/ldup.txt"     # runs of duplicated lines
printf 'a\nb\nb\nc\n' > "$TMP/rdup.txt"
printf '' > "$TMP/empty.txt"
printf 'solo\n' > "$TMP/lone.txt"
printf 'm\nn' > "$TMP/nl1.txt"                    # no trailing newline
printf 'm\no\n' > "$TMP/nl2.txt"
printf 'ab\nb\n' > "$TMP/pre1.txt"                # prefix tie: ab < abc
printf 'abc\nb\n' > "$TMP/pre2.txt"
printf 'b\na\n' > "$TMP/uns1.txt"                 # deliberately unsorted
printf 'a\nb\n' > "$TMP/uns2.txt"
printf 'c\nb\n' > "$TMP/uns3.txt"                 # unsorted, descending run
printf 'b\nc\n' > "$TMP/uns4.txt"
cp "$TMP/l1.txt" "$TMP/stdin.txt"
# Binary fixtures need python3 (bash printf cannot emit NUL or high bytes).
# Every fixture below is genuinely sorted (byte order): the port, the 2.1
# reference and modern comm then agree on both output and exit code.
python3 - "$TMP" <<'PY'
import os, sys
t = sys.argv[1]
open(os.path.join(t, "long1.txt"), "wb").write(b"L" * 5000 + b"\n" + b"q" * 300 + b"\n" + b"z\n")
open(os.path.join(t, "long2.txt"), "wb").write(b"L" * 5000 + b"\n" + b"q" * 299 + b"\n" + b"zz\n")
open(os.path.join(t, "hi1.txt"), "wb").write(b"low\n\x80\n\xff\n\xff\xfe\n")
open(os.path.join(t, "hi2.txt"), "wb").write(b"low\n\x7f\n\xff\n\xff\xfd\n")
open(os.path.join(t, "nul1.txt"), "wb").write(b"\x00\n" + b"a\x00b\n")
open(os.path.join(t, "nul2.txt"), "wb").write(b"\x00c\n" + b"a\x00b\n")
# sorted multi-thousand-line merge with shared and duplicated lines
import random
random.seed(3)
shared = sorted(random.sample(range(4000), 900))
l1 = sorted(shared + random.sample(range(4000), 700))
l2 = sorted(shared + random.sample(range(4000), 900))
open(os.path.join(t, "big1.txt"), "w").write("".join("k%04d\n" % v for v in l1))
open(os.path.join(t, "big2.txt"), "w").write("".join("k%04d\n" % v for v in l2))
PY

# Bound every run so a hanging reference (some modern comm builds block on
# `comm - -`) fails the case instead of hanging the harness.
RUN=()
if command -v timeout >/dev/null 2>&1; then
    RUN=(timeout 30)
fi

# Run a case for one implementation.  $1 = args string (word-split; a token
# @NAME expands to $TMP/NAME, a plain "-" is the stdin operand); $2 = stdin
# fixture name to redirect in (empty = /dev/null).
run_one() {
    local args_str="$1" stdin_file="$2" impl="$3"
    local args=()
    local i exe
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    for i in "${!args[@]}"; do
        case "${args[$i]}" in
            @*) args[$i]="$TMP/${args[$i]#@}" ;;
        esac
    done
    if [ "$impl" = hb ]; then
        exe="$COMM_HOST"
    else
        exe="$REF"
    fi
    if [ -n "$stdin_file" ]; then
        "${RUN[@]+"${RUN[@]}"}" bash -c 'exec -a comm "$0" "$@"' "$exe" \
            ${args[@]+"${args[@]}"} <"$TMP/$stdin_file" >"$TMP/out" 2>"$TMP/err"
    else
        "${RUN[@]+"${RUN[@]}"}" bash -c 'exec -a comm "$0" "$@"' "$exe" \
            ${args[@]+"${args[@]}"} </dev/null >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" > "$TMP/rc"
}

cmp_case() {  # $1 = args string; $2 = stdin fixture (optional)
    local args_str="$1" stdin_file="${2:-}"
    N=$((N+1))
    run_one "$args_str" "$stdin_file" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$stdin_file" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: comm $args_str (stdin=${stdin_file:-none})"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: comm $args_str (stdin=${stdin_file:-none})"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb vs $rc_ref): comm $args_str (stdin=${stdin_file:-none})"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# --- default 3-column output -----------------------------------------
cmp_case "@l1.txt @r1.txt"                 # shared + both uniques
cmp_case "@ldup.txt @rdup.txt"             # duplicated runs
cmp_case "@empty.txt @lone.txt"            # left empty
cmp_case "@lone.txt @empty.txt"            # right empty
cmp_case "@empty.txt @empty.txt"           # both empty
cmp_case "@nl1.txt @nl2.txt"               # missing final newline on both
cmp_case "@pre1.txt @pre2.txt"             # one line a prefix of the other
cmp_case "@long1.txt @long2.txt"           # 5000-char line (linebuffer growth)
cmp_case "@hi1.txt @hi2.txt"               # high bytes (>0x7f)
cmp_case "@nul1.txt @nul2.txt"             # embedded NUL bytes
cmp_case "@l1.txt @l1.txt"                 # identical files
cmp_case "@big1.txt @big2.txt"             # ~1600/~1800-line merge

# --- -1/-2/-3 singles, pairs and the full mask ------------------------
cmp_case "-1 @l1.txt @r1.txt"
cmp_case "-2 @l1.txt @r1.txt"
cmp_case "-3 @l1.txt @r1.txt"
cmp_case "-12 @l1.txt @r1.txt"
cmp_case "-13 @l1.txt @r1.txt"
cmp_case "-23 @l1.txt @r1.txt"
cmp_case "-123 @l1.txt @r1.txt"
cmp_case "-1 -2 @l1.txt @r1.txt"           # separate flags, same as -12
cmp_case "-2 -3 @l1.txt @r1.txt"           # separate flags, same as -23
cmp_case "-1 @ldup.txt @rdup.txt"
cmp_case "-2 @ldup.txt @rdup.txt"
cmp_case "-3 @ldup.txt @rdup.txt"
cmp_case "-13 @ldup.txt @rdup.txt"
cmp_case "-23 @ldup.txt @rdup.txt"
cmp_case "-1 @hi1.txt @hi2.txt"
cmp_case "-3 @nul1.txt @nul2.txt"
cmp_case "-2 @empty.txt @lone.txt"
cmp_case "-1 @lone.txt @empty.txt"
cmp_case "-12 @nl1.txt @nl2.txt"
cmp_case "-2 @pre1.txt @pre2.txt"
cmp_case "-123 @l1.txt @l1.txt"
cmp_case "-3 @big1.txt @big2.txt"
cmp_case "-1 @long1.txt @long2.txt"
cmp_case "-23 @empty.txt @empty.txt"

# --- stdin ("-") forms -------------------------------------------------
cmp_case "- @l1.txt" stdin.txt
cmp_case "@l1.txt -" stdin.txt
cmp_case "-12 - @l1.txt" stdin.txt
cmp_case "@r1.txt -" stdin.txt

# --- error paths -------------------------------------------------------
cmp_case "@nosuch_left.txt @r1.txt"        # missing left operand (rc 1)
cmp_case "@l1.txt @nosuch_right.txt"       # missing right operand (rc 1)
cmp_case "@nosuch_left.txt @nosuch_right.txt"
cmp_case ""                                # no operands -> usage (1)
cmp_case "@l1.txt"                         # one operand -> usage (1)
cmp_case "@l1.txt @r1.txt @l1.txt"         # extra operand -> usage (1)
cmp_case "-1"                              # flag but no operands
cmp_case "-x @l1.txt @r1.txt"              # unknown option (getopt + usage)

# --- cases gated to the 2.1 reference ----------------------------------
# --help/--version text: 2.1 prints its own help/`comm (textutils) 2.1';
# modern coreutils prints rewritten text, so these only compare strictly.
if [ "$REF_MODE" = strict ]; then
    cmp_case "--help"
    cmp_case "--version"
else
    echo "note: skipping --help/--version cases (2.1 text vs modern coreutils)"
fi

# Unsorted input: textutils-2.1 has no order check at all -- it just merges
# (the output is the well-defined memcmp merge of the two files) and exits
# 0.  Modern coreutils added --check-order/--nocheck-order and its default
# diagnoses "input is not in sorted order" with exit 1, so the exit code
# legitimately differs on unsorted inputs; the stdout still matches, and
# the port follows its 2.1 lineage (no order check).
if [ "$REF_MODE" = strict ]; then
    cmp_case "@uns1.txt @uns2.txt"
    cmp_case "@uns3.txt @uns4.txt"
    cmp_case "-1 @uns1.txt @uns2.txt"
    cmp_case "-12 @uns1.txt @uns2.txt"
else
    echo "note: skipping unsorted-input cases (2.1 has no order check, modern comm exits 1)"
fi

# `comm - -` (stdin for both operands): 2.1 reads the shared stream
# interleaved and then fclose()s stdin twice -- identical behavior in the
# port, but modern comm blocks/errors on it, so it only compares strictly.
if [ "$REF_MODE" = strict ]; then
    cmp_case "- -" stdin.txt
else
    echo "note: skipping 'comm - -' (2.1 double-fcloses stdin; modern comm hangs)"
fi

if [ "$fail" = 0 ]; then
    echo "comm parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "comm parity: FAIL (of $N cases)"
exit 1
