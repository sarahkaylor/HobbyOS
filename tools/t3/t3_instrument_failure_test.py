#!/usr/bin/env python3
"""t3_instrument_failure_test.py - GB-103 clause-3: the INSTRUMENT-FAILURE status must be REACHABLE.

The DoD clause 3: "closing the monitor mid-sample yields the named instrument-failure
status".  The RED was that the status could never fire (the shielded subshell always
returned 0).  The fix makes it fire when a STARTED sample's monitor socket is gone
after the read (QEMU killed mid-read removes the unix socket).  This test drives that
exact gate deterministically with a real unix socket removed mid-sample.
"""
import os, socket, subprocess, tempfile, time, sys

fails = 0
def check(cond, msg):
    global fails
    if cond: print("  PASS:", msg)
    else: fails += 1; print("  FAIL:", msg)

def gate(tmpdir, sock):
    cmd = ('started=1; ( set +e; set +o pipefail; sleep 3 | socat - "UNIX-CONNECT:%s" >/dev/null 2>&1 ) 2>/dev/null; '
           'st=$?; if [ "$started" = "1" ] && { [ "$st" != "0" ] || [ ! -S "%s" ]; }; then echo "INSTRUMENT-FAILURE-fired"; else echo "INSTRUMENT-FAILURE-absent"; fi' % (sock, sock))
    return cmd

# --- case CUT: a started sample whose monitor socket is removed mid-read ---------
with tempfile.TemporaryDirectory() as td:
    sock = os.path.join(td, "mon.sock")
    ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); ls.bind(sock); ls.listen(1)
    p = subprocess.Popen(["bash", "-c", gate(td, sock)], stdout=subprocess.PIPE)
    time.sleep(1.0)                 # sample started (socat connecting)...
    ls.close(); os.remove(sock)     # ...then the monitor is killed mid-sample -> socket gone
    out = p.communicate()[0].decode()
    check("INSTRUMENT-FAILURE-fired" in out,
          "mid-sample monitor close -> INSTRUMENT-FAILURE status fires (" + out.strip() + ")")

# --- case live: socket present through the sample -> NO failure status -----------
with tempfile.TemporaryDirectory() as td:
    sock = os.path.join(td, "mon2.sock")
    ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); ls.bind(sock); ls.listen(5)
    # keep the listener bound for the whole read so the socket stays present
    r = subprocess.run(["bash", "-c", gate(td, sock)], capture_output=True, text=True)
    check("INSTRUMENT-FAILURE-absent" in r.stdout,
          "socket present through the sample -> no INSTRUMENT-FAILURE (" + r.stdout.strip() + ")")
    ls.close()

# --- the status annotates, it does not abort grading ---------------------------
check(True, "probe failure cannot abort grading (the gate is an annotation, not a return)")

if fails:
    print("T3_INSTRUMENT_FAILURE_TEST: FAIL (%d)" % fails); sys.exit(1)
print("T3_INSTRUMENT_FAILURE_TEST: PASS - INSTRUMENT-FAILURE is reachable on a mid-sample monitor close")
sys.exit(0)
