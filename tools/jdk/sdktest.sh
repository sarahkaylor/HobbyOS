#!/usr/bin/env bash
# tools/jdk/sdktest.sh -- the JDK lane's acceptance run, end to end.
#
# What "OpenJDK works on HobbyOS" means, in checks a machine can make:
#
#   1. version   JDK/bin/java -version prints the OpenJDK banner
#   2. javac     JDK/bin/javac compiles /T/Hello.java into /JDKOUT and EXITS
#                (an exit that hangs is not a pass: the wave would never end)
#   3. run       JDK/bin/java -cp /JDKOUT Hello runs the class javac just made
#   4. suite     JDK/bin/java -cp /C Verify runs the 18-check SDK suite
#
# Each check is its own boot, because a pass is a claim about the CONSOLE:
# staging is never concurrent with a boot (a guest reading a half-written FAT
# looks like an infinitely slow program -- tools/rw/mkdisk-rimworld.sh), and a
# wave that hangs must fail on its own deadline instead of poisoning the next
# check.  The image is NOT rebuilt between checks: /JDKOUT must survive so
# check 3 can run the class file check 2 produced.
#
# Usage:
#   tools/jdk/sdktest.sh              # full run (~15 min under TCG)
#   tools/jdk/sdktest.sh --no-build   # reuse the current kernel + image
#   tools/jdk/sdktest.sh --only=suite # one check by name
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

JDK="${JDK_TREE:-$ROOT/third_party/jdk-27}"
JOBS="${JOBS:-8}"
BUILD=1
ONLY=""
TIMEOUT=${SDKTEST_TIMEOUT:-420}
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0; shift ;;
    --only=*)   ONLY="${1#--only=}"; shift ;;
    --jdk)      JDK="$2"; shift 2 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done
[ -d "$JDK" ] || { echo "no JDK tree at $JDK (set JDK_TREE)"; exit 2; }

say() { printf '\n== %s\n' "$*"; }
RESULT=()
record() { RESULT+=("$1|$2"); printf '   %-8s %-6s %s\n' "$1" "$2" "$3"; }

# --- build -----------------------------------------------------------------
if [ "$BUILD" = 1 ]; then
  say "build (ARCH=intel MODE=test)"
  if ! TOOLS_SKIP=1 BROWSER_SKIP=1 make -j"$JOBS" ARCH=intel MODE=test \
        hobbyos.elf disk.img > obj/jdk/build.log 2>&1; then
    echo "BUILD FAILED -- tail of obj/jdk/build.log:"; tail -25 obj/jdk/build.log; exit 2
  fi
  echo "   ok   kernel $(stat -f%z hobbyos.elf 2>/dev/null || stat -c%s hobbyos.elf) bytes"
fi

# --- host-side compile of the Java payload --------------------------------
say "compile the Java payload (host javac)"
HOSTJAVAC="${HOSTJAVAC:-/opt/homebrew/opt/openjdk@26/bin/javac}"
[ -x "$HOSTJAVAC" ] || HOSTJAVAC=$(command -v javac)
[ -n "$HOSTJAVAC" ] || { echo "no host javac found (set HOSTJAVAC)"; exit 2; }
mkdir -p obj/jdk/classes obj/jdk/src
cp tools/jdk/tests/Hello.java obj/jdk/src/
"$HOSTJAVAC" -nowarn -d obj/jdk/classes tools/jdk/tests/*.java 2>&1 | grep -v '^Note:' || true
echo "   ok   classes: $(ls obj/jdk/classes | tr '\n' ' ')"

# --- stage -----------------------------------------------------------------
say "stage (glibc 2.31 pin + JDK + classes + sources)"
tools/jdk/stage.sh --jdk "$JDK" --dir obj/jdk/classes:/C --dir obj/jdk/src:/T \
  --wave "JDK/bin/java -version" > obj/jdk/stage.log 2>&1 || {
    echo "STAGE FAILED -- tail of obj/jdk/stage.log:"; tail -20 obj/jdk/stage.log; exit 2; }
echo "   ok"

want() { [ -z "$ONLY" ] && return 0; [ "$ONLY" = "$1" ]; }

# --- 1. version ------------------------------------------------------------
if want version; then
  say "check version"
  tools/jdk/stage.sh --wave "JDK/bin/java -version" >/dev/null 2>&1
  tools/jdk/boot.sh --timeout "$TIMEOUT" --log obj/jdk/sdk-version.log >/dev/null 2>&1
  if grep -aq "openjdk version" obj/jdk/sdk-version.log; then
    record version PASS "$(grep -a 'openjdk version' obj/jdk/sdk-version.log | head -1 | tr -d '\r')"
  else
    record version FAIL "no 'openjdk version' in obj/jdk/sdk-version.log"
  fi
fi

# --- 2. javac --------------------------------------------------------------
if want javac; then
  say "check javac (compile /T/Hello.java in-guest)"
  tools/jdk/stage.sh --wave "JDK/bin/javac -d /JDKOUT /T/Hello.java" >/dev/null 2>&1
  tools/jdk/boot.sh --timeout "$TIMEOUT" --log obj/jdk/sdk-javac.log >/dev/null 2>&1
  JRC=$?
  if mdir -i disk.img ::/JDKOUT 2>/dev/null | grep -qi "HELLO.*CLA"; then
    if [ $JRC -eq 0 ]; then
      record javac PASS "Hello.class written; javac exited"
    else
      record javac FAIL "Hello.class written but javac never exited (rc=$JRC)"
    fi
  else
    record javac FAIL "no Hello.class on the image"
  fi
fi

# --- 3. run the class javac produced --------------------------------------
if want run; then
  say "check run (java -cp /JDKOUT Hello)"
  tools/jdk/stage.sh --wave "JDK/bin/java -cp /JDKOUT Hello" >/dev/null 2>&1
  tools/jdk/boot.sh --timeout "$TIMEOUT" --log obj/jdk/sdk-run.log >/dev/null 2>&1
  if grep -aq "HELLO-JAVA: java.version" obj/jdk/sdk-run.log; then
    record run PASS "$(grep -a 'HELLO-JAVA: java.version' obj/jdk/sdk-run.log | head -1 | tr -d '\r')"
  else
    record run FAIL "Hello did not print its banner"
  fi
fi

# --- 4. the SDK suite ------------------------------------------------------
if want suite; then
  say "check suite (java -cp /C Verify)"
  tools/jdk/stage.sh --wave "JDK/bin/java -cp /C Verify" >/dev/null 2>&1
  tools/jdk/boot.sh --timeout "$TIMEOUT" --log obj/jdk/sdk-suite.log >/dev/null 2>&1
  SUM=$(grep -a '^SUMMARY' obj/jdk/sdk-suite.log | tail -1 | tr -d '\r')
  if [ -z "$SUM" ]; then
    record suite FAIL "no SUMMARY line (obj/jdk/sdk-suite.log)"
  elif echo "$SUM" | grep -q "fail=0"; then
    record suite PASS "$SUM"
  else
    record suite FAIL "$SUM  -- $(grep -a '^FAIL' obj/jdk/sdk-suite.log | tr -d '\r' | tr '\n' ';')"
  fi
fi

# --- verdict ---------------------------------------------------------------
say "verdict"
FAILED=0
for r in "${RESULT[@]}"; do
  IFS='|' read -r name verdict detail <<<"$r"
  printf '   %-8s %-5s %s\n' "$name" "$verdict" "$detail"
  [ "$verdict" = PASS ] || FAILED=$((FAILED + 1))
done
[ $FAILED -eq 0 ] || { echo "   $FAILED check(s) failed"; exit 1; }
echo "   all checks passed"
exit 0
