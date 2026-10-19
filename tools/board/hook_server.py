#!/usr/bin/env python3
"""tools/board/hook_server.py — Forgejo board webhook receiver (GB-003).

Listens for Forgejo `issues` / `issue_comment` webhook deliveries and appends one
JSON line per event to $BOARD_EVENT_LOG (default ~/.hermes/board-events.jsonl),
which is the Coordinator's inbox: a lane move to `Merge Gate` or `decision` is a
signal to act, everything else is board telemetry.

  GET  /health  -> {"ok": true, "events": N}
  GET  /events?n=20 -> last N events (jsonl lines)
  POST /board-hook  <- Forgejo webhook (any Content-Type: application/json)

Run (systemd unit `hobbyos-board-hook.service` on swarm-coordinator):
  FORGEJO_TOKEN=... python3 tools/board/hook_server.py --port 8765
"""
import argparse
import datetime as dt
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

LOG = os.path.expanduser(os.environ.get("BOARD_EVENT_LOG", "~/.hermes/board-events.jsonl"))


def append(ev):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps(ev, sort_keys=True) + "\n")


def summarize(payload):
    kind = payload.get("action") or payload.get("event") or "?"
    iss = payload.get("issue") or {}
    labels = [l.get("name") for l in iss.get("labels", [])]
    lane = next((n for n in ("Backlog", "Ready", "In Progress", "In Review", "Merge Gate", "Done", "Icebox") if n in labels), None)
    return {
        "ts": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "action": kind,
        "issue": iss.get("number"),
        "title": (iss.get("title") or "")[:120],
        "lane": lane,
        "labels": labels,
        "comment": ((payload.get("comment") or {}).get("body") or "")[:280] or None,
        "sender": (payload.get("sender") or {}).get("login"),
        "url": iss.get("html_url"),
        "wake": (lane in ("Merge Gate", "In Review")) or ("decision" in labels) or (kind == "closed"),
    }


class H(BaseHTTPRequestHandler):
    server_version = "hobbyos-board-hook/1"

    def _send(self, code, obj=None, text=None):
        body = (text if text is not None else json.dumps(obj, indent=1)).encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain" if text is not None else "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        u = urlparse(self.path)
        if u.path == "/health":
            n = sum(1 for _ in open(LOG, encoding="utf-8")) if os.path.exists(LOG) else 0
            return self._send(200, {"ok": True, "events": n, "log": LOG})
        if u.path == "/events":
            n = int(parse_qs(u.query).get("n", ["20"])[0])
            lines = open(LOG, encoding="utf-8").read().splitlines() if os.path.exists(LOG) else []
            return self._send(200, [json.loads(x) for x in lines[-n:]])
        return self._send(404, None, "not found\n")

    def do_POST(self):  # noqa: N802
        if urlparse(self.path).path != "/board-hook":
            return self._send(404, None, "not found\n")
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0))
        try:
            payload = json.loads(raw.decode() or "{}")
        except Exception as e:
            return self._send(400, {"error": f"bad json: {e}"})
        ev = summarize(payload)
        append(ev)
        print(f"[hook] {ev['ts']} {ev['action']:<12} #{ev['issue']} lane={ev['lane']} wake={ev['wake']}", flush=True)
        return self._send(200, {"ok": True, "recorded": ev})

    def log_message(self, fmt, *args):
        pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=int(os.environ.get("BOARD_HOOK_PORT", "8765")))
    p.add_argument("--bind", default=os.environ.get("BOARD_HOOK_BIND", "0.0.0.0"))
    a = p.parse_args()
    srv = ThreadingHTTPServer((a.bind, a.port), H)
    print(f"board hook listening on http://{a.bind}:{a.port}/board-hook  (log: {LOG})", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
