#!/usr/bin/env bash
# n3_run_probe.sh — lane N3 identity-gate probe (G11 g11_ab_probe pattern).
#
# Boots MY fixed kernel (fs-r5, MODE=desktop) in ARM QEMU with the fleet
# reference browser (bw-verify-w3 WebProcess) and, in ONE session, fetches
# over the local identity relay:
#   ctrl8894  http://10.0.2.2:8894/small    226 KB control   (must pass)
#   head8892  http://10.0.2.2:8892/head?head=4000  4 KB control  (must pass)
#   blob8893  http://10.0.2.2:8893/blob     6 MiB identity   (GATE)
#   full8892  http://10.0.2.2:8892/         5,704,241 B CNN identity (GATE)
#
# Evidence: per-step JSON, serial.log, PAGE-NET.HTM copy-back, relay log.
set -u
LANE=/home/sarah/hobbyos-lanes/fs-r5
EV="$LANE/continuation/fs-n3/evidence/probe"
SRC=/home/sarah/hobbyos-lanes/fs-g11/disk-wk6-1.img
REF=/home/sarah/webkit-lanes/wbn/WebKitBuild/HobbyOS-arm/bin/WebProcess
CNN="$LANE/continuation/fs-n3/evidence/CNN-5704241.HTM"
CTRL=/tmp/n3-probe-ctrl.sock; QMP=/tmp/n3-probe-qmp.sock; SER=/tmp/n3-probe-serial.log
REL=/tmp/n3-relay.log; PORTF=/tmp/n3-relay-port
RUN=${1:-p1}
mkdir -p "$EV"
rm -f "$CTRL" "$QMP" "$SER" "$REL" "$PORTF"

echo "== N3 probe $RUN: assemble disk =="
DISK="$EV/disk.img"
TMPX="$EV/frag"
rm -rf "$TMPX"; mkdir -p "$TMPX"
for f in DESKTOP.BIN CONSOLE.BIN SH.BIN FREE.BIN PS.BIN HOME.HTM; do
  mcopy -i "$SRC" "::$f" "$TMPX/$f" 2>/dev/null || true
done
mcopy -i "$SRC" ::/boot/limine.conf "$TMPX/limine.conf" 2>/dev/null || true
mcopy -i "$SRC" ::/CERTS/CA.PEM "$TMPX/CA.PEM" 2>/dev/null || true
mcopy -i "$SRC" ::/EFI/BOOT/BOOTAA64.EFI "$TMPX/BOOTAA64.EFI" 2>/dev/null || true
# MY fixed kernel
[ -f "$LANE/hobbyos.bin" ] || { echo "missing $LANE/hobbyos.bin (build first)"; exit 2; }
cp -f "$LANE/hobbyos.bin" "$TMPX/hobbyos.bin"
# reference browser (bw-verify-w3 WebProcess) as BROWSER.BIN — must be the
# objcopy FLAT image (the raw ELF is 147 MB and exceeds the OS v2 loader's
# max image size; the flat form is what the harness disks actually run).
cp -f "$REF" "$TMPX/WebProcess.elf"
/usr/bin/llvm-objcopy -O binary "$TMPX/WebProcess.elf" "$TMPX/BROWSER.BIN" 2>/dev/null \
  || cp -f /tmp/ref-WebProcess.bin "$TMPX/BROWSER.BIN" 2>/dev/null \
  || { echo "objcopy failed"; exit 2; }
sha256sum "$TMPX/WebProcess.elf" "$TMPX/BROWSER.BIN" | sed 's/^/  /'
echo "-- kernel $(sha256sum "$TMPX/hobbyos.bin" | cut -d' ' -f1) browser $(sha256sum "$TMPX/BROWSER.BIN" | cut -d' ' -f1)"
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
mcopy -i "$DISK" "$TMPX/FREE.BIN" ::/FREE.BIN 2>/dev/null || true
mcopy -i "$DISK" "$TMPX/PS.BIN" ::/PS.BIN 2>/dev/null || true
mcopy -i "$DISK" "$TMPX/HOME.HTM" ::/HOME.HTM 2>/dev/null || true
mcopy -i "$DISK" "$TMPX/CA.PEM" ::/CERTS/CA.PEM 2>/dev/null || true
printf ' ' > "$EV/WNPAGE.HTM"; mcopy -i "$DISK" "$EV/WNPAGE.HTM" ::/WNPAGE.HTM 2>/dev/null || true

echo "== start identity relay (8892 CNN / 8893 blob / 8894 small) =="
python3 "$LANE/continuation/fs-n3/tools/n3_identity_relay.py" --port 8892 \
  --cnn "$CNN" --log "$REL" --port-file "$PORTF" > "$EV/relay8892.out" 2>&1 &
R2=$!
python3 - "$EV/relay8893.log" <<'EOF' &
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
http.server.ThreadingHTTPServer(("0.0.0.0", 8893), H).serve_forever()
EOF
R3=$!
python3 - "$EV/relay8891.log" <<'EOF' &
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
http.server.ThreadingHTTPServer(("0.0.0.0", 8894), H).serve_forever()
EOF
R1=$!
sleep 1

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

for pair in \
  "ctrl8891|http://10.0.2.2:8894/small" \
  "head8892|http://10.0.2.2:8892/head?head=4000" \
  "blob8893|http://10.0.2.2:8893/blob" \
  "full8892|http://10.0.2.2:8892/"; do
  LABEL="${pair%%|*}"; URL="${pair##*|}"
  echo "== $LABEL: $URL =="
  ctrl "{\"action\":\"type-url\",\"url\":\"$URL\"}" > "$EV/type-$LABEL.json"
  ctrl "{\"action\":\"go\",\"url\":\"$URL\",\"timeout\":660}" > "$EV/go-$LABEL.json"
  sleep 1
done
ctrl '{"action":"poweroff"}' >/dev/null 2>&1
sleep 2; kill "$DPID" 2>/dev/null
kill "$R1" "$R2" "$R3" 2>/dev/null
cp -f "$SER" "$EV/serial.log"

echo "== per-step =="
for f in type-ctrl8894 go-ctrl8894 type-head8892 go-head8892 \
         type-blob8893 go-blob8893 type-full8892 go-full8892; do
  echo "-- $f"; cat "$EV/$f.json" 2>/dev/null; echo
done
echo "== net fetch lines =="
grep -E "net fetch|net persist|load open-fail" "$EV/serial.log" | tail -8
echo "== relay served =="
cat "$REL" 2>/dev/null | tail -10
echo "== PAGE-NET copy-back =="
if mdir -i "$DISK" ::/PAGE-NET.HTM >/dev/null 2>&1; then
  mcopy -i "$DISK" ::/PAGE-NET.HTM "$EV/PAGE-NET.HTM"
  echo "size=$(stat -c %s "$EV/PAGE-NET.HTM") sha=$(sha256sum "$EV/PAGE-NET.HTM" | cut -d' ' -f1)"
else
  echo "no PAGE-NET.HTM on guest disk"
fi
echo "== flow-control guard evidence =="
grep -c "rx flow-control drop" "$EV/serial.log" 2>/dev/null || echo 0
