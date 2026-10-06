#!/usr/bin/env python3
"""n3_identity_relay.py — lane N3 identity (non-gzip) relay + blob server.

G11 proved the guest kernel's per-socket RX ring (SOCKET_RX_BUF_SIZE = 4 MiB,
src/include/net.h) aborts any single-response wire body >= ~4 MiB on the
identity path (open-fail 'Unsupported protocol', bytes=0).  N3's retest must
exercise the IDENTITY wire (NOT gzip), so this relay NEVER sets
Content-Encoding and always sends the full raw body with an honest
Content-Length.

Endpoints (one process, portable via --port):
  /                     -> the staged 5,704,241 B CNN HTML (byte-exact copy
                           of G11's cnn-direct persist) — THE gate fixture.
  /head?head=N          -> same fixture truncated to N bytes (small control).
  /blob                 -> deterministic 6 MiB synthetic text/html blob.
  /small                -> 226,000 B synthetic control page.
Every reply: identity, Connection: close, HTTP/1.0.  Logs each served body
size/sha so the host side can prove what went on the wire.
"""
import argparse
import hashlib
import http.server
import time

UA = "HobbyOS-N3-gate/1.0"

def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        pass

    def _serve(self, body: bytes, ctype: str = "text/html"):
        log = getattr(self.server, "relay_log", None)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
            self.wfile.flush()
        except OSError:
            pass
        with open(log, "a") if log else open("/dev/null", "a") as f:
            f.write("[%s] %s %s status=200 wire=%d sha=%s\n" % (
                time.strftime("%H:%M:%S"), self.command, self.path,
                len(body), sha(body)))

    def do_GET(self):
        blob = getattr(self.server, "blob", b"")
        cnn = getattr(self.server, "cnn", b"")
        path = self.path.split("?", 1)[0]
        head = None
        if "?" in self.path:
            for kv in self.path.split("?")[1].split("&"):
                if kv.startswith("head="):
                    try:
                        head = int(kv.split("=")[1])
                    except ValueError:
                        head = None
        if path in ("/", "/index.html"):
            body = cnn if head is None else cnn[:head]
        elif path == "/head":
            body = cnn[:head] if head else cnn[:4000]
        elif path == "/blob":
            body = blob
        elif path == "/small":
            body = b"n3-small-control " * (226000 // 18)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self._serve(body)


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8892)
    ap.add_argument("--cnn", default="")
    ap.add_argument("--log", default="/tmp/n3-relay.log")
    ap.add_argument("--port-file", default="/tmp/n3-relay-port")
    ARGS = ap.parse_args()

    with open(ARGS.cnn, "rb") as f:
        cnn = f.read()
    # deterministic 6 MiB synthetic blob (same production as G11's probe)
    blob = (b"hello-hobbyos-browser-blob " * (6 * 1024 * 1024 // 28)
            + b"hello-hobbyos-browser-blob"[:6 * 1024 * 1024 % 28])
    blob = blob[:6 * 1024 * 1024]
    with open(ARGS.log + ".blob", "w") as f:
        f.write("blob len=%d sha=%s\n" % (len(blob), sha(blob)))
    print("n3 relay :%d cnn=%dB(sha=%s) blob=%dMiB" % (
        ARGS.port, len(cnn), sha(cnn), len(blob) // (1024 * 1024)), flush=True)

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", ARGS.port), H)
    srv.cnn = cnn
    srv.blob = blob
    srv.relay_log = ARGS.log
    with open(ARGS.port_file, "w") as f:
        f.write(str(ARGS.port))
    srv.serve_forever()


if __name__ == "__main__":
    main()
