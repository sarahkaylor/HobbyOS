#!/usr/bin/env python3
"""tools/cp/p0host/evict.py <dir>... — drop THIS host's page cache for every file under the
given dirs (posix_fadvise DONTNEED; read-only open, nothing is written).  Used to measure a
cold-client-cache launch without `drop_caches` (which would hit the whole machine).  The
NFS server's (192.168.0.47) own cache is not touched and may still be warm."""
import os, sys
n = b = 0
for d in sys.argv[1:]:
    for root, _, files in os.walk(d):
        for f in files:
            p = os.path.join(root, f)
            try:
                fd = os.open(p, os.O_RDONLY | os.O_NOATIME)
            except OSError:
                try: fd = os.open(p, os.O_RDONLY)
                except OSError: continue
            try:
                st = os.fstat(fd)
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                n += 1; b += st.st_size
            finally:
                os.close(fd)
print(f"evicted {n} files, {b/2**30:.1f} GiB")
