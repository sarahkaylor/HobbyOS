#!/bin/bash
# unexpand_parity.sh — byte-exact acceptance test for the HobbyOS ported
# unexpand.
#
# Runs our host-built unexpand (obj/unexpand_host, argv[0] forced to
# "unexpand" via exec -a so diagnostics are comparable) against a GNU
# unexpand reference on the same inputs and compares stdout bytes, exit
# codes and stderr text across the leading-only default, -a/--all,
# -t/--tabs (single size, explicit list, obsolete -LIST), --first-only,
# clustered/abbreviated/long options, stdin, file and multi-file modes,
# no-EOL/CR/high-bit/blank/backspace inputs, and the error paths (bad tab
# lists, invalid options, missing files).
#
# Reference selection:
#   $2 = explicit path, else $UNEXPAND_REF, else the first of these that
#        exists: /usr/bin/unexpand.  The strictest bar is
#        `make unexpand_parity_strict`, which builds textutils-2.1's
#        original unexpand as the reference instead; the port is a
#        transcription of that source, so that run is byte-exact on
#        stdout, exit codes AND stderr.
#
# Flavor handling (mirroring src/host/cut_parity.sh): a reference that
# reports "textutils" is strict (byte-exact rc + stderr), anything else is
# loose (rc compared as success/failure only, since 2003-era coreutils
# rewrote the diagnostics).  A few cases are additionally gated, each with
# its reason printed the first time it is skipped:
#   gnu-list  obsolete `-LIST' tab stops: the port and 2.1 reject them
#             (posix2_version == 200809) and accept them again under
#             _POSIX2_VERSION=199209, while modern references gate them
#             differently or dropped the version check, so these cases are
#             compared against the 2.1 reference only.
#   pending1  GNU's `if (pending == 1)' shortcut prints a literal space for
#             a lone pending blank, even when that column reaches a tab stop
#             (`a\tb` with -t 2,4 -> "a b"); some modern references run it
#             through the flush loop and emit a tab (`a\tb').  Compared
#             against the 2.1 reference, which the port matches byte-exact.
#   tabstop   a blank/tab run that overshoots the last explicit tab stop:
#             GNU saturates its stop list with an INT_MAX sentinel (5 spaces
#             + 2 tabs with -t 2,4 -> "\t\t\t\t"), modern references re-space
#             it ("\t\t \t\t").  Compared against the 2.1 reference.
#   abbrev    GNU long-option prefix matching (`--ta=4'); some modern
#             parsers (clap-based ones) reject prefixes outright.
#   helptext  --help/--version wording belongs to the builds' own versions
#             (2.1 vs modern), so only the 2.1 reference compares them.
#   optq      option-diagnostic quoting: 2.1's vendored getopt prints
#             `x'/x where the sysroot getopt and glibc print 'x'.  Both
#             sides get `' and ' stripped before the stderr comparison.
set -u

HB_HOST="${1:-./obj/unexpand_host}"
REF="${2:-${UNEXPAND_REF:-$(command -v unexpand || echo /usr/bin/unexpand)}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0
NGATED=0
declare -A GATED_WHY=()
GATED_NOTE() {
    case "$1" in
        gnu-list) echo "note: gating the obsolete \`-LIST' cases (2.1/port reject, modern references accept)" ;;
        pending1) echo "note: gating lone-pending-blank cases (GNU prints a space where modern references emit a tab)" ;;
        tabstop)  echo "note: gating tab-stop-list overshoot cases (modern references re-space past the last stop)" ;;
        abbrev)   echo "note: gating long-option prefix matching (modern parsers reject prefixes)" ;;
        helptext) echo "note: gating --help/--version text (belongs to each build's own version)" ;;
    esac
}

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Reference flavor: textutils-2.1 original (strict) vs modern (loose).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# --- fixtures -------------------------------------------------------------
printf '        x\n'            > "$TMP/lead8.txt"    # 8 spaces -> one tab
printf '       x\n'             > "$TMP/lead7.txt"    # 7 spaces -> stays
printf '         x\n'           > "$TMP/lead9.txt"    # 9 spaces -> tab + space
printf ' x\n'                   > "$TMP/one.txt"      # 1 space -> stays
printf 'a       b\nab      c\na\tb\n' > "$TMP/mid.txt" # leading blanks + interior
printf 'x   y    z     w\n'     > "$TMP/runs.txt"      # interior runs, -a bait
python3 - <<'PY' > "$TMP/stops.txt"                   # runs of 1..16 spaces at col 0
for i in range(1, 17):
    print(" " * i + "x")
PY
printf '  \t  x\n\t x\n \t\n\t\t a\n     \t\t\n' > "$TMP/mixed.txt"
printf '\n\n \n\t\n'            > "$TMP/blank.txt"
printf '   x'                   > "$TMP/noeol.txt"     # no final newline, 3 spaces
printf '        x'              > "$TMP/noeol8.txt"    # no final newline, 8 spaces
printf '    a\r\n    b\r\n'     > "$TMP/cr.txt"       # CRLF: CR ends conversion
printf '    \b\b ab\n \b\n'     > "$TMP/bs.txt"       # backspace rewinds the column
python3 - <<'PY' > "$TMP/hi.txt"                       # bytes >= 0x80 are column+1 bytes
import sys
sys.stdout.buffer.write(bytes(range(0x80, 0xa0)) + b"  \t x\n" + bytes(range(0xc0, 0xe0)) + b"\n")
PY
printf ''                       > "$TMP/empty.txt"
python3 - <<'PY' > "$TMP/big.txt"                      # random spaces/tabs/text
import random, sys
random.seed(21)
out = b""
for _ in range(3000):
    parts = []
    for _ in range(random.randint(0, 9)):
        parts.append(random.choice([b" ", b" ", b" ", b"\t", b"x", b"y", b"0", b" ", b"a"]))
    out += b"".join(parts) + random.choice([b"\n", b"\n", b"\n", b""])
sys.stdout.buffer.write(out)
PY

# HB/REF runners.  $1 = args string (word-split, __SP__/__EMPTY__ tokens
# stand in for quoted " " / ""), $2 = input spec:
#   file:NAME | stdin:NAME | file:- | multi:NAME[,NAME] | nofile
run_one() {
    local args_str="$1" spec="$2" impl="$3" envs="${4:-}"
    local args=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
    done
    local cmd
    if [ "$impl" = "hb" ]; then
        cmd=(bash -c 'exec -a unexpand "$0" "$@"' "$HB_HOST")
    else
        cmd=(bash -c 'exec -a unexpand "$0" "$@"' "$REF")
    fi
    local stdin_file="$TMP/empty.txt"
    case "$spec" in
        file:-)   args+=("-") ;;
        multi:*)  local m; IFS=',' read -r -a m <<< "${spec#multi:}"
                  for n in "${m[@]}"; do args+=("$TMP/$n.txt"); done ;;
        stdin:*)  stdin_file="$TMP/${spec#stdin:}.txt" ;;
        nofile)   ;;
        file:*)   args+=("$TMP/${spec#file:}.txt") ;;
    esac
    local envcmd=()
    [ -n "$envs" ] && envcmd=(env "$envs")
    "${envcmd[@]}" "${cmd[@]}" "${args[@]}" <"$stdin_file" >"$TMP/out" 2>"$TMP/err"
    echo "$?" > "$TMP/rc"
}

cmp_case() { # $1 = args string; $2 = input spec; $3 = gate; $4 = env (optional)
    local args_str="$1" spec="$2" gate="${3:-}" envs="${4:-}"
    N=$((N+1))
    case "$gate" in
        gnu-list|pending1|tabstop|abbrev|helptext)
            if [ "$REF_MODE" != strict ]; then
                NGATED=$((NGATED+1))
                if [ -z "${GATED_WHY[$gate]:-}" ]; then GATED_WHY[$gate]=1; GATED_NOTE "$gate"; fi
                return
            fi ;;
    esac
    run_one "$args_str" "$spec" hb "$envs"
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$spec" ref "$envs"
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: unexpand $args_str ($spec)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: unexpand $args_str ($spec)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        if [ "$gate" = optq ]; then
            tr -d "\`'" < "$TMP/eh" > "$TMP/ehq"
            tr -d "\`'" < "$TMP/err" > "$TMP/erq"
            SAME_OUT "$TMP/ehq" "$TMP/erq" || {
                echo "FAIL: stderr differs (rc=$rc_hb, quote-normalized): unexpand $args_str ($spec)"
                echo "  hb:  $(cat "$TMP/eh")"
                echo "  ref: $(cat "$TMP/err")"
                fail=1
            }
        else
            echo "FAIL: stderr differs (rc=$rc_hb): unexpand $args_str ($spec)"
            echo "  hb:  $(cat "$TMP/eh")"
            echo "  ref: $(cat "$TMP/err")"
            fail=1
        fi
    fi
}

# --- default: leading blanks only (the classic corner cases) --------------
cmp_case "" "file:lead8"      # 8 spaces reach stop 8 -> one tab
cmp_case "" "file:lead7"      # 7 spaces fall short -> kept as spaces
cmp_case "" "file:lead9"      # 9 spaces -> tab + one space
cmp_case "" "file:one"        # a lone blank stays a space
cmp_case "" "file:stops"      # 1..16-space runs, every remainder
cmp_case "" "file:mid"        # nothing after the first non-blank changes
cmp_case "" "file:mixed"      # blanks before/after tabs at line start
cmp_case "" "file:blank"      # blank lines, whitespace-only lines
cmp_case "" "file:noeol"      # short run, no final newline
cmp_case "" "file:noeol8"     # run reaching the stop, no final newline
cmp_case "" "file:cr"         # CR occupies a column and ends conversion
cmp_case "" "file:bs"         # backspace decrements the column
cmp_case "" "file:hi"         # bytes >= 0x80 are one column each
cmp_case "" "file:empty"      # no bytes at all
cmp_case "" "file:big"        # random spaces/tabs/text, 3000 lines

# --- -a / --all: blanks anywhere on the line ------------------------------
cmp_case "-a" "file:mid"
cmp_case "-a" "file:runs"
cmp_case "-a" "file:one"
cmp_case "-a" "file:stops"
cmp_case "-a" "file:mixed"
cmp_case "-a" "file:cr"
cmp_case "-a" "file:hi"
cmp_case "-a" "file:big" pending1
cmp_case "--all" "file:mid"
cmp_case "--all" "file:runs"

# --- -t / --tabs with a single tab size ----------------------------------
cmp_case "-t 4" "file:lead8"
cmp_case "-t 4" "file:lead9"
cmp_case "-t 4" "file:stops"
cmp_case "-t 4" "file:mid"     # -t implies -a
cmp_case "-t 4" "file:big" pending1
cmp_case "-t 8" "file:mid"     # explicit 8 == the default spacing, but -a
cmp_case "-t 3" "file:stops"
cmp_case "-t 16" "file:stops"
cmp_case "--tabs=4" "file:stops"
cmp_case "--tabs 4" "file:stops"
cmp_case "-at 4" "file:lead9"  # clustered short options

# --- -t with an explicit tab-stop list (tab_size == 0) -------------------
cmp_case "-t 2,4" "file:stops"
cmp_case "-t 2,4" "file:mid" pending1
cmp_case "-t 1,3,5" "file:stops" pending1
cmp_case "-t 4,8,12" "file:stops"
cmp_case "-a -t 2,4" "file:mid" pending1
cmp_case "-t 2,4" "file:mixed" tabstop
cmp_case "-t 1,3,5" "file:mixed"
cmp_case "-t 1" "file:one"     pending1
cmp_case "-t 1" "file:stops"   pending1

# --- --first-only (and its interaction with -a / -t) ---------------------
cmp_case "--first-only" "file:lead8"
cmp_case "--first-only" "file:mid"
cmp_case "-a --first-only" "file:mid"
cmp_case "--first-only -a" "file:mid"

# --- stdin, "-", multiple files ------------------------------------------
cmp_case "" "stdin:lead8"
cmp_case "-a" "stdin:mid"
cmp_case "-t 4" "stdin:stops"
cmp_case "" "file:-"
cmp_case "-a -t 4" "file:-"
cmp_case "" "multi:lead8,mid,empty"
cmp_case "-a" "multi:mid,stops"
cmp_case "-t 4" "multi:lead8,stops,noeol"

# --- long-option spelling -------------------------------------------------
cmp_case "--ta=4" "file:stops" abbrev
cmp_case "--al" "file:mid"     abbrev
cmp_case "--help" "nofile"     helptext
cmp_case "--version" "nofile"  helptext

# --- error paths ---------------------------------------------------------
cmp_case "" "file:no_such_unexpand_file"        # missing file (rc 1)
cmp_case "-t 4" "file:no_such_unexpand_file"
cmp_case "-t 4" "multi:lead8,no_such_unexpand_file"  # one good, one missing
cmp_case "-t 0" "file:lead8"                    # tab size cannot be 0
cmp_case "-t 4,2" "file:lead8"                  # not ascending
cmp_case "-t 4,4" "file:lead8"                  # not ascending (equal)
cmp_case "-t x" "file:lead8"                    # invalid character
cmp_case "-t 4x" "file:lead8"                   # invalid character
cmp_case "-t" "file:lead8"                      # filename swallowed by -t
cmp_case "-t" "nofile" optq                     # missing required argument
cmp_case "--tabs" "nofile" optq                 # missing required argument
cmp_case "-Z" "file:lead8" optq                 # invalid option
cmp_case "--bogus" "file:lead8" optq            # unrecognized long option
cmp_case "-4" "file:lead8" gnu-list             # obsolete -LIST
cmp_case "-4,8" "file:lead8" gnu-list           # obsolete -LIST
# ... unless the environment asks for the POSIX-1992 version, where 2.1's
# posix2_version() turns the obsolete list back into a tab size.
cmp_case "-4" "file:lead8" gnu-list "_POSIX2_VERSION=199209"

if [ "$fail" = 0 ]; then
    if [ "$NGATED" != 0 ]; then
        echo "unexpand parity: PASS ($((N-NGATED))/$N cases compared, $NGATED gated vs the $REF_MODE reference: $REF)"
    else
        echo "unexpand parity: PASS ($N cases byte-exact vs $REF)"
    fi
    exit 0
fi
echo "unexpand parity: FAIL (of $N cases)"
exit 1
