#!/bin/bash
# fold_parity.sh — byte-exact acceptance test for the HobbyOS ported fold.
#
# Runs our host-built fold (fold_host, argv[0] forced to "fold" via exec -a
# so error messages are comparable) against a GNU fold reference on the same
# inputs and compares stdout bytes, exit codes and stderr text across the
# default 80-column wrap, -w widths, -b byte counting, -s space breaking,
# tabs, \r/\b column tricks, long words, empty files, missing trailing
# newlines, high-bit bytes, stdin mode, multi-file mode and error paths.
#
# Reference selection:
#   $2 = explicit path, else $FOLD_REF, else the first of these that exists:
#        /usr/bin/fold (uutils coreutils on this host) — loose; the strict
#        bar is `bash src/host/build_tu21_fold_ref.sh <out>`, which builds
#        textutils-2.1's original fold, and the port must match it
#        byte-for-byte.
#
# _POSIX2_VERSION: textutils-2.1's posix2_version() takes its default from
# the compile-time _POSIX2_VERSION of <unistd.h>.  Modern glibc says 200809L,
# so the 2.1 reference *built on this host* would hard-error on the obsolete
# `-N' syntax ("`-10' option is obsolete; use `-w10'") unless the environment
# overrides it; the port's sysroot publishes no POSIX version (default 0) so
# it accepts the form, matching the era this release shipped in and HobbyOS
# itself.  Like src/host/head_parity.sh does for head, the corpus therefore
# runs both binaries with _POSIX2_VERSION=199901 (< 200112) set, and the
# rejection path is exercised separately with 200112.  The no-variable
# behavior of the two builds is a gated, informational divergence (below).
#
# Known faithful-to-2.1 spin: `-s' with a width < 8 on a line containing a
# tab that lands at column 0 spins forever in the original algorithm (the
# tab jumps past the width, nothing is buffered to break on, so fold_file
# prints an empty line and re-enters the same state).  Our port reproduces
# it; the corpus avoids those combinations (every -s case on a tab-bearing
# fixture uses width >= 8) and every run is wrapped in a timeout.
set -u

FOLD_HOST="${1:-./obj/fold_host}"
REF="${3:-${2:-${FOLD_REF:-$(command -v fold || echo /usr/bin/fold)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs modern GNU/uutils
# (loose: diagnostics were rewritten, so error paths compare only whether
# the run succeeded, not the wording).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

note() { echo "note: $*"; }

# Fixtures
printf 'hello world\nsecond line\n\nfourth\n' > "$TMP/small.txt"
python3 - <<'PY' > "$TMP/num.txt"
print("\n".join("0123456789" * 3 for _ in range(4)))
PY
python3 - <<'PY' > "$TMP/cols80.txt"
lines = []
for n in (78, 79, 80, 81, 82, 160, 161, 240):
    lines.append("x" * n)
lines.append("")
lines.append("A" * 79 + " ")
lines.append("B" * 40 + " " + "C" * 40)
lines.append("D" * 79 + " E" * 2)
print("\n".join(lines))
PY
python3 - <<'PY' > "$TMP/words.txt"
lines = []
lines.append("word " * 40)
lines.append(("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu " * 3).strip())
lines.append(" " * 100)
lines.append("lead" + " " * 76 + "tail")
lines.append("x" * 79 + " " + "y" * 79)
lines.append(("one two three four five six seven eight nine ten " * 6).strip())
lines.append("trailing spaces here" + " " * 30)
lines.append("mixed\twords and\ttabs in one line with spaces " * 3)
print("\n".join(lines))
PY
python3 - <<'PY' > "$TMP/tabs.txt"
lines = ["\talpha\tbeta", "\t\t\tx", "seven77\tb", "ab\tcd\tef",
         "0123456\tB", "0" * 15 + "\tZ", "\t\t", "a\ta\ta\ta\ta\ta\ta\ta\ta\ta\ta\ta\ta"]
print("\n".join(lines))
PY
python3 - <<'PY' > "$TMP/longword.txt"
print("L" * 300)
print(("M" * 37) + " " + ("N" * 121))
PY
printf 'no trailing newline here' > "$TMP/nonl.txt"
python3 - <<'PY' > "$TMP/nonl2.txt"
import sys
sys.stdout.write("N" * 120)
PY
printf '' > "$TMP/empty.txt"
python3 - <<'PY' > "$TMP/highbit.txt"
import sys
out = bytearray()
out += bytes(range(128, 256)) + b"\n"
out += bytes(range(0xa0, 0x100)) * 2 + b"\n"
out += bytes(range(160, 256)) + b" " + bytes(range(160, 220)) + b"\n"
out += b"\x80" * 200 + b"\n"
sys.stdout.buffer.write(bytes(out))
PY
python3 - <<'PY' > "$TMP/crbs.txt"
lines = ["abc\rdef", "abcdef\b\bxy", "x" * 85 + "\r" + "y" * 10,
         "ab\b\b\b\bcd", "\b\b\b", "z" * 20 + "\r",
         "column tricks: \rrestart and \b\b\bback"]
print("\n".join(lines))
PY
python3 - <<'PY' > "$TMP/big.txt"
import random, sys
random.seed(7)
words = ["alpha", "beta", "gamma", "longwordislong", "x", "yy", "zzz", "a" * 25]
out = []
for _ in range(1200):
    k = random.random()
    if k < 0.15:
        line = "\t" * random.randint(1, 3) + " ".join(random.choice(words) for _ in range(random.randint(1, 8)))
    elif k < 0.25:
        line = "x" * random.randint(70, 200)
    elif k < 0.32:
        line = "\r" + "".join(random.choice("ab \t") for _ in range(random.randint(5, 120)))
    elif k < 0.38:
        line = "".join(random.choice("ab\bcd ") for _ in range(random.randint(3, 100)))
    elif k < 0.45:
        line = bytes(random.randint(128, 255) for _ in range(random.randint(5, 150))).decode("latin-1")
    else:
        line = " ".join(random.choice(words) for _ in range(random.randint(1, 30)))
    out.append(line)
sys.stdout.buffer.write(("\n".join(out) + "\n").encode("latin-1"))
PY

# Run a case for one implementation.  $1 = args string (word-split, with
# __SP__/__EMPTY__ tokens standing in for quoted " " / "" arguments);
# $2 = input spec: a space-separated list of fixture names read as
# $TMP/<name>, where "-" means standard input (fed from $TMP/small.txt); an
# empty spec also means stdin.  $3 = hb|ref.
run_one() {
    local args_str="$1" input="$2" impl="$3"
    local -a args=() paths=()
    local use_stdin=0 i nm
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
    done
    for nm in $input; do
        nm="${nm#file:}"
        if [ "$nm" = "-" ]; then
            paths+=("-")
            use_stdin=1
        else
            paths+=("$TMP/$nm")
        fi
    done
    [ -z "$input" ] && use_stdin=1
    local -a cmd
    if [ "$impl" = hb ]; then
        cmd=(bash -c 'exec -a fold "$0" "$@"' "$FOLD_HOST")
    else
        cmd=(bash -c 'exec -a fold "$0" "$@"' "$REF")
    fi
    if [ "$use_stdin" = 1 ]; then
        timeout 15 env _POSIX2_VERSION=199901 "${cmd[@]}" "${args[@]}" "${paths[@]}" \
            <"$TMP/small.txt" >"$TMP/out" 2>"$TMP/err"
    else
        timeout 15 env _POSIX2_VERSION=199901 "${cmd[@]}" "${args[@]}" "${paths[@]}" \
            >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" >"$TMP/rc"
}

cmp_case() { # $1 = args string; $2 = input spec
    local args_str="$1" input="$2"
    N=$((N + 1))
    run_one "$args_str" "$input" hb
    local rc_hb
    rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"
    mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$input" ref
    local rc_ref
    rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: fold $args_str ($input)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: fold $args_str ($input)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && [ "$rc_hb" != "0" ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): fold $args_str ($input)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# Compare a full run where both sides must agree on stdout, exit status AND
# stderr, used for paths whose text is identical in 2.1 (usage, version).
cmp_case_exact() { # $1 = args string; $2 = input spec
    local args_str="$1" input="$2"
    N=$((N + 1))
    run_one "$args_str" "$input" hb
    local rc_hb
    rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"
    mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$input" ref
    local rc_ref
    rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out" || ! SAME_OUT "$TMP/eh" "$TMP/err" \
        || [ "$rc_hb" != "$rc_ref" ]; then
        echo "FAIL: differs (rc $rc_hb vs $rc_ref): fold $args_str ($input)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        echo "  hberr: $(head -2 "$TMP/eh" | tr '\n' '|')"
        echo "  referr:$(head -2 "$TMP/err" | tr '\n' '|')"
        fail=1
    fi
}

# --- default width (80) over every fixture ---------------------------------
for f in small.txt cols80.txt words.txt tabs.txt longword.txt empty.txt \
    nonl.txt nonl2.txt highbit.txt crbs.txt big.txt; do
    cmp_case "" "file:$f"
done

# --- -w widths -------------------------------------------------------------
for w in 1 2 3 5 10 40 79 80 81 200 1000; do
    cmp_case "-w $w" "file:small.txt"
done
for w in 1 3 10 40 80 81 200; do
    cmp_case "-w $w" "file:cols80.txt"
done
for w in 2 5 7 10 12 33 80; do
    cmp_case "-w $w" "file:words.txt"
done
for w in 1 7 8 16 64; do
    cmp_case "-w $w" "file:tabs.txt"
done
cmp_case "-w 30" "file:longword.txt"
cmp_case "-w 10" "file:longword.txt"
cmp_case "-w 10" "file:highbit.txt"
cmp_case "-w 3" "file:highbit.txt"
cmp_case "-w 5" "file:crbs.txt"
cmp_case "-w 7" "file:crbs.txt"
cmp_case "-w 10" "file:nonl.txt"
cmp_case "-w 12" "file:nonl2.txt"
cmp_case "-w 3" "file:empty.txt"
cmp_case "-w 2147483647" "file:small.txt"

# --- option spellings / obsolete -N ---------------------------------------
cmp_case "-w10" "file:words.txt"
cmp_case "-w80" "file:cols80.txt"
cmp_case "--width=10" "file:words.txt"
cmp_case "--width 10" "file:words.txt"
cmp_case "-w 007" "file:words.txt"
cmp_case "-w +7" "file:words.txt"
cmp_case "-10" "file:num.txt"
cmp_case "-3" "file:cols80.txt"
cmp_case "-80" "file:words.txt"
cmp_case "-1" "file:small.txt"
cmp_case "-w 20 --" "file:words.txt"

# --- -b (byte counting) ----------------------------------------------------
cmp_case "-b" "file:tabs.txt"
cmp_case "-b" "file:highbit.txt"
cmp_case "-b" "file:crbs.txt"
cmp_case "-b -w 5" "file:tabs.txt"
cmp_case "-b -w 1" "file:small.txt"
cmp_case "-b -w 3" "file:highbit.txt"
cmp_case "-b -w 3" "file:crbs.txt"
cmp_case "-b -w 10" "file:big.txt"
cmp_case "-b -w 64" "file:big.txt"
cmp_case "-b -s -w 10" "file:words.txt"
cmp_case "-b -w 10" "file:nonl2.txt"

# --- -s (break at spaces); widths >= 8 where the fixture has tabs ----------
cmp_case "-s" "file:words.txt"
cmp_case "-s" "file:cols80.txt"
cmp_case "-s" "file:big.txt"
cmp_case "-s -w 10" "file:small.txt"
cmp_case "-s -w 5" "file:cols80.txt"
cmp_case "-s -w 3" "file:cols80.txt"
cmp_case "-s -w 1" "file:small.txt"
cmp_case "-s -w 79" "file:words.txt"
cmp_case "-s -w 10" "file:words.txt"
cmp_case "-s -w 8" "file:words.txt"
cmp_case "-s -w 8" "file:tabs.txt"
cmp_case "-s -w 16" "file:tabs.txt"
cmp_case "-s -w 30" "file:longword.txt"
cmp_case "-s -w 5" "file:nonl.txt"
cmp_case "-s -w 10" "file:nonl2.txt"
cmp_case "-s -w 5" "file:highbit.txt"
cmp_case "-s -w 5" "file:crbs.txt"
cmp_case "-s -w 40" "file:num.txt"

# --- stdin mode ------------------------------------------------------------
cmp_case "" ""
cmp_case "-w 5" ""
cmp_case "-s -w 10" ""
cmp_case "-b -w 4" ""
cmp_case "-s" ""
cmp_case "-w 1" ""
cmp_case "-w 10 -" ""
cmp_case "-s -w 20 -" "file:-"
cmp_case "-w 3" "- file:small.txt"
cmp_case "-w 3" "- -"

# --- multi-file ------------------------------------------------------------
cmp_case "-w 10" "file:small.txt file:words.txt"
cmp_case "-w 10" "file:cols80.txt file:empty.txt file:tabs.txt"
cmp_case "-b -w 7" "file:highbit.txt file:crbs.txt"
cmp_case "-w 3" "file:- file:small.txt"
cmp_case "-w 10" "file:small.txt file:nosuch_xyz.txt"
cmp_case "-w 10" "file:nosuch_xyz.txt"

# --- error paths (invalid widths, unknown options, missing files) ----------
cmp_case "-w abc" "file:small.txt"
cmp_case "-w __EMPTY__" "file:small.txt"
cmp_case "-w 12k" "file:small.txt"
cmp_case "-w 1.5" "file:small.txt"
cmp_case "-w 0x10" "file:small.txt"
cmp_case "-w 99999999999999999999" "file:small.txt"
cmp_case "-w -5" "file:small.txt"
cmp_case "-w" "file:small.txt"
cmp_case "--width" "file:small.txt"
cmp_case "--bogus" "file:small.txt"
cmp_case "-x" "file:small.txt"
cmp_case "-s10" "file:small.txt"
cmp_case "-bw" "file:small.txt"
cmp_case "-w 10 -s -b -w 0" "file:small.txt"

# Non-positive widths (and widths past INT_MAX) are rejected by
# textutils-2.1 AND by GNU coreutils fold ("invalid number of columns"),
# but this host's /usr/bin/fold is uutils, which accepts 0 (spinning
# forever) and clamps huge widths, so those cases only run when the
# reference really is 2.1.
if [ "$REF_MODE" = strict ]; then
    cmp_case "-w 0" "file:small.txt"
    cmp_case "-w 0" "file:empty.txt"
    cmp_case "-w 0" "file:big.txt"
    cmp_case "-w 00" "file:small.txt"
    cmp_case "-w +0" "file:small.txt"
    cmp_case "-b -w 0" "file:tabs.txt"
    cmp_case "-w 2147483648" "file:small.txt"
else
    note "skipping -w 0 / -w 2147483648 cases (uutils fold accepts 0 and loops, and clamps huge widths; 2.1/GNU reject)"
fi

# Remaining cases where this host's modern reference (uutils 0.8.0) diverges
# from its own 2.1 lineage; they only run against the 2.1 reference:
#   * tab/space runs: uutils breaks them differently from 2.1/GNU, and its
#     output for a line even changes when later input is appended (appending
#     one line to the same fixture moves uutils' break points while the port
#     and 2.1 stay byte-identical) -- a uutils buffering defect;
#   * `-w " 7"': 2.1/GNU accept leading whitespace (strtol), uutils errors;
#   * `--' as end-of-options: uutils errors instead of folding the operand;
#   * a missing operand: 2.1/GNU report it, fold the remaining files and exit
#     1, uutils stops at it.
if [ "$REF_MODE" = strict ]; then
    cmp_case "-w 10" "file:big.txt"
    cmp_case "-w 3" "file:big.txt"
    cmp_case "-w 81" "file:big.txt"
    cmp_case "-s -w 20" "file:big.txt"
    cmp_case "-s -w 8" "file:big.txt"
    cmp_case "-s -b -w 9" "file:big.txt"
    cmp_case "-s -b -w 12" "file:words.txt"
    cmp_case "-b -s -w 3" "file:tabs.txt"
    cmp_case "-w __SP__7" "file:words.txt"
    cmp_case "-- small.txt" "file:small.txt"
    cmp_case "-w 10" "file:nosuch_xyz.txt file:small.txt"
else
    note "skipping 11 uutils-vs-2.1 divergence cases (tab/space-run break points incl. context-dependent output, -w ' 7', -- end-of-options, missing-operand continue)"
fi

# --- --help / --version and the posix2_version gate (strict only: the text
# --- is 2.1's own, and modern fold rewrote it) -----------------------------
if [ "$REF_MODE" = strict ]; then
    cmp_case_exact "--help" "file:small.txt"
    cmp_case_exact "--version" "file:small.txt"
    N=$((N + 1))
    env _POSIX2_VERSION=200112 bash -c 'exec -a fold "$0" "$@"' "$FOLD_HOST" \
        -10 "$TMP/small.txt" >"$TMP/oh" 2>"$TMP/eh"
    rc_hb=$?
    env _POSIX2_VERSION=200112 bash -c 'exec -a fold "$0" "$@"' "$REF" \
        -10 "$TMP/small.txt" >"$TMP/out" 2>"$TMP/err"
    rc_ref=$?
    if [ "$rc_hb" != "$rc_ref" ] || ! SAME_OUT "$TMP/oh" "$TMP/out" \
        || ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: obsolete-option rejection (_POSIX2_VERSION=200112) rc $rc_hb vs $rc_ref"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
else
    note "skipping --help/--version text and 200112-rejection cases (modern fold text differs)"
fi

# --- obsolete -N with NO _POSIX2_VERSION in the environment ----------------
# Gated informational divergence: the 2.1 reference was compiled against
# modern glibc, whose compile-time _POSIX2_VERSION (200809) is >= 200112, so
# it rejects `-10' as obsolete; the port's sysroot publishes no POSIX version
# (posix2_version() -> 0), the era/HobbyOS default, and accepts it.  Assert
# the port's own (HobbyOS) behavior -- `-10' must equal `-w 10' -- and just
# report what the reference does.
N=$((N + 1))
env -u _POSIX2_VERSION bash -c 'exec -a fold "$0" "$@"' "$FOLD_HOST" \
    -10 "$TMP/num.txt" >"$TMP/oh" 2>"$TMP/eh"
rc_hb=$?
env _POSIX2_VERSION=199901 bash -c 'exec -a fold "$0" "$@"' "$FOLD_HOST" \
    -w 10 "$TMP/num.txt" >"$TMP/out" 2>/dev/null
if [ "$rc_hb" != 0 ] || ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: port must accept obsolete '-10' when _POSIX2_VERSION is unset (HobbyOS default)"
    echo "  rc=$rc_hb, err: $(cat "$TMP/eh")"
    fail=1
fi
env -u _POSIX2_VERSION bash -c 'exec -a fold "$0" "$@"' "$REF" \
    -10 "$TMP/num.txt" >/dev/null 2>&1
rc_ref=$?
if [ "$REF_MODE" = strict ]; then
    note "with _POSIX2_VERSION unset the 2.1-on-glibc reference exits $rc_ref on 'fold -10' (compile-time 200809 >= 200112); the port accepts it (rc 0) — the era default"
fi

if [ "$fail" = 0 ]; then
    echo "fold parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "fold parity: FAIL (of $N cases)"
exit 1
