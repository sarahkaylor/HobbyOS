#!/bin/bash
# t4_parity.sh — T4 differential compat-syscall parity harness.
#
# Purpose (per gaming swarm ABI worker charter): run the SAME compat probe
# on two targets and require byte-identical stdout + exit status + errno:
#
#   leg A  reference : static-musl Linux binary  (musl-gcc -static)
#   leg B  HobbyOS   : the same probe compiled for HobbyOS's compatibility
#                      surface and run on the host (glibc ho_* mocks), OR
#                      run inside QEMU on HobbyOS (ARCH=intel) once the ELF
#                      loader lands (Coordinator ticket GB-013, [DEEP]).
#
# What leg B proves, and what it does not (GB-083).  Leg B proves that the
# ho_* shim surface of compat.c, when actually exercised by the probe, returns
# byte-identical syscall rows to a real static-musl binary on the same kernel:
# it locks the MOCK CONTRACT — the ABI of the HobbyOS-compat layer that the
# real guest dispatch will present — against Linux.  Because leg B runs on real
# Linux (glibc), it DOES NOT prove HobbyOS's kernel implements those syscalls;
# that is the device leg (gated on the ELF loader).  It is a host-verifiable
# litmus: if the shim model and Linux disagree here, no guest can pass, so the
# model is wrong before any kernel work starts.  Leg B is LIVE only because we
# route the probe's core syscalls through ho_* (see --self-test): a flipped shim
# must make the differential FAIL.
#
# The host leg (A vs B-on-host) is fully runnable today — HobbyOS's in-tree
# errno.h uses Linux numbering and compat.c mirrors the wrapper contract, so
# a musl binary and the HobbyOS host build must agree exactly.  The QEMU
# device leg is gated on the ELF loader; until then it reports SKIP, never a
# fake pass.
#
# Usage:
#   t4_parity.sh [--device ARCH] [--obj obj]
#   --device intel|arm   also boot HobbyOS in QEMU (needs ELF loader; SKIP otherwise)
#
# 30-second deadlock rule: every run (host or device) carries a hard timeout
# so a hang is reported as a defect, not silently retried.

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OBJ="${OBJ_DIR:-$ROOT/obj}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

DEVICE=""
if [ "$#" -ge 1 ] && [ "$1" = "--device" ]; then
  DEVICE="${2:-intel}"
fi

# ---------------------------------------------------------------------------
# --self-test (GB-083): the "wrong shim must FAIL" criterion must be LIVE.
# Flipping any single ho_* return in compat.c must make the differential
# exit non-zero, and restoring it must return to PASS.  Before GB-083, leg B
# linked compat.c but the probe called libc directly, so a flipped shim left
# the verdict unchanged — the criterion was inert.  This runs the real harness
# three times: clean PASS, flipped-FAIL, restored-PASS, by re-invoking itself.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
  COMPAT="$ROOT/src/host/compat.c"
  R=""
  bash "$0" --flip ho_open >/tmp/t4p.selftest 2>&1; R="$?"
  # restore the flipped source by applying the inverse flip (in-shell, no git):
  # the --flip run changed `host, mode) + 1000` — undo it to the faithful form.
  sed -i 's/return open(filename, host, mode) + 1000;/return open(filename, host, mode);/' "$COMPAT"
  if [ "$R" = "0" ]; then
    echo "self-test: FAIL — flipping ho_open still PASSed (criterion inert); run=$R"; exit 1
  fi
  # clean restore must pass
  bash "$0" >/tmp/t4p.selftest 2>&1; R="$?"
  if [ "$R" != "0" ]; then
    echo "self-test: FAIL — clean (unflipped) run returned $R; see /tmp/t4p.selftest"; exit 1
  fi
  echo "self-test: OK — flipping ho_open FAILs (rc=$R clean pass), restoring passes"
  exit 0
fi

# --flip <name>: flip one ho_* return so the differential must FAIL (used by
# --self-test).  Not a normal invocation.
if [ "$#" -ge 1 ] && [ "$1" = "--flip" ]; then
  case "${2:-}" in
    ho_open) FLIP_SHIM=ho_open ;;
    *) echo "unknown flip target: ${2:-}"; exit 2 ;;
  esac
fi

fail=0
N=0

# ---- build leg A: static musl -------------------------------------------------
MUSL_GCC="${MUSL_GCC:-$(command -v musl-gcc || true)}"
if [ -z "$MUSL_GCC" ]; then
  echo "SKIP leg A: musl-gcc not found (install musl musl-tools)"
  A=skip
else
  if ! "$MUSL_GCC" -static -O2 -Wall src/host/t4_compat_probe.c \
        -o "$TMP/t4_probe.musl" 2>"$TMP/buildA.log"; then
    echo "FAIL: leg A build (static musl)"; tail -20 "$TMP/buildA.log"; fail=1
    A=buildfail
  else
    A="$TMP/t4_probe.musl"
  fi
fi

# ---- build leg B: HobbyOS host compat surface ---------------------------------
# Mirror the errno_test_host rule: host probe + compat.c ho_* mocks as glibc.
# GB-083 --flip: break one ho_* shim so the differential MUST fail (the live
# "wrong shim must FAIL" criterion).  Applied to compat.c before the build and
# restored by the make target's shell (see the --self-test re-invocation: it
# runs --flip then an unflipped run, so a leftover flip is restored by the
# next clean invocation).
if [ -n "${FLIP_SHIM:-}" ]; then
  cp "$ROOT/src/host/compat.c" "$TMP/compat.orig"
  case "$FLIP_SHIM" in
    ho_open) sed -i 's/return open(filename, host, mode);/return open(filename, host, mode) + 1000;/' "$ROOT/src/host/compat.c" ;;
  esac
fi
if command -v "${HOST_CC:-clang}" >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; then
  HOST_CC="${HOST_CC:-clang}"
  HOST_CFLAGS="${HOST_CFLAGS:--Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -DHOST_TEST}"
  if ! "$HOST_CC" $HOST_CFLAGS -Isrc/include -DLEG_B_SHIMS \
        src/host/t4_compat_probe.c src/host/compat.c \
        -o "$TMP/t4_probe.host" 2>"$TMP/buildB.log"; then
    echo "FAIL: leg B build (HobbyOS host compat)"; tail -20 "$TMP/buildB.log"; fail=1
    B=buildfail
  else
    B="$TMP/t4_probe.host"
  fi
else
  echo "SKIP leg B: no host C compiler"
  B=skip
fi

# ---- run helpers with hard timeout -------------------------------------------
run_timeout() {  # $1 timeout_s, rest = command...
  local t="$1"; shift
  timeout "$t" "$@" >"$TMP/out" 2>"$TMP/err"; echo "$?" > "$TMP/rc"
}

capture_run() {  # $1 = label, $2 = binary
  N=$((N+1))
  run_timeout 30 "$2"
  local rc; rc=$(<"$TMP/rc")
  if [ "$rc" = "124" ]; then
    echo "FAIL: $1 HUNG (timeout 30s) — defect, not retried"; fail=1
    return 1
  fi
  mkdir -p "$TMP/runs"
  cp "$TMP/out" "$TMP/runs/$1.out"
  cp "$TMP/err" "$TMP/runs/$1.err"
  echo "$rc" > "$TMP/runs/$1.rc"
  return 0
}

cmp_out() {  # $1 = legA label, $2 = legB label
  N=$((N+1))
  local a="$TMP/runs/$1.out" b="$TMP/runs/$2.out"
  local ra="$TMP/runs/$1.rc" rb="$TMP/runs/$2.rc"
  if ! diff -q "$a" "$b" >/dev/null 2>&1; then
    echo "FAIL: stdout differs ($1 vs $2)"
    echo "  A: $(tr '\n' '|' < "$a")"
    echo "  B: $(tr '\n' '|' < "$b")"
    fail=1
  fi
  if [ "$(cat "$ra")" != "$(cat "$rb")" ]; then
    echo "FAIL: exit code differs ($1=$(cat "$ra") vs $2=$(cat "$rb"))"
    fail=1
  fi
}

# ---- execute the host differential (A vs B) ------------------------------------
if [ "$A" != "skip" ] && [ "$A" != "buildfail" ] && \
   [ "$B" != "skip" ] && [ "$B" != "buildfail" ]; then
  capture_run legA_musl "$A" || true
  capture_run legB_host "$B" || true
  cmp_out legA_musl legB_host
  echo "t4 host differential: A=$(cat "$TMP/runs/legA_musl.rc" 2>/dev/null || echo n/a) B=$(cat "$TMP/runs/legB_host.rc" 2>/dev/null || echo n/a) cases=$N"
else
  # GB-066 (tier honesty, GB-075 law): a MISSING/ABSENT required leg must FAIL
  # the tier, not skip-and-PASS.  If either the musl reference (A) or the
  # HobbyOS compat surface (B) cannot be built/run, no differential was
  # produced, so this is NOT a green.  (A real leg-B *device* run is a separate
  # clause, gated on the ELF loader and handled below.)
  echo "t4 host differential: SKIPPED (a required leg did not run: A=$A B=$B) — FAILING the tier per GB-066 (no false green)"
  fail=1
fi

# ---- device leg (gated on ELF loader) -------------------------------------------
if [ -n "$DEVICE" ]; then
  # The in-QEMU leg needs the kernel ELF loader (flat-binary loader today).
  # Detect its presence; until GB-013 lands this is structurally unavailable,
  # and we must not fake a pass.
  ELF_LOADER_PRESENT=$(
    grep -rq "ET_EXEC\|ELF" src/kernel 2>/dev/null && \
    grep -rq "phdr\|Elf64_Ehdr\|Elf32_Ehdr" src/kernel 2>/dev/null && echo 1 || echo 0
  )
  if [ "$ELF_LOADER_PRESENT" = "1" ]; then
    echo "t4 device ($DEVICE): ELF loader present — device leg not wired yet"
    echo "t4 device: SKIP (device-leg runner TBD; host differential is the gate today)"
  else
    echo "t4 device ($DEVICE): SKIP — ELF loader not yet in kernel (Coordinator GB-013 [DEEP]); host differential stands in"
  fi
fi

if [ "$fail" = "0" ]; then
  echo "t4_parity: PASS (host differential locked; device leg gated)"
  exit 0
fi
echo "t4_parity: FAIL"
exit 1
