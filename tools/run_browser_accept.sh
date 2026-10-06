#!/usr/bin/env bash
# run_browser_accept.sh — E2E address-bar acceptance run for the HobbyOS
# windowed browser over the host Wikipedia proxy (lane WC).
#
# Boots the ARM startup disk, opens the browser from the Apps menu (F1 path),
# drives the address bar (type-url / go) against the proxied Wikipedia URL and
# collects on-device receipts into <evdir> (serial + screenshots + guest-file
# byte proofs), plus the guest-shell HTTP GET / DNS receipts for the host-net
# gates.  Strict JSON report at <evdir>/report.json and on stdout.
#
# Usage:
#   run_browser_accept.sh [--url URL] [--port PORT] [--evdir DIR]
#                         [--disk DISK] [--no-go] [--shell-get PATH]
#                         [--keep] [--boot-timeout S]
#
#   --url      URL to drive in the address bar (default: the proxied wikipedia
#              Main_Page at http://10.0.2.2:$PORT/wiki/Main_Page)
#   --port     host wiki-proxy port (default 8800)
#   --shell-get  also run the guest-shell HTTP GET for PATH (default /robots.txt)
#   --no-go    boot + launch browser only (no address-bar drive)
#   --keep     leave qemu up after the run (else poweroff)
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
: "${WIKI_PORT:=8800}"
URL=""
EV="evidence/e2e"
DISK="$REPO/disk.img"
BOOT_TIMEOUT=420
SHELL_GET=""
DO_GO=1
KEEP=0

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --url) URL="$2"; shift 2 ;;
    --port) WIKI_PORT="$2"; shift 2 ;;
    --evdir) EV="$2"; shift 2 ;;
    --disk) DISK="$2"; shift 2 ;;
    --boot-timeout) BOOT_TIMEOUT="$2"; shift 2 ;;
    --shell-get) SHELL_GET="${2:-/robots.txt}"; shift 2 ;;
    --no-go) DO_GO=0; shift ;;
    --keep) KEEP=1; shift ;;
    *) usage ;;
  esac
done

[ -f "$DISK" ] || { echo "disk not found: $DISK (build with: make ARCH=arm MODE=desktop disk.img)"; exit 1; }
mkdir -p "$EV"

PROXY_LOG="$EV/wiki-proxy.log"
CTRL="/tmp/br-wc-ctrl.sock"
QMP="/tmp/br-wc-qmp.sock"
SER="/tmp/br-wc-serial.log"
RUNNER_LOG="$EV/runner.log"
: > "$RUNNER_LOG"
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$RUNNER_LOG"; }

# ------------------------------------------------------------------ proxy --
rm -f /tmp/wiki-proxy-port
log "starting wiki proxy on :$WIKI_PORT (upstream en.wikipedia.org)"
python3 "$REPO/tools/wiki_proxy.py" --port "$WIKI_PORT" --scratch /tmp --log "$PROXY_LOG" \
  >> "$EV/wiki-proxy-console.log" 2>&1 &
PROXY_PID=$!
for _ in $(seq 1 20); do [ -f /tmp/wiki-proxy-port ] && break; sleep 0.5; done
PORT="$(cat /tmp/wiki-proxy-port 2>/dev/null || echo "$WIKI_PORT")"
log "proxy port file: /tmp/wiki-proxy-port=$PORT (listener=$WIKI_PORT)"
# host self-check: the proxy really serves real wikipedia bytes
curl -sS --max-time 30 -o "$EV/host-robots.txt" -w "proxy self-check HTTP %{http_code} %{size_download} B\n" \
  "http://127.0.0.1:$PORT/robots.txt" | tee -a "$RUNNER_LOG"
MARKERS_HOST="$(grep -ci 'user-agent' "$EV/host-robots.txt" 2>/dev/null || echo 0)"
log "host-side robots.txt marker lines: $MARKERS_HOST"

# ------------------------------------------------------------- disk stage --
STAGE="$EV/disk.img"
if [ "$STAGE" != "$DISK" ]; then
  cp -f "$DISK" "$STAGE"
  # WK-6 (WN3): the real-URL fetch facade file:///WNPAGE.HTM must exist on the
  # disk for in-place substitute replacement of the fetched body.
  if ! mdir -i "$STAGE" ::/WNPAGE.HTM >/dev/null 2>&1; then
    printf ' ' > "$EV/WNPAGE.HTM"
    mcopy -i "$STAGE" "$EV/WNPAGE.HTM" ::/WNPAGE.HTM
    log "staged /WNPAGE.HTM on the guest disk"
  fi
fi

# --------------------------------------------------------------- QEMU ------
rm -f "$CTRL" "$QMP" "$SER"
log "booting ARM disk ($STAGE, smp8/16G AAVMF); boot timeout ${BOOT_TIMEOUT}s"
python3 "$REPO/tools/br_e2e.py" --disk "$STAGE" --qmp "$QMP" --serial "$SER" \
  --evdir "$EV" --serve "$CTRL" --boot-timeout "$BOOT_TIMEOUT" \
  > "$EV/driver.log" 2>&1 &
DRIVER_PID=$!

READY=0
for _ in $(seq 1 600); do
  if grep -q 'READY ctrl=' "$EV/driver.log" 2>/dev/null; then READY=1; break; fi
  if ! kill -0 "$DRIVER_PID" 2>/dev/null; then
    log "driver died early: $(tail -3 "$EV/driver.log")"
    break
  fi
  sleep 1
done
if [ "$READY" != 1 ]; then
  log "FAIL: browser never reached READY"
  cp -f "$SER" "$EV/serial.log" 2>/dev/null || true
  echo '{"session":{"ok":false},"verdict":"fail"}'
  exit 1
fi
log "driver READY (desktop + browser up)"

# ctrl socket helpers
ctrl() {
  local payload="$1"
  python3 - "$CTRL" "$payload" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(600)
s.connect(sys.argv[1])
s.sendall(sys.argv[2].encode() + b"\n")
buf = b""
while True:
    c = s.recv(1 << 16)
    if not c:
        break
    buf += c
    if buf.endswith(b"\n"):
        break
s.close()
print(buf.decode(errors="replace").strip())
EOF
}

STEPS='{}'
step() {  # step <name> <json-msg> -> writes name.json and appends to report
  local name="$1"; shift
  local out
  out="$(ctrl "$1")"
  echo "$out" > "$EV/step-$name.json"
  echo "  step $name -> $out" | tee -a "$RUNNER_LOG"
}

# ------------------------------------------------------ address-bar drive --
if [ "$DO_GO" = 1 ]; then
  if [ -z "$URL" ]; then
    URL="http://10.0.2.2:$PORT/wiki/Main_Page"
  fi
  log "address-bar run -> $URL"
  step type-url "{\"action\":\"type-url\",\"url\":\"$URL\"}"
  step go "{\"action\":\"go\",\"url\":\"$URL\",\"timeout\":300}"
fi

# ----------------------------------------------- guest shell net receipts --
if [ -n "$SHELL_GET" ]; then
  SHELL_URL="http://10.0.2.2:$PORT$SHELL_GET"
  # Deterministic receipt: the request file is staged ONTO the guest disk by
  # the runner (byte-exact, CRLF, host-written) — the guest only types one
  # simple redirect line (`nc < REQ > ROB`), the shape proven by the WK-6
  # mem-probe receipts.  No multi-line `echo` construction (raced the shell).
  # The target uses ?head=N so the relayed body (a byte-exact prefix of the
  # REAL wikipedia robots.txt, markers at ~600 B) fits the guest's fixed
  # 2048-B TCP window (net.c window_size=2048 — see the proxy doc).
  SHELL_GET_HEAD="${SHELL_GET_HEAD:-?head=900}"
  printf 'GET %s%s HTTP/1.0\r\nHost: 10.0.2.2:%s\r\n\r\n' "$SHELL_GET" "$SHELL_GET_HEAD" "$PORT" > "$EV/REQ.WC"
  mcopy -i "$STAGE" "$EV/REQ.WC" ::/REQ.WC
  log "staged /REQ.WC on the guest disk ($(wc -c < "$EV/REQ.WC") B, GET $SHELL_GET$SHELL_GET_HEAD)"
  step console-warmup "{\"action\":\"console\",\"cmd\":\"echo WARMUP-WC > /TMP.WC\"}"
  # The guest TCP connect/send leg is stochastic (WK-6 racy-RX + fixed 2048-B
  # window classes; run3 landed one fully-served guest GET, runs5/6 the
  # request echo only).  Retry a few bounded attempts so a landed connection
  # is caught; each writes its own file (nc never exits on EOF).
  for i in 1 2 3; do
    step "console-nc$i" "{\"action\":\"console\",\"cmd\":\"nc 10.0.2.2 $PORT < /REQ.WC > /ROB.WC.$i &\",\"launch\":false,\"type_pause\":0.18}"
    log "guest GET attempt $i issued; waiting 8s for response in /ROB.WC.$i"
    sleep 8
  done
  step console-last "{\"action\":\"console\",\"cmd\":\"cat /ROB.WC.3\",\"launch\":false,\"settle\":2.0}"
  sleep 2
  step shell-robots-shot "{\"action\":\"shot\",\"name\":\"console-shell\"}"
  # Independent on-device live HTTP GET: SOCK2TST does a real non-blocking
  # connect + GET round-trip and prints the outcome to the serial.
  step sock2tst "{\"action\":\"console\",\"cmd\":\"sock2tst\",\"settle\":10.0}"
  # DNS on-serial receipt: a FRESH console launch worked (run3: [DNSTST]
  # printed to the serial); typing into the reused console did not.
  step dns-start "{\"action\":\"console\",\"cmd\":\"dnstst\",\"settle\":5.0}"
  sleep 2
fi

step final-status "{\"action\":\"status\"}"
cp -f "$SER" "$EV/serial.log"
if [ "$KEEP" != 1 ]; then
  ctrl '{"action":"poweroff"}' >/dev/null 2>&1
  sleep 2
  kill -0 "$DRIVER_PID" 2>/dev/null && { kill "$DRIVER_PID" 2>/dev/null; sleep 1; }
fi
kill -0 "$PROXY_PID" 2>/dev/null && kill "$PROXY_PID" 2>/dev/null

# ------------------------------------------------------- byte level proof --
# guest-file copy-back: the browser persists the fetched page to /PAGE-NET.HTM
# and the shell nc writes /ROB.WC; both are on the staged disk.
PG=""
if mdir -i "$STAGE" ::/PAGE-NET.HTM >/dev/null 2>&1; then
  mcopy -i "$STAGE" ::/PAGE-NET.HTM "$EV/PAGE-NET.HTM"
  PG="$(sha256sum "$EV/PAGE-NET.HTM" | cut -d' ' -f1)"
  log "browser page copy-back: PAGE-NET.HTM sha256=$PG size=$(stat -c %s "$EV/PAGE-NET.HTM")"
fi
RW=""
if [ -n "$SHELL_GET" ]; then
  # per-attempt nc output files on the guest disk; merge host-side
  ( mcopy -i "$STAGE" ::/ROB.WC.1 "$EV/ROB.WC.1" 2>/dev/null; \
    mcopy -i "$STAGE" ::/ROB.WC.2 "$EV/ROB.WC.2" 2>/dev/null; \
    mcopy -i "$STAGE" ::/ROB.WC.3 "$EV/ROB.WC.3" 2>/dev/null )
  : > "$EV/ROB.WC"
  for f in "$EV"/ROB.WC.[123]; do
    [ -f "$f" ] && cat "$f" >> "$EV/ROB.WC"
  done
  if [ -s "$EV/ROB.WC" ]; then
    RW="$(sha256sum "$EV/ROB.WC" | cut -d' ' -f1)"
    log "shell GET copy-back: ROB.WC sha256=$RW size=$(stat -c %s "$EV/ROB.WC") marker=$(grep -ci 'user-agent' "$EV/ROB.WC" || echo 0)"
  fi
fi

# ------------------------------------------------------------ report -------
python3 - "$EV" "$PORT" "$URL" "$SHELL_GET" "$MARKERS_HOST" "$PG" "$RW" <<'EOF'
import hashlib, json, os, re, sys
ev, port, url, shell_get, mhost, pg, rw = sys.argv[1:]
ev = ev.strip('"'); port = port.strip('"'); url = url.strip('"')
shell_get = shell_get.strip('"') if shell_get.strip('"') else None

def sha(p):
    try:
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for c in iter(lambda: f.read(1 << 20), b""):
                h.update(c)
        return h.hexdigest()
    except OSError:
        return None

def load(name):
    p = os.path.join(ev, "step-" + name + ".json")
    try:
        with open(p) as f:
            return json.load(f)
    except OSError:
        return {}

serial = ""
try:
    with open(os.path.join(ev, "serial.log"), errors="ignore") as f:
        serial = f.read()
except OSError:
    pass

guest_http = {
    "sock2tst_live_get_passes": len(re.findall(r"SOCK2TST .*: PASS", serial)),
    "sock2tst_response_prefix": [
        l for l in serial.splitlines() if "SOCK2TST response prefix" in l],
}

markers = {}
for m in ["[WIN] BOOT", "[WIN] created", "[WIN] geom", "[WIN] url-prompt",
          "[WIN] url-entry", "[WIN] net fetch", "[WIN] load start url=",
          "[WIN] load-ok", "[WIN] load open-fail", "[WIN] frame",
          "[WIN] net persist", "[LAUNCH] CONSOLE.BIN", "[DNSTST]",
          "DHCP DNS server", "Network configured", "FATAL", "IDLESTUCK",
          "SOCK2TST"]:
    markers[m] = serial.count(m)

go = load("go")
type_url = load("type-url")
verdict = "fail"
notes = []
if go.get("load_ok"):
    verdict = "pass"
elif go.get("open_fail"):
    verdict = "gated"   # proxied fetch worked at the network layer? open-fail
    notes.append("browser reported open-fail: " + str(go.get("open_fail"))[:160])
elif type_url and not url:
    verdict = "pass"    # --no-go: boot+launch+hooks evidence only
    notes.append("no address-bar drive requested")
else:
    notes.append("no load-ok and no open-fail observed")

screens = sorted(n for n in os.listdir(ev) if n.endswith(".png"))
artifacts = {}
for n in ("serial.log", "driver.log", "wiki-proxy.log", "host-robots.txt",
          "PAGE-NET.HTM", "ROB.WC", "WNPAGE.HTM"):
    p = os.path.join(ev, n)
    if os.path.isfile(p):
        artifacts[n] = {"size": os.path.getsize(p), "sha256": sha(p)}

report = {
    "toolkit": "wc-e2e-browser-accept", "schema": 1,
    "proxy": {"port": int(port), "port_file": "/tmp/wiki-proxy-port",
              "upstream": "https://en.wikipedia.org",
              "host_self_check_robots_markers": int(mhost)},
    "url": url,
    "steps": {"type_url": type_url, "go": go},
    "guest_http": guest_http,
    "dns": {
        "guest_dhcp_dns": "10.0.2.3",
        "serial_evidence": markers.get("DHCP DNS server", 0) > 0,
        "resolver": "guest resolv_lookup -> 10.0.2.3 (QEMU user-net -> host resolver)",
        "etc_hosts": "no /etc/hosts on this OS; resolver queries 10.0.2.3 directly",
    },
    "session": {
        "ok": verdict == "pass",
        "verdict": verdict,
        "notes": notes,
        "markers": markers,
        "screenshots": screens,
        "artifacts": artifacts,
        "sha256_page_net": pg,
        "sha256_rob_wc": rw,
    },
}
with open(os.path.join(ev, "report.json"), "w") as f:
    json.dump(report, f, indent=2)
print(json.dumps({"verdict": verdict, "notes": notes, "markers": markers,
                  "screenshots": screens, "page_sha": pg, "rob_sha": rw}))
EOF
echo "== done: evidence in $EV (report.json)"
