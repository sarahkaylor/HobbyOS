#!/usr/bin/env python3
"""l8-netfix host echo server.

Binds 127.0.0.1:8765 (QEMU user networking maps guest 10.0.2.2 -> host
127.0.0.1).  For every connection:

  * records the EXACT first bytes it received to <logdir>/rx-<n>.txt as a
    hex dump (raw on-device TX evidence for bug #1),
  * replies with an ASCII envelope that echoes the received bytes back to
    the guest as uppercase hex, so the guest can verify TX integrity from
    inside the VM without trusting the serial channel:
        NETFIX-SRV-OK
        rxlen=<n>
        rxhex=<hex of first WANT bytes>
        PAYLOAD=NETFIX-PAYLOAD-42
        END

The guest program (src/user/netfix_test.c) checks: no leading NULs before
the magic (bug #2), and the echoed hex decodes to exactly the request it
sent (bug #1).  Exit 0; run forever; logs to <logdir>.
"""

import os
import socketserver
import sys

WANT = 200          # hex depth we echo (and log)
LOG = "/tmp/l8-netfix-log"
if len(sys.argv) > 1:
    LOG = sys.argv[1]
os.makedirs(LOG, exist_ok=True)
_complete = []


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        data = b""
        self.request.settimeout(2.0)
        try:
            while len(data) < 4096:
                chunk = self.request.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"\r\n\r\n" in data or b"\n\n" in data:
                    pass  # keep reading until the client closes (simple)
        except Exception:
            pass

        idx = len(_complete)
        _complete.append(data)
        with open(os.path.join(LOG, "rx-%03d.txt" % idx), "wb") as f:
            f.write(data)
        with open(os.path.join(LOG, "rx-%03d.hex" % idx), "w") as f:
            f.write(data[:WANT].hex(" "))

        hdr = data[:WANT]
        resp = (
            "NETFIX-SRV-OK\r\n"
            "rxlen=%d\r\n"
            "rxhex=%s\r\n"
            "PAYLOAD=NETFIX-PAYLOAD-42\r\n"
            "END\r\n" % (len(data), hdr.hex().upper())
        )
        self.wfile.write(resp.encode("ascii"))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    port = int(os.environ.get("NETFIX_PORT", "8765"))
    print("netfix echo server on 127.0.0.1:%d, logs in %s" % (port, LOG),
          flush=True)
    with Server(("127.0.0.1", port), Handler) as srv:
        srv.serve_forever()


if __name__ == "__main__":
    main()
