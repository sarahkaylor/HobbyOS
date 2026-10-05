#!/usr/bin/env python3
"""Host Wikipedia proxy for the HobbyOS browser host-net leg (lane WC).

A small plain-HTTP server that, for any request path, fetches the REAL site
from this host via curl (https://en.wikipedia.org<path>) and relays the
upstream response to the guest verbatim: status line + headers + body.

Guest target: http://10.0.2.2:<port>/<path>  (QEMU user-net host alias).

Design notes (browser.md §8.4 — "no done without observed transfer"):
- The guest speaks plain HTTP to us (https/CA legs follow separately).
- The backend is `curl` on the host (per WC-BRIEF), so the bytes come from
  the real internet edge, not from a fixture.  Every transfer is logged with
  upstream status + byte count to a log file.
- Upstream is forced to `Accept-Encoding: identity` so the relayed body is
  the raw text (no gzip hop for the guest's stack).
- curl removes transfer-coding, so the relay re-states Content-Length from
  the actual body and drops any upstream Transfer-Encoding/Content-Encoding.
- On start the chosen port is written to /tmp/wiki-proxy-port (per lane WC
  convention: the runner reads it back for the guest URL).

Usage: python3 tools/wiki_proxy.py [--port PORT] [--log FILE] [--upstream URL]
  --port     explicit listen port; default 8800 (written to /tmp/wiki-proxy-port)
  --upstream base URL; default https://en.wikipedia.org
"""
import argparse
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT_FILE = "/tmp/wiki-proxy-port"
DEFAULT_UPSTREAM = "https://en.wikipedia.org"
MAX_BODY = 32 << 20  # 32 MiB cap, mirroring the guest shell's page-body cap

ARGS = None


def upstream_fetch(path):
    """Fetch the real page via host curl. Returns (status, headers, body)
    where headers is the verbatim upstream status line + header lines
    (curl -D output form), or (0, "", b"") on transport error."""
    url = ARGS.upstream.rstrip("/") + path
    hdr = os.path.join(ARGS.scratch, "whdr_%d.txt" % os.getpid())
    body = os.path.join(ARGS.scratch, "wbody_%d.bin" % os.getpid())
    try:
        p = subprocess.run(
            ["curl", "-sS", "--http1.1", "-L", "--max-time", "60", "--connect-timeout", "15",
             "--header", "Accept-Encoding: identity",
             "-A", "Mozilla/5.0 (HobbyOS-WC-proxy/1.0) en.wikipedia.org proxy",
             "-D", hdr, "-o", body, url],
            capture_output=True, timeout=90)
    except subprocess.TimeoutExpired:
        return 0, "", b""
    if p.returncode != 0 or not os.path.exists(body):
        err = (p.stderr or b"").decode("utf-8", "replace").strip()
        return 0, "", err.encode("utf-8", "replace")
    with open(body, "rb") as f:
        data = f.read()
    # parse status line + headers from curl -D output
    try:
        with open(hdr, "rb") as f:
            raw_hdr = f.read()
    except OSError:
        raw_hdr = b""
    status = 0
    if raw_hdr:
        first = raw_hdr.split(b"\r\n", 1)[0]
        parts = first.split(b" ", 2)
        if len(parts) >= 2 and parts[0].startswith(b"HTTP/"):
            try:
                status = int(parts[1])
            except ValueError:
                status = 0
    return status, raw_hdr, data


class WikiProxy(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):  # quiet the base handler
        return

    def do_GET(self):
        status, headers, data = upstream_fetch(self.path)
        if status == 0:
            err = data.decode("utf-8", "replace") if data else "curl-fail"
            self.send_response(502)
            self.send_header("Content-Length", str(len(b"proxy-err " + err.encode())))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(b"proxy-err " + err.encode("utf-8", "replace"))
            except OSError:
                pass
            self._log("UPSTREAM-FAIL path=%s err=%s" % (self.path, err[:200]))
            return
        # Rebuild the relay headers verbatim but fixed-up for our raw body.
        out = bytearray()
        first = True
        for line in headers.split(b"\r\n"):
            if not line or line.startswith(b"HTTP/"):
                if first and line.startswith(b"HTTP/"):
                    out += line + b"\r\n"
                    first = False
                continue
            low = line.lower()
            if low.startswith(b"transfer-encoding") or low.startswith(b"content-encoding") \
                    or low.startswith(b"content-length") or low.startswith(b"connection") \
                    or low.startswith(b"keep-alive"):
                continue
            out += line + b"\r\n"
        out += b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(data)
        try:
            self.wfile.write(bytes(out) + data)
        except OSError:
            pass
        req_host = self.headers.get("Host", "-")
        self._log("REQ %s Host=%s path=%s upstream=%s%s status=%d bytes=%d -> guest %s" % (
            self.requestline, req_host, self.path, ARGS.upstream, self.path,
            status, len(data), self.client_address[0]))

    def _log(self, msg):
        line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        try:
            with open(ARGS.log, "a") as f:
                f.write(line + "\n")
        except OSError:
            pass


def main():
    global ARGS
    ap = argparse.ArgumentParser(description="host Wikipedia proxy for the guest browser")
    ap.add_argument("--port", type=int, default=int(os.environ.get("WIKI_PROXY_PORT", "8800")))
    ap.add_argument("--log", default="/tmp/wiki-proxy.log")
    ap.add_argument("--scratch", default="/tmp")
    ap.add_argument("--upstream", default=DEFAULT_UPSTREAM)
    ARGS = ap.parse_args()

    srv = ThreadingHTTPServer(("0.0.0.0", ARGS.port), WikiProxy)
    with open(PORT_FILE, "w") as f:
        f.write(str(ARGS.port))
    print("wiki-proxy: listening on 0.0.0.0:%d (upstream %s) port->%s" % (
        ARGS.port, ARGS.upstream, PORT_FILE), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            os.unlink(PORT_FILE)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
