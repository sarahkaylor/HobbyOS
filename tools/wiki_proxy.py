#!/usr/bin/env python3
"""Host Wikipedia proxy for the HobbyOS browser host-net leg (lane WC / FS).

A small plain-HTTP server that, for any request path, fetches the REAL site
from this host via curl (https://en.wikipedia.org<path>) and relays the
upstream response to the guest verbatim: status line + headers + body.

Guest target: http://10.0.2.2:<port>/<path>  (QEMU user-net host alias).

Routes (backward compatible — adding routes never changes the defaults):
  /wiki/<path>  or any non-reserved path -> raw upstream relay
                (REAL wikipedia bytes via host curl; ?head=N serves a
                byte-exact prefix of the real body for the guest's 2048-B
                TCP window)
  /reader/<Topic>                       -> real-Wikipedia REST "reader mode"
                minimal HTML document (h1/p/a) the windowed browser paints
  /fixture/<name>                       -> T0 fixtures: fork
                continuation/wk3/fixtures FIX01..FIX05 (+ FIX02D) and OS
                tests/fixtures/browser HOME/TALL.  Served byte-exact from
                disk, text/html — the deterministic render leg (no real
                internet).  The fixture set is configured with --fixtures
                (colon-separated dirs) or $WIKI_PROXY_FIXTURES.

Per-request network log: alongside the human text log (--log), this proxy
writes a structured JSONL net log (--netlog, default <log>.jsonl): one JSON
object per request with ts/ts_ms/method/path/route/status/bytes/content-type/
content-encoding/upstream_status/client — the "per-request network log
capture" artifact referenced from report.json v2.

Design notes (browser.md §8.4 — "no done without observed transfer"):
- The guest speaks plain HTTP to us (https/CA legs follow separately).
- The backend is `curl` on the host (per WC-BRIEF), so the bytes come from
  the real internet edge, not from a fixture.  Every transfer is logged with
  upstream status + byte count.
- Upstream is forced to `Accept-Encoding: identity` so the relayed body is
  the raw text (no gzip hop for the guest's stack).
- curl removes transfer-coding, so the relay re-states Content-Length from
  the actual body and drops any upstream Transfer-Encoding/Content-Encoding.
- On start the chosen port is written to /tmp/wiki-proxy-port (per lane WC
  convention: the runner reads it back for the guest URL).  The proxy is
  host-global single-instance (fixed port when defaulted): a second instance
  on the same port exits with a clear error instead of silently double-binding.

Usage: python3 tools/wiki_proxy.py [--port PORT] [--log FILE] [--netlog FILE]
  [--upstream URL] [--fixtures DIR[:DIR...]]
  --port     explicit listen port; default 8800 (written to /tmp/wiki-proxy-port)
  --upstream base URL; default https://en.wikipedia.org
  --fixtures colon-separated fixture dirs (default: $WIKI_PROXY_FIXTURES)
  --port-file path for the port file (default: $WIKI_PROXY_PORT_FILE or
             /tmp/wiki-proxy-port — the F-R2 contract path).  Per-instance
             runners pass e.g. /tmp/wiki-proxy-port-<id> so concurrent
             instances never clobber each other's port file.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT_FILE = os.environ.get("WIKI_PROXY_PORT_FILE", "/tmp/wiki-proxy-port")
DEFAULT_UPSTREAM = "https://en.wikipedia.org"
MAX_BODY = 32 << 20  # 32 MiB cap, mirroring the guest shell's page-body cap

ARGS = None


def upstream_fetch(path, head=None):
    """Fetch the real page via host curl. Returns (status, headers, body)
    where headers is the verbatim upstream status line + header lines
    (curl -D output form), or (0, "", b"") on transport error.

    `head`: when set, return only the first `head` bytes of the real body
    (still upstream bytes, byte-exact prefix) — the guest should only be
    asked for window-sized transfers below (see runner docs)."""
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
    if head is not None:
        data = data[:head]
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


def reader_page(topic):
    """Real-Wikipedia 'reader mode' page: fetch the official REST summary
    for <topic> from en.wikipedia.org and emit a minimal HTML document the
    HobbyOS shell renderer can paint (mirrors the fixture styling: inline
    style, plain <h1>/<p>/<a>).  Content is REAL wikipedia data (title +
    article extract) — this is the display-friendly path for the windowed
    browser's address bar; the raw-page path stays /wiki-proxy upstream.

    fs-r4 (nav surface, ADDITIVE): the served reader page is the T2 nav
    gate material, so it now ships a small chrome/surface block on top:
      * a GET search form (empty action = submit to the current URL with
        '?q=<query>'; the /reader/ route honors q= to switch the topic,
        which makes a real form-GET submission navigate to new content),
      * three deterministic block links: two reader-mode topics
        (/reader/HTML, /reader/CSS — exercises cross-page link follow +
        back/forward), one same-document anchor (#sec2 — exercises a
        fragment jump),
      * the original live-Wikipedia link (real-internet, gated class),
      * tall filler + an id=sec2 anchor below the fold so the windowed
        page actually scrolls and fragment jumps visibly move the viewport.
    No-query output keeps the exact previous structure plus these blocks;
    nothing downstream (fixture/relay/head routes, netlog) changes."""
    import json as _json
    from urllib.request import urlopen, Request
    api = "https://en.wikipedia.org/api/rest_v1/page/summary/" + topic.replace(" ", "_")
    title, extract = topic, ""
    try:
        req = Request(api, headers={"User-Agent": "HobbyOS-WC-proxy/1.0 (reader mode)",
                                    "Accept": "application/json"})
        with urlopen(req, timeout=45) as r:
            data = _json.loads(r.read().decode("utf-8", "replace"))
        title = data.get("title") or title
        extract = data.get("extract") or ""
    except Exception as e:  # noqa: BLE001 — degrade to a readable error page
        extract = "[reader: upstream fetch failed: %s]" % e
    import html as _html
    t = _html.escape(title)
    x = _html.escape(extract)
    topic_esc = _html.escape(topic.replace(" ", "_"))
    nav = (
        "<form id=\"f\" method=\"get\" action=\"\">"
        "<input id=\"qi\" name=\"q\" type=\"search\" size=\"24\">"
        "<button type=\"submit\">search</button></form>"
        "<a id=\"nl0\" href=\"/reader/HTML\">go to HTML article</a>"
        "<a id=\"nl1\" href=\"/reader/CSS\">go to CSS article</a>"
        "<a id=\"nl2\" href=\"#sec2\">jump to Section 2 (same page)</a>"
    )
    body = (
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<style>html,body{margin:0;padding:0;background:#ffffff;color:#000000}"
        "h1{margin:0;padding:8px;background:#3366cc;color:#ffffff;font-size:18px}"
        "p{margin:8px;padding:4px;font-size:15px}"
        "a{display:block;margin:8px;padding:8px;background:#e8f0fe;color:#0000aa;font-size:15px}</style>"
        "</head><body>"
        + nav +
        "<h1>%s</h1><p>%s</p>"
        "<a href=\"https://en.wikipedia.org/wiki/%s\">[open] %s on Wikipedia (live)</a>"
        "<div style=\"height:900px\"></div>"
        "<h2 id=\"sec2\">Section 2</h2><p>fragment target (id=sec2)</p>"
        "<div style=\"height:900px\"></div>"
        "<p id=\"tail\">end of %s</p>"
        "</body></html>"
    ) % (t, x, topic_esc, t, topic_esc)
    return body.encode("utf-8")


def fixture_page(name):
    """Serve a T0 fixture by name across the configured fixture dirs.

    Mirrors the on-disk fixture bytes exactly (byte-identical to the file in
    tree).  Lookup is case-insensitive on the file basename (extension
    optional).  Returns (bytes, content_type) or (None, None) when the
    fixture is unknown.  Deterministic — this is the 'serve T0 fixtures' leg
    the acceptance runner drives the address bar against (render markers +
    frame pixel checksum).

    fs-r3: fixture-referenced non-HTML assets (the FIX02 page references
    IMG01.PNG in the same fixtures dir) are also served byte-exact when the
    requested name carries a non-.HTM extension; content type comes from the
    extension so the windowed browser's image decoder gets image/png etc.
    This is additive — extension-less fixture names (FIX01/HOME/TALL) keep
    the exact legacy .HTM behaviour."""
    dirs = (ARGS.fixtures or "").split(":")
    want = name.upper()
    ext = os.path.splitext(name)[1].upper()
    want_candidates = [want]
    if ext and ext != ".HTM":
        # exact asset name first (IMG01.PNG), then the .HTM fallback below
        pass
    elif not want.endswith(".HTM"):
        want_candidates = [want + ".HTM"]
    for want_file in want_candidates:
        for d in dirs:
            if not d:
                continue
            try:
                entries = os.listdir(d)
            except OSError:
                continue
            for e in entries:
                if e.upper() == want_file:
                    p = os.path.join(d, e)
                    try:
                        with open(p, "rb") as f:
                            data = f.read()
                    except OSError:
                        continue
                    if ext and ext != ".HTM":
                        import mimetypes
                        ctype = mimetypes.guess_type(e)[0] or "application/octet-stream"
                    else:
                        ctype = "text/html; charset=utf-8"
                    return data, ctype
    return None, None


class WikiProxy(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):  # quiet the base handler
        return

    def _net(self, **kw):
        """Structured JSONL network-log line (report.json v2 artifact)."""
        rec = {"ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "ts_ms": int(time.time() * 1000)}
        rec.update(kw)
        try:
            with open(ARGS.netlog, "a") as f:
                f.write(json.dumps(rec) + "\n")
        except OSError:
            pass

    def _serve_bytes(self, data, ctype, route, status=200, guest=None,
                     encoding=None, upstream_status=None, path="", method="GET"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        if encoding:
            self.send_header("Content-Encoding", encoding)
        self.end_headers()
        try:
            self.wfile.write(data)
        except OSError:
            pass
        self._net(method=method, path=path or self.path, route=route,
                  status=status, bytes=len(data),
                  content_type=ctype.split(";")[0].strip(),
                  content_encoding=encoding,
                  upstream_status=upstream_status,
                  client=self.client_address[0],
                  guest_host=self.headers.get("Host", "-"))

    def do_GET(self):
        # /reader/<Topic>: reader-mode Wikipedia page (real REST extract).
        if self.path.startswith("/reader/"):
            from urllib.parse import urlsplit, parse_qs
            topic = self.path[len("/reader/"):].split("?")[0].strip("/")
            # fs-r4: honor a GET-submit query — the reader page's empty-action
            # search form submits to the current URL with '?q=<topic>', so the
            # navigation is real (new content rendered), not a mock.
            q = parse_qs(urlsplit(self.path).query)
            if q.get("q"):
                topic = q["q"][0].strip("/")
            data = reader_page(topic)
            self._serve_bytes(data, "text/html; charset=utf-8", "reader",
                              guest=self.client_address[0], path=self.path)
            self._log("READER topic=%s bytes=%d -> guest %s" % (
                topic, len(data), self.client_address[0]))
            return
        # /fixture/<name>: deterministic T0 fixture leg (no real internet).
        if self.path.startswith("/fixture/"):
            name = self.path[len("/fixture/"):].split("?")[0].strip("/")
            data, ctype = fixture_page(name)
            if data is None:
                self._serve_bytes(b"fixture-not-found: " + name.encode(),
                                  "text/plain", "fixture", status=404,
                                  path=self.path)
                self._log("FIXTURE-404 name=%s" % name)
                return
            self._serve_bytes(data, ctype, "fixture", path=self.path)
            self._log("FIXTURE name=%s bytes=%d -> guest %s" % (
                name, len(data), self.client_address[0]))
            return
        # everything else: raw /wiki-proxy upstream relay (?head=N prefix)
        from urllib.parse import urlsplit, parse_qs
        parts = urlsplit(self.path)
        pathname = parts.path
        q = parse_qs(parts.query)
        head = None
        if "head" in q:
            try:
                head = int(q["head"][0])
            except (ValueError, IndexError):
                head = None
        # fs-r9 root-cause fix: relay the guest's query string upstream.
        # Wikipedia's /w/load.php and /w/api.php carry their real parameters
        # (the %7C-joined module list, skin, lang, only=styles, ...) in the
        # URL query; the legacy pathname-only relay turned EVERY load.php
        # request into the 196-byte "no modules were requested" stub, so the
        # full-skin relay leg laid out without the skin stylesheet (article
        # body pushed below the fold -> blank content column) while the
        # direct TLS leg served the real 213-KB CSS bundle.  ?head=N stays a
        # proxy-internal control param (windowed prefix) — stripped, never
        # relayed upstream.
        up_path = pathname
        if parts.query:
            keep = [p for p in parts.query.split("&")
                    if not p.split("=", 1)[0] == "head"]
            if keep:
                up_path = pathname + "?" + "&".join(keep)
        status, headers, data = upstream_fetch(up_path, head=head)
        route = "head" if head is not None else "relay"
        if status == 0:
            err = data.decode("utf-8", "replace") if data else "curl-fail"
            self._serve_bytes(b"proxy-err " + err.encode("utf-8", "replace"),
                              "text/plain", route, status=502,
                              upstream_status=0, path=self.path)
            self._log("UPSTREAM-FAIL path=%s err=%s" % (self.path, err[:200]))
            return
        # Rebuild the relay headers verbatim but fixed-up for our raw body.
        out = bytearray()
        first = True
        encoding = None
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
                if low.startswith(b"content-encoding"):
                    encoding = line.split(b":", 1)[1].strip().decode("ascii", "ignore") or None
                continue
            out += line + b"\r\n"
        out += b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(data)
        try:
            self.wfile.write(bytes(out) + data)
        except OSError:
            pass
        req_host = self.headers.get("Host", "-")
        self._net(method="GET", path=self.path, route=route, status=status,
                  bytes=len(data), content_type="text/html",
                  content_encoding=encoding, upstream_status=status,
                  client=self.client_address[0], guest_host=req_host)
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
    ap.add_argument("--netlog", default=None,
                    help="structured JSONL net log (default: <log>.jsonl)")
    ap.add_argument("--scratch", default="/tmp")
    ap.add_argument("--upstream", default=DEFAULT_UPSTREAM)
    ap.add_argument("--fixtures", default=os.environ.get("WIKI_PROXY_FIXTURES", ""),
                    help="colon-separated fixture dirs for the /fixture/<name> route")
    ap.add_argument("--port-file", default=None,
                    help="override the port file path (per-instance concurrency)")
    ARGS = ap.parse_args()
    if ARGS.port_file:
        pf = ARGS.port_file
    else:
        pf = PORT_FILE
    ARGS.port_file = pf
    if not ARGS.netlog:
        ARGS.netlog = ARGS.log + ".jsonl" if ARGS.log != "-" else "/tmp/wiki-proxy.jsonl"
    os.makedirs(os.path.dirname(os.path.abspath(ARGS.netlog)), exist_ok=True)

    try:
        srv = ThreadingHTTPServer(("0.0.0.0", ARGS.port), WikiProxy)
    except OSError as e:
        # one-instance contract: a second proxy on the same port must fail
        # loudly, never double-bind / silently shadow the first.
        sys.stderr.write("wiki-proxy: cannot bind 0.0.0.0:%d (%s) — "
                         "another instance already owns the port?\n"
                         % (ARGS.port, e))
        return 1
    try:
        os.unlink(ARGS.port_file)   # stale per-instance port file
    except OSError:
        pass
    with open(ARGS.port_file, "w") as f:
        f.write(str(ARGS.port))
    with open(ARGS.netlog, "a") as f:
        f.write(json.dumps({"ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                            "event": "proxy-start", "port": ARGS.port,
                            "upstream": ARGS.upstream,
                            "fixtures": ARGS.fixtures,
                            "port_file": ARGS.port_file}) + "\n")
    print("wiki-proxy: listening on 0.0.0.0:%d (upstream %s) port->%s netlog->%s" % (
        ARGS.port, ARGS.upstream, ARGS.port_file, ARGS.netlog), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            os.unlink(ARGS.port_file)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
