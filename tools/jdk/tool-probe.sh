#!/usr/bin/env bash
# tools/jdk/tool-probe.sh -- one JDK tool, one boot, one log.
#
# A wave line is a *program*: MODE=test dispatches every /WAVE.CFG line
# concurrently, so a probe that needs a second live process is not expressible
# here and must be argued about separately (see docs/java-tools.md).
#
# Usage:
#   tools/jdk/tool-probe.sh --log keytool-gen --wave "JDK/bin/keytool -genkeypair ..."
#   tools/jdk/tool-probe.sh --log first --jdk --payload --mkdir EXT --wave "..."
#
#   --jdk        re-stage the whole JDK tree (slow; only the first probe needs it,
#                the FAT image keeps it because staging never recreates disk.img)
#   --payload    stage payload/src -> /T and payload/classes -> /C
#   --mkdir D    mmd ::/JDKOUT/D before booting (a tool that writes into a
#                directory can then be judged on the tool, not on mkdir)
#   --timeout S  boot deadline (default 240: TCG + a JVM start is ~60-90 s, and
#                anything past 4 min is a hang worth reporting, not waiting on)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

JDK_TREE="${JDK_TREE:-/Users/sarah/Documents/GitHub/HobbyOS-openjdk/third_party/jdk-27}"
LOG=""; WAVE=""; TIMEOUT_S=240; STAGE_JDK=0; STAGE_PAYLOAD=0; MKDIRS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --log)     LOG=$2; shift 2 ;;
    --wave)    WAVE=$2; shift 2 ;;
    --timeout) TIMEOUT_S=$2; shift 2 ;;
    --jdk)     STAGE_JDK=1; shift ;;
    --payload) STAGE_PAYLOAD=1; shift ;;
    --mkdir)   MKDIRS+=("$2"); shift 2 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done
[ -n "$LOG" ] && [ -n "$WAVE" ] || { echo "need --log and --wave"; exit 2; }

ARGS=(--wave "$WAVE")
[ "$STAGE_JDK" = 1 ] && ARGS+=(--jdk "$JDK_TREE")
# payload/classes is populated by the guest's own javac (boot 1), so it is
# normally absent here -- passing an empty dir to stage.sh is an mcopy error, not
# a copy of nothing.
[ "$STAGE_PAYLOAD" = 1 ] && ARGS+=(--dir payload/src:/T)
[ "$STAGE_PAYLOAD" = 1 ] && [ -n "$(ls -A payload/classes 2>/dev/null)" ] && ARGS+=(--dir payload/classes:/C)
echo "== stage"
tools/jdk/stage.sh "${ARGS[@]}" || exit 2
for d in "${MKDIRS[@]:-}"; do
  [ -n "$d" ] || continue
  mmd -i disk.img "::/JDKOUT/$d" >/dev/null 2>&1 || true
  echo "   mkdir ::/JDKOUT/$d"
done

echo "== boot ($WAVE)"
start=$(date +%s)
tools/jdk/boot.sh --timeout "$TIMEOUT_S" --log "obj/jdk/$LOG.log"
RC=$?
echo "== rc=$RC elapsed=$(( $(date +%s) - start ))s log=obj/jdk/$LOG.log"
exit $RC
