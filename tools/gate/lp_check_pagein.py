#!/usr/bin/env python3
"""lp_check_pagein.py <serial.log> -- linux_probe_pagein's console check
(H10-pagein, finding H9-12, docs/cp-pagein.md): a page-in that finds no free
frame must not be reported (and made permanent) as an I/O error.

The probe prints its two children's pids:
  [LP] P1 child pid=A   exhausts the guest's frames, then executes unread
                        pages of a FAT16 file mapping;
  [LP] P4 child pid=C   executes an all-zero text page (the exec gate's EIO).
PASS needs
  - no [EIO] row for pid A (base: "[EIO] pid=A ... page-in failed ... -> SIGBUS");
  - at least one [PAGEIN-NOMEM] row: the kernel met the shortage and named it
    (vm.c's reclaim row "... -> recovered" / "... -> ENOMEM", or the trap's
    "... -> SIGKILL (out of memory, not an I/O error)");
  - at least one [EIO] row for pid C: a real read failure is still EIO.
Exit 0 = pass, 1 = fail, 2 = no run."""
import re
import sys

text = open(sys.argv[1], "rb").read().decode("latin-1")
lines = text.splitlines()
pa = re.search(r"\[LP\] P1 child pid=(\d+)", text)
pc = re.search(r"\[LP\] P4 child pid=(\d+)", text)
if not pa or not pc:
    print("lp_check_pagein: the probe's child pid rows are missing (the probe did not run)")
    sys.exit(2)
a, c = pa.group(1), pc.group(1)


def rows(tag, pid=None):
    out = []
    for ln in lines:
        i = ln.find(tag)
        if i < 0:
            continue
        r = ln[i:]
        if pid is not None and not re.search(r"\bpid=%s\b" % pid, r):
            continue
        out.append(r)
    return out


eio_a = rows("[EIO]", a)
eio_c = rows("[EIO]", c)
nomem = rows("[PAGEIN-NOMEM]")
rec = [r for r in nomem if "-> recovered" in r]
enomem = [r for r in nomem if "-> ENOMEM" in r]
kills = [r for r in nomem if "-> SIGKILL" in r]
for r in (eio_a[:3] + nomem[:6] + eio_c[:2]):
    print("  " + r[:230])
ok = not eio_a and nomem and eio_c
print(f"lp_check_pagein: {'PASS' if ok else 'FAIL'} -- child A pid={a}: [EIO] rows={len(eio_a)} "
      f"(want 0); [PAGEIN-NOMEM] rows={len(nomem)} (recovered={len(rec)} enomem={len(enomem)} "
      f"oom-kill={len(kills)}; want >= 1); child C pid={c}: [EIO] rows={len(eio_c)} (want >= 1)")
sys.exit(0 if ok else 1)
