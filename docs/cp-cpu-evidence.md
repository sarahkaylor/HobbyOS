# cp-cpu (lane C1-cpu) gate evidence — P1 CPU state: XSAVE/AVX, user GS base, F2 frame

Recorded on **strix** (interim gate host while linuxci is out of service):
AMD Ryzen AI MAX+ 395 (Zen 5, AVX-512), Linux 7.0.0-43-generic, QEMU 10.2.1,
KVM.  Tree under test: `cp-cpu` @ 54a5a1e9, file-for-file identical to the
strix checkout (md5 of all 1520 tracked files under src/ tools/ tests/
Makefile run_unit_tests_intel.sh).  Parity baseline: `~/cp/base` = integrator
`cyberpunk` @ 6e73e34e.  No result from linuxci is used here (its disk went
bad on 2026-11-15; nothing read from it after 08:23 is trusted).

CPU models: `-cpu host` (the default X64_KVM_CPU under KVM; Zen 5 CPUID
advertises AVX-512 and XSAVE components 0x2e7 / 2440 bytes) and
`-cpu qemu64` (no XSAVE: the FXSAVE fallback).

## Commands (every boot under `flock /tmp/cp-qemu.lock`)

```
rsync -a --delete --exclude .git --exclude obj/ --exclude third_party/ ~/hobbyos-cp-lanes/cpu/ strix:cp/lanes/cpu/
export TOOLS_SKIP=1          # on strix, in ~/cp/lanes/cpu (and ~/cp/base for parity)
make ARCH=intel MODE=test -j16 hobbyos.elf disk.img
for c in host qemu64; do for p in avx gs fpu_sig tramp_sig futex_sig join timers hello; do
  x=""; [ $p = fpu_sig ] && x="PROBE.ELF nest"
  flock /tmp/cp-qemu.lock env T3_QEMU_ARGS="-enable-kvm -cpu $c" LP_WAVE_EXTRA="$x" \
    bash tools/gate/t3_linux_probe.sh $p
done; done
for c in host qemu64; do rm -f hobbyos.elf
  flock /tmp/cp-qemu.lock env UNIT_TESTS_TIMEOUT=900 EXTRA_QEMU_ARGS="-enable-kvm -cpu $c" \
    bash ./run_unit_tests_intel.sh
done
make -i host_tests -j16
```
`make test_intel` / the full T3 wave were not run (integrator ban: known base
wedge).  Native reference: the same probe ELFs run directly on strix's Linux.

## XCR0 on an AVX-512 host

The kernel enables exactly x87|SSE|AVX even though the CPU offers more, so the
F2 frame is 832 bytes (+4 magic2).  Native Linux on the same CPU enables
0x2e7 and uses 2440.

```
guest -cpu host : [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF
guest -cpu host : [LP] C0 cpu xsave=1 osxsave=1 avx=1 avx2=1 avx512f=1 xcr0=0x7 xsave_size=832 avx_off=576 cpu_xfeatures=0x2e7 cpu_xsave_max=2440
guest -cpu host : [LP] C3 frame uc_flags=0x1 fpregs%64=0 magic1=0x46505853 ext=836 xfeatures=0x7 xstate_size=832 magic2=0x46505845 xstate_bv=0x6 saved_state_ok=1
guest qemu64    : [LP] C0 cpu xsave=0 osxsave=0 avx=0 avx2=0 avx512f=0 xcr0=0x0 xsave_size=0 avx_off=0 cpu_xfeatures=0x0 cpu_xsave_max=0
guest qemu64    : [LP] C3 frame uc_flags=0x0 fpregs%64=0 magic1=0x0 ext=0 xfeatures=0x0 xstate_size=0 magic2=0x0 xstate_bv=0x0 saved_state_ok=1

```
native strix: [LP] C0 cpu xsave=1 osxsave=1 avx=1 avx2=1 avx512f=1 xcr0=0x2e7 xsave_size=2440 avx_off=576 cpu_xfeatures=0x2e7 cpu_xsave_max=2440
native strix: [LP] C1 auxv AT_HWCAP=0x178bfbff (cpuid1.edx=0x178bfbff) AT_HWCAP2=0x2 AT_MINSIGSTKSZ=3376 MINSIGSTKSZ=2048
native strix: [LP] C3 frame uc_flags=0x7 fpregs%64=0 magic1=0x46505853 ext=2444 xfeatures=0x2e7 xstate_size=2440 magic2=0x46505845 xstate_bv=0x207 saved_state_ok=1
native strix: [LP] done rc=0
native strix: [LP] G3 PASS SIG_DFL default-ignore signals survived: 17,18,23,28
native strix: [LP] done rc=0
native strix: Linux 7.0.0-43-generic x86_64
native strix: model name	: AMD RYZEN AI MAX+ 395 w/ Radeon 8060S
native strix: QEMU emulator version 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.3)
native strix: 6e73e34e Merge branch 'cp-vk' into cyberpunk
```

## cp-cpu @ 54a5a1e9 (strix, raw summary)

```
probe cpu cpu=host avx rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host gs rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host fpu_sig rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host tramp_sig rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host futex_sig rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host join rc=1 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=2
probe cpu cpu=host timers rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host hello rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 avx rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 gs rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 fpu_sig rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 tramp_sig rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 futex_sig rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 join rc=1 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=1
probe cpu cpu=qemu64 timers rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 hello rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
unit cpu cpu=host rc=1 wall=28s [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: Tests failed! Tests run:    450 Tests failed: 2 UNIT TESTS FAILED 
    EXPECT_EQ FAILED: gpu_abi_pages_for(64ull * 1024 * 1024 + 1) == 0 at src/kernel/virtio_gpu_test.c:213
    EXPECT_EQ FAILED: gpu_abi_res_ok(&r) == 0 at src/kernel/virtio_gpu_test.c:243
unit cpu cpu=qemu64 rc=1 wall=15s [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: Tests failed! Tests run:    450 Tests failed: 2 UNIT TESTS FAILED 
    EXPECT_EQ FAILED: gpu_abi_pages_for(64ull * 1024 * 1024 + 1) == 0 at src/kernel/virtio_gpu_test.c:213
    EXPECT_EQ FAILED: gpu_abi_res_ok(&r) == 0 at src/kernel/virtio_gpu_test.c:243
host_tests cpu :: [host-tests] done: 38/40 passed :: 2 FAIL rows
      FAIL  tools/board/sync_gaming_md_test.py  (FileNotFoundError: [Errno 2] No such file or directory: 'gaming.md')
      FAIL  tools/t5/roadmap_doc_test.py  (FileNotFoundError: [Errno 2] No such file or directory: 'gaming.md')
      PASS all 99 layout golden rows match (sizes + offsets of linux_layout.h)
      PASS all 22 F2 golden rows match linux_sigframe.h
    LINUX SIGFRAME TESTS PASSED
MATRIXDONE
```

## Parity run: cp-cpu @ 703e5df7 content and base 6e73e34e (strix, raw summary)

Same commands; base has no avx/gs probes.  The only code between this run and 54a5a1e9 is the SIGCONT/send-path commit.

```
probe cpu cpu=host avx rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host gs rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host fpu_sig rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host tramp_sig rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host futex_sig rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host join rc=1 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=2
probe cpu cpu=host timers rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=host hello rc=0 [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe base cpu=host fpu_sig rc=0  :: [LP] done rc=0
probe base cpu=host tramp_sig rc=0  :: [LP] done rc=0
probe base cpu=host futex_sig rc=0  :: [LP] done rc=0
probe base cpu=host join rc=1  :: [LP] done rc=2
probe base cpu=host timers rc=0  :: [LP] done rc=0
probe base cpu=host hello rc=0  :: [LP] done rc=0
probe cpu cpu=qemu64 avx rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 gs rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 fpu_sig rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 tramp_sig rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 futex_sig rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 join rc=1 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=2
probe cpu cpu=qemu64 timers rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe cpu cpu=qemu64 hello rc=0 [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: [LP] done rc=0
probe base cpu=qemu64 fpu_sig rc=0  :: [LP] done rc=0
probe base cpu=qemu64 tramp_sig rc=0  :: [LP] done rc=0
probe base cpu=qemu64 futex_sig rc=0  :: [LP] done rc=0
probe base cpu=qemu64 join rc=1  :: [LP] done rc=2
probe base cpu=qemu64 timers rc=0  :: [LP] done rc=0
probe base cpu=qemu64 hello rc=0  :: [LP] done rc=0
unit cpu cpu=host rc=1 wall=28s [FPU] xsave=1 xcr0=0x7 size=832 mxcsr_mask=0x2FFFF :: Tests failed! Tests run:    450 Tests failed: 2 UNIT TESTS FAILED 
    EXPECT_EQ FAILED: gpu_abi_pages_for(64ull * 1024 * 1024 + 1) == 0 at src/kernel/virtio_gpu_test.c:213
    EXPECT_EQ FAILED: gpu_abi_res_ok(&r) == 0 at src/kernel/virtio_gpu_test.c:243
unit cpu cpu=qemu64 rc=1 wall=14s [FPU] xsave=0 xcr0=0x0 size=512 mxcsr_mask=0x2FFFF :: Tests failed! Tests run:    450 Tests failed: 2 UNIT TESTS FAILED 
    EXPECT_EQ FAILED: gpu_abi_pages_for(64ull * 1024 * 1024 + 1) == 0 at src/kernel/virtio_gpu_test.c:213
    EXPECT_EQ FAILED: gpu_abi_res_ok(&r) == 0 at src/kernel/virtio_gpu_test.c:243
unit base cpu=host rc=1 wall=30s  :: Tests failed! Tests run:    448 Tests failed: 2 UNIT TESTS FAILED 
    EXPECT_EQ FAILED: gpu_abi_pages_for(64ull * 1024 * 1024 + 1) == 0 at src/kernel/virtio_gpu_test.c:213
    EXPECT_EQ FAILED: gpu_abi_res_ok(&r) == 0 at src/kernel/virtio_gpu_test.c:243
unit base cpu=qemu64 rc=1 wall=15s  :: Tests failed! Tests run:    448 Tests failed: 2 UNIT TESTS FAILED 
    EXPECT_EQ FAILED: gpu_abi_pages_for(64ull * 1024 * 1024 + 1) == 0 at src/kernel/virtio_gpu_test.c:213
    EXPECT_EQ FAILED: gpu_abi_res_ok(&r) == 0 at src/kernel/virtio_gpu_test.c:243
host_tests cpu :: [host-tests] done: 38/40 passed :: 2 FAIL rows
      FAIL  tools/board/sync_gaming_md_test.py  (FileNotFoundError: [Errno 2] No such file or directory: 'gaming.md')
      FAIL  tools/t5/roadmap_doc_test.py  (FileNotFoundError: [Errno 2] No such file or directory: 'gaming.md')
      PASS all 99 layout golden rows match (sizes + offsets of linux_layout.h)
      PASS all 22 F2 golden rows match linux_sigframe.h
    LINUX SIGFRAME TESTS PASSED
host_tests base :: [host-tests] done: 38/40 passed :: 2 FAIL rows
      FAIL  tools/board/sync_gaming_md_test.py  (FileNotFoundError: [Errno 2] No such file or directory: 'gaming.md')
      FAIL  tools/t5/roadmap_doc_test.py  (FileNotFoundError: [Errno 2] No such file or directory: 'gaming.md')
      PASS all 99 layout golden rows match (sizes + offsets of linux_layout.h)
MATRIXDONE
```

## tramp_sig T2 repeats (strix, -cpu host and qemu64, cp-cpu vs base)

```
cpu cpu=host i=1 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
base cpu=host i=1 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
cpu cpu=qemu64 i=1 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
base cpu=qemu64 i=1 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
cpu cpu=host i=2 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
base cpu=host i=2 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
cpu cpu=qemu64 i=2 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
base cpu=qemu64 i=2 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
cpu cpu=host i=3 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
base cpu=host i=3 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
cpu cpu=qemu64 i=3 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
base cpu=qemu64 i=3 rc=0 [LP] T2 still_parked=1 calls=1 ret=0 errno=0 rsp_delta=0 call_after_restart=8 ok=1
ALLDONE
```

## Reading

- Every probe except `join` reaches `[LP] done rc=0` on both CPU models.
  `join` fails identically on base: row E is the compat clone() refusal of a
  fork-shaped clone (owned by C2-sys), and row A (`wake=-1`) is a pre-existing
  park-first race that passed once (rc=1 instead of 2) on cp-cpu qemu64.
- Unit tests: 450 run on cp-cpu (448 + the two new FPU tests), 448 on base;
  the same two failures in all four runs (virtio_gpu_test.c:213 and :243).
  The IRQ-off budget failure seen on linuxci does not reproduce on strix,
  on either tree.
- host_tests: 38/40 on both trees; the same two failures need a `gaming.md`
  the repo does not track.  F2 golden 22/22 and layout golden 99/99 pass.
- tramp_sig T2: 4/4 per tree per CPU model here.  The fix (32579325) is the
  missing idle-CPU kick after a signal ends a compat thread's park.
