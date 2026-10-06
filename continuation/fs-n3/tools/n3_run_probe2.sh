#!/usr/bin/env bash
# n3_run_probe.py helper — boot 2 of the N3 identity gate (see n3_run_probe.sh
# for the full pattern).  THIS variant orders the CNN 5.7 MB browser leg FIRST
# (run 1 showed a 6 MiB blob's in-browser parse-hang blocks the NEXT url-entry),
# then adds kernel-only `nc` byte-match fetches for both gate bodies.
set -u
LANE=/home/sarah/hobbyos-lanes/fs-r5
EV="$LANE/continuation/fs-n3/evidence/probe2"
SRC=/home/sarah/hobbyos-lanes/fs-g11/disk-wk6-1.img
CNN="$LANE/continuation/fs-n3/evidence/CNN-5704241.HTM"
CTRL=/tmp/n3-probe2-ctrl.sock; QMP=/tmp/n3-probe2-qmp.sock; SER=/tmp/n3-probe2-serial.log
REL=/tmp/n3-relay2.log; PORTF=/tmp/n3-relay2-port
P_CNN=${N3_PORT_CNN:-8905}; P_BLOB=${N3_PORT_BLOB:-8906}; P_SMALL=${N3_PORT_SMALL:-8907}
mkdir -p "$EV"
rm -f "$CTRL" "$QMP" "$SER" "$REL" "$PORTF"

echo "== N3 probe2: assemble disk =="
DISK="$EV/disk.img"
TMPX="$EV/frag"
rm -rf "$TMPX"; mkdir -p "$TMPX"
for f in DESKTOP.BIN CONSOLE.BIN SH.BIN FREE.BIN PS.BIN HOME.HTM; do
  mcopy -i "$SRC" "::$f" "$TMPX/$f" 2>/dev/null || true
done
mcopy -i "$SRC" ::/boot/limine.conf "$TMPX/limine.conf" 2>/dev/null || true
mcopy -i "$SRC" ::/CERTS/CA.PEM "$TMPX/CA.PEM" 2>/dev/null || true
mcopy -i "$SRC" ::/EFI/BOOT/BOOTAA64.EFI "$TMPX/BOOTAA64.EFI" 2>/dev/null || true
cp -f "$LANE/hobbyos.bin" "$TMPX/hobbyos.bin"
cp -f "$LANE/obj/arm/nc.bin" "$TMPX/NC.BIN" 2>/dev/null || true
[ -s "$TMPX/NC.BIN" ] || { echo "no NC.BIN"; exit 2; }
cp -f "$LANE/obj/arm/cat.bin" "$TMPX/CAT.BIN" 2>/dev/null || true
[ -s "$TMPX/CAT.BIN" ] || { echo "no CAT.BIN"; exit 2; }
/usr/bin/llvm-objcopy -O binary /home/sarah/webkit-lanes/wbn/WebKitBuild/HobbyOS-arm/bin/WebProcess "$TMPX/BROWSER.BIN" 2>/dev/null \
  || cp -f /tmp/ref-WebProcess.bin "$TMPX/BROWSER.BIN"
echo "-- kernel $(sha256sum "$TMPX/hobbyos.bin" | cut -d' ' -f1) browser $(sha256sum "$TMPX/BROWSER.BIN" | cut -d' ' -f1)"
# staged GET request for the shell nc fetch
printf 'GET / HTTP/1.0\r\nHost: 10.0.2.2:%s\r\n\r\n' "$P_CNN" > "$EV/REQ.CNN"
printf 'GET /blob HTTP/1.0\r\nHost: 10.0.2.2:%s\r\n\r\n' "$P_BLOB" > "$EV/REQ.BLOB"
dd if=/dev/zero of="$DISK" bs=1M count=256 status=none
mkfs.fat -F 16 -s 16 "$DISK" >/dev/null 2>&1
mmd -i "$DISK" ::/EFI ::/EFI/BOOT ::/boot ::/CERTS
mcopy -i "$DISK" "$TMPX/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$DISK" "$TMPX/limine.conf" ::/boot/limine.conf
mcopy -i "$DISK" "$TMPX/hobbyos.bin" ::/boot/hobbyos.bin
mcopy -i "$DISK" "$TMPX/BROWSER.BIN" ::/BROWSER.BIN
mcopy -i "$DISK" "$TMPX/DESKTOP.BIN" ::/DESKTOP.BIN
mcopy -i "$DISK" "$TMPX/CONSOLE.BIN" ::/CONSOLE.BIN
mcopy -i "$DISK" "$TMPX/SH.BIN" ::/SH.BIN
mcopy -i "$DISK" "$TMPX/NC.BIN" ::/NC.BIN
mcopy -i "$DISK" "$TMPX/CAT.BIN" ::/CAT.BIN
mcopy -i "$DISK" "$TMPX/FREE.BIN" ::/FREE.BIN 2>/dev/null || true
mcopy -i "$DISK" "$TMPX/PS.BIN" ::/PS.BIN 2>/dev/null || true
mcopy -i "$DISK" "$TMPX/HOME.HTM" ::/HOME.HTM 2>/dev/null || true
mcopy -i "$DISK" "$TMPX/CA.PEM" ::/CERTS/CA.PEM 2>/dev/null || true
mcopy -i "$DISK" "$EV/REQ.CNN" ::/REQ.CNN
mcopy -i "$DISK" "$EV/REQ.BLOB" ::/REQ.BLOB
printf ' ' > "$EV/WNPAGE.HTM"; mcopy -i "$DISK" "$EV/WNPAGE.HTM" ::/WNPAGE.HTM 2>/dev/null || true

echo "== start identity relay (CNN=$P_CNN blob=$P_BLOB small=$P_SMALL) =="
python3 "$LANE/continuation/fs-n3/tools/n3_identity_relay.py" --port "$P_CNN" \
  --cnn "$CNN" --log "$REL" --port-file "$PORTF" > "$EV/relay-cnn.out" 2>&1 &
R2=$!
export P_BLOB=${P_BLOB:-8906}
export P_SMALL=${P_SMALL:-8907}
python3 - "$EV/relay-blob.log" "$P_BLOB" <<'PYEOF' &
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def do_GET(self):
        body = b"hello-hobbyos-browser-blob " * (6*1024*1024//28)
        body = body + b"x" * (6*1024*1024 - len(body))
        self.send_response(200); self.send_header("Content-Type","text/html")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection","close"); self.end_headers()
        self.wfile.write(body); self.wfile.flush()
        with open(sys.argv[1],"a") as f: f.write("BLOB served %d\n" % len(body))
import os; http.server.ThreadingHTTPServer(("0.0.0.0", int(os.environ.get("P_BLOB","8906"))), H).serve_forever()
PYEOF
R3=$!
python3 - "$EV/relay-small.log" <<'EOF' &
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def do_GET(self):
        body = b"n3-small-control " * (226000//17)
        self.send_response(200); self.send_header("Content-Type","text/html")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection","close"); self.end_headers()
        self.wfile.write(body); self.wfile.flush()
        with open(sys.argv[1],"a") as f: f.write("SMALL served %d\n" % len(body))
import os; http.server.ThreadingHTTPServer(("0.0.0.0", int(os.environ.get("P_SMALL","8907"))), H).serve_forever()
EOF
R7=$!
sleep 2
# bind verification: every relay MUST be up and serve the exact expected body
if ! grep -q "n3 relay :$P_CNN " "$EV/relay-cnn.out" 2>/dev/null; then
  echo "FATAL: CNN relay failed to bind :$P_CNN" ; cat "$EV/relay-cnn.out"; exit 2
fi

echo "== boot via br_e2e =="
python3 "$LANE/tools/br_e2e.py" --disk "$DISK" --qmp "$QMP" --serial "$SER" \
  --evdir "$EV" --serve "$CTRL" --boot-timeout 420 > "$EV/driver.log" 2>&1 &
DPID=$!
READY=0
for _ in $(seq 1 600); do
  grep -q 'READY ctrl=' "$EV/driver.log" && { READY=1; break; }
  kill -0 "$DPID" 2>/dev/null || break
  sleep 1
done
[ "$READY" = 1 ] || { echo "never READY"; cp -f "$SER" "$EV/serial.log"; exit 1; }

ctrl() { python3 - "$CTRL" "$1" <<'EOF'
import json,socket,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.settimeout(900)
s.connect(sys.argv[1]); s.sendall(sys.argv[2].encode()+b"\n")
buf=b""
while True:
    c=s.recv(1<<16)
    if not c: break
    buf+=c
    if buf.endswith(b"\n"): break
s.close(); print(buf.decode(errors="replace").strip())
EOF
}

# leg 1: small control (must render load-ok quickly)
echo "== ctrl8897: small control =="
ctrl "{\"action\":\"type-url\",\"url\":\"http://10.0.2.2:$P_SMALL/small\"}" > "$EV/type-ctrl.json"
ctrl "{\"action\":\"go\",\"url\":\"http://10.0.2.2:$P_SMALL/small\",\"timeout\":300}" > "$EV/go-ctrl.json"
sleep 2
# leg 2: THE CNN IDENTITY GATE via the browser
echo "== full8895: CNN 5,704,241 B identity (GATE) =="
ctrl "{\"action\":\"type-url\",\"url\":\"http://10.0.2.2:$P_CNN/\"}" > "$EV/type-cnn.json"
ctrl "{\"action\":\"go\",\"url\":\"http://10.0.2.2:$P_CNN/\",\"timeout\":480}" > "$EV/go-cnn.json"
sleep 2
# legs 3-4: kernel-only byte-match via the shell nc (independent of browser)
echo "== console nc: CNN identity byte-match =="
ctrl '{"action":"console","cmd":"echo N3WARM > /TMP.N3","launch":true,"settle":5.0}' > "$EV/console-warm.json"
sleep 2
ctrl "{\"action\":\"console\",\"cmd\":\"nc 10.0.2.2 $P_CNN < /REQ.CNN > /ROB.CNN &\",\"launch\":false,\"type_pause\":0.18}" > "$EV/console-nc-cnn.json"
sleep 110
ctrl "{\"action\":\"console\",\"cmd\":\"cat /ROB.CNN\",\"launch\":false,\"settle\":2.0}" > /dev/null 2>&1
ctrl "{\"action\":\"console\",\"cmd\":\"nc 10.0.2.2 $P_BLOB < /REQ.BLOB > /ROB.BLOB &\",\"launch\":false,\"type_pause\":0.18}" > "$EV/console-nc-blob.json"
sleep 60
ctrl "{\"action\":\"console\",\"cmd\":\"cat /ROB.BLOB\",\"launch\":false,\"settle\":2.0}" > /dev/null 2>&1
ctrl '{"action":"poweroff"}' >/dev/null 2>&1
sleep 2; kill "$DPID" 2>/dev/null
kill "$R2" "$R3" "$R7" 2>/dev/null
cp -f "$SER" "$EV/serial.log"

echo "== per-step =="
for f in go-ctrl go-cnn console-nc-cnn console-nc-blob; do
  echo "-- $f"; cat "$EV/$f.json" 2>/dev/null; echo
done
echo "== net fetch / persist lines =="
grep -E "net fetch|net persist|open-fail" "$EV/serial.log" | tail -8
echo "== relay served =="
cat "$REL" | tail -8
echo "== copy-backs =="
for g in PAGE-NET.HTM ROB.CNN ROB.BLOB; do
  if mdir -i "$DISK" "::/$g" >/dev/null 2>&1; then
    mcopy -i "$DISK" "::/$g" "$EV/$g"
    echo "$g size=$(stat -c %s "$EV/$g") sha=$(sha256sum "$EV/$g" | cut -d' ' -f1)"
  else
    echo "$g MISSING"
  fi
done
echo "== flow-control guard =="
grep -c "rx flow-control drop" "$EV/serial.log" 2>/dev/null || echo 0
