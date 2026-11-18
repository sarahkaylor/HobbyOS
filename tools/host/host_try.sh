#!/bin/sh
# tools/host/host_try.sh -- run one `make host_tests` step and keep going.
#
#   sh tools/host/host_try.sh FAILFILE [-t SECS] [-C DIR] CMD [ARGS...]
#   sh tools/host/host_try.sh FAILFILE --report
#
# Why (H4-kglitch, docs/cp-kglitch.md §4): the host_tests recipe ran its test
# binaries as one command per recipe line, so the first red line ended the
# recipe -- `make -k` keeps going across TARGETS, not across the lines of one
# recipe.  The python suite (known red: the two gaming.md tests) sat in the
# middle, so every binary after it (AUDIO_STUB_TEST, LINUX_SND_PARAMS_TEST, the
# libc parity tests, ...) was built and never run by the gate, and a regression
# in any of them could not turn anything red.
#
# Each step now runs under a timeout (default 40 s, as HOST_RUN did); a failing
# step appends one line to FAILFILE and returns 0, so the recipe runs to its
# end.  The last line of the recipe (--report) prints every failure and exits 1
# if there was one.  FAILFILE is per make process (the recipe passes
# obj/host_tests.fails.$PPID), so two concurrent `make host_tests` runs
# (GB-085's host_concurrency_gate) never share it.
set -u
fails=${1:?usage: host_try.sh FAILFILE [-t SECS] [-C DIR] CMD... | FAILFILE --report}
shift
if [ "${1:-}" = "--report" ]; then
  if [ -s "$fails" ]; then
    n=$(wc -l < "$fails" | tr -d ' ')
    echo "[host_tests] summary: $n step(s) failed:"
    sed 's/^/  FAILED: /' "$fails"
    rm -f "$fails"
    exit 1
  fi
  rm -f "$fails"
  echo "[host_tests] summary: every step passed"
  exit 0
fi
secs=40
dir=
while :; do
  case "${1:-}" in
  -t) secs=$2; shift 2 ;;
  -C) dir=$2; shift 2 ;;
  *) break ;;
  esac
done
[ $# -gt 0 ] || { echo "host_try.sh: no command" >&2; exit 2; }
(
  [ -z "$dir" ] || cd "$dir" || exit 125
  if command -v timeout >/dev/null 2>&1; then
    exec timeout "$secs" "$@"
  fi
  exec "$@"
)
rc=$?
if [ "$rc" -ne 0 ]; then
  mkdir -p "$(dirname "$fails")"
  why="exit $rc"
  [ "$rc" -eq 124 ] && why="timeout ${secs}s"
  echo "$* ($why)" >> "$fails"
  echo "[host_tests] FAILED: $* ($why) -- continuing"
fi
exit 0
