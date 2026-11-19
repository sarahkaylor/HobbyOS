cp-int-ext2b @ 31ce9638 (= cyberpunk 4b5616cf + cp-ext2b 7e498323 + merge fix), gated on strix
in a fresh checkout ~/cp/lanes/int-ext2b (rsync of this tree, third_party from ~/cp/base,
glibc 2.31 sysroot copied from ~/cp/lanes/ext2b/obj), MAKEFLAGS=-j16, every boot under
flock /tmp/cp-qemu.lock.  T3 wave: known base wedge, not run.
  build (intel)            rc=0
  make ext2_test_run       714 checks, 0 failures
  ext2_gate.sh boots 1-3   ext2_gate: PASS (ext2_gate.txt, boot3_lp_rows.txt)
  unit tier                450 run, 2 failed: virtio_gpu_test.c:213, :243 (known)
  probes                   12 PASS incl. tmpfs (T11 lock across fork PASS); join known red
                           (ext2 probe: gate boots 1-2)
  host_tests               38/40 (the two gaming.md board tests)
  ARM build                rc=0; 0 FP/SIMD instructions in ext2*, vfs*, fs, vm, elf_loader, tmpfs
