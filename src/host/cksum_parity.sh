#!/bin/bash
# cksum_parity.sh — byte-exact acceptance test for the HobbyOS ported cksum.
#
# Runs our host-built cksum (cksum_host, argv[0] forced to "cksum" via
# exec -a so diagnostics are comparable) against a reference on the same
# inputs and compares stdout bytes, exit codes and stderr text across
# file/stdin/multi-file modes, empty / all-256-bytes / pattern fixtures up
# to 130 KB (crossing the 64 KB BUFLEN read boundary), files with awkward
# names, the --help/--version paths and the error paths (missing file,
# directory, unreadable file, bad options).  cksum's CRC is deterministic,
# so the strict bar is a 100% byte-exact match on every case.
#
# Reference selection:
#   $2 = explicit path, else $CKSUM_REF, else /usr/bin/cksum.  The
#   textutils-2.1 original (built by build_tu21_cksum_ref.sh, or by
#   `make cksum_parity_strict`) selects STRICT mode: exit codes, stderr
#   and the --help/--version text compare byte-for-byte, because the port
#   was transcribed from that source.  A modern implementation (GNU
#   coreutils, uutils, BSD) selects LOOSE mode: only stdout bytes and the
#   success/failure class of the exit code compare, and the cases whose
#   behavior legitimately changed since 2.1 (a `-` operand no longer
#   prints the "-" name, --help/--version text, rewrite option
#   diagnostics) are skipped with a note.
set -u

CKSUM_HOST="${1:-./obj/cksum_host}"
REF="${3:-${2:-${CKSUM_REF:-$(command -v cksum || echo /usr/bin/cksum)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# --- fixtures -------------------------------------------------------------
: > "$TMP/empty.bin"
printf 'a' > "$TMP/one-nonl.bin"
printf 'a\n' > "$TMP/one-nl.bin"
printf 'hello world\n' > "$TMP/hello.txt"
printf 'alpha\nbeta\n\ngamma\ndelta' > "$TMP/lines.txt"
printf 'spaced content\n' > "$TMP/space name.txt"
printf 'dashfile\n' > "$TMP/-dash.bin"
printf 'literally named dash\n' > "$TMP/-"
printf 'nope\n' > "$TMP/unreadable.bin"
chmod 000 "$TMP/unreadable.bin"
mkdir -p "$TMP/cdir"
python3 - "$TMP" <<'PY'
import sys
d = sys.argv[1]
open(d + "/allbytes.bin", "wb").write(bytes(range(256)))
open(d + "/pattern40k.bin", "wb").write(b"0123456789abcdef" * 2500)   # 40000
open(d + "/pattern64k.bin", "wb").write(b"0123456789abcdef" * 4096)   # 65536
open(d + "/pattern65k.bin", "wb").write(b"0123456789abcdef" * 4096 + b"!")  # 65537
open(d + "/big130k.bin", "wb").write(b"AB" * 65000)                   # 130000
PY

# stdin fixture used by every case unless the case overrides it
STDIN_FIX="$TMP/empty.bin"

run_one() {  # $1 = impl (hb|ref); rest = operands
    local impl="$1"; shift
    local bin
    if [ "$impl" = hb ]; then bin="$CKSUM_HOST"; else bin="$REF"; fi
    bash -c 'exec -a cksum "$0" "$@"' "$bin" "$@" \
        <"$STDIN_FIX" >"$TMP/out.$impl" 2>"$TMP/err.$impl"
    echo $? > "$TMP/rc.$impl"
}

cmp_case() {  # rest = operands; stdin comes from $STDIN_FIX
    N=$((N+1))
    run_one hb "$@"
    local rc_hb; rc_hb=$(<"$TMP/rc.hb")
    mv "$TMP/out.hb" "$TMP/oh"; mv "$TMP/err.hb" "$TMP/eh"
    run_one ref "$@"
    local rc_ref; rc_ref=$(<"$TMP/rc.ref")

    if ! SAME_OUT "$TMP/oh" "$TMP/out.ref"; then
        echo "FAIL: stdout differs: cksum $* [stdin=$STDIN_FIX]"
        echo "  hb:  $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref: $(od -c "$TMP/out.ref" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: cksum $*"
            fail=1
        fi
    fi
    # The 2.1 reference is byte-comparable on diagnostics too (the port
    # was transcribed from it); modern implementations rewrote them.
    if [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err.ref"; then
        echo "FAIL: stderr differs (rc=$rc_hb): cksum $*"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err.ref")"
        fail=1
    fi
}

skip_case() {  # $1 = reason-ish label; rest = operands
    echo "note: skipping (strict-only): cksum $* — $1"
}

# --- file operands --------------------------------------------------------
cmp_case "$TMP/empty.bin"
cmp_case "$TMP/one-nonl.bin"
cmp_case "$TMP/one-nl.bin"
cmp_case "$TMP/hello.txt"
cmp_case "$TMP/lines.txt"
cmp_case "$TMP/allbytes.bin"
cmp_case "$TMP/pattern40k.bin"
cmp_case "$TMP/pattern64k.bin"
cmp_case "$TMP/pattern65k.bin"
cmp_case "$TMP/big130k.bin"
cmp_case "$TMP/space name.txt"
cmp_case "$TMP/-dash.bin"
cmp_case "$TMP/-"
cmp_case "$TMP/hello.txt" "$TMP/hello.txt"
cmp_case "$TMP/empty.bin" "$TMP/hello.txt" "$TMP/allbytes.bin"
cmp_case "$TMP/lines.txt" "$TMP/pattern40k.bin"
cmp_case /dev/null
cmp_case -- "$TMP/hello.txt"

# --- error paths: multiple files, missing file, directory, unreadable -----
cmp_case "$TMP/hello.txt" "$TMP/missing.txt" "$TMP/one-nl.bin"
cmp_case "$TMP/missing.txt" "$TMP/hello.txt"
cmp_case "$TMP/missing.txt"
cmp_case "$TMP/cdir"
cmp_case "$TMP/unreadable.bin"

# --- bad options (rc + stderr wording) ------------------------------------
cmp_case -x
cmp_case --bogus

# --- stdin (no operands) --------------------------------------------------
STDIN_FIX="$TMP/empty.bin";         cmp_case
STDIN_FIX="$TMP/hello.txt";         cmp_case
STDIN_FIX="$TMP/allbytes.bin";      cmp_case
STDIN_FIX="$TMP/pattern40k.bin";    cmp_case
STDIN_FIX="$TMP/pattern65k.bin";    cmp_case

# --- `-` operands / --help / --version ------------------------------------
# 2.1 prints the "-" name for a "-" operand; GNU coreutils also does, but
# uutils coreutils (this machine's /usr/bin/cksum) does not, and the
# --help/--version text is per-project.  These are byte-compared only
# against the 2.1 reference, which is the source this port was transcribed
# from.
if [ "$REF_MODE" = strict ]; then
    STDIN_FIX="$TMP/empty.bin";      cmp_case -
    STDIN_FIX="$TMP/hello.txt";      cmp_case -
    STDIN_FIX="$TMP/pattern40k.bin"; cmp_case -
    STDIN_FIX="$TMP/hello.txt";      cmp_case - -
    STDIN_FIX="$TMP/hello.txt";      cmp_case "$TMP/hello.txt" -
    STDIN_FIX="$TMP/empty.bin";      cmp_case - "$TMP/hello.txt"
    STDIN_FIX="$TMP/empty.bin";      cmp_case --help
    STDIN_FIX="$TMP/empty.bin";      cmp_case --version
    STDIN_FIX="$TMP/empty.bin";      cmp_case --hel
    STDIN_FIX="$TMP/empty.bin";      cmp_case --help "$TMP/hello.txt"
else
    skip_case "uutils/GNU differ on the '-' operand name" -
    STDIN_FIX="$TMP/hello.txt";      skip_case "uutils/GNU differ on the '-' operand name" -
    STDIN_FIX="$TMP/pattern40k.bin"; skip_case "uutils/GNU differ on the '-' operand name" -
    STDIN_FIX="$TMP/hello.txt";      skip_case "uutils/GNU differ on the '-' operand name" - -
    STDIN_FIX="$TMP/hello.txt";      skip_case "uutils/GNU differ on the '-' operand name" "$TMP/hello.txt" -
    STDIN_FIX="$TMP/empty.bin";      skip_case "uutils/GNU differ on the '-' operand name" - "$TMP/hello.txt"
    skip_case "2.1 vs modern --help text differs" --help
    skip_case "2.1 vs modern --version text differs" --version
    skip_case "2.1 vs modern --help text differs" --hel
    skip_case "2.1 vs modern --help text differs" --help "$TMP/hello.txt"
fi

if [ "$fail" = 0 ]; then
    echo "cksum parity: PASS ($N cases byte-exact vs $REF, $REF_MODE mode)"
    exit 0
fi
echo "cksum parity: FAIL (of $N cases)"
exit 1
