#!/usr/bin/env bash
# tools/jdk/nettest.sh -- does java.net TCP work over the guest's real NIC?
#
# The suite's sockets-loopback check proves only the in-kernel 127.0.0.1 pair
# (fs.c answers that in-kernel).  This runs tools/jdk/tests/TcpProbe under the
# pinned JDK against an echo server on the HOST, reached the way the other lanes
# reach it: 10.0.2.2 is QEMU slirp's alias for the host loopback
# (tools/run_browser_accept.sh, tools/rw/nfs-export.sh use the same address).
#
# Nothing depends on the internet or on DNS: the peer is a literal address and
# the server is a python process started and stopped by this script.  So a green
# run means "the guest's TCP client works", not "the network was up today".
#
# What it asserts (each line is graded by TcpProbe):
#   resolve-literal       InetAddress parses the literal without a resolver
#   connect-echo          SYN/SYN-ACK/ACK + byte-identical round trip
#   local-address         the socket is bound to the DHCP lease, not 0.0.0.0
#   bulk-8192             8192 B = 6 segments past TCP_MAX_PAYLOAD (1446)
#   reconnect             a second connection after the first closed
#   refused-closed-port   the host's RST arrives as ConnectException, not a hang
#
# Usage:
#   tools/jdk/nettest.sh                 # build kernel if needed, reuse staged JDK
#   tools/jdk/nettest.sh --no-build      # skip make (reuse hobbyos.elf/disk.img)
#   tools/jdk/nettest.sh --stage-jdk     # re-copy the JDK tree into the image
#   PORT=18089 tools/jdk/nettest.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

JDK="${JDK_TREE:-$ROOT/third_party/jdk-27}"
PORT="${PORT:-18089}"
BUILD=1
STAGE_JDK=0
TIMEOUT=${NETTEST_TIMEOUT:-420}
OUT="$ROOT/obj/jdk"
LOG="$OUT/nettest-tcp.log"
SRV="$OUT/nettest/echo_server.py"
READY="$OUT/nettest/echo.ready"

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)  BUILD=0; shift ;;
    --stage-jdk) STAGE_JDK=1; shift ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done

say() { printf '\n== %s\n' "$*"; }
[ -d "$JDK" ] || { echo "no JDK tree at $JDK (run tools/jdk/fetch-jdk.sh)"; exit 2; }
mkdir -p "$OUT/nettest"

# --- the host side: an echo server, started before the boot, killed after ----
cat > "$SRV" <<'PY'
# Echo server for tools/jdk/nettest.sh.  Binds the host loopback only, so the
# only way in is through slirp's 10.0.2.2 translation -- which is the thing
# under test.  Readiness is signalled by creating argv[2]; the runner polls it
# instead of sleeping, because a sleep that is too short produces a guest
# "connection refused" that looks like a stack bug.
import socket, sys, threading
port, ready = int(sys.argv[1]), sys.argv[2]

def handle(c):
    try:
        c.settimeout(120)
        while True:
            b = c.recv(65536)
            if not b:
                break
            c.sendall(b)          # echo byte-for-byte
    except Exception:
        pass                      # RST/EOF mid-stream is the probe's business
    finally:
        try: c.close()
        except Exception: pass

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(8)
open(ready, "w").write(str(port))
while True:
    try:
        c, _ = s.accept()
    except Exception:
        break
    threading.Thread(target=handle, args=(c,), daemon=True).start()
PY

rm -f "$READY"
python3 "$SRV" "$PORT" "$READY" > "$OUT/nettest/echo.log" 2>&1 &
SRV_PID=$!
cleanup() { kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 1 50); do
  [ -f "$READY" ] && break
  sleep 0.1
done
[ -f "$READY" ] || { echo "echo server never became ready (see $OUT/nettest/echo.log)"; exit 2; }
# Self-check the server from the host, so "the guest could not connect" is not
# confounded with "the server was not listening".
python3 - "$PORT" <<'PY' || { echo "host self-check failed: server not answering"; exit 2; }
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(b"hostselftest\n")
assert s.recv(64) == b"hostselftest\n", "echo mismatch"
s.close()
PY
echo "   host echo server ready on 127.0.0.1:$PORT (guest sees 10.0.2.2:$PORT)"

# --- build (same invocation as sdktest) ------------------------------------
if [ "$BUILD" = 1 ]; then
  say "build (ARCH=intel MODE=test)"
  if ! TOOLS_SKIP=1 BROWSER_SKIP=1 make -j8 ARCH=intel MODE=test \
        hobbyos.elf disk.img > "$OUT/build-nettest.log" 2>&1; then
    echo "BUILD FAILED -- tail of $OUT/build-nettest.log:"; tail -25 "$OUT/build-nettest.log"; exit 2
  fi
  echo "   ok   kernel $(stat -f%z hobbyos.elf 2>/dev/null || stat -c%s hobbyos.elf) bytes"
fi

# --- compile the probe on the host (bytecode level <= the guest's) ----------
say "compile the probe (host javac)"
HOSTJAVAC="${HOSTJAVAC:-/opt/homebrew/opt/openjdk@26/bin/javac}"
[ -x "$HOSTJAVAC" ] || HOSTJAVAC=$(command -v javac)
[ -n "$HOSTJAVAC" ] || { echo "no host javac found (set HOSTJAVAC)"; exit 2; }
mkdir -p "$OUT/classes"
"$HOSTJAVAC" -nowarn -d "$OUT/classes" tools/jdk/tests/TcpProbe.java 2>&1 | grep -v '^Note:' || true
[ -f "$OUT/classes/TcpProbe.class" ] || { echo "TcpProbe did not compile"; exit 2; }
echo "   ok   obj/jdk/classes/TcpProbe.class"

# --- stage: the JDK tree only when the image does not already carry it -----
say "stage"
JDK_ARG=()
if [ "$STAGE_JDK" = 1 ] || ! mdir -i disk.img ::/JDK >/dev/null 2>&1; then
  JDK_ARG=(--jdk "$JDK")
fi
tools/jdk/stage.sh "${JDK_ARG[@]}" --dir "$OUT/classes:/C" \
  --wave "JDK/bin/java -cp /C TcpProbe 10.0.2.2 $PORT" > "$OUT/nettest-stage.log" 2>&1 || {
    echo "STAGE FAILED -- tail of $OUT/nettest-stage.log:"; tail -20 "$OUT/nettest-stage.log"; exit 2; }
grep -a "WAVE.CFG" -A2 "$OUT/nettest-stage.log" | tail -3
echo "   ok"

# --- boot ------------------------------------------------------------------
say "boot (deadline ${TIMEOUT}s)"
tools/jdk/boot.sh --timeout "$TIMEOUT" --log "$LOG"
BOOT_RC=$?
echo "   boot rc=$BOOT_RC log=$LOG"

# --- grade ----------------------------------------------------------------
say "result"
grep -a "^TCP " "$LOG" | sed 's/\r//' | sed 's/^/   /'
SUM=$(grep -a "^TCP SUMMARY" "$LOG" | tail -1 | sed 's/\r//')
if [ -z "$SUM" ]; then
  echo "   FAIL  no TCP SUMMARY line -- the probe did not finish"
  echo "   last console lines:"; tail -8 "$LOG" | sed 's/^/     /'
  exit 1
fi
if echo "$SUM" | grep -q "fail=0"; then
  echo "   PASS  $SUM"
  exit 0
fi
echo "   FAIL  $SUM"
exit 1
