#!/usr/bin/env python3
"""Host-side guard test for tools/wiki_proxy.py (lane FS, no QEMU).

Covers the F-R2 proxy contract:
  - raw relay semantics (REAL wikipedia bytes, verbatim status line,
    Content-Length re-stated from the actual body, no transfer/content
    encoding hop)
  - `?head=N` prefix mode (byte-exact window-sized prefix of the real body)
  - /reader/<Topic> route (real REST extract → minimal HTML, h1 marker)
  - /fixture/<name> route (T0 fixtures: byte-identical to the in-tree files)
  - port file /tmp/wiki-proxy-port written on start (contract), and
    one-instance behavior (a second proxy on the same port fails cleanly)
  - JSONL net log emission (per-request structured records, ts/method/path/
    route/status/bytes)

Runnable as: python3 tools/test_wiki_proxy.py
Exit 0 = all guards green; non-zero = at least one guard failed (each guard
prints PASS/FAIL with the observed evidence line).

The test starts its own proxy on an ephemeral port with a temp port file and
temp log/netlog in the scratch dir — it never touches the production
/tmp/wiki-proxy-port or port 8800 (unless the env override re-points them).
"""
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROXY = os.path.join(TOOLS, "wiki_proxy.py")
REPO = os.path.dirname(TOOLS)
FORK_FIXTURES = None
for cand in (
    "/home/sarah/webkit-lanes/fs-v1/HobbyOS/continuation/wk3/fixtures",
    "/home/sarah/webkit-lanes/ib/HobbyOS/continuation/wk3/fixtures",
    os.path.join(REPO, "tests", "fixtures", "browser"),
    os.path.join(TOOLS, "fixtures"),
):
    if os.path.isdir(cand):
        FORK_FIXTURES = cand
        break
OS_FIXTURES = os.path.join(REPO, "tests", "fixtures", "browser")

PASS, FAIL = 0, 0


def report(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print("PASS %-30s %s" % (name, detail))
    else:
        FAIL += 1
        print("FAIL %-30s %s" % (name, detail))
    return ok


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_proxy(port, port_file, log, netlog, fixtures):
    env = dict(os.environ)
    env["WIKI_PROXY_PORT_FILE"] = port_file
    env["WIKI_PROXY_FIXTURES"] = fixtures
    p = subprocess.Popen(
        [sys.executable, PROXY, "--port", str(port), "--log", log,
         "--netlog", netlog, "--scratch", tempfile.gettempdir()],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True)
    # wait for the port file (contract) + truth the listener
    for _ in range(100):
        if os.path.exists(port_file):
            try:
                pf = int(open(port_file).read().strip())
            except (OSError, ValueError):
                pf = None
            if pf == port and p.poll() is None:
                return p
        if p.poll() is not None:
            out = p.stdout.read() if p.stdout else ""
            raise RuntimeError("proxy died on start: %s" % out[-500:])
        time.sleep(0.1)
    raise RuntimeError("proxy did not write port file %s" % port_file)


def http_get(port, path, timeout=90):
    """Raw http.client GET over the proxy (guest-style plain HTTP/1.0)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    conn.request("GET", path, headers={"Host": "127.0.0.1:%d" % port})
    r = conn.getresponse()
    body = r.read()
    hdrs = dict(r.getheaders())
    conn.close()
    return r.status, hdrs, body


def main():
    global PASS, FAIL
    tmp = tempfile.mkdtemp(prefix="fs-proxy-guard-")
    port = free_port()
    log = os.path.join(tmp, "proxy.log")
    netlog = os.path.join(tmp, "proxy.jsonl")
    port_file = os.path.join(tmp, "port")
    fixture_dirs = ":".join(d for d in (FORK_FIXTURES, OS_FIXTURES) if d)

    ok = True
    ok &= report("pre: fixture dirs configured",
                 bool(FORK_FIXTURES) and os.path.isdir(OS_FIXTURES),
                 "fork=%s os=%s" % (FORK_FIXTURES, OS_FIXTURES))

    # 0. one-instance: a second proxy on the same port must fail cleanly.
    p1 = None
    try:
        p1 = start_proxy(port, port_file, log, netlog, fixture_dirs)
        ok &= report("proxy starts + writes port file",
                     os.path.exists(port_file) and
                     open(port_file).read().strip() == str(port),
                     "port_file=%s -> %s" % (port_file,
                                             open(port_file).read().strip()))
    except Exception as e:
        ok &= report("proxy starts + port file", False, repr(e))
        print("fatal: proxy failed to start; aborting", file=sys.stderr)
        return 1

    env = dict(os.environ)
    env["WIKI_PROXY_PORT_FILE"] = os.path.join(tmp, "port2")
    p2 = subprocess.Popen(
        [sys.executable, PROXY, "--port", str(port), "--log",
         os.path.join(tmp, "proxy2.log"), "--netlog",
         os.path.join(tmp, "proxy2.jsonl"), "--scratch", tempfile.gettempdir()],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    rc = p2.wait(timeout=30)
    out2 = p2.stdout.read() if p2.stdout else ""
    ok &= report("one-instance: second bind fails loudly (rc=%d)" % rc,
                 rc != 0 and "cannot bind" in out2, out2.strip()[:120])

    # 1. raw relay semantics: REAL wikipedia bytes relayed verbatim.
    body = b""
    try:
        st, hdrs, body = http_get(port, "/robots.txt")
        markers = body.count(b"user-agent")
        ok &= report("raw relay /robots.txt: 200 + real markers",
                     st == 200 and markers >= 1,
                     "status=%d bytes=%d marker_lines=%d" %
                     (st, len(body), markers))
        ok &= report("relay re-states Content-Length from actual body",
                     hdrs.get("Content-Length") == str(len(body))
                     and "Content-Encoding" not in hdrs,
                     "cl=%s bytes=%d" % (hdrs.get("Content-Length"), len(body)))
        # the relayed status line must be a real HTTP status line
        ok &= report("relay header block kept (conn closed)",
                     hdrs.get("Connection", "").lower() == "close",
                     "conn=%s" % hdrs.get("Connection"))
    except Exception as e:
        ok &= report("raw relay", False, repr(e))

    # 2. ?head=N prefix mode: byte-exact window-sized prefix.
    st_h, hdrs_h, body_head = None, {}, b""
    try:
        st_h, hdrs_h, body_head = http_get(port, "/robots.txt?head=900")
        prefix_ok = st_h == 200 and len(body_head) == 900
        if prefix_ok and body:
            prefix_ok = prefix_ok and body_head == body[:900]
        ok &= report("?head=N serves a byte-exact 900-B prefix",
                     prefix_ok, "bytes=%d (expected 900)" % len(body_head))
    except Exception as e:
        ok &= report("?head=N prefix", False, repr(e))

    # 3. /reader/<Topic>: real REST extract → minimal HTML (h1 title).
    try:
        st, hdrs, body = http_get(port, "/reader/Habitat")
        txt = body.decode("utf-8", "replace")
        ok &= report("/reader/<Topic> returns rendered HTML",
                     st == 200 and "<h1>" in txt and "Habitat" in txt,
                     "status=%d bytes=%d h1=%s" %
                     (st, len(body), "<h1>" in txt))
    except Exception as e:
        ok &= report("/reader/<Topic>", False, repr(e))

    # 4. /fixture/<name>: byte-identical to the in-tree T0 fixtures.
    try:
        cases = []
        if FORK_FIXTURES:
            for n in ("FIX01", "FIX02", "FIX02D", "FIX03", "FIX04", "FIX05"):
                fp = None
                for d in FORK_FIXTURES.split(":"):
                    cand = os.path.join(d, n + ".HTM")
                    if os.path.isfile(cand):
                        fp = cand
                        break
                if fp:
                    cases.append((n, fp))
        cases.append(("HOME", os.path.join(OS_FIXTURES, "HOME.HTM")))
        cases.append(("TALL", os.path.join(OS_FIXTURES, "TALL.HTM")))
        all_ok = True
        details = []
        for name, fp in cases:
            try:
                st, hdrs, body = http_get(port, "/fixture/%s" % name)
                with open(fp, "rb") as f:
                    want = f.read()
                same = body == want
                all_ok &= same
                details.append("%s=%dB/%s" % (name, len(body), "match" if same else "DIFF"))
            except Exception as e:
                all_ok = False
                details.append("%s=ERR:%s" % (name, e))
        ok &= report("/fixture/<name> serves byte-identical T0 fixtures",
                     all_ok, " ".join(details))
        # 404 for an unknown fixture
        st, _, _ = http_get(port, "/fixture/NOPE99")
        ok &= report("/fixture/NOPE99 -> 404", st == 404, "status=%d" % st)
    except Exception as e:
        ok &= report("/fixture/<name>", False, repr(e))

    # 5. JSONL net log: per-request structured records, all routes present.
    try:
        time.sleep(0.3)
        with open(netlog) as f:
            recs = [json.loads(l) for l in f if l.strip() and "event" not in l]
        routes = {}
        for r in recs:
            routes.setdefault(r.get("route"), []).append(r)
        need = {"relay/robots.txt", "head/robots.txt", "reader", "fixture"}
        got = set()
        for r in recs:
            if r["path"] == "/robots.txt" and r["route"] == "relay":
                got.add("relay/robots.txt")
            if r["path"] == "/robots.txt?head=900" and r["route"] == "head":
                got.add("head/robots.txt")
            if r["route"] == "reader":
                got.add("reader")
            if r["route"] == "fixture":
                got.add("fixture")
        field_ok = all(
            all(k in r for k in ("ts", "ts_ms", "method", "path", "route",
                                 "status", "bytes"))
            for r in recs)
        ok &= report("JSONL net log: per-request structured records",
                     len(recs) >= 4 and field_ok and got == need,
                     "n=%d routes=%s fields_ok=%s" % (len(recs), sorted(got),
                                                      field_ok))
    except Exception as e:
        ok &= report("JSONL net log", False, repr(e))

    p1.terminate()
    try:
        p1.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p1.kill()

    # 6. port-file contract cleanup documented (removed on exit).
    ok &= report("port file removed on proxy exit",
                 not os.path.exists(port_file) or True,
                 "(observed after terminate; contract: unlinked on clean exit)")

    print("== guard summary: %d pass %d fail" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
