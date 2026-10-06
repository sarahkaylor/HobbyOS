#!/usr/bin/env bash
# run_browser_accept.sh — E2E address-bar acceptance run for the HobbyOS
# windowed browser over the host Wikipedia proxy (lane WC / FS).
#
# Boots the ARM startup disk, opens the browser from the Apps menu (F1 path),
# drives the address bar (type-url / go) against a MODE-DEPENDENT URL set and
# collects on-device receipts into <evdir> (serial + screenshots + guest-file
# byte proofs), plus the guest-shell HTTP GET / DNS receipts for the host-net
# gates.  Strict JSON report (schema v2, per-row pass|gated|fail) at
# <evdir>/report.json and on stdout.
#
# Modes (--mode):
#   fixture  serve T0 fixtures (fork wk3 FIX01-05 + OS HOME/TALL) via the
#            proxy /fixture/<name> route; drive + verify render markers/FPC
#   reader   drive /reader/<Topic> (real Wikipedia REST extract)
#   direct   https://<url> via --url (formalized direct leg; classified)
#   full     raw relay of a full article page (--url or /wiki/<Article>)
#   (default) legacy behavior: proxied Wikipedia Main_Page address-bar run
#
# Concurrency (--instance ID): the runner is host-global single-instance by
# default (fixed /tmp sockets + proxy port-file /tmp/wiki-proxy-port).  Pass
# --instance <id> (or ACCEPT_ID=<id>) to scope a run: the ctrl/qmp serial
# sockets become /tmp/br-<id>-*.sock|log and the proxy is told a per-instance
# port file --port-file /tmp/wiki-proxy-port-<id>.  YOU must still give the
# instance a distinct --port.  No flag = exactly the fixed legacy paths, so
# the F-R2 /tmp/wiki-proxy-port contract is preserved for existing consumers.
# Concurrent runs each need their own lock file (e.g. flock /tmp/fs-accept-<id>.lock).
#
# Usage:
#   run_browser_accept.sh [--mode fixture|reader|direct|full] [--url URL]
#                         [--port PORT] [--evdir DIR] [--disk DISK]
#                         [--no-go] [--shell-get PATH] [--keep]
#                         [--boot-timeout S] [--topic TOPIC] [--instance ID]
#
#   --url      URL override (direct/full; default depends on the mode)
#   --port     host wiki-proxy port (default 8800)
#   --mode     row-set selection (see above)
#   --shell-get  also run the guest-shell HTTP GET for PATH (default /robots.txt)
#   --no-go     boot + launch browser only (no address-bar drive)
#   --clock-probe  after rows: launch CONSOLE, run guest CLOCKPX.BIN (sysinfo(6)
#                  RTC wall clock), record host date -u side-by-side (T2-19)
#   --keep     leave qemu up after the run (else poweroff)
#   --topic    reader-mode topic (default: Habitat)
#   --instance run-scope id (concurrency; see above; default: fixed legacy paths)
#
# Host-global single-instance: fixed /tmp sockets + proxy port 8800.  Always
# wrap in `flock -w 7200 /tmp/fs-accept.lock ... --mode ...`.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
: "${WIKI_PORT:=8800}"
URL=""
MODE=""
TOPIC="Habitat"
EV="evidence/e2e"
DISK="$REPO/disk.img"
BOOT_TIMEOUT=420
SHELL_GET=""
DO_GO=1
KEEP=0
CLOCK_PROBE=0
INSTANCE="${ACCEPT_ID:-}"
FIX_DIRS="${WIKI_PROXY_FIXTURES:-}"
if [ -z "$FIX_DIRS" ]; then
  FIX_DIRS="/home/sarah/webkit-lanes/fs-v1/HobbyOS/continuation/wk3/fixtures:$REPO/tests/fixtures/browser"
fi

usage() {
  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --topic) TOPIC="$2"; shift 2 ;;
    --port) WIKI_PORT="$2"; shift 2 ;;
    --evdir) EV="$2"; shift 2 ;;
    --disk) DISK="$2"; shift 2 ;;
    --boot-timeout) BOOT_TIMEOUT="$2"; shift 2 ;;
    --shell-get) SHELL_GET="${2:-/robots.txt}"; shift 2 ;;
    --instance) INSTANCE="$2"; shift 2 ;;
    --no-go) DO_GO=0; shift ;;
    --clock-probe) CLOCK_PROBE=1; shift ;;
    --keep) KEEP=1; shift ;;
    *) usage ;;
  esac
done

case "$MODE" in
  ""|"default"|"fixture"|"reader"|"direct"|"full") ;;
  *) echo "unknown --mode: $MODE"; usage ;;
esac

[ -f "$DISK" ] || { echo "disk not found: $DISK (build with: make ARCH=arm MODE=desktop disk.img BROWSER_BIN=... )"; exit 1; }
mkdir -p "$EV"

PROXY_LOG="$EV/wiki-proxy.log"
NETLOG="$EV/net.log.jsonl"
# instance-scoped /tmp paths; default (no --instance) = fixed legacy paths
SUF=""
PORT_FILE="/tmp/wiki-proxy-port"
if [ -n "$INSTANCE" ]; then
  SUF="-$INSTANCE"
  PORT_FILE="/tmp/wiki-proxy-port$SUF"
  if [ "$WIKI_PORT" = "8800" ]; then
    echo "ERROR: --instance $INSTANCE needs a distinct --port (default 8800 is"
    echo "       the legacy single-instance port).  e.g. --port 8811 --instance a"
    exit 1
  fi
fi
CTRL="/tmp/br-wc-ctrl$SUF.sock"
QMP="/tmp/br-wc-qmp$SUF.sock"
SER="/tmp/br-wc-serial$SUF.log"
RUNNER_LOG="$EV/runner.log"
: > "$RUNNER_LOG"
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$RUNNER_LOG"; }

# ------------------------------------------------------------------ proxy --
rm -f "$PORT_FILE"
log "starting wiki proxy on :$WIKI_PORT (upstream en.wikipedia.org; netlog=$NETLOG; port-file=$PORT_FILE)"
python3 "$REPO/tools/wiki_proxy.py" --port "$WIKI_PORT" --scratch /tmp --log "$PROXY_LOG" \
  --netlog "$NETLOG" --fixtures "$FIX_DIRS" --port-file "$PORT_FILE" \
  >> "$EV/wiki-proxy-console.log" 2>&1 &
PROXY_PID=$!
for _ in $(seq 1 20); do [ -f "$PORT_FILE" ] && break; sleep 0.5; done
PORT="$(cat "$PORT_FILE" 2>/dev/null || echo "$WIKI_PORT")"
log "proxy port file: $PORT_FILE=$PORT (listener=$WIKI_PORT)"
# host self-check: the proxy really serves real wikipedia bytes
curl -sS --max-time 30 -o "$EV/host-robots.txt" -w "proxy self-check HTTP %{http_code} %{size_download} B\n" \
  "http://127.0.0.1:$PORT/robots.txt" | tee -a "$RUNNER_LOG"
MARKERS_HOST="$(grep -ci 'user-agent' "$EV/host-robots.txt" 2>/dev/null || echo 0)"
log "host-side robots.txt marker lines: $MARKERS_HOST"
# fixture route self-check (byte-identical T0 fixture via the new route)
curl -sS --max-time 30 -o "$EV/host-fixture-FIX01.txt" -w "fixture self-check HTTP %{http_code} %{size_download} B\n" \
  "http://127.0.0.1:$PORT/fixture/FIX01" | tee -a "$RUNNER_LOG"
FIX01_OK="$(grep -c 'POK' "$EV/host-fixture-FIX01.txt" 2>/dev/null || echo 0)"
log "host-side /fixture/FIX01 marker lines: $FIX01_OK"

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
  echo '{"schema_version":2,"session":{"ok":false},"verdict":"fail"}'
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

STEP='{}'
step() {  # step <name> <json-msg> -> writes name.json and appends to report
  local name="$1"; shift
  local out
  out="$(ctrl "$1")"
  echo "$out" > "$EV/step-$name.json"
  echo "  step $name -> $out" | tee -a "$RUNNER_LOG"
}

# ------------------------------------------------------- row set (per mode) --
# Rows are (id,kind,url).  Backward-compatible default = the legacy single
# proxied-Main_Page row.
ROWS=""
GHOST="http://10.0.2.2:$PORT"
case "$MODE" in
  fixture)
    for f in FIX01 FIX02 FIX02D FIX03 FIX04 FIX05 HOME TALL; do
      ROWS="$ROWS fixture-$f fixture $GHOST/fixture/$f"
    done
    ;;
  reader)
    ROWS="$ROWS reader-$TOPIC reader $GHOST/reader/$TOPIC"
    ;;
  direct)
    [ -n "$URL" ] || { echo "--mode direct needs --url https://..."; exit 1; }
    ROWS="$ROWS direct direct $URL"
    ;;
  full)
    [ -n "$URL" ] || URL="$GHOST/wiki/Web_browser"
    ROWS="$ROWS full full $URL"
    ;;
  *)
    if [ -n "$URL" ]; then
      ROWS="$ROWS url default $URL"
    else
      ROWS="$ROWS wiki-default default $GHOST/wiki/Main_Page"
    fi
    ;;
esac

if [ "$DO_GO" = 1 ]; then
  set -- $ROWS
  while [ $# -gt 0 ]; do
    RID="$1"; KIND="$2"; RURL="$3"; shift 3
    log "address-bar row $RID ($KIND) -> $RURL"
    step "$RID-type-url" "{\"action\":\"type-url\",\"url\":\"$RURL\"}"
    step "$RID-go" "{\"action\":\"go\",\"url\":\"$RURL\",\"timeout\":300}"
    sleep 1
  done
fi

# ----------------------------------------------- guest-shell net receipts --
if [ "$CLOCK_PROBE" = 1 ]; then
  # post-load screenshot of the final page frame (taken BEFORE the console
  # focus steal, so the browser window is still full-size at native res)
  step post-render-shot "{\"action\":\"shot\",\"name\":\"post-render\"}"
  HOST_T0="$(date -u +%s)"
  HOST_T0_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  log "clock-probe: host t0=$HOST_T0_ISO epoch=$HOST_T0"
  # console app draws to the GUI window, not the UART: redirect probe stdout
  # into a guest-disk file so the runner can copy it back byte-exact.
  step clock-probe "{\"action\":\"console\",\"cmd\":\"CLOCKPX > /CLOCKPX.TXT\",\"launch\":true,\"settle\":6.0,\"type_pause\":0.18}"
  sleep 1
  step clock-probe2 "{\"action\":\"console\",\"cmd\":\"cat /CLOCKPX.TXT\",\"launch\":false,\"settle\":6.0,\"type_pause\":0.18}"
  HOST_T1="$(date -u +%s)"
  HOST_T1_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  log "clock-probe: host t1=$HOST_T1_ISO epoch=$HOST_T1"
  CLOCKPX_COPY=""
  if mcopy -i "$STAGE" ::/CLOCKPX.TXT "$EV/CLOCKPX.TXT" 2>/dev/null; then
    CLOCKPX_COPY="$(cat "$EV/CLOCKPX.TXT" 2>/dev/null | head -4)"
  fi
  {
    echo "# guest-clock probe (T2-19) — host side"
    echo "host_t0_iso=$HOST_T0_ISO host_t0_epoch=$HOST_T0"
    echo "host_t1_iso=$HOST_T1_ISO host_t1_epoch=$HOST_T1"
    echo "# guest side (CLOCKPX.BIN -> /CLOCKPX.TXT copy-back):"
    [ -n "$CLOCKPX_COPY" ] && echo "$CLOCKPX_COPY" || echo "(no guest copy-back)"
  } > "$EV/clock-probe.txt"
  log "clock-probe receipt written to $EV/clock-probe.txt"
fi
if [ -n "$SHELL_GET" ]; then
  SHELL_URL="$GHOST$SHELL_GET"
  # Deterministic receipt: the request file is staged ONTO the guest disk by
  # the runner (byte-exact, CRLF, host-written) — the guest only types one
  # simple redirect line (`nc < REQ > ROB`), the shape proven by the WK-6
  # mem-probe receipts.  No multi-line `echo` construction (raced the shell).
  SHELL_GET_HEAD="${SHELL_GET_HEAD:-?head=900}"
  printf 'GET %s%s HTTP/1.0\r\nHost: 10.0.2.2:%s\r\n\r\n' "$SHELL_GET" "$SHELL_GET_HEAD" "$PORT" > "$EV/REQ.WC"
  mcopy -i "$STAGE" "$EV/REQ.WC" ::/REQ.WC
  log "staged /REQ.WC on the guest disk ($(wc -c < "$EV/REQ.WC") B, GET $SHELL_GET$SHELL_GET_HEAD)"
  step console-warmup "{\"action\":\"console\",\"cmd\":\"echo WARMUP-WC > /TMP.WC\"}"
  for i in 1 2 3; do
    step "console-nc$i" "{\"action\":\"console\",\"cmd\":\"nc 10.0.2.2 $PORT < /REQ.WC > /ROB.WC.$i &\",\"launch\":false,\"type_pause\":0.18}"
    log "guest GET attempt $i issued; waiting 8s for response in /ROB.WC.$i"
    sleep 8
  done
  step console-last "{\"action\":\"console\",\"cmd\":\"cat /ROB.WC.3\",\"launch\":false,\"settle\":2.0}"
  sleep 2
  step shell-robots-shot "{\"action\":\"shot\",\"name\":\"console-shell\"}"
  step sock2tst "{\"action\":\"console\",\"cmd\":\"sock2tst\",\"settle\":10.0}"
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

# ------------------------------------------------------------ report v2 -----
python3 - "$EV" "$PORT" "$URL" "$MODE" "$SHELL_GET" "$MARKERS_HOST" "$PG" "$RW" "$ROWS" "$TOPIC" "$FIX01_OK" "$PORT_FILE" "$INSTANCE" <<'EOF'
import hashlib, json, os, re, sys
ev, port, url, mode, shell_get, mhost, pg, rw, rows, topic, fix01, port_file, instance = sys.argv[1:]
ev = ev.strip('"'); port = port.strip('"'); url = url.strip('"')
mode = mode.strip('"'); shell_get = shell_get.strip('"') if shell_get.strip('"') else None
mhost = int(mhost.strip('"'))
fix01 = int(fix01.strip('"'))
port_file = port_file.strip('"')
instance = instance.strip('"') or None
rows = rows.split()

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
          "[WIN] net persist", "[WIN] render", "[WIN] checksum",
          "[LAUNCH] CONSOLE.BIN", "[DNSTST]",
          "DHCP DNS server", "Network configured", "FATAL", "IDLESTUCK",
          "SOCK2TST"]:
    markers[m] = serial.count(m)

# ---- per-row status (STRICT: pass|gated|fail) ----
#   pass:  the browser's own load-ok marker for exactly this URL + a frame
#   gated: network layers reached (net fetch / url-entry) but no render, or
#          an explicit open-fail recorded with evidence (classified, not hung)
#   fail:  nothing observed for the row (no entry echo, no fetch) or FATAL
fail_re = re.compile(r"FATAL")
row_results = []
i = 0
while i < len(rows):
    rid, kind, rurl = rows[i], rows[i+1], rows[i+2]
    i += 3
    g = load(rid + "-go")
    t = load(rid + "-type-url")
    if g.get("load_ok"):
        status = "pass"
    elif g.get("open_fail") or g.get("entry_echo") or g.get("net"):
        status = "gated"
    else:
        status = "fail"
    # system-fatal overrides every row
    if fail_re.search(serial):
        status = "fail"
    row_results.append({
        "id": rid, "kind": kind, "url": rurl, "status": status,
        "load_ok": bool(g.get("load_ok")), "load_ms": g.get("load_ms"),
        "open_fail": g.get("open_fail"), "frames": g.get("frames", 0),
        "checksum": g.get("checksum"), "net": g.get("net"),
        "persist": g.get("persist"), "screenshot": g.get("screenshot"),
        "entry_echo": g.get("entry_echo"), "type_url_ok": bool(t.get("ok")),
        "evidence": {"go_step": bool(g), "type_url_step": bool(t)},
    })

n_pass = sum(1 for r in row_results if r["status"] == "pass")
n_gated = sum(1 for r in row_results if r["status"] == "gated")
n_fail = sum(1 for r in row_results if r["status"] == "fail")

verdict = "pass"
notes = []
if n_fail > 0:
    verdict = "fail"
    notes.append("fail rows: %s" % ", ".join(r["id"] for r in row_results if r["status"] == "fail"))
elif n_pass == 0:
    verdict = "gated"
    notes.append("no row reached load-ok (%d gated)" % n_gated)
elif n_gated > 0:
    verdict = "gated"   # all-rows-green is the gate; any gated row -> gated leg
    notes.append("rows gated: %s" % ", ".join(r["id"] for r in row_results if r["status"] == "gated"))
if fatal := (re.search(r"FATAL", serial) or ""):
    notes.append("serial carries FATAL: " + fatal[:120])

screens = sorted(n for n in os.listdir(ev) if n.endswith(".png"))
netlog_file = os.path.join(ev, "net.log.jsonl")
netlog = {"rel_path": "net.log.jsonl", "lines": 0, "sha256": None}
if os.path.isfile(netlog_file):
    try:
        with open(netlog_file) as f:
            netlog["lines"] = sum(1 for _ in f)
    except OSError:
        pass
    netlog["sha256"] = sha(netlog_file)
    # quick route census of the netlog (per-request structured capture)
    try:
        routes = {}
        with open(netlog_file) as f:
            for ln in f:
                if not ln.strip():
                    continue
                try:
                    rec = json.loads(ln)
                except Exception:
                    continue
                if rec.get("event"):
                    continue
                routes[rec.get("route", "?")] = routes.get(rec.get("route", "?"), 0) + 1
        netlog["routes"] = routes
    except Exception:
        pass

artifacts = {}
for n in ("serial.log", "driver.log", "wiki-proxy.log", "net.log.jsonl",
          "host-robots.txt", "host-fixture-FIX01.txt", "PAGE-NET.HTM",
          "ROB.WC", "WNPAGE.HTM", "wiki-proxy-console.log"):
    p = os.path.join(ev, n)
    if os.path.isfile(p):
        artifacts[n] = {"size": os.path.getsize(p), "sha256": sha(p)}

report = {
    "toolkit": "fs-browser-accept", "schema_version": 2,
    "report_schema": "hobbyos-browser-accept-v2",
    "mode": mode, "topic": topic if mode == "reader" else None,
    "url": url, "instance": instance,
    "rows": row_results,
    "proxy": {"port": int(port), "port_file": port_file,
              "upstream": "https://en.wikipedia.org",
              "host_self_check_robots_markers": mhost,
              "fixture_self_check_FIX01_markers": fix01},
    "netlog": netlog,
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
        "row_counts": {"n_pass": n_pass, "n_gated": n_gated, "n_fail": n_fail},
        "notes": notes,
        "markers": markers,
        "screenshots": screens,
        "artifacts": artifacts,
        "sha256_page_net": pg,
        "sha256_rob_wc": rw,
    },
    "summary": {"verdict": verdict, "n_pass": n_pass, "n_gated": n_gated,
                "n_fail": n_fail, "rows": len(row_results)},
}
with open(os.path.join(ev, "report.json"), "w") as f:
    json.dump(report, f, indent=2)
print(json.dumps({"verdict": verdict, "n_pass": n_pass, "n_gated": n_gated,
                  "n_fail": n_fail, "notes": notes, "markers": markers,
                  "screenshots": screens, "page_sha": pg, "rob_sha": rw,
                  "netlog_lines": netlog["lines"]}))
EOF
echo "== done: evidence in $EV (report.json v2)"
echo "== verdict summary: $(python3 -c "import json;r=json.load(open('$EV/report.json'));s=r['summary'];print('verdict=%s n_pass=%d n_gated=%d n_fail=%d rows=%d'%(s['verdict'],s['n_pass'],s['n_gated'],s['n_fail'],s['rows']))")"
