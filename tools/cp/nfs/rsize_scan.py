#!/usr/bin/env python3
"""cp/nfsguard: source scan for the 32 KiB NFS READ size (docs/cp-nfs.md).

The 32 KiB READ fix has been made more than once and lost again, usually when
an older nfs.c came back through a merge.  The old code clamped every READ to
the single-frame NFS_READ_RSIZE (1344 B), and that clamp sits on several
separate paths.  This scan runs in `make host_tests`, needs no QEMU and no
NFS server, and FAILs when any known regression is present:

  R1  nfs.h no longer defines NFS_READ_RSIZE_MAX >= 32768
  R2  nfs.c clamps a READ size back to NFS_READ_RSIZE
      (`if (rs > NFS_READ_RSIZE) rs = NFS_READ_RSIZE;`, the old code)
  R3  readahead/pipelined READ passes the single-frame size directly
      (`nfs_read_pipelined(..., NFS_READ_RSIZE)`)
  R4  the mount no longer asks FSINFO, or no longer sets read_size from
      nfs_pick_rsize()
  R5  nfs.c no longer uses NFS_RPC_RBIG reply buffers (a 2 KiB reply buffer
      truncates every large READ)
  R6  net.c no longer sends IPv4 fragments to reassembly (ipr_add): a 32 KiB
      reply arrives as ~23 fragments

--selftest re-applies each regression to a temporary copy of the files and
requires the scan to FAIL on every one and PASS on the untouched copy, so the
scan cannot quietly stop detecting them.

The boot guard (tools/cp/nfs/rsize_guard.sh) checks the behaviour end to end:
2 READ RPCs per 64 KiB against a real server.  This scan is the cheap
tripwire that runs everywhere.
"""
import os
import re
import shutil
import sys
import tempfile

ROOT_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
FILES = ("src/include/nfs.h", "src/kernel/nfs.c", "src/kernel/net.c")


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def scan(root):
    src = {}
    for f in FILES:
        p = os.path.join(root, f)
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                src[f] = strip_comments(fh.read())
        except OSError as e:
            return ["missing %s (%s)" % (f, e)]
    h, c, n = src["src/include/nfs.h"], src["src/kernel/nfs.c"], src["src/kernel/net.c"]
    bad = []

    m = re.search(r"^\s*#\s*define\s+NFS_READ_RSIZE_MAX\s+\(?\s*(\d+)", h, re.M)
    if not m or int(m.group(1)) < 32768:
        bad.append("R1 nfs.h: NFS_READ_RSIZE_MAX missing or below 32768 (%s)"
                   % (m.group(1) if m else "not defined"))

    for mm in re.finditer(r">\s*NFS_READ_RSIZE\s*\)\s*\{?\s*\w+\s*=\s*NFS_READ_RSIZE\s*;", c):
        line = c.count("\n", 0, mm.start()) + 1
        bad.append("R2 nfs.c:%d: READ size clamped back to the 1344-byte NFS_READ_RSIZE" % line)

    for mm in re.finditer(r"nfs_read_pipelined\s*\(([^;]*?)\)\s*;", c, re.S):
        args = [a.strip() for a in mm.group(1).split(",")]
        if args and args[-1] == "NFS_READ_RSIZE":
            line = c.count("\n", 0, mm.start()) + 1
            bad.append("R3 nfs.c:%d: nfs_read_pipelined() called with the 1344-byte rsize" % line)

    if "NFS_PROC_FSINFO" not in c:
        bad.append("R4 nfs.c: the mount never sends FSINFO (READ size never learned)")
    if not re.search(r"read_size\s*=\s*nfs_pick_rsize\s*\(", c):
        bad.append("R4 nfs.c: read_size is never set from nfs_pick_rsize()")

    if len(re.findall(r"\bNFS_RPC_RBIG\b", c)) < 2:
        bad.append("R5 nfs.c: READ replies no longer use NFS_RPC_RBIG buffers")

    if len(re.findall(r"\bipr_add\s*\(", n)) < 2:  # the definition plus the call
        bad.append("R6 net.c: IPv4 fragments are no longer sent to ipr_add (no reassembly)")
    return bad


MUTATIONS = [
    ("R1", "src/include/nfs.h",
     r"(#define\s+NFS_READ_RSIZE_MAX\s+)\d+", r"\g<1>1344"),
    ("R2", "src/kernel/nfs.c",
     r"if \(rs > NFS_READ_RSIZE_MAX\) rs = NFS_READ_RSIZE_MAX;",
     "if (rs > NFS_READ_RSIZE) rs = NFS_READ_RSIZE;"),
    ("R3", "src/kernel/nfs.c",
     r"(nfs_read_pipelined\(m, fh, base, nfs_ra_buf, NFS_RA_BYTES,\s*)rars\)",
     r"\g<1>NFS_READ_RSIZE)"),
    ("R4", "src/kernel/nfs.c",
     r"m->read_size\s*=\s*nfs_pick_rsize\s*\(rtmax, rtpref\);", "(void)rtpref;"),
    ("R5", "src/kernel/nfs.c", r"\bNFS_RPC_RBIG\b", "NFS_RPC_BUF"),
    ("R6", "src/kernel/net.c",
     r"struct ipr_slot \*sl = ipr_add\(", "struct ipr_slot *sl = (struct ipr_slot *)0; (void)("),
]


def selftest(root):
    fails = 0
    base = scan(root)
    if base:
        print("rsize_scan selftest: the tree itself fails, so the selftest cannot run:")
        for b in base:
            print("  " + b)
        return 1
    for rid, f, pat, rep in MUTATIONS:
        tmp = tempfile.mkdtemp(prefix="rsize_scan.")
        try:
            for g in FILES:
                os.makedirs(os.path.join(tmp, os.path.dirname(g)), exist_ok=True)
                shutil.copy(os.path.join(root, g), os.path.join(tmp, g))
            p = os.path.join(tmp, f)
            with open(p, encoding="utf-8", errors="replace") as fh:
                s = fh.read()
            s2, k = re.subn(pat, rep, s)
            if k == 0:
                print("rsize_scan selftest: %s mutation no longer applies to %s "
                      "(update MUTATIONS with the code)" % (rid, f))
                fails += 1
                continue
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(s2)
            got = scan(tmp)
            if not any(b.startswith(rid) for b in got):
                print("rsize_scan selftest: %s regression NOT detected" % rid)
                fails += 1
            else:
                print("rsize_scan selftest: %s regression detected (%d site(s))" % (rid, k))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("rsize_scan selftest: %s" % ("PASS" if fails == 0 else "FAIL"))
    return 1 if fails else 0


def main(argv):
    root = os.path.abspath(os.environ.get("NFS_RSIZE_SCAN_ROOT", ROOT_DEFAULT))
    if "--selftest" in argv:
        return selftest(root)
    bad = scan(root)
    if bad:
        print("rsize_scan: FAIL: the 32 KiB NFS READ size has regressed (docs/cp-nfs.md):")
        for b in bad:
            print("  " + b)
        return 1
    print("rsize_scan: PASS (32 KiB NFS READ paths intact)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
