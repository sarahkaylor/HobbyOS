#!/usr/bin/env python3
"""tools/cp/p0host/mountas.py — bind-mount helper for ns.sh (runs as root INSIDE a private
mount namespace).  /mnt/fast is NFS with root_squash, so root cannot even look up the
library's 0770 directories; we keep CAP_SYS_ADMIN but switch the filesystem uid/gid to
sarah (setfsuid/setfsgid) so the NFS lookups carry her credentials.
usage: mountas.py bind <src> <dst> | robind <dir>"""
import ctypes, os, sys
libc = ctypes.CDLL(None, use_errno=True)
MS_RDONLY, MS_REMOUNT, MS_BIND = 1, 32, 4096
libc.setfsgid(1000); libc.setfsuid(1000)

def mnt(src, dst, flags):
    if libc.mount(src.encode(), dst.encode(), None, flags, None) != 0:
        e = ctypes.get_errno(); sys.exit(f"mount {src} {dst} flags={flags}: {os.strerror(e)}")

if sys.argv[1] == "bind":
    mnt(sys.argv[2], sys.argv[3], MS_BIND)
elif sys.argv[1] == "robind":
    mnt(sys.argv[2], sys.argv[2], MS_BIND)
    mnt(sys.argv[2], sys.argv[2], MS_BIND | MS_REMOUNT | MS_RDONLY)
else:
    sys.exit(__doc__)
