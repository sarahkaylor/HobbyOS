# SWARM-REPORT — WP-ARM (restore the AArch64 link)

Lane: `wp-arm` · branch `nv/wp-arm-bss` (cut from `d44659a2`, tip `4647c8f1`) · box
`sarah@192.168.0.250` (`/srv/nv/wp-arm`) · worktree
`/Users/sarah/Documents/GitHub/hobbyos-nv/wp-arm` · 2026-11-23.

Tags follow `PLAN.md` §6: **MEASURED** = I ran it on c4 and read the number; **SOURCE** =
read from the tree/doc; **INFERRED** = reasoning from those, not observed. Hex arithmetic
in this report was computed in code, not by hand.

## Outcome

- **The fix is upstream, and this lane only carried it.** `origin/main` already had
  `6ca54e70 fix(net): the socket PCB table is frame-backed -- the ARM unit image links
  again` and `7d8d6b7d fix(net): reserve the socket table without a 64 MiB IRQ-off memset`.
  Both are cherry-picked onto `nv/wp-arm-bss` (`-x`), in that order, with **no conflicts**:
  our base `d44659a2` is the exact 3-commit-behind ancestor set (`git rev-list --count
  d44659a2..origin/main` = 3), so nothing in the branch had diverged from the files those
  commits touch (`src/kernel/net.c`, `src/kernel/dhcp.c`, `src/kernel/net_rdma.c`,
  `docs/blk-cache.md`).
- **One carrying correction was required** (`4647c8f1`): the upstream reservation used the
  `frame_alloc_contig()` *physical* address as a pointer, which is right on AArch64
  (identity) and wrong on x86_64 (DMAP). Measured: with the two cherry-picks alone the
  intel unit tier hangs at the first `net_socket_create()`; with `phys_to_virt()` it is
  561 / 0. Details in "Divergence" below.
- **Full ARM link is green** on c4, first lane to pass it.
- **Intel gate unchanged:** `Tests run: 561 / Tests failed: 0 / UNIT TESTS PASSED`.
- For the record: I had independently built an equivalent frame-backing of the *per-slot*
  RX rings before being told the fix was upstream. That patch was thrown away uncommitted
  (kept at `/tmp/wp-arm-mine.patch` on the Mac, nothing in the branch). Its measurements
  are the ones in the next-but-one section — they agree with the upstream numbers to
  within the 3 commits' text growth, which is a useful cross-check, not a second fix.

## Commits on `nv/wp-arm-bss`

| sha | what |
|---|---|
| `98a9d75a` | `fix(net): the socket PCB table is frame-backed -- the ARM unit image links again` (cherry-pick of `6ca54e70`) |
| `26a00298` | `fix(net): reserve the socket table without a 64 MiB IRQ-off memset` (cherry-pick of `7d8d6b7d`) |
| `4647c8f1` | `fix(net): the socket table goes through phys_to_virt -- x86_64 frames are DMAP-addressed` (this lane) |

## Measurement — what I did before the cherry-pick

I was asked to diagnose the offender first, so this is independent of the upstream
message's numbers. Method: the ASSERT-free link of the identical 136 objects
(`head -n 73 linker.ld` → `/tmp/link_nocheck.ld`, `ld.lld -T … -o /tmp/hobbyos_nocheck.elf`),
`llvm-readelf -S` / `llvm-nm` on the result, and `llvm-size -A obj/arm/*.o` per object.
All MEASURED on c4 at `d44659a2`, `ARCH=arm MODE=unit_tests LO_ON_DISK=`.

Geometry of the failing link:

| symbol / section | value |
|---|---|
| `.text` | `0x40080000`, 1,443,288 B |
| `.rodata` | `0x401e09a8`, 242,160 B |
| `.bss` start | `0x40400000` (2 MiB-aligned by `SORT_BY_ALIGNMENT`) |
| `.bss` size | `0x53B114C` = 87,757,132 B (83.69 MiB) |
| `__bss_end` | `0x457B114C` |
| ASSERT line | `0x45600000` → **over by 0x1B114C = 1,773,900 B (1.69 MiB)** |
| hard ceiling (slide `0x3FA600000`, RAM top `0x440000000`) | `0x45A00000` → under by 2,420,404 B (2.31 MiB) |

So the image was *inside* the placement window by 2.31 MiB but had lost the 4 MB margin
`linker.ld` requires — i.e. it was one 2 MiB alignment step from the documented silent
data-abort cascade. The offender, by object `.bss` (sum over 82 objects = 87,798,616 B;
the 41,484 B difference from the linked figure is alignment/COMDAT collapse):

| object | `.bss` | share |
|---|---|---|
| `net.o` | 67,662,808 B | **77.10 %** |
| `virtio_gpu.o` | 10,391,556 B | 11.84 % |
| `process.o` | 2,248,196 B | 2.56 % |
| `gpu_abi.o` | 1,183,744 B | 1.35 % |
| `fs.o` | 1,169,868 B | 1.33 % |
| `program_loader.o` | 906,372 B | 1.03 % |
| `frame.o` | 491,652 B | 0.56 % |
| all other 75 objects | 3,744,420 B | 4.26 % |

`llvm-nm --size-sort obj/arm/net.o` names it in one line: `sockets` = `0x4005080` =
16 × 4 MiB RX ring. `static struct socket_pcb sockets[MAX_SOCKETS]` with
`SOCKET_RX_BUF_SIZE = 4 MiB` and `MAX_SOCKETS = 16`.

The fix removes that table from `.bss` (frame-backed, reserved once). Measured after
`98a9d75a` + `26a00298` + `4647c8f1` on the same box and command:

| | before | after |
|---|---|---|
| `.bss` | `0x53B114C` = 83.69 MiB | `0x13AC0D4` = 20,627,668 B (19.67 MiB) |
| `__bss_end` | `0x457B114C` | `0x417AC0D4` |
| headroom under `0x45600000` | −1,773,900 B | **+65,355,564 B (62.33 MiB)** |
| headroom under `0x45A00000` | +2,420,404 B | +69,549,868 B (66.33 MiB) |
| `net.o` `.bss` | 67,662,808 B | 533,344 B |

## Why the assertion was not raised

The window did not move; the image did. `0x45A00000` is `RAM top (0x440000000) − Limine
slide (0x3FA600000)` — SOURCE: the crash-forensics pair in `docs/blk-cache.md` §11
(FAR `0x44042E050` → link `0x45E2E050` implies exactly that slide), and the run's QEMU
command line is still `-m 16384M` (MEASURED: `pgrep -af qemu-system-aarch64` on the box),
the config the doc's ceiling assumes. `linker.ld` keeps 4 MiB of margin under it
(`0x45600000`). Before the fix the image was 2.31 MiB under the ceiling — no margin left
for any other lane's table — so raising the constant would have converted a link-time
failure into the documented runtime cascade. Removing 64 MiB of `.bss` is the fix; the
constant is untouched.

## Gates (run on c4, exact lines)

**ARM link (the point of this lane) — PASS**

```
$ nvswarm sh wp-arm 'TOOLS_SKIP=1 BROWSER_SKIP=1 make -j12 ARCH=arm MODE=unit_tests LO_ON_DISK= hobbyos.elf'
... ld.lld -T linker.ld -o hobbyos.elf obj/arm/... (136 objects)
$ echo $?  → 0          # no ASSERT error; tested before the cherry-picks: exit 2 + the ASSERT text
$ llvm-nm hobbyos.elf | grep __bss_end
00000000417ac0d4 B __bss_end
```

**Intel build + unit tier — PASS (unchanged count)**

```
$ nvswarm build wp-arm                                   # ARCH=intel MODE=unit_tests
$ nvswarm sh wp-arm 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'
Tests run:    561
Tests failed: 0
UNIT TESTS PASSED
```

**ARM unit tier (aarch64 under QEMU, TCG) — see "ARM unit-tier boot" below.**

## Divergence found while carrying the fix (x86_64)

`net_sockets_reserve()` in `6ca54e70` did `struct socket_pcb *tab = (struct
socket_pcb *)base;`. `frame_alloc_contig()` returns a *physical* address; on x86_64 the
direct map is `DMAP_BASE + phys` and the only identity window is the image itself,
`[KIMG_BASE, CP_KHOLE_HI)` = `[0x70000000, 0x78000000)`, whose frames the pool may not
hand out (`src/include/kmem.h`; every other `frame_alloc_contig()` caller — `blk_cache.c`,
`elf_loader.c`, `nfs.c`, `nv_fw.c` — converts with `phys_to_virt()`). The upstream
verification said "x64 unit_tests *link* clean" plus a runtime boot, and a runtime boot
reserves the table in `net_init()` while the pool is still giving out low, incidentally
mapped frames (INFERRED from `net_init()`'s early position); the unit tier reserves it at
the first `net_socket_create()`, long after.

```
cherry-picks only        run_unit_tests_intel.sh stops after
                          "Running test_net_socket_fd_table..." and never prints a
                          summary: "Tests timed out!" with the 20 s default budget AND
                          with UNIT_TESTS_TIMEOUT=180 (two runs, same point)
+ phys_to_virt (4647c8f1) Tests run: 561 / Tests failed: 0 / UNIT TESTS PASSED
```

Suggested upstream follow-up: the same one-line change on `main` (it compiles and runs on
AArch64 unchanged — `phys_to_virt()` is the identity there).

## Guards left behind

What is automatic today, all of it from the upstream commits — no fourth guard was added,
because a parallel `_Static_assert`/test here would be a second source of truth for an
invariant that belongs upstream:

1. `linker.ld`'s `ASSERT(__bss_end <= 0x45600000, …)` — unchanged, and it did its job: the
   regression surfaced at *link* time, not as a boot with no serial output.
2. The table is no longer a static object at all: `sockets` is a `struct socket_pcb *` into
   pool frames reserved by `net_sockets_reserve()`. Re-introducing 64 MiB of `.bss` now
   takes a deliberate revert to a static array, which is visible in review (it is not a
   silent one-line growth).
3. `docs/blk-cache.md` §11 carries the second instance of the rule ("a multi-MiB table is
   not *large*, it is the whole budget") with the numbers.

Recommendation upstream: a `_Static_assert(sizeof(struct socket_pcb) <= 4096, …)` in
`net.c` would catch a re-inlined ring at compile time instead of 60 MB later. I verified on
c4 that such an assert does fire when the 4 MiB array is put back (temporary `/tmp/inc/net.h`
copy, `clang -I/tmp/inc … -c src/kernel/net.c` → "static assertion failed … must stay
small"), but it is not in this branch: it belongs in the commit that owns the table.

## ARM unit-tier boot (what actually happened)

Two separate facts; both MEASURED on c4.

**1. A fresh ARM disk image does not build — before QEMU starts.** The unmodified tier
script fails in userland, not in the kernel:

```
$ nvswarm sh wp-arm 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests.sh'
src/user/nvsmi.c:68:18: error: variable has incomplete type 'struct nv_info'
   68 |   struct nv_info info;
      |                  ^
1 error generated.
make: *** [Makefile:1874: obj/arm/nvsmi.o] Error 1
```

`struct nv_info` and the whole `/dev/nvctl` ABI block in `src/include/nv.h` sit inside
`#ifdef __x86_64__` (the native nv driver is x86_64-only), so the AArch64 userland does not
compile. Nothing in this lane touches `nvsmi.c` or `nv.h` (`git diff d44659a2 --name-only`
lists neither); it is a pre-existing ARM break that only shows up now because building
`disk.img` — not just `hobbyos.elf` — was never possible with the link red.
**Not fixed here: it is another lane's file.** To still answer the boot question I compiled
a 5-line stub at `obj/arm/nvsmi.o` (box only, never in the tree, reported as a harness
workaround; no unit test reads `/NVSMI.BIN`) and built the otherwise-real ARM image.

**2. With the image built, the tier boots and then stops in the shim suite.**

```
$ make ARCH=arm MODE=unit_tests TOOLS_SKIP=1 BROWSER_SKIP=1 run QEMU_ARGS="-display none"
...
  Running test_nvshim_probe...
[KERNEL] FATAL: CPU 0x0 Synchronous Exception in EL1! ESR: 0x2000000 EC: 0x0 ISS: 0x0,
         FAR: 0x0, ELR: 0x401484D0, PROC pid=42 name=, SPSR: 0x60000345
         (ELR = mock_device_open, src/kernel/nv_rm_mock.c)
```

- **180 `Running test_…` rows executed, 0 `ASSERTION FAILED`, 0 `EXPECT_EQ FAILED`,** through
  frame / fat16 / locks / pipe / unix / fs / mmu / vm / process / program_loader / trap /
  virtio_blk / blk_cache / blk_cache_press / virtio_gpu / gpu_abi_m2 — then the EL1
  exception at the first `mock_device_open()` call and no further output. No
  `UNIT TESTS PASSED` line was ever printed; QEMU was still sitting at that EL1 dump when the
  poller killed it.
- **It is not this lane's change.** Same fault, same suite, same 180 rows on the pre-fix
  layout: I re-linked `d44659a2`'s own `net.c`/`net.h`/`net_test.c` with the ASSERT raised
  to `0x46000000` (`__bss_end` 0x457B114C, i.e. the old 64 MiB table back) and reproduced
  it identically (`ELR` = `mock_device_open` there too). It also cannot be reached-by-accident
  of the fix: net's suites are registered after `nv_shim_test_suite()`.
- **It is our branch's code, not main's.** `origin/main` contains none of the round-1 nv
  merges (`git rev-list origin/main | grep -c f8801ad1` → 0, `d44659a2` → 0); the shim and
  its mock RM exist only on `nv/integ-swarm`. That is why upstream's ARM run on `main`
  reports `438 tests, 0 failed, UNIT TESTS PASSED` (SOURCE: their commit message) while this
  branch does not (INFERRED: different suite set — main has no nv_shim to stop in).
  **This becomes main's problem at round-3 integration.**

So: **the ARM link is fixed and the ARM tier now boots and runs 180 tests clean; it does
not pass yet, and the blocker is `nv_rm_mock.c`/`nv_shim`, not the linker.**

## What is NOT verified

- The ARM tier's suites that come after `test_nvshim_probe` (net, net_rdma, nv_dma, nfs,
  timer, smp, time, math, elf_loader) were never reached on AArch64 — this run stops there.
- `tools/t3_tier.sh arm` (88 programs) was not run for this lane; upstream reports it green
  apart from two pre-existing LKSTEST "fstat mode perms" FAILs.
- Runtime networking (`DHCP offer/request/ack`, NFS) was not exercised here — only the
  unit tier; upstream reports it on the same change.
- The Limine slide was not re-measured on this run (the boot log does not print it); the
  window numbers are SOURCE (doc §11 forensic pair, `kmem.h` identity window) with the
  arithmetic re-checked in code, and the QEMU `-m 16384M` config MEASURED.
- Nothing was pushed; nothing merged into `ada/` or `lane-*`.

## Next step

1. **Round-3 integration: merge `origin/main` into `nv/integ-swarm` wholesale**, not these
   three commits by hand. `main` is 3 commits ahead of `d44659a2` and the third,
   `9b9d946a fix(lkstest): FAT16 files stat as vfat's 0755`, is a *T3 red* fix that the
   integration branch also wants; carrying only the ARM two would leave integ-swarm
   re-fixing it.
2. Land the `phys_to_virt()` line on `main` (see Divergence).
3. Two ARM blockers remain, both pre-existing and independent of the link (details in the
   boot section): `src/user/nvsmi.c` does not compile for AArch64 (its ABI struct lives
   inside `#ifdef __x86_64__` in `src/include/nv.h`), so a fresh `make ARCH=arm … disk.img`
   fails before QEMU starts.
