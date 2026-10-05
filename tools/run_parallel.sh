#!/bin/bash
# run_parallel.sh — parallel test-run helper (GX/battery infra).
# Run up to N named shell commands concurrently; each gets its own log +
# rc file; a summary prints when all finish.
#
# Usage: tools/run_parallel.sh [-j N] [-d LOGDIR] name1 'command1' [name2 'command2' ...]
# Defaults: -j 3 (3 x 8 guest cores = 24; the user-directed parallelism
# budget), -d /tmp/par-logs.
#
# Notes:
#  - Commands run via bash -c; quote them as single arguments.
#  - Never point two jobs at the same disk.img (QEMU image locking) or at
#    the same tree when either does a `make` (mode-tracker flips) — give
#    each job its own worktree / disk.
#  - Throttling uses `wait -n`; jobs start in order.
set -u
J=3
D=/tmp/par-logs
while getopts "j:d:" o; do
  case "$o" in
    j) J="$OPTARG" ;;
    d) D="$OPTARG" ;;
    *) echo "usage: $0 [-j N] [-d LOGDIR] name 'cmd' [name2 'cmd2' ...]" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
if [ $# -lt 2 ] || [ $(( $# % 2 )) -ne 0 ]; then
  echo "usage: $0 [-j N] [-d LOGDIR] name 'cmd' [name2 'cmd2' ...]" >&2
  exit 2
fi
mkdir -p "$D"
rm -f "$D"/*.rc 2>/dev/null
echo "run_parallel: j=$J logs=$D"
while [ $# -ge 2 ]; do
  name="$1"; cmd="$2"; shift 2
  while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n; done
  ( bash -c "$cmd" >"$D/$name.log" 2>&1; echo "$?" >"$D/$name.rc" ) &
  echo "  started $name"
done
wait
echo "== run_parallel summary =="
rc_all=0
for f in "$D"/*.rc; do
  [ -e "$f" ] || continue
  n=$(basename "$f" .rc); r=$(cat "$f")
  printf '  %-24s rc=%s\n' "$n" "$r"
  [ "$r" = "0" ] || rc_all=1
done
exit $rc_all
