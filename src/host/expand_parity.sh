#!/bin/bash
# expand_parity.sh — byte-exact acceptance test for the HobbyOS ported expand.
#
# Runs our host-built expand (expand_host, argv[0] forced to "expand" via
# exec -a so error messages and usage text are comparable) against a GNU
# expand reference on the same inputs and compares stdout bytes, exit codes
# and stderr text across the default 8-column tabbing, -t/--tabs single
# sizes and explicit tab-stop lists (including empty elements, blank
# separators and the run-out-of-stops path), -i/--initial, columns past the
# last stop, blank lines, missing final newline, CR bytes, high-bit bytes,
# backspace/control bytes, stdin (no FILE and "-"), multiple FILEs, missing
# files, a directory operand and the error paths (bad tab list characters,
# 0, negative, descending, non-numeric, missing option argument, unknown
# options, huge/overflowing values).
#
# Reference selection:
#   $2 = explicit path, else $EXPAND_REF, else the first of these that
#   exists: /usr/bin/expand — tests the port against a modern GNU expand
#   (loose: diagnostics/tab-list tightening differ between releases, so
#   error paths compare stdout + whether the run succeeded, and the
#   obsolete `-LIST' syntax is gated off).  The strictest bar is
#   `make expand_parity_strict`, which builds textutils-2.1's original
#   expand as the reference instead (strict: exit codes and error text
#   compare byte-for-byte).
#
# Obsolete `-LIST' tab syntax (`expand -8'): textutils-2.1 gates it on
# posix2_version(), which reads the _POSIX2_VERSION environment variable at
# runtime.  Our port's compile-time default is 0 (the sysroot publishes no
# _POSIX2_VERSION; same convention as the head/tail/fold ports), so it
# accepts the obsolete form; the 2.1 reference built against modern glibc
# (unistd.h hard-defines _POSIX2_VERSION 200809L) and modern coreutils
# reject it.  Those cases are therefore run with _POSIX2_VERSION=0 exported
# for BOTH implementations, where both accept and must agree byte-for-byte,
# plus an explicit port-convention check documenting the default-context
# difference.
#
# Argument order: every case spells options BEFORE operands.  The sysroot
# getopt_long permutes options that follow a non-option operand but hands
# an argument-taking option the wrong optarg in that shape (pre-existing
# sysroot behavior shared by cut/head/wc, not part of this port: e.g.
# `expand FILE -t 4' fails here while GNU permutes it), so permuted
# orderings are deliberately not raced.
set -u

EXPAND_HOST="${1:-./obj/expand_host}"
REF="${3:-${2:-${EXPAND_REF:-$(command -v expand || echo /usr/bin/expand)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs modern GNU coreutils
# (loose: diagnostics were rewritten in later releases, so error paths
# compare only whether the run succeeded, not the wording).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# --- fixtures ---------------------------------------------------------
printf 'a\tb\tc\n\tlead\n\ttwo\ttabs\nno tabs at all\n\nx\t\n' > "$TMP/tabs1.txt"
printf 'col0\tafter text\tmore\n\t\t\ttriple lead\nx\t\n' > "$TMP/mixed.txt"
printf 'a\tb\tc\td\te\tf\tg\th\n' > "$TMP/many.txt"
printf '\t\t\t\tx\n' > "$TMP/over.txt"
printf 'ab\tcd\nef\n' > "$TMP/huge.txt"
printf 'one\tTwo\tTHREE\tfour\n' > "$TMP/case.txt"
printf 'no tabs here\njust text\n' > "$TMP/notabs.txt"
printf 'a\tb\tc' > "$TMP/noeol.txt"
printf 'x\ty' > "$TMP/noeol2.txt"
printf 'a\tb\r\nc\td\r\n' > "$TMP/cr.txt"
printf 'pre\r\tpost\r\n' > "$TMP/crmid.txt"
printf '\n\n\n' > "$TMP/blank.txt"
printf '' > "$TMP/empty.txt"
printf '\xff\t\x80\tA\n\xfe\xfd\tB\n' > "$TMP/hi.txt"
printf 'a\x08\tb\n\x01\t\x07x\n' > "$TMP/binish.txt"
python3 - <<'PY' > "$TMP/big.txt"
import random
random.seed(23)
for i in range(2000):
    chars = []
    for _ in range(random.randint(0, 12)):
        chars.append(random.choice("abZ09 \t\t,.-"))
    print("".join(chars))
PY
printf 's\tt1\n\tlead2\nlast\t\n' > "$TMP/stdin.txt"   # always the stdin content
mkdir -p "$TMP/adir"                                    # a directory (read error)

# Run a case for one implementation.
#   $1 = args string (word-split with read -r, so a quoted " " survives;
#        __SP__ stands in for a single-space argument and __EMPTY__ for an
#        empty one);
#   $2 = operand spec: "-" = no FILE operands at all (stdin is stdin.txt),
#        otherwise a comma-separated list of operands appended after the
#        options, where the token "-" means a literal "-" argument and any
#        other token is the file $TMP/<token> (an optional "f:" prefix is
#        accepted and ignored); stdin is always redirected from stdin.txt
#        so a "-" operand has real data and a second "-" reads EOF;
#   $3 = "hb" or "ref";
#   $4 = extra env assignment(s) for env(1), e.g. _POSIX2_VERSION=0
#        (default: -u _POSIX2_VERSION, so an ambient value cannot skew a run).
run_one() {
    local args_str="$1" spec="$2" impl="$3" envspec="${4:--u _POSIX2_VERSION}"
    local args=() operands=() tok
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
    done
    if [ "$spec" != "-" ]; then
        local IFS=','
        for tok in $spec; do
            if [ "$tok" = "-" ]; then
                operands+=("-")
            else
                operands+=("$TMP/${tok#f:}")
            fi
        done
    fi
    local bin="$EXPAND_HOST"
    [ "$impl" = "ref" ] && bin="$REF"
    # shellcheck disable=SC2086
    env $envspec bash -c 'exec -a expand "$0" "$@"' "$bin" \
        "${args[@]}" "${operands[@]}" \
        <"$TMP/stdin.txt" >"$TMP/out" 2>"$TMP/err"
    echo "$?" > "$TMP/rc"
}

cmp_case() {  # $1 = args string; $2 = operand spec; $3 = extra env (optional)
    local args_str="$1" spec="$2" envspec="${3:--u _POSIX2_VERSION}"
    N=$((N+1))
    run_one "$args_str" "$spec" hb "$envspec"
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$spec" ref "$envspec"
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: expand $args_str ($spec)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: expand $args_str ($spec)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && [ "$rc_hb" != "0" ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): expand $args_str ($spec)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# --- default 8-column tabbing ----------------------------------------
for f in tabs1 mixed many over case noeol noeol2 cr crmid blank empty hi binish big notabs; do
    cmp_case "" "f:$f"
done

# --- -t/--tabs single sizes ------------------------------------------
for size in 1 2 3 4 5 8 16 1000; do
    cmp_case "-t $size" "f:tabs1"
    cmp_case "-t$size" "f:many"
done
cmp_case "-t 4" "f:mixed"
cmp_case "-t 4" "f:over"
cmp_case "-t 12" "f:big"
cmp_case "-t 4" "f:cr"
cmp_case "-t 4" "f:hi"
cmp_case "-t 2" "f:binish"
cmp_case "-t 1" "f:blank"
cmp_case "-t 4" "f:noeol"
cmp_case "-t 4" "f:empty"
cmp_case "-t 1000" "f:notabs"
cmp_case "--tabs=4" "f:tabs1"
cmp_case "--tabs=8" "f:many"
cmp_case "--tabs=1" "f:over"

# --- explicit tab-stop lists ----------------------------------------
for lst in 4,8 1,4 1,4,8 2,4,8,16 3,7 1,2,3,4,5 4,8,12,16 2,3; do
    cmp_case "-t $lst" "f:tabs1"
    cmp_case "-t $lst" "f:many"
done
cmp_case "-t 4,8" "f:over"
cmp_case "-t 2,4,8" "f:mixed"
cmp_case "-t 2,4,8" "f:cr"
cmp_case "-t 2,4,8" "f:crmid"
cmp_case "-t 4,8" "f:hi"
cmp_case "-t 1,2" "f:binish"
cmp_case "-t 4,8" "f:big"
cmp_case "-t 4,8" "f:blank"
cmp_case "-t 4,8" "f:noeol2"
cmp_case "-t 4,8" "f:notabs"
cmp_case "--tabs=4,8" "f:tabs1"
cmp_case "--tabs=1,4,8,16" "f:many"
# empty elements and blanks are separators in the 2.1 list grammar
cmp_case "-t 4,,8" "f:tabs1"
cmp_case "-t ,4,8" "f:tabs1"
cmp_case "-t 4,8," "f:tabs1"
cmp_case "-t __SP__4__SP__,__SP__8__SP__" "f:tabs1"
cmp_case "-t __SP__" "f:tabs1"
cmp_case "-t __EMPTY__" "f:tabs1"
cmp_case "--tabs=__EMPTY__" "f:tabs1"
cmp_case "-t ,,," "f:tabs1"
cmp_case "-t 4 8" "f:tabs1"
cmp_case "-t 1  4" "f:many"

# --- -i / --initial ---------------------------------------------------
for size in "-i" "-i -t 4" "-i -t 1" "-i -t 4,8" "--initial" "--initial --tabs=8" "-i -t 2,4,8" "-it 4"; do
    cmp_case "$size" "f:mixed"
done
cmp_case "-i" "f:tabs1"
cmp_case "-i -t 4" "f:over"
cmp_case "-i -t 4,8" "f:many"
cmp_case "-i" "f:cr"
cmp_case "-i" "f:crmid"
cmp_case "-i" "f:hi"
cmp_case "-i" "f:big"
cmp_case "-i" "f:blank"
cmp_case "-i -t 4" "f:noeol"
cmp_case "-i" "f:notabs"

# --- stdin (no FILE, "-", repeated "-") ------------------------------
cmp_case "" "-"
cmp_case "-t 4" "-"
cmp_case "-t 4,8" "-"
cmp_case "-i" "-"
cmp_case "-i -t 4" "-"
cmp_case "" "f:-"
cmp_case "-t 4" "f:-"
cmp_case "--" "f:tabs1"
cmp_case "-t 4 --" "f:-"           # "--" ends option scanning, then stdin
cmp_case "-i --initial" "f:mixed"
cmp_case "-i -t 4,8" "f:-"
cmp_case "-t 4" "f:-,f:notabs.txt"
cmp_case "-t 4" "f:tabs1.txt,f:-"
cmp_case "-t 4" "f:-,f:-"

# --- FILE operands / multiple files / read errors --------------------
cmp_case "-t 4" "f:tabs1,many,empty,over"
cmp_case "-t 4,8" "f:notabs,blank,cr"
cmp_case "" "f:tabs1,many"
cmp_case "-t 4" "f:nonexistent_xyz.txt"
cmp_case "-t 4" "f:nonexistent_xyz.txt,f:tabs1"
cmp_case "-t 4" "f:tabs1,f:nonexistent_xyz.txt"
cmp_case "-t 4" "f:tabs1,f:nonexistent_xyz.txt,f:many"
cmp_case "-t 4" "f:adir"
cmp_case "-t 4" "f:adir,f:tabs1"
cmp_case "" "f:empty,empty,empty"

# --- error paths ------------------------------------------------------
cmp_case "-t abc" "f:tabs1"
cmp_case "-t 0" "f:tabs1"
cmp_case "-t 4,0" "f:tabs1"
cmp_case "-t 0,4" "f:tabs1"
cmp_case "-t -1" "f:tabs1"
cmp_case "-t +2" "f:tabs1"           # +N//N styles are NOT a 2.1 feature
cmp_case "-t /2" "f:tabs1"
cmp_case "-t 4x" "f:tabs1"
cmp_case "-t x4" "f:tabs1"
cmp_case "-t 4,x" "f:tabs1"
cmp_case "-t 8,4" "f:tabs1"          # descending
cmp_case "-t 4,4" "f:tabs1"          # degenerate
cmp_case "-t 8  4" "f:tabs1"         # blank-separated, descending
cmp_case "-t 1,2,2" "f:many"
cmp_case "-t 99999999999999999999" "f:tabs1"
cmp_case "-t 4294967296" "f:tabs1"   # wraps to 0 in the 2.1 int arithmetic
cmp_case "-t 2147483648" "f:tabs1"   # wraps negative -> not ascending
cmp_case "-t 4,2000000000" "f:huge"  # a huge stop that is never reached
cmp_case "-t 2000000000" "f:notabs"  # huge single size, no tab to expand
cmp_case "-t" "f:tabs1"              # missing option argument
cmp_case "-t" "-"
cmp_case "--tabs" "f:tabs1"
cmp_case "---tabs=4" "f:tabs1"
cmp_case "-z" "f:tabs1"              # invalid option
cmp_case "--bogus" "f:tabs1"         # unrecognized long option
cmp_case "--tab=4" "f:tabs1"         # unique-prefix abbreviation
cmp_case "--ini" "f:mixed"           # unique-prefix abbreviation
cmp_case "-i -t abc" "f:mixed"
cmp_case "-t abc -i" "f:mixed"

# --- obsolete -LIST tab syntax (2.1's older form) ---------------------
# Both sides accept it when _POSIX2_VERSION=0 (posixver.c's getenv path);
# see the header note.  Without it, our port still accepts (sysroot default
# 0) while the 2.1-on-glibc and modern-coreutils references reject, so the
# default-context difference is asserted separately below.
for obs in "-4" "-8" "-16" "-4,8" "-1,4,8" "-2" "-1"; do
    cmp_case "$obs" "f:tabs1" "_POSIX2_VERSION=0"
done
cmp_case "-4" "f:many" "_POSIX2_VERSION=0"
cmp_case "-8" "f:big" "_POSIX2_VERSION=0"
cmp_case "-4,8" "f:over" "_POSIX2_VERSION=0"
cmp_case "-8" "-" "_POSIX2_VERSION=0"
cmp_case "-0" "f:tabs1" "_POSIX2_VERSION=0"    # tab size cannot be 0
cmp_case "-8,4" "f:tabs1" "_POSIX2_VERSION=0"  # descending
cmp_case "-," "f:tabs1" "_POSIX2_VERSION=0"    # separator only -> default 8
cmp_case "-4,8" "f:big" "_POSIX2_VERSION=0"
cmp_case "-t 4 -i" "f:mixed" "_POSIX2_VERSION=0"
cmp_case "-8 -i" "f:mixed" "_POSIX2_VERSION=0"

# Port-convention check (not a reference race): with the ambient
# environment our port accepts the obsolete form and it must be exactly
# equivalent to the modern -t spelling; the references reject it outright.
N=$((N+1))
bash -c 'exec -a expand "$0" "$@"' "$EXPAND_HOST" -4,8 "$TMP/many.txt" >"$TMP/obs" 2>"$TMP/obs_err"
rc_obs=$?
bash -c 'exec -a expand "$0" "$@"' "$EXPAND_HOST" -t 4,8 "$TMP/many.txt" >"$TMP/mod" 2>/dev/null
if [ "$rc_obs" != 0 ] || ! SAME_OUT "$TMP/obs" "$TMP/mod"; then
    echo "FAIL: port's obsolete -4,8 is not equivalent to -t 4,8 (rc=$rc_obs)"
    fail=1
fi
bash -c 'exec -a expand "$0" "$@"' "$REF" -4,8 "$TMP/many.txt" >"$TMP/refobs" 2>/dev/null
if [ -s "$TMP/refobs" ]; then
    echo "note: reference accepts -4,8 too (its POSIX-200112 gate is off/absent)"
fi

# --- usage/version text (strict only: 2.1's text, not coreutils') -----
if [ "$REF_MODE" = strict ]; then
    for a in "--help" "--version"; do
        N=$((N+1))
        bash -c 'exec -a expand "$0" "$@"' "$EXPAND_HOST" "$a" >"$TMP/oh" 2>&1
        rc_hb=$?
        bash -c 'exec -a expand "$0" "$@"' "$REF" "$a" >"$TMP/out" 2>&1
        rc_ref=$?
        if [ "$rc_hb" != "$rc_ref" ] || ! SAME_OUT "$TMP/oh" "$TMP/out"; then
            echo "FAIL: $a text/status differs (rc $rc_hb vs $rc_ref)"
            diff "$TMP/oh" "$TMP/out" | head -5
            fail=1
        fi
    done
else
    echo "note: skipping --help/--version text cases (2.1 text vs modern coreutils)"
fi

if [ "$fail" = 0 ]; then
    echo "expand parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "expand parity: FAIL (of $N cases)"
exit 1
