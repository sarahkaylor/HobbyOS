#!/bin/bash
# tail_parity.sh — byte-exact acceptance test for the HobbyOS ported tail.
#
# Runs our host-built tail (tail_host, argv[0] forced to "tail" so error
# messages are comparable) against a GNU tail reference (default: the
# system's /usr/bin/tail; pass a genuine GNU reference, e.g. one built by
# build_tu21_tail_ref.sh, as the second argument).  Output is plain file
# content plus GNU header banners, so byte-exact comparison is meaningful.
#
# Old-syntax (-N) and the error-message wording are only compared against
# genuine GNU references (uutils/busybox reject or reword them), exactly
# like the head harness.
set -u

TAIL_HOST="${1:-./obj/tail_host}"
REF="${2:-/usr/bin/tail}"
TMP="$(mktemp -d /tmp/tailparity.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
FAIL=0
N=0

REF_GNU=1
if "$REF" --version 2>/dev/null | grep -qi "uutils\|busybox"; then
    REF_GNU=0
    echo "note: reference is not GNU; skipping old-syntax and stderr wording"
fi

# Fixtures: (name, size, lines, big-enough-for byte tests)
printf 'alpha\nbeta\ngamma\ndelta\nepsilon\n' > "$TMP/small.txt"   # 5 lines
printf 'a\tb\nc\td\n' > "$TMP/tabs.txt"                             # 2 lines
: > "$TMP/empty.txt"
printf 'single-line-no-eol' > "$TMP/noeol.txt"
seq 1 300 > "$TMP/big.txt"                                          # 300 lines
python3 -c "print('x'*5000)" > "$TMP/longline.txt"                  # 1 line, >4096 bytes
python3 -c "print(''.join('line %05d\\n' % i for i in range(1200)))" > "$TMP/buffer.txt" # >4096 bytes, many lines

FIXTURES="small.txt tabs.txt empty.txt noeol.txt big.txt longline.txt buffer.txt"

# run_one <impl hb|ref> <file> <option-string> ; sets rc, oh/eh
run_one() {
    local impl="$1" file="$2" optstr="$3"
    local -a cmd
    if [ "$impl" = hb ]; then
        cmd=(bash -c 'exec -a tail "$0" "$@"' "$TAIL_HOST")
    else
        cmd=(env _POSIX2_VERSION=199901 \
             bash -c 'exec -a tail "$0" "$@"' "$REF")
    fi
    if [ "$file" = "-" ]; then
        printf 'stdin-line-1\nstdin-line-2\nstdin-line-3\n' | \
            "${cmd[@]}" $optstr -  >"$TMP/oh" 2>"$TMP/eh"
    else
        "${cmd[@]}" $optstr "$TMP/$file" >"$TMP/oh" 2>"$TMP/eh"
    fi
    rc=$?
}

cmp_case() {
    local file="$1" optstr="$2" label="$3"
    local bad=0
    N=$((N+1))
    run_one hb "$file" "$optstr"
    local rc_hb=$?; cp "$TMP/oh" "$TMP/hb_out"; cp "$TMP/eh" "$TMP/hb_err"
    run_one ref "$file" "$optstr"
    local rc_ref=$?
    if [ "$rc_hb" != "$rc_ref" ]; then
        echo "FAIL: exit $rc_hb vs $rc_ref: $label '$optstr' $file"
        bad=1
    elif ! diff -q "$TMP/hb_out" "$TMP/oh" >/dev/null; then
        echo "FAIL: stdout differs: $label '$optstr' $file"
        bad=1
    fi
    if [ $bad = 1 ]; then
        FAIL=1
        echo "  hb:  [$(cat "$TMP/hb_out" | head -3 | tr '\n' '|')]"
        echo "  ref: [$(cat "$TMP/oh" | head -3 | tr '\n' '|')]"
    fi
}

# main matrix
for f in $FIXTURES; do
    for o in "" \
             "-n 1" "-n 3" "-n 200" "-n 0" "-n 99999" \
             "-c 1" "-c 5" "-c 100" "-c 4096" "-c 4097" "-c 9001" \
             "-c 0" "-c 99999" \
             "-n 1b" "-n 2k" "-c 1b" "-c 2k" \
             "-n +3" "-c +11" \
             "-q -n 2" "-v -n 2"; do
        cmp_case "$f" "$o" "$f"
    done
    if [ "$REF_GNU" = 1 ]; then
        for o in "-1" "-2" "-90" "-3c" "-1b" "-2k" "+2" "+3c"; do
            cmp_case "$f" "$o" "$f"
        done
    fi
done

# stdin mode
N=$((N+1)); cmp_case "-" "" "stdin-default"
N=$((N+1)); cmp_case "-" "-n 2" "stdin-n2"

# multi-file: headers + totals
for mf in "small.txt,tabs.txt" "small.txt,tabs.txt,empty.txt"; do
    for o in "" "-n 2" "-q" "-v"; do
        # expand the comma list into real file args for BOTH sides
        fileargs=""
        IFS=',' read -ra parts <<< "$mf"
        for part in "${parts[@]}"; do
            fileargs="$fileargs $TMP/$part"
        done
        N=$((N+1))
        bash -c 'exec -a tail "$0" "$@"' "$TAIL_HOST" $o $fileargs \
            >"$TMP/hb_out" 2>"$TMP/hb_err"; rc_hb=$?
        env _POSIX2_VERSION=199901 bash -c 'exec -a tail "$0" "$@"' "$REF" \
            $o $fileargs >"$TMP/oh" 2>"$TMP/err"; rc_ref=$?
        if [ "$rc_hb" != "$rc_ref" ] || ! diff -q "$TMP/hb_out" "$TMP/oh" >/dev/null; then
            echo "FAIL: multi-file '$mf' opts='$o' (exit $rc_hb vs $rc_ref)"
            FAIL=1
        fi
    done
done

# --- --help / --version: raced byte-exact only against the textutils-2.1
#     reference (modern coreutils rewrote both texts) ---
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    for opt in --help --version; do
        N=$((N+1))
        bash -c 'exec -a tail "$0" "$@"' "$TAIL_HOST" "$opt" \
            >"$TMP/hb_out" 2>"$TMP/hb_err"; rc_hb=$?
        bash -c 'exec -a tail "$0" "$@"' "$REF" "$opt" \
            >"$TMP/oh" 2>"$TMP/err"; rc_ref=$?
        if [ "$rc_hb" != "$rc_ref" ] || ! diff -q "$TMP/hb_out" "$TMP/oh" >/dev/null; then
            echo "FAIL: $opt differs (exit $rc_hb vs $rc_ref)"
            diff "$TMP/hb_out" "$TMP/oh" | head -4
            FAIL=1
        fi
    done
else
    echo "note: skipping --help/--version cases (2.1-lineage texts; run tail_parity_strict)"
fi

if [ "$FAIL" = 0 ]; then
    echo "tail parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "tail parity: FAIL (of $N cases)"
exit 1
