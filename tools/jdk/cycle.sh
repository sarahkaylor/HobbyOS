#!/usr/bin/env bash
# tools/jdk/cycle.sh -- one iteration: build → stage → boot → report.
#
# This is the JDK lane's loop.  Each step is separate inside the script because
# each has a distinct failure mode that must not be confused with the next one:
#
#   build   kernel + FAT16 image (ARCH=intel MODE=test).  A build failure is a
#           compile error, never a "java died".
#   stage   glibc 2.31 pin + the JDK tree + WAVE.CFG into the image.  Staging is
#           never concurrent with a boot (a guest reading a half-written FAT
#           looks like an infinitely slow program).
#   boot    tools/jdk/boot.sh, fixed deadline, serial to obj/jdk/<log>.
#
# Usage:
#   tools/jdk/cycle.sh --wave "JDK/bin/java -version" --log java-version
#   tools/jdk/cycle.sh --no-build --wave "JDK/bin/java --version"    # rerun
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

WAVE=${WAVE:-"JDK/bin/java -version"}
LOG="run"
TIMEOUT_S=${TIMEOUT_S:-300}
BUILD=1
BOOT=1
J="${JDK_TREE:-$ROOT/third_party/jdk-27}"
JOBS=${JOBS:-8}
while [ $# -gt 0 ]; do
  case "$1" in
    --wave)    WAVE=$2; shift 2 ;;
    --log)     LOG=$2; shift 2 ;;
    --timeout) TIMEOUT_S=$2; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    --no-boot)  BOOT=0; shift ;;
    --jdk)     J=$2; shift 2 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done

say() { printf '\n== %s\n' "$*"; }

if [ "$BUILD" = 1 ]; then
  say "build (ARCH=intel MODE=test)"
  # TOOLS_SKIP: the in-OS LLVM toolchain is a disk input this lane never uses.
  # BROWSER_SKIP: the browser is a multi-hour vendored build and is not on the
  # path of anything here.
  if ! TOOLS_SKIP=1 BROWSER_SKIP=1 make -j"$JOBS" ARCH=intel MODE=test \
        hobbyos.elf disk.img > "obj/jdk/build.log" 2>&1; then
    echo "BUILD FAILED -- tail of obj/jdk/build.log:"; tail -25 obj/jdk/build.log; exit 2
  fi
  echo "   ok: $(stat -f%z hobbyos.elf 2>/dev/null || stat -c%s hobbyos.elf) bytes ELF"
fi

say "stage"
JDK_ARGS=()
[ -d "$J" ] && JDK_ARGS=(--jdk "$J")
PROBE_ARGS=()
for p in obj/jdk/*.exe; do
  [ -f "$p" ] || continue
  case "$p" in
    *.exe) PROBE_ARGS+=(--probe "$p:$(basename "${p%.exe}" | tr 'a-z' 'A-Z').EXE") ;;
  esac
done
tools/jdk/stage.sh "${JDK_ARGS[@]}" "${PROBE_ARGS[@]}" --wave "$WAVE" | sed 's/^/   /' || exit 2

[ "$BOOT" = 1 ] || exit 0

say "boot (deadline ${TIMEOUT_S}s)"
tools/jdk/boot.sh --timeout "$TIMEOUT_S" --log "obj/jdk/$LOG.log"
RC=$?

say "marks"
grep -aE "WAVE-MANIFEST|ARRIVED|java\.|version|Error|error|Exception|\[LCOMPAT\] (mmap|fault|syscall)|memory fault|SIGDUMP|System halt|Unrecognized|openjdk" \
  "obj/jdk/$LOG.log" | tail -60 | sed 's/^/   /'
exit $RC
