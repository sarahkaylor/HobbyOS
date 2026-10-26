#!/usr/bin/env bash
# tools/gate/t4_device.sh — GB-066 T4 device-leg gate.
#
# The M1 device leg: run the SAME W0 differential probe (src/user/t4_probe.c,
# built with musl-gcc -static) inside HobbyOS-intel via the real ELF loader
# (GB-013) and Linux-compat syscall dispatch (GB-014), capture its lines from
# the serial console, and diff rc/errno/stdout row-by-row against leg A (the
# same binary on real Linux).  This tests the KERNEL, never a host shim.
#
# DoD clauses this enforces (gaming §7 / the GB-066 mint):
#   1. Leg B runs inside HobbyOS-intel and the diff passes.
#   2. A deliberately-wrong shim makes the same harness FAIL.
#   3. An absent probe FAILS the tier rather than skipping: SKIP must exit
#      non-zero.
#   4. The tier asserts the probe's ARRIVAL in the guest via the probe's OWN
#      console line (t4-probe-arrived) — never an image listing (mdir shows
#      "PIE      BIN" and a grep of it is a tautology, per the mint).
#
# Usage:
#   t4_device.sh          run the device differential (needs staged T4PROBE ELF)
#   t4_device.sh --self-test
#                         prove the gate can fail (DoD clauses 2 + 3) from
#                         synthetic fixtures, exit 0 iff both checks FAIL
#                         as they must.
#
# Env: T4_LEGA_OUT   path to leg A captured output (default: <this>/../../obj/t4_legA.out)
#      T4_SERIAL     path to the HobbyOS serial log (default: obj/t3_intel.log)
#      T4_TIMEOUT    seconds to wait for the probe to arrive (default 120)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LEGA="${T4_LEGA_OUT:-$ROOT/obj/t4_legA.out}"
SERIAL="${T4_SERIAL:-$ROOT/obj/t3_intel.log}"
TIMEOUT="${T4_TIMEOUT:-120}"
GOLDEN="${T4_ROWS_GOLDEN:-$ROOT/tests/fixtures/abi/t4_rows.golden}"
HOSTPROBE="${T4_HOST_PROBE:-$ROOT/src/host/t4_compat_probe.c}"
DEVPROBE="${T4_DEV_PROBE:-$ROOT/src/user/t4_probe.c}"

# ---------------------------------------------------------------------------
# --check: GB-073.  Fail when the guest probe and the host model disagree on
# the row-name set the M1 -V DoDs name.  Every golden row must be emitted by
# BOTH probes, the device probe must carry no extra row, and deleting a row
# here or in either probe must fail the gate.
# Alias: the host model names its stdout-write row 'write_stdout' while the
# device probe names the identical write 'write3' — they are the same logical
# row, mapped below so the parity line agrees.
host_alias() { [ "$1" = "write3" ] && echo write_stdout || echo "$1"; }
# Mode-independent row-name extraction (GB-079): given the device probe source,
# return the row-name set a HEALTHY run emits.  The probe emits a couple of
# diagnostic rows ONLY inside an error guard (if (write(...)!=1) prow(...) —
# pipe_write / sock_write); a healthy leg-A capture never contains them, so the
# source path must drop `if`-gated prow() lines or the two modes would disagree
# on the same tree.
dev_rows() { # $1 = '' (use emitted leg A if present) or 'source' (force source)
  if [ "${1:-}" != "source" ] && [ -f "$LEGA" ] && [ -s "$LEGA" ]; then
    grep '^row=' "$LEGA" | sed -E 's/^row=([^ ]+).*/\1/'
  else
    grep -vE '^[[:space:]]*if ' "$DEVPROBE" \
      | grep -o 'prow("[^"]*"' | cut -d'"' -f2
  fi
}
# ---------------------------------------------------------------------------
# --check: GB-073 row-set parity, hardened by GB-079 (re-opened 2026-10-23).
#
# GB-079's original defect: the verdict depended on whether obj/t4_legA.out
# happened to exist.  The first fix printed the mode and dropped `if`-gated
# rows, which made the VERDICT mode-independent — but failure detection was
# still artefact-bound: in the default mode (leg A present) the row set came
# from that saved file, so an edit to the probe itself was invisible.  Measured
# 2026-10-23 on origin/main: an injected unconditional prow() PASSED --check
# with leg A present and FAILED only when leg A was removed.
#
# The check therefore now grades BOTH sets and requires BOTH to pass:
#   * source  — what the probe's own text says it emits (catches probe edits
#               without needing a build, a guest, or a fresh capture)
#   * emitted — what a recorded leg-A run actually emitted (catches guest-side
#               drift, which source text cannot know about)
# and it refuses a disagreement between them: an emitted capture that does not
# match the source is stale, and a stale capture must fail rather than answer.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--check" ]; then
  fail=0
  echo "=== t4_device --check: row-name set parity (GB-073, GB-079) ==="
  [ -f "$GOLDEN" ]      || { echo "  FAIL: golden manifest '$GOLDEN' absent"; fail=1; }
  [ -f "$DEVPROBE" ]    || { echo "  FAIL: device probe '$DEVPROBE' absent"; fail=1; }
  [ -f "$HOSTPROBE" ]   || { echo "  FAIL: host model '$HOSTPROBE' absent"; fail=1; }
  HAVE_LEGA=1
  { [ -f "$LEGA" ] && [ -s "$LEGA" ]; } || HAVE_LEGA=0

  # golden set
  mapfile -t G < <(grep -v '^#' "$GOLDEN" 2>/dev/null | grep -v '^$')
  echo "  golden rows: ${#G[@]}"
  # host model — report("rowname", ...)
  mapfile -t H < <(grep -o 'report("[^"]*"' "$HOSTPROBE" | cut -d'"' -f2)

  # check_one <mode> — grade one extraction of the device row set.
  check_one() {
    local mode="$1" rc=0
    local -a A
    mapfile -t A < <(dev_rows "$mode")
    echo "  --- mode=$mode: device probe rows: ${#A[@]}"
    # every golden row must be emitted by BOTH (host name via alias)
    for r in "${G[@]}"; do
      local inA=0 inH=0 x hr
      for x in "${A[@]}"; do [ "$x" = "$r" ] && inA=1; done
      hr="$(host_alias "$r")"
      for x in "${H[@]}"; do [ "$x" = "$hr" ] && inH=1; done
      if [ "$inA" = "1" ] && [ "$inH" = "1" ]; then
        echo "  PASS row: $r (device+host)"
      else
        echo "  FAIL row: $r (device=$inA host=$inH) [mode=$mode] — a row the M1 -V DoD names is missing from a probe"
        rc=1
      fi
    done
    # device must not carry EXTRA rows outside the golden (a W0 device probe that
    # emits a row the -V DoDs do not name is a row no DoD checks — a smaller,
    # unchecked surface).  The host model is a SUPERSET compat probe (it also
    # carries GB-019/021/022/023 rows), so it must CONTAIN every golden row but is
    # allowed its own extra rows.
    for x in "${A[@]}"; do
      local inG=0 g
      for g in "${G[@]}"; do [ "$g" = "$x" ] && inG=1; done
      [ "$inG" = "1" ] || { echo "  FAIL: device probe emits extra row '$x' not in golden [mode=$mode]"; rc=1; }
    done
    [ "$rc" = "0" ] && echo "  mode=$mode: PASS"
    [ "$rc" = "0" ] || fail=1
    return 0
  }

  check_one source
  if [ "$HAVE_LEGA" = "1" ]; then
    check_one emitted
    # A recorded capture that disagrees with the probe's own source is STALE.
    # Refusing it is the whole point: otherwise a probe edit is invisible until
    # somebody happens to regenerate the capture.
    TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT
    dev_rows source | sort -u >"$TMPD/src"
    dev_rows emitted | sort -u >"$TMPD/emit"
    if ! diff -q "$TMPD/src" "$TMPD/emit" >/dev/null 2>&1; then
      echo "  FAIL: recorded leg A is STALE — it disagrees with the device probe's source:"
      diff -u "$TMPD/src" "$TMPD/emit" | sed -n '3,20p' | sed 's/^/        /'
      echo "        regenerate obj/t4_legA.out (make t4_legA) or delete it; a stale capture is not a verdict."
      fail=1
    else
      echo "  PASS: recorded leg A agrees with the probe source (${#G[@]} golden rows, no drift)"
    fi
  else
    echo "  note: leg A ('$LEGA') absent — emitted mode not graded (source mode is)"

  fi
  if [ "$fail" = "0" ]; then
    if [ "$HAVE_LEGA" = "1" ]; then
      echo "t4_device --check: PASS (mode=source+emitted) — both extractions carry exactly the golden row-name set and agree; the host model contains every golden row."
    else
      echo "t4_device --check: PASS (mode=source) — the probe source carries exactly the golden row-name set; the host model contains every golden row."
    fi
    exit 0
  fi
  if [ "$HAVE_LEGA" = "1" ]; then echo "t4_device --check: FAIL (mode=source+emitted)"; else echo "t4_device --check: FAIL (mode=source)"; fi
  exit 1
fi

# ---------------------------------------------------------------------------
# --self-test: prove the gate can FAIL (clauses 2 and 3).  Uses synthetic
# fixtures so it runs anywhere a shell exists.  Never touches the kernel.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

  # leg A fixture: the real probe's expected output.
  cat >"$TMP/legA" <<'EOF'
t4-probe
hi
row=write3 rc=3 errno=0 out=68690a
t4-probe-arrived
probe-exit
EOF

  # (a) A deliberately-wrong shim (rc=2 instead of 3, and no arrival line
  #     proves nothing about the guest).  The gate MUST fail.
  cat >"$TMP/legB_wrong" <<'EOF'
t4-probe
row=write3 rc=2 errno=0 out=68690a
t4-probe-arrived
probe-exit
EOF

  # (b) An absent probe: serial log has no t4-probe / arrival lines at all
  #     (e.g. the guest never ran it).  The gate MUST fail, not skip.
  cat >"$TMP/legB_absent" <<'EOF'
system boot
some other wave output
System halt
EOF

  fail_count=0

  # --- inline, self-contained verdict logic (mirrors the real diff below) ---
  verdict() { # $1 legA, $2 legB, returns 0 pass / 1 fail
    local a="$1" b="$2" rc=0
    grep -q "t4-probe-arrived" "$b" || { echo "  FAIL: probe never arrived in guest (no t4-probe-arrived)"; return 1; }
    if ! grep -q "row=" "$b"; then echo "  FAIL: no row= lines in guest output"; return 1; fi
    local na nb
    na=$(grep -c "^row=" "$a"); nb=$(grep -c "^row=" "$b")
    [ "$na" = "$nb" ] || { echo "  FAIL: row count differs A=$na B=$nb"; return 1; }
    while IFS= read -r ra; do
      if ! grep -Fqx "$ra" "$b"; then echo "  FAIL: leg A row missing/mismatched in B: $ra"; rc=1; fi
    done < <(grep "^row=" "$a")
    return $rc
  }

  echo "[self-test] clause 2 (wrong shim must FAIL):"
  if verdict "$TMP/legA" "$TMP/legB_wrong" >/dev/null 2>&1; then
    echo "  UNEXPECTED PASS — wrong shim did not fail"; fail_count=$((fail_count+1))
  else
    echo "  OK — wrong shim FAILED as required"; :
  fi

  echo "[self-test] clause 3 (absent probe must FAIL, not skip):"
  if verdict "$TMP/legA" "$TMP/legB_absent" >/dev/null 2>&1; then
    echo "  UNEXPECTED PASS — absent probe did not fail"; fail_count=$((fail_count+1))
  else
    echo "  OK — absent probe FAILED as required"; :
  fi

  # (d) GB-079 re-open: the --check must catch a probe EDIT in the mode a
  #     developer actually sees (leg A present), not only when it is absent.
  #     Runs the real --check against synthetic fixtures: an unconditional row
  #     added to the probe must FAIL with leg A present.
  echo "[self-test] GB-079 (a probe edit must FAIL in both --check modes):"
  mkdir -p "$TMP/c"
  printf 'write3\nuname_rc\n' >"$TMP/c/golden"
  printf 'row=write3 rc=3 errno=0\nrow=uname_rc rc=0 errno=0\n' >"$TMP/c/legA"
  cat >"$TMP/c/probe_ok.c" <<'EOF'
int main(void) { prow("write3", 3, 0); prow("uname_rc", 0, 0); return 0; }
EOF
  cat >"$TMP/c/probe_bad.c" <<'EOF'
int main(void) { prow("bogus_unconditional", 7, 0); prow("write3", 3, 0); prow("uname_rc", 0, 0); return 0; }
EOF
  check_rc() { # $1 probe $2 legA-or-"" -> echoes exit code of --check
    local probe="$1" lega="$2" rc
    local lega_arg="$TMP/c/absent.out"
    [ -n "$lega" ] && lega_arg="$lega"
    T4_DEV_PROBE="$probe" T4_ROWS_GOLDEN="$TMP/c/golden" T4_LEGA_OUT="$lega_arg" \
      bash "${BASH_SOURCE[0]}" --check >"$TMP/c/out.log" 2>&1
    rc=$?
    echo "$rc"
  }
  for lega_case in "" "$TMP/c/legA"; do
    local_name=$([ -n "$lega_case" ] && echo "leg A present" || echo "leg A absent")
    if [ "$(check_rc "$TMP/c/probe_ok.c" "$lega_case")" != "0" ]; then
      echo "  UNEXPECTED FAIL — healthy probe rejected ($local_name)"; fail_count=$((fail_count+1))
    else
      echo "  OK — healthy probe PASSES ($local_name)"
    fi
    if [ "$(check_rc "$TMP/c/probe_bad.c" "$lega_case")" = "0" ]; then
      echo "  UNEXPECTED PASS — injected probe row not caught ($local_name)"; fail_count=$((fail_count+1))
    else
      echo "  OK — injected probe row FAILS ($local_name)"
    fi
  done

  echo "self-test result: fail_count=$fail_count"
  [ "$fail_count" = "0" ]
  exit $?
fi
# ---------------------------------------------------------------------------
# real device gate
# ---------------------------------------------------------------------------
echo "=== t4_device: leg A reference: $LEGA ==="
if [ ! -s "$LEGA" ]; then
  echo "t4_device: FATAL — leg A reference '$LEGA' absent/empty. Build leg A first:"
  echo "  musl-gcc -static -O2 src/user/t4_probe.c -o \$TMP/t4.elf && \$TMP/t4.elf > '$LEGA'"
  exit 2
fi

echo "=== t4_device: capture leg B from serial '$SERIAL' ==="
if [ ! -f "$SERIAL" ]; then
  echo "t4_device: FATAL — serial log '$SERIAL' absent. Boot HobbyOS-intel through"
  echo "  tools/t3_tier.sh intel (T3_STAGE staging of T4PROBE) before this gate."
  echo "t4_device: FAIL (environment: no serial log) — not a SKIP."
  exit 1
fi

# Wait (bounded) for the probe to appear in the serial log, i.e. the guest
# actually ran it.  Absent after TIMEOUT == FAIL, never a silent skip.
deadline=$(( $(date +%s) + TIMEOUT ))
arrived=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  if grep -q "t4-probe-arrived" "$SERIAL"; then arrived=1; break; fi
  sleep 2
done
if [ "$arrived" != "1" ]; then
  echo "t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'"
  echo "  in $SERIAL after ${TIMEOUT}s).  This is a FAIL, not a SKIP (DoD clause 3)."
  echo "  (tail of serial:)"
  tail -20 "$SERIAL" || true
  exit 1
fi
echo "t4_device: probe ARRIVED in guest (t4-probe-arrived present)."

# GB-107: record every monitored run's serial shape and classify TRUNCATED vs
# GUEST_STOPPED vs FULL.  "alive at cutoff" = the probe ARRIVED but the log lacks
# the probe's own end marker at cutoff (the guest was still writing when the
# capture ended) -> TRUNCATED; a log that never shows an end marker with the guest
# gone is GUEST-STOPPED.  This is the verdict distinct from a plain diff PASS.
_GUEST_EXPECTED="${T4_ROWS_EXPECTED:-43}"
if command -v python3 >/dev/null 2>&1 && [ -f "$ROOT/tools/t3/serial_classifier.py" ]; then
  if grep -q "probe-exit" "$SERIAL"; then
    echo "t4_device: SERIAL-FULL lines-captured-end-marker (probe-exit seen)"
  else
    python3 "$ROOT/tools/t3/serial_classifier.py" "$SERIAL" \
      --alive 1 --expected-lines "$_GUEST_EXPECTED" 2>/dev/null \
      | sed 's/^/t4_device: /' \
      || true   # classification is a recorded annotation, never a hard gate here
  fi
fi


# Per-row diff of rc/errno/stdout against leg A.
fail=0
NA=$(grep -c "^row=" "$LEGA"); NB=$(grep -c "^row=" "$SERIAL")
echo "t4_device: row count A=$NA B=$NB"
[ "$NA" = "$NB" ] || { echo "t4_device: FAIL — row count differs (A=$NA B=$NB)"; fail=1; }
while IFS= read -r ra; do
  if grep -Fqx "$ra" <(grep "^row=" "$SERIAL"); then
    echo "  PASS $ra"
  else
    echo "  FAIL $ra  (not present in guest output; see B rows below)"
    fail=1
  fi
done < <(grep "^row=" "$LEGA")

if [ "$fail" = "0" ]; then
  echo "t4_device: PASS — leg B (HobbyOS-intel) matches leg A (Linux) on all rows."
  exit 0
fi
echo "t4_device: FAIL — differential mismatch.  Guest rows:"
grep "^row=" "$SERIAL" || true
exit 1
