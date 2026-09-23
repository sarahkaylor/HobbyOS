#!/bin/bash
# head_parity.sh — byte-exact acceptance test for the HobbyOS ported head.
#
# Runs our host-built head (head_host, argv[0] forced to "head" so error
# messages are comparable) against a GNU-compatible reference head on the
# same inputs and compares stdout bytes, exit codes and stderr for the
# error case, across option combinations, fixtures, stdin mode,
# multi-file (header) mode and the old -Nc/-Nb/-Nk syntax.
#
# head's output is plain content, so no layout normalization is needed:
# every comparison is byte-for-byte against the reference.
#
# Reference selection:
#   $2 = explicit path, else $HEAD_REF, else /usr/bin/head.
# With a genuine GNU reference (textutils-2.1 via build_tu21_head_ref.sh
# or coreutils) the comparison is strict by construction.
set -u

HEAD_HOST="${1:-./obj/head_host}"
REF="${2:-${HEAD_REF:-$(command -v head || echo /usr/bin/head)}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

if "$REF" --version 2>/dev/null | grep -qi "uutils\|busybox"; then
    REF_GNU=0
    echo "note: reference $REF is not GNU; using modern-coreutils wording"
    echo "      and skipping old-syntax (-Nl) cases it does not support"
else
    REF_GNU=1
fi

# Fixtures
printf 'hello world\nsecond line\n\nfourth\n' > "$TMP/small.txt"
printf 'a\tb\tc\n' > "$TMP/tabs.txt"
printf 'one\ntwo three\n' > "$TMP/noeol.txt"
printf '' > "$TMP/empty.txt"
python3 - <<'PY' > "$TMP/big.txt"
import random
random.seed(7)
words = ["alpha","beta","gamma","delta","-","x", "a:b", "q'uote",
         "tab\tsep", "longword_"*8]
for i in range(5000):
    n = random.randint(0, 14)
    print(" ".join(random.choice(words) for _ in range(n)))
PY
# a file with no newline at all in the first line, longer than BUFSIZE
python3 - <<'PY' > "$TMP/longline.txt"
print("x" * 9000, end="")
print("\nsecond line\nthird\n")
PY

FIXTURES="small.txt tabs.txt noeol.txt empty.txt big.txt longline.txt"

# argv[0] forced to "head" so error-message prefixes compare byte for byte.
run_one() {  # $1 = file or '-'; $2 = full option string; $3 = hb|ref
    local file="$1" opt="$2" impl="$3"
    local cmd
    if [ "$impl" = "hb" ]; then
        cmd=(bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST")
    else
        # _POSIX2_VERSION < 200112: textutils-2.1's head deprecation check
        # (posix2_version()) must not reject the old -Nc syntax, matching
        # both modern coreutils and our port
        cmd=(env _POSIX2_VERSION=199901 \
             bash -c 'exec -a head "$0" "$@"' "$REF")
    fi
    # shellcheck disable=SC2086
    if [ "$file" = "-" ]; then
        cat "$TMP/small.txt" "$TMP/tabs.txt" | "${cmd[@]}" $opt \
            >"$TMP/out" 2>"$TMP/err"
    else
        "${cmd[@]}" $opt "$TMP/$file" >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" > "$TMP/rc"
}

cmp_case() {  # $1 = fixture; $2 = option string (word-split)
    local file="$1" opt="$2"
    N=$((N+1))
    run_one "$file" "$opt" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$file" "$opt" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! diff -q "$TMP/oh" "$TMP/out" >/dev/null; then
        echo "FAIL: stdout differs: $file opts='$opt'"
        echo "  hb:   $(head -2 "$TMP/oh" | tr '\n' '|')"
        echo "  ref:  $(head -2 "$TMP/out" | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        echo "FAIL: exit code $rc_hb vs $rc_ref: $file opts='$opt'"
        fail=1
    fi
}

# main matrix: defaults, -n/-c with counts and suffix multipliers,
# BUFSIZE (4096) boundaries, -q/-v, multi-file headers.
for f in $FIXTURES; do
    for o in "" \
             "-n 1" "-n 2" "-n 9999" "-n 0" \
             "-c 1" "-c 5" "-c 100" "-c 4096" "-c 4097" "-c 9001" \
             "-n 3 -c 7" \
             "-q -n 4" "-v -n 2"; do
        cmp_case "$f" "$o"
    done
    # old head -Nc/-Nb/-Nk/-Nl syntax: only genuine GNU references
    # support it (uutils rejects it)
    if [ "$REF_GNU" = 1 ]; then
        for o in "-1" "-3c" "-0" "-5b" "-2k" "-10l"; do
            cmp_case "$f" "$o"
        done
    fi
done

# stdin mode
for o in "" "-n 2" "-c 11" "-v -n 1"; do
    cmp_case "-" "$o"
done

# multi-file: headers + blank separation; -q suppresses them
N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" "$TMP/small.txt" "$TMP/big.txt" \
    >"$TMP/oh" 2>/dev/null
bash -c 'exec -a head "$0" "$@"' "$REF" "$TMP/small.txt" "$TMP/big.txt" \
    >"$TMP/out" 2>/dev/null
if ! diff -q "$TMP/oh" "$TMP/out" >/dev/null; then
    echo "FAIL: multi-file headers differ"; fail=1
fi

N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" -q -n 5 "$TMP/small.txt" \
    "$TMP/tabs.txt" >"$TMP/oh" 2>/dev/null
bash -c 'exec -a head "$0" "$@"' "$REF" -q -n 5 "$TMP/small.txt" \
    "$TMP/tabs.txt" >"$TMP/out" 2>/dev/null
if ! diff -q "$TMP/oh" "$TMP/out" >/dev/null; then
    echo "FAIL: -q multi-file differs"; fail=1
fi

N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" -v -n 2 "$TMP/small.txt" \
    >"$TMP/oh" 2>/dev/null
bash -c 'exec -a head "$0" "$@"' "$REF" -v -n 2 "$TMP/small.txt" \
    >"$TMP/out" 2>/dev/null
if ! diff -q "$TMP/oh" "$TMP/out" >/dev/null; then
    echo "FAIL: -v header differs"; fail=1
fi

# long options
N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" --bytes=11 --lines=2 \
    "$TMP/small.txt" >"$TMP/oh" 2>/dev/null
bash -c 'exec -a head "$0" "$@"' "$REF" --bytes=11 --lines=2 \
    "$TMP/small.txt" >"$TMP/out" 2>/dev/null
if ! diff -q "$TMP/oh" "$TMP/out" >/dev/null; then
    echo "FAIL: long option output differs"; fail=1
fi

N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" --lines 0 "$TMP/empty.txt" \
    >"$TMP/oh" 2>/dev/null
bash -c 'exec -a head "$0" "$@"' "$REF" --lines 0 "$TMP/empty.txt" \
    >"$TMP/out" 2>/dev/null
if ! diff -q "$TMP/oh" "$TMP/out" >/dev/null; then
    echo "FAIL: -n 0 empty output differs"; fail=1
fi

# error path: nonexistent file (stderr + exit 1) — stderr wording is
# compared byte-exact only against genuine GNU references (uutils uses
# the modern "cannot open X for reading" phrasing)
N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" "$TMP/nonexistent_xyz.txt" \
    >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
bash -c 'exec -a head "$0" "$@"' "$REF" "$TMP/nonexistent_xyz.txt" \
    >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
if [ "$rc_hb" != "$rc_ref" ]; then
    echo "FAIL: missing-file exit $rc_hb vs $rc_ref"; fail=1
fi
if [ "$REF_GNU" = 1 ] && ! diff -q "$TMP/eh" "$TMP/err" >/dev/null; then
    echo "FAIL: missing-file stderr differs:"
    echo "  hb:  $(cat "$TMP/eh")"
    echo "  ref: $(cat "$TMP/err")"
    fail=1
fi

# error path: invalid number
N=$((N+1))
bash -c 'exec -a head "$0" "$@"' "$HEAD_HOST" -n junk "$TMP/small.txt" \
    >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
bash -c 'exec -a head "$0" "$@"' "$REF" -n junk "$TMP/small.txt" \
    >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
if [ "$rc_hb" != "$rc_ref" ]; then
    echo "FAIL: invalid-number exit $rc_hb vs $rc_ref"; fail=1
fi

if [ "$fail" = 0 ]; then
    echo "head parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "head parity: FAIL (of $N cases)"
exit 1
