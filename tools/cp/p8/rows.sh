#!/usr/bin/env bash
# tools/cp/p8/rows.sh SERIAL [FROM_REGEX] -- the rows that matter in a P8 serial
# log (lane C8-game, docs/cp-p8.md): launcher rows, Wine/vkd3d/DXVK output that
# is not a loaddll trace, kernel faults/kills, unhandled syscalls and ioctls.
# The rest (page-fault publication, futex/idle dumps, region maps) is noise
# for the stop list.  Starts at FROM_REGEX (default: the launcher's first row).
S="$1"; FROM="${2:-\[P8\] p8launch}"
tr -d '\r' < "$S" | { if [ -n "$FROM" ]; then sed -n "/$FROM/,\$p"; else cat; fi; } |
  grep -aE '^\[P8\]|^\[X\] |\[KERNEL\] (pid=|User process fault|fault-pc)|memory fault|SIGDUMP|fault-no-handler|unhandled|ENOSYS|not implemented|\[LCOMPAT\] (unknown|unsupported|ioctl)|EMFILE|ENFILE|out of memory|OOM' |
  grep -avE 'trace:loaddll|^\[X\] \[PUB|exited unexpectedly or gracefully' |
  cut -c1-300
