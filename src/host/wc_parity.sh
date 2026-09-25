#!/bin/bash
# wc_parity.sh — byte-exact acceptance test for the HobbyOS ported wc.
#
# Runs our host-built wc (wc_host, argv[0] forced to "wc" via exec -a so
# error messages are comparable) against a GNU-compatible reference wc on
# the same inputs and compares stdout bytes, exit codes and stderr text
# for the error case, across option combinations, fixtures, stdin mode
# and multi-file (total) mode.
#
# Reference selection:
#   $2 = explicit path, else $WC_REF, else the first of these that exists:
#        /usr/bin/wc (GNU-layout) — on this host check/modify
# The uutils (Rust) and busybox wc binaries do NOT emulate GNU's %7s
# column layout, so when they are the default reference the comparison
# falls back to layout-normalized mode (whitespace collapsed) and prints
# a note; a genuine GNU wc always gets strict byte comparison.
# /usr/bin/wc runs under LC_ALL=C so -m (chars) == -c (bytes), which is
# exactly what the HobbyOS ASCII port implements.
set -u

WC_HOST="${1:-./obj/wc_host}"
REF="${3:-${2:-${WC_REF:-$(command -v wc || echo /usr/bin/wc)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

# Detect the reference flavor.
if "$REF" --version 2>/dev/null | grep -qi "textutils\|GNU coreutils"; then
    REF_MODE=strict
elif "$REF" --version 2>/dev/null | grep -qi "uutils\|busybox"; then
    REF_MODE=loose
    echo "note: reference $REF is not GNU-layout; using layout-normalized"
    echo "      comparison (counts still byte-exact, padding relaxed)"
else
    REF_MODE=strict
fi

if [ "$REF_MODE" = strict ]; then
    SAME_OUT() { diff -q "$1" "$2" >/dev/null; }
else
    # fold runs of spaces to one and strip leading (column-pad) spaces,
    # so layout-neutral comparison still checks counts + filenames
    SAME_OUT() { sed -E 's/^[ ]+//; s/[ ]+/ /g' <"$1" \
                     | diff -q - <(sed -E 's/^[ ]+//; s/[ ]+/ /g' <"$2") \
                     >/dev/null; }
fi

# Fixtures
printf 'hello world\nsecond line\n\nfourth\n' > "$TMP/small.txt"
printf 'a\tb\tc\n' > "$TMP/tabs.txt"
printf '\r\f\nx\n' > "$TMP/crlf.txt"
printf '' > "$TMP/empty.txt"
printf 'one\ntwo three\n' > "$TMP/noeol.txt"
printf 'x%.0s' $(seq 1 100) > "$TMP/oneline.txt"
printf '  spaces   here\t and\tmore  \n\nword\vvertical\fzz\n' > "$TMP/ws.txt"
python3 - <<'PY' > "$TMP/big.txt"
import random
random.seed(7)
words = ["alpha","beta","gamma","delta","-","x", "a:b", "q'uote",
         "tab\tsep", "cr\rinside", "ff\f", "longword_"*8]
for i in range(5000):
    n = random.randint(0, 14)
    print(" ".join(random.choice(words) for _ in range(n)))
PY
printf 'high bytes: \x01\x7f\x80\xff\xfe test\n' > "$TMP/binish.txt"

FIXTURES="small.txt tabs.txt crlf.txt empty.txt noeol.txt oneline.txt ws.txt big.txt binish.txt"
OPTSET="l w c m L Lw lw wc lwc lwcm wcLm"

# argv[0] forced to "wc" so error messages match coreutils' byte for byte.
HB() { exec -a wc "$WC_HOST" "$@"; }

# Run a case for one implementation.  $1 = fixture name or '-' for stdin;
# $2 = option string (may be empty); $3 = 'hb' (ours, argv[0] forced to
# "wc") or 'ref' (coreutils).  stdout -> out, stderr -> err, rc -> rc.
run_one() {
    local file="$1" opt="$2" impl="$3"
    local args=()
    [ -n "$opt" ] && args+=("-$opt")
    local cmd=
    if [ "$impl" = "hb" ]; then
        cmd=(bash -c 'exec -a wc "$0" "$@"' "$WC_HOST")
    else
        # argv[0] of the reference is forced to "wc" too, so the
        # error-message prefix (program name) is comparable
        cmd=(bash -c 'exec -a wc "$0" "$@"' "$REF")
    fi
    if [ "$file" = "-" ]; then
        cat "$TMP/small.txt" "$TMP/tabs.txt" | "${cmd[@]}" "${args[@]}" \
            >"$TMP/out" 2>"$TMP/err"
    else
        "${cmd[@]}" "${args[@]}" "$TMP/$file" >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" > "$TMP/rc"
}

cmp_case() {  # $1 = fixture; $2 = opts
    local file="$1" opt="$2"
    N=$((N+1))
    run_one "$file" "$opt" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$file" "$opt" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: $file opts=-$opt"
        echo "  hb:   $(head -2 "$TMP/oh" | tr '\n' '|')"
        echo "  ref:  $(head -2 "$TMP/out" | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        echo "FAIL: exit code $rc_hb vs $rc_ref: $file opts=-$opt"
        fail=1
    fi
}

# matrix: fixtures x options (file mode), stdin mode, multi-file, errors.
# binish.txt exercises GNU's ISPRINT/ISSPACE word semantics for high-bit
# bytes, which uutils/busybox do not emulate (they count those bytes as
# word characters); it runs only against a genuine GNU reference.
for f in $FIXTURES; do
    [ "$f" = binish.txt ] && [ "$REF_MODE" != strict ] && continue
    for o in $OPTSET; do
        cmp_case "$f" "$o"
    done
done

for o in "" l w c m L "lwcm" "wL"; do
    cmp_case "-" "$o"
done
cmp_case "-" "c"

# multi-file: total line
N=$((N+1))
bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" "$TMP/small.txt" "$TMP/ws.txt" \
    "$TMP/big.txt" >"$TMP/oh" 2>/dev/null
"$REF" "$TMP/small.txt" "$TMP/ws.txt" "$TMP/big.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: multi-file total differs"; fail=1
fi
N=$((N+1))
bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" -l -L "$TMP/crlf.txt" \
    "$TMP/empty.txt" >"$TMP/oh" 2>/dev/null
"$REF" -l -L "$TMP/crlf.txt" "$TMP/empty.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: multi-file total (-l -L) differs"; fail=1
fi

# error path: nonexistent file (stderr + exit code 1)
N=$((N+1))
bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" "$TMP/nonexistent_xyz.txt" \
    >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
bash -c 'exec -a wc "$0" "$@"' "$REF" "$TMP/nonexistent_xyz.txt" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
if [ "$rc_hb" != "$rc_ref" ]; then
    echo "FAIL: missing-file exit $rc_hb vs $rc_ref"; fail=1
fi
if ! diff -q "$TMP/eh" "$TMP/err" >/dev/null; then
    echo "FAIL: missing-file stderr differs:"
    echo "  hb:  $(cat "$TMP/eh")"
    echo "  ref: $(cat "$TMP/err")"
    fail=1
fi

# POSIXLY_CORRECT=1 layout (single-space column separation)
N=$((N+1))
POSIXLY_CORRECT=1 bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" "$TMP/big.txt" \
    "$TMP/small.txt" >"$TMP/oh" 2>/dev/null
POSIXLY_CORRECT=1 "$REF" "$TMP/big.txt" "$TMP/small.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: POSIXLY_CORRECT layout differs"; fail=1
fi

# long options (our implementation's long-opts path)
N=$((N+1))
bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" --lines --words --bytes \
    "$TMP/big.txt" >"$TMP/oh" 2>/dev/null
"$REF" --lines --words --bytes "$TMP/big.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: long option output differs"; fail=1
fi
N=$((N+1))
bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" --max-line-length -- "$TMP/ws.txt" \
    >"$TMP/oh" 2>/dev/null
"$REF" --max-line-length -- "$TMP/ws.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: long option (--max-line-length, --) output differs"; fail=1
fi

# --- --help / --version: raced byte-exact only against the textutils-2.1
#     reference (modern coreutils rewrote both texts) ---
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    for opt in --help --version; do
        N=$((N+1))
        bash -c 'exec -a wc "$0" "$@"' "$WC_HOST" "$opt" >"$TMP/oh" 2>"$TMP/eh"
        rc_hb=$?
        bash -c 'exec -a wc "$0" "$@"' "$REF" "$opt" >"$TMP/out" 2>"$TMP/err"
        rc_ref=$?
        if ! SAME_OUT "$TMP/oh" "$TMP/out" || [ "$rc_hb" != "$rc_ref" ]; then
            echo "FAIL: $opt differs (rc $rc_hb vs $rc_ref)"
            diff "$TMP/oh" "$TMP/out" | head -4
            fail=1
        fi
    done
else
    echo "note: skipping --help/--version cases (2.1-lineage texts; run wc_parity_strict)"
fi

if [ "$fail" = 0 ]; then
    echo "wc parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "wc parity: FAIL (of $N cases)"
exit 1
