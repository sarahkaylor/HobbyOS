# w12-rcut attempt-1 S2 -j8 dirent loss — diagnostic summary (evidence)

## What happened
S0/S1/T2.5 all PASS in one boot (S1 -j8 = 1982 s wall, first real -j8 win).
S2 -j8: 56 kernel-compile rc=0 + 3 kernel-ascompile rc=0 (57/59), then the
lld link failed:
    LLD.BIN: error: cannot open /SRC/obj/arm/net_rdma_test.o: No such file
    or directory   (line 3078; a second interleaved ".../net_test.o"
    fragment belongs to the same relayed error block)
MAKE.BIN: *** [Makefile.hobbyos:424: kernel] Error 1
[SELFHOST] phase=s2-kernel-fail reason=tool-rc rc=512
[SELFHOST] FAILED-AT stage=s2-kernel
System halt from CPU 3.   w5 outcome=1 wall=3219 s.

## On-image dirent forensics (::/SRC/obj/arm, attempts-after-halt)
- 75 LIVE short entries (58/59 kernel objects + S1 products), 45 LFN
  records, 15 deleted (0xE5) slots.
- ZERO mangled twins and ZERO `-XXXX`/`.O.T` leftovers as LIVE entries —
  the w9 writer fix holds for the 7 objects whose temp-deletes show as
  proper 0xE5 LFN+short pairs (process, fat16, fs, vm, trap, mmu, uart).
- net_rdma_test.o: short entry 100% absent; its LFN chain is GONE except a
  torn fragment at SLOT 47: an LFN record whose name decodes to ".o.tmp"
  (the tail chunk of "net_rdma_test-74d2309e.o.tmp"), sitting between
  NET_TESTO (slot 46) and NET_RDMAO (slot 48).  The clean
  "net_rdma_test.o" LFN+short that the rename logged ("new=/SRC/obj/arm/
  net_rdma_test.o", serial line /tmp trace) was clobbered.
- net_rdma.o survives as NET_RDMAO (10776 B, the w9 collide-pair partner).

## Classification
NOT the w9 serial close-after-rename bug (that fix is effective here:
other temp pairs delete cleanly).  A NEW concurrency-sensitive dirent loss:
8 concurrent clang TempFile create/rename/close sequences in one directory
cluster under -j8; one entry pair torn mid-write.  First exposure of the
w9-fix x w10-timestamps x w11-make merged combo to -j8 on real kernel TUs.
Prime suspect: FAT directory mutations not serialized across processes (a
check-then-write window in rename / LFN chain write-back under the
7-8-way rename burst).

## Fallback (per mission + integrator): correctness first
- Integrator discriminator order: save dumps (done), then fresh-disk S2 at
  -j1 (serial) as the merged-combo baseline; optionally -j4 to bound the
  safe width.  NO repeated -j8 retries.
- The receipt does NOT require -j8; attempts 2+ run S2 serial (proven
  w9-crown config) with -j8/-j4 recorded as findings.

evidence files: rcut-loop.serial(.ts), rcut-loop.phases.json,
rcut-s2fail.dirents.txt (fatdir_dump), rcut-s2fail.scan.txt (raw slot scan)
