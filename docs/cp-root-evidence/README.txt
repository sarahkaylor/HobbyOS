H8-root evidence (docs/cp-root.md).  Host: strix (Zen 5, KVM, QEMU 10.2.1), 8 vCPUs, 12 GiB.
Short excerpts only; raw consoles stay in strix:~/cp/lanes/root-ev/ and ~/cp/lanes/root-glogs/.

base-freerace-integ22-23.txt  the three [FREERACE] rows of the integ22/23 gate batches, with context
probe-base-instrument.txt     linux_probe_rootfree on d25cd54e (instrument only): 0/5 complete, 21 unsafe frees
probe-integ25.txt             linux_probe_rootfree on integ25 itself: 0/5 complete
probe-lane.txt                park without the H8-02 fix (2/5), the [ROOTSW0] capture, the lane 5/5 PASS
arm-unit.txt                  ARM unit tier, lane vs integ25 (same single failure)
gates.txt                     the lane's gate batch rows (copy of strix:~/cp/lanes/root-gates.txt)
rootsweep.txt                 [ROOTPARK]/[FREERACE]/[ROOTSW0] counts in every console of the batch
fps.txt                       the paired P9 fp720 boots (integ25 vs lane)
shootwait-ab.txt              shootwait interleaved integ25 / lane under host load (both fail)
thrstorm-ab.txt               thrstorm interleaved integ25 / lane
