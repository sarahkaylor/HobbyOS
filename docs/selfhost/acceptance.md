# HobbyOS self-hosting — acceptance design (lane L5, wave 1)

Status: design + staging first cut (2026-10-14). This is the executable shape
for the M4 gate — commands + markers, not vibes. Wave-2 implements the harness
(`run_selfhost_build_test.py`) and the remaining drivers; everything here is
anchored to measured numbers (see `docs/selfhost/lanes/l5-tcg-boot-timing.md`
and `l5-stage-test-a.md` for the transcripts).

Frozen inputs this doc obeys: AD-8/AD-9/AD-11, F1–F8 (§7 of `selfhost.md`),
the L4 `Makefile.hobbyos` sketch and `[SELFHOST] phase=…` marker vocabulary,
and the §8.4 evidence standard (raw serial logs, marker substrings, FAIL-token
scan — **never exit codes**).

---

## 1. Stages S0–S5 (the gate, end to end)

Each stage runs inside the booted in-OS shell except S4/S5 (host). The in-OS
stages are driven by the verify loop (§5); every stage is bounded by a timeout
and judged by marker substrings in the serial capture, with a FAIL-token scan
as the hard kill.

Marker vocabulary (aligned with L4's sketch; echoed by `Makefile.hobbyos`'s
heavy steps and by the optional `SELFHOST.BIN` driver):

```text
[SELFHOST] phase=s0-check-start   …[SELFHOST] phase=s0-check-done
[SELFHOST] phase=s1-user-start    …[SELFHOST] phase=s1-user-done
[SELFHOST] phase=s2-kernel-start  …[SELFHOST] phase=s2-kernel-done
[SELFHOST] phase=s3-disk-start    …[SELFHOST] phase=s3-disk-done
[SELFHOST] phase=s4-boot-start    …[SELFHOST] phase=s4-boot-done
[SELFHOST] phase=s5-run-start     …[SELFHOST] phase=s5-run-done
[SELFHOST] ACCEPTANCE-COMPLETE      <- the only completion marker
```

Verdict rule (frozen): a stage passes iff its `phase=<sX>-done` marker
substring appears **and** the capture contains **zero** `FAIL`/`FAILED`
tokens since the last start marker. Exit codes, frame counts and file
existence alone are NOT verdicts (§8.4). Every stage's `rc=<n>` inside the
marker is diagnostic only.

`Makefile.hobbyos` targets (F3): `check` / `user` / `kernel` / `disk` / `all`.
The `-f Makefile.hobbyos` recipe rules: one command per line (sh-conservative),
no `$(shell)`, `SHELL=/TOOLS/SH.BIN`.

### S0 — single-TU compile smoke

In-OS command:

```text
make -f Makefile.hobbyos check
```

Runs one tiny TU (`check` recipe: compile `src/user/hello_check.c` or the L4
sketch's designated TU → `/SRC/obj/check.o`, no link). Expected output:
the compile line + `[SELFHOST] phase=s0-check-done`. Timeout: 300 s wall
(measured in-OS: ≈ 1 min on the merged tip).
Purpose: fail fast on toolchain/flag/sysroot breakage before the 71-TU grind.
Typical failure: clang driver reject (`clang: error: unknown argument`),
missing builtin/resource header (`-isystem /TOOLS/RESDIR/include` wrong) →
check the curated RESDIR list (L1) first.

### S1 — user set (AD-9)

In-OS command:

```text
make -f Makefile.hobbyos user
```

Builds libc (31 TUs) + the acceptance set `HELLO, SH, LS, CAT` (+ `DESKTOP`
if it links cheaply, AD-9) → `.bin` files in `/SRC/obj/`. Expected: each
compile line, link lines, `[SELFHOST] phase=s1-user-done`. Timeout: **7200 s
(two hours)** — the dominant term is `sh.c`'s -O2 TCG compile, measured
≥ 53 min in-OS vs 1.38 s on the host (see §4).
Typical failure: a ported tool's runtime gap (linker not finding a symbol,
libc missing a syscall the C++ runtime wants) → L2's audit items are the
first places to look; the FAIL-token scan catches the compiler's own errors
textually.

### S2 — the kernel, in-OS

In-OS command:

```text
make -f Makefile.hobbyos kernel
```

Compiles all **59** kernel TUs on the merged tip (58 C + 1 asm `fpu.s`;
the L4-era 71-TU count shrank with the source tree — re-grep at execution
time, §8.4), links `hobbyos.elf` with lld, `llvm-objcopy -O binary` →
`hobbyos.bin`. Expected: 59 object lines, link line with no errors,
`[SELFHOST] phase=s2-kernel-done`. `obj/` and the `.elf`/`.bin` sizes are
recorded into the log (evidence). Timeout: **10800 s (3 h)** — in-OS the
kernel took ~90–150 min on the merged tip (host serial 5.8 s; in-OS per-TU
1–15 min, §4).
Typical failure: link OOM (guest RAM is 16 GB, should be ample; check
`---` alloc dumps), `ld.lld: error: undefined symbol` (a TU failed silently —
compile issue), or disk full mid-build (`/SRC/obj` growth).

### S3 — assemble OUT.IMG in-OS

In-OS command:

```text
make -f Makefile.hobbyos disk
```

Uses the in-OS `MKFSFAT` + `MCOPY` (L4 ports) to assemble `/SRC/OUT.IMG`
(FAT16, size from §3): BPB + FATs + root dir created by MKFSFAT, then
`/boot/limine.conf` (template below), `/boot/hobbyos.bin` (the S2 output),
`/EFI/BOOT/BOOTAA64.EFI`, and the S1 user set. No `dd` (see L4's dd-replacement
analysis — `ftruncate`-only is the recommendation for ≤256 MiB). Expected:
`[SELFHOST] phase=s3-disk-done`. Timeout: **1800 s** — measured 365 s total
in-OS (MKFSFAT of the 128 MiB image ≈ 284 s alone, §4). Typical failure: MCOPY
write path bug (LFN at depth — L3 owns), disk full (OUT.IMG over the §3
budget), MKFSFAT geometry mismatch (BPB the kernel parser rejects — boot test
S4 catches it).

### S4 — host: extract OUT.IMG, boot it, assert kernel marker

Host (verify loop):

```text
python3 fat16img.py get <boot-disk> OUT.IMG > out.img      # from repo root
# then boot out.img (same QEMU line as §5.1, -drive file=out.img)
```

Boot `out.img` headless with the exact ARM args (§5.1). Assert in the serial
capture: `Booting AArch64 OS` (kernel marker, main.c:354) and the kernel's
SELFHOST phase marker. This proves the in-OS-built kernel boots. Timeout:
120 s (measured boot ≈ 9 s, §4). Typical failure: limine.conf path wrong
(`boot():/boot/hobbyos.bin`), BOOTAA64.EFI missing from OUT.IMG, or the
kernel itself panics early — serial tail shows where.

### S5 — host: boot OUT.IMG and run an in-OS-built program

Second boot of `out.img`; after the kernel marker, run a program from the S1
user set. Primary drive: the `SELFHOST.BIN` boot driver (§5.3) auto-runs it
with stdout on the serial-console fd (JSC/WEBPROC precedent) — assert its
marker line `[SELFHOST] hello-run` on serial. Fallback (no kernel change):
QMP `input-send-event` keystrokes into the console/shell (run_console_test.py
precedent) typing `HELLO` + Enter, asserting the program's output — note the
keystroke path's output lands on the framebuffer, so the fallback's verdict
is the driver's serial relay or the file-based check in §5.4. Timeout: 180 s.
Typical failure: the program isn't on OUT.IMG (S3 recipe bug), filename
bounds (8.3 policy F8), or load-path gaps (loader can't resolve the long
`.bin` name — L3).

### Retry-cost table — in-OS make is NON-incremental (w6/w7 finding)

**In-OS `make` never skips a TU**: the in-OS read-only FAT driver exposes
8.3-clean UPPERCASE SFNs while GNU make's pattern rules do a case-sensitive
dir-cache compare, so `make`'s up-to-date check (and its pattern-rule match)
breaks — the kernel recipe is therefore written as 59 **explicit** rules and
every `make <stage>` re-runs everything (compile + link) from scratch. The
retry-cost table below is therefore NOT "recompiles only changed TUs"; a
re-run of a stage costs the same as the first run (minus the boot):

| stage | re-run cost | notes |
|---|---|---|
| S0 | ~1 min (mostly QEMU boot+teardown) | one TU; no disk rebuild |
| S1 | ~1–2 h + boot | **full** 5 TU rebuild re-observed (make non-incremental in-OS) |
| S2 | ~1.5–2.5 h + boot | **full** 59-TU rebuild re-observed |
| S3 | ~10–30 min + boot | rebuilds OUT.IMG from existing objs — the one stage that IS cheap to repeat |
| S4 | ~1–2 min | reuses `out.img` already extracted — cheapest repeat |
| S5 | ~1–3 min | reuses `out.img` |

Consequence: **one long single boot per stage-set** (S0…S3 back-to-back in
one `SELFHOST.RUN`) is the only sane plan; iterating on S1 or S2 is a
multi-hour commitment. Never rebuild the boot disk to iterate on S4/S5 —
extract `OUT.IMG` again with `fat16img.py get`.

---

## 2. Payload & staging (measured today)

`tools/selfhost-stage.sh` (this lane's first cut, tested — transcript in
`l5-stage-test-a.md`) stages frozen interface F2 into `/SRC` on the boot disk:

| payload item | files | bytes | staged by |
|---|---|---|---|
| `src/**` (real tree, AD-11) | 613 | 22,330,467 (21.3 MiB) | `mcopy -s dir ::/SRC/./` |
| `tools/**` | 26 | 385,595 (0.37 MiB) | same |
| `bootloader/BOOTAA64.EFI` | 1 | 315,392 (0.30 MiB) | `mcopy` |
| `limine.conf` (template, see §2.2) | 1 | 99 | `mcopy` from stdin |
| `Makefile.hobbyos` | optional | — | staged with a WARNING when absent (absent today) |
| libcxx include subset (C++ set only) | 1681 | 12,286,278 (11.7 MiB) | deep path via `mmkdir_p` + `mcopy -s` |
| **total** | **2322** | **30,108,339 (28.7 MiB)** | verified `mdir` + content hashes |

- **libcxx: AD-9 freeze (w4-src, wave-4) — staged OFF by default
  (`STAGE_LIBCXX=0`).** Evidence: every TU on the S0..S3 in-OS path is C —
  check/kernel are C+asm, the acceptance user set (HELLO SH LS CAT) links
  the carried libc.a, and DESKTOP (required for S5's desktop path) is C
  (src/user/desktop.c, USER_CFLAGS).  Only CXX_LIBCXX_USER_FLAGS programs
  outside the acceptance set (browser/ICU/libcxx tiers) reference the
  libcxx include tree.  Re-enable with `STAGE_LIBCXX=1` when a C++ TU
  enters the in-OS user set; both staging modes then carry the subset at
  the frozen path (11.7 MiB / 1681 files).

- **8.3-excess: 1362 of 2322 staged basenames exceed FAT 8.3** (informational
  for F8). LFN write is proven by the test (arrival of `program_loader.c`,
  `__config`, … verified by `mtype`+sha256); in-OS *read* is lane L3's gate.
- Two workstation-tree entries are absent from lane trees (correctly):
  `src/user/actual_qemu.ppm` (untracked screenshot byproduct — must not be
  staged) and `src/user_include/graphics/theme.h` (tracked on the
  workstation's newer main tip — wave-seed skew; parent owns the merge).

### 2.2 limine.conf template (verbatim Makefile arm branch, :1763–1768)

```text
timeout: 0
default_entry: 1

/HobbyOS (ARM AArch64)
protocol: linux
path: boot():/boot/hobbyos.bin
```

---

## 3. Sizing math (measured on the workstation tree, 2026-10-14)

### Disk (boot/staging disk, the 1 GiB `disk.img`)

- 174 files, 133,032,359 B used, **938,786,816 B free** (~895 MiB; re-measured
  today — the brief's 939,114,496 B predates a concurrent desktop VM's temp
  writes; planning number = 895 MiB).
- Staging 30.1 MiB of payload consumed ~58 MiB of image (32 KiB clusters
  → ~2× slack on small files — measured, §1.3 of the timing evidence).

### Payload + build footprint budget (in-OS, on the boot disk)

| item | size | status |
|---|---|---|
| `/TOOLS` toolchain (clang+lld+ar+objcopy, stripped — L1 measures exact) | ~150–250 MiB (est., VERIFY L1) | on boot disk |
| `/SRC` staged payload | 30.1 MiB bytes / ~58 MiB on disk | fixed |
| `/SRC/obj` in-OS build outputs (objects, elf, bin) | ~30–60 MiB (est., VERIFY M2) | transient |
| build logs (`/SRC/*.log`) | <1 MiB | transient |
| **OUT.IMG** | §3.1 | final artifact |
| total worst case | ~150+250+58+60+256 ≈ 574 MiB | ≪ 895 MiB free ✓ |

### 3.1 OUT.IMG size decision (F4: ≤256 MiB)

OUT.IMG content = `hobbyos.bin` (0.20 MiB today; expect ≤1 MiB) +
`limine.conf` + `BOOTAA64.EFI` (0.30 MiB) + user set (~1–2 MiB) ≈ **3–4 MiB
of file bytes**. Cluster math (mkfs.fat -F 16 auto-pick; FAT16 caps at 65525
clusters):

| OUT.IMG | clusters | FAT slack on ~4 MiB content | fits in 895 MiB free? |
|---|---|---|---|
| 64 MiB | 2 sectors (1 KiB) | ~1.1× | yes (64+58+250+60 ≈ 432 MiB) ✓ |
| 128 MiB | 4 sectors (2 KiB) | ~1.2× | yes (128+58+250+60 ≈ 496 MiB) ✓ |
| 256 MiB | 8 sectors (4 KiB) | ~1.4× | yes (256+58+250+60 ≈ 624 MiB) ✓ |

**Proposal: OUT.IMG = 128 MiB FAT16.** 64 MiB leaves no headroom if the kernel
or toolchain products grow (R3: binaries "tens of MB" — the loader already
handles 30 MB images), while 256 MiB buys nothing the acceptance needs and
eats margin. 128 MiB keeps ~2 KiB clusters (low slack), fits any growth path,
and even a worst-case full disk stays ~380 MiB under the 895 MiB ceiling.
Freeze the number in `Makefile.hobbyos` (`MKFSFAT -F 16` sizing hook) at M3.

---

## 4. Budget guidance (measured TCG reality, w6/w7)

All in-OS times below are **measured** on the merged tip (ARM aarch64 +
MTTCG on an x86-64 host, 8 vCPU / 16 GiB guest) — the L4-era "8×" placeholder
was off by ~2 orders of magnitude.

| term | host clang (aarch64) | in-OS (TCG) | multiplier |
|---|---|---|---|
| `src/user/sh.c` -O2 | 1.38 s | **≥ 53 min** (S1 dominant term) | ~900–1500× |
| whole 59-TU kernel | 5.8 s serial | **~90–150 min** (per-TU 1–15 min) | ~900–1500× |
| one kernel TU | 0.06–0.59 s | 1–15 min | ~900–1500× |
| MKFSFAT 128 MiB | — | **≈ 284 s** (w4-kernel measured) | TCG disk-write bound |
| S3 disk stage (all of it) | — | **365 s** | — |
| boot to kernel marker | — | **6.5–9 s** | — |

In-OS clang's per-TU cost is dominated by the driver + cc1 process spawn
(R3) plus TCG emulation of the compiler's own codegen; a whole acceptance
loop S0→S3 is a **~3–4 h single-boot proposition** (S1 ≈ 1 h, S2 ≈ 1.5–2.5 h,
S3 ≈ 6–10 min). Plan the harness around ONE long boot per stage-set, big
timeouts (§1), and **bounded polling every 5–10 min** — never a tight loop.

The CI VM `hobytest` (192.168.0.20, 64 cores) is the right place for
long/parallel tiers; on a busy workstation (concurrent VMs) expect the boots
and stages to stretch proportionally.

- **Boot:** ≈ 6.5–9 s to kernel marker under MTTCG; cheap, never optimize.
- **S1:** dominated by the `sh.c` 53-min compile; the other 4 user TUs and
  the 4 links/objcopies are minutes. Budget 1–2 h.
- **S2:** 59 TUs × ~1–15 min. Budget 3 h.
- **S3:** MKFSFAT alone ≈ 284 s on 128 MiB; full stage 365 s. Budget 30 min.
- **CI runner:** `~/bin/hobbyos-ci.sh` **exists on hobytest** (verified this
  wave; marker-gated per tier, fresh-copy-per-label pattern, `all` tier).
  The workstation `~/bin` is empty (the script lives on the VM, not here).
  Replacement spec if it ever disappears: rsync the tree (exclude
  `obj/ disk.img *.log qmp-sock hobbyos.elf`) → per-run fresh copy → run
  tier with `timeout` → verdict = completion marker substring + zero FAIL
  tokens (rc ignored) → keep logs per label.

---

## 5. Host verify-loop spec — `run_selfhost_build_test.py` (wave-2)

### 5.1 Boot line (exact)

From Makefile:197 (ARM branch), headless, serial to file, QMP socket for the
keystroke path:

```python
QEMU = "qemu-system-aarch64"
EDK2_AARCH64 = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")  # Makefile EDK2_AARCH64
QEMU_CMD = [
    QEMU, "-M", "virt", "-cpu", "cortex-a53", "-smp", "8", "-m", "16384M",
    "-accel", "tcg,thread=multi", "-bios", EDK2_AARCH64,
    "-display", "none",
    "-serial", "file:SERIAL.log",
    "-drive", f"if=none,file={IMG},format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-device", "virtio-gpu-device",          # headless tier = soft GPU (Makefile:309)
    "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
    "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56",
    "-semihosting", "-action", "shutdown=poweroff",
]  # + ["-qmp", "unix:./qmp-selfhost,server,nowait"] for the keystroke path
```

Firmware file: exists on this host at `~/.local/share/AAVMF/AAVMF_CODE.fd`
(verified). S4/S5 differ only in `-drive` file (extracted `out.img`).

### 5.2 Drive decision (why)

The desktop/`make test` python runners drive the guest via **QMP
`input-send-event`** (run_console_test.py, run_desktop_test.py — proven with
`-display none`). There is **no AUTORUN/startup-script mechanism in the shell
today** (verified: no autorun/rc reading in `sh.c` or the kernel). Therefore:

- **Primary (recommended):** a small in-OS `SELFHOST.BIN` driver launched by a
  dedicated boot mode (`MODE=selfhost`, kernel change owned by the wave-2
  harness lane / parent), running the S0–S3 phases and relaying their
  output to the **serial-console fd** — the exact console-fd fallback the
  JSC/WEBPROC modes already use (fd1/2 unset → serial, main.c:481-501). This
  puts every `[SELFHOST]` marker on the serial capture with zero keystroke
  fragility and no framebuffer-only output. Documented precedent in-tree.
- **Fallback (no kernel change, today's kernel):** QMP keystrokes into the
  desktop console/shell (virtio-keyboard → desktop → console.c stdin → sh),
  char-by-char with an evdev-qcode map. Framebuffer-only output is the
  consequence; evidence then comes from the file-based checks (§5.4). Choose
  the fallback only if the primary LANE falls behind — it is measurably the
  weaker of the two.

### 5.3 Skeleton (comment block that ships to the wave-2 lane)

```python
#!/usr/bin/env python3
# run_selfhost_build_test.py — M4 acceptance driver (wave-2, from lane L5's spec).
# Drive: boot the staged disk (QEMU line §5.1), run S0..S5.
# Primary driver: MODE=selfhost launches SELFHOST.BIN whose stdout is the
# serial-console fd (JSC/WEBPROC fallback), so markers land in the serial file.
# Fallback driver: QMP input-send-event keystrokes (run_console_test.py pattern).
# Verdicts: marker substring + zero FAIL/FAILED tokens (§8.4) — NEVER exit codes.
# Steps:
#  1. tools/selfhost-stage.sh -f disk.img            (already proven this wave)
#  2. boot qemu(disk.img) -> SERIAL-s0.log           (build driver runs S0-S3)
#  3. wait for "[SELFHOST] phase=s3-disk-done" or timeout (build budget §4)
#  4. python3 fat16img.py get disk.img OUT.IMG > out.img   # read-only extract
#  5. boot qemu(out.img) -> SERIAL-s4.log            (S4: assert Booting AArch64 OS)
#  6. boot qemu(out.img) -> SERIAL-s5.log            (S5: run HELLO via driver; assert marker)
# 7. print verdict table; exit 0 only if all phases done + zero FAIL tokens.
```

### 5.4 Evidence recovery on the fallback path

Extract the in-OS build log written by the driver (`/SRC/OUT.LOG` style) after
shutdown with `python3 fat16img.py get disk.img SRC/OUT.LOG` (the read-only
extractor handles deep paths via its 8.3/LFN resolution). Raw giant logs stay
uncommitted; the driver logs only lines that carry markers or start with a
compile/link line.

---

## 6. Failure-diagnostics plan

Per-stage serial captures: `SERIAL-s0.log … SERIAL-s5.log` (final verdict table
lists each with its marker row). First checks by stage:

| stage | first things to check |
|---|---|
| S0 | driver error text (`clang: error:`), RESDIR header set (L1), `/TOOLS` presence |
| S1 | `undefined symbol` (L2 audit items), C++ runtime harness (cxxrt), disk-full guard |
| S2 | link OOM (`ld.lld` alloc dump), silent missing TU (count 71 object lines!), `hobbyos.bin` size |
| S3 | MCOPY LFN write (L3), OUT.IMG size vs §3.1, MKFSFAT BPB fields |
| S4 | limine.conf / EFI presence in OUT.IMG, early kernel panic tail |
| S5 | user `.bin` presence, 8.3 naming (F8), loader paths (L3) |

Marker vocabulary is grep-only: `[SELFHOST] phase=`, `Booting AArch64 OS`,
`Mode: DESKTOP`, `System halt.`, `FAIL`/`FAILED`. The single-writer sanity
rule from §8.4 applies: re-grep at execution time; this doc is a snapshot.

---

## 7. Open questions for M3 / harness wave

1. ~~Freeze AD-9 (LIBLLM-hello set incl. C++? → flips STAGE_LIBCXX default).~~
   **DECIDED (w4-src, wave-4): the acceptance user set is C-only (HELLO SH
   LS CAT + DESKTOP in C); STAGE_LIBCXX defaults OFF.** See §2 libcxx row.
2. `MODE=selfhost` + `SELFHOST.BIN` ownership (harness lane vs kernel lane).
3. mkfs.fat port must byte-match enough (BPB+FATs+root) for Limine/AAVMF —
   L4 records what "enough" means.
4. `dd` replacement: `ftruncate`-only (recommended) vs zero-fill.
5. Exact `hobbyos.bin` size after the in-OS build (feeds §3.1 freeze).

---

## 8. S4/S5 pre-test evidence (lane w3-accept, 2026-10-14)

Both host stages were pre-tested out-of-order with a hand-built OUT.IMG
*before* the in-OS build can succeed (still gated on MAKE.BIN / the w2-mkfs
extend-then-write kernel gap), using the L5-proven mtools path (`truncate`
+ `mformat` + `mcopy`; mkfs.fat/dd are command-blocked for the lane).

### 8.1 Hand-built OUT.IMG (what was proven bootable)

128 MiB FAT16, created with `truncate -s 134217728 img && mformat -i img ::`
— **mformat's `-F` means "FAT32", never use it for OUT.IMG** (the w2-harness
1 GiB recipe is `truncate` + plain `mformat`). Verified BPB: bps=512,
spc=4, nfats=2, root=512 entries, fatsize=256, total=262144 sectors — the
same geometry dosfstools picks at the frozen 128 MiB size (`w2-mkfs-report`
"128 MiB → spc4"; DISK_BLOCKS=131072 is 1024-byte units).

Layout that actually boots (QEMU §5.1 line, AAVMF + Limine, verified):

```text
/EFI/BOOT/BOOTAA64.EFI      (the firmware's PE loader — bootloader/BOOTAA64.EFI)
/boot/limine.conf           (verbatim ARM template: path boot():/boot/hobbyos.bin)
/boot/hobbyos.bin           (kernel flat binary, llvm-objcopy of hobbyos.elf)
/DESKTOP.BIN                REQUIRED for S5's Apps-menu path — the kernel's
                            DESKTOP mode parallel-loads DESKTOP.BIN from the
                            boot disk root at start (main.c); without it no
                            desktop / Apps menu ever appears (w3 finding)
/CONSOLE.BIN, /SH.BIN       the desktop's shell stack (w2 desktop-disk set)
/OUTPROG.BIN                the S1 user-set program to launch (8.3 root name)
/OUTMARK.TXT                marker file for host file-based checks (§5.4)
```

A bare kernel-less OUT.IMG (`OUTPROG.BIN` + `OUTMARK.TXT` only) does NOT
boot — correct, and the runner classifies it `stage_failure` ("S4 failed:
OUT.IMG did not reach the kernel marker").

### 8.2 Runner evidence (tools/run_selfhost_build_test.py, runs A/B)

| run | OUT.IMG on boot disk | runner result (exit via --expected-class stage_failure) |
|---|---|---|
| A | bootable (§8.1) | driver clean stage_failure at s0 (MAKE.BIN missing — honest); **S4: extracted 134,217,728 B, sha256 MATCH hand master, OUT.IMG boot marker ok 6.5 s**; **S5: OUT.IMG booted, `[MENU] 5=OUTPROG.BIN`, `[LAUNCH] OUTPROG.BIN`, `[KERNEL] Process 3: OUTPROG.BIN exited` on serial**; exit 0 |
| B | kernel-less | class `stage_failure` "S4 failed: OUT.IMG did not reach the kernel marker", S5 blocked (same image); exit 0 |

S4 byte-identity: `out-selfhost.img` (extracted by the runner via fat16img)
sha256 == hand-built master sha256 (both runs); host re-extraction of
`OUTPROG.BIN` from the runner's file == `obj/arm/hello.bin` sha256.
Runner changes: S4 records extracted sha256 + boots the image; S5 boots it
with QMP and launches `--s5-program` (default OUTPROG.BIN) from the Apps
menu; S4/S5 only override the verdict when they themselves fail;
`--s5-program` / `--s5-wait`. fat16img read_file made linear (~1 s for the
128 MiB extract; the old bytes-concat was O(n²) and hung >120 s).
Classifier self-test still 7/7. Raw serials (uncommitted):
`~/.hermes/cache/scratch/w3-accept/run{A,B}-serial.log(.s4/.s5)`.

### 8.3 What S5's keystroke path can and cannot see today

Desktop-launched programs render stdout to their window (framebuffer), not
serial — HELLO's `[HELLO]` lines do not reach the capture. S5 therefore
proves the program-side via the loader lines (`[LAUNCH] OUTPROG.BIN`,
`[KERNEL] Process ... exited`) plus host file checks, exactly the §5.4 /
§5.3-fallback framing. The program's own marker on serial needs
`MODE=selfhost` + SELFHOST.BIN's serial-console fd (kernel change, §5.2
primary drive) — that is the one remaining kernel-side piece for a *full*
S5 marker assertion.

### 8.4 Remaining harness/build work for the real acceptance run

1. **In-OS toolchain** (`MAKE.BIN` + clang in `/TOOLS`): makes S0–S3 run for
   real; blocked first by the w2-mkfs kernel gap (MKFSFAT `-C` content
   writes into ftruncate-extended files do not persist — extend-then-write).
2. **S3 recipe (Makefile.hobbyos disk)** must produce exactly the §8.1
   layout — including **DESKTOP.BIN + SH.BIN + CONSOLE.BIN on OUT.IMG**.
   Consequence for AD-9: DESKTOP is not "optional if it links cheaply" — it
   is **required** for S5's desktop-path boot (w3 finding; alternative is
   the MODE=selfhost driver which needs no desktop, item 3).
3. **`MODE=selfhost` boot mode** (kernel lane): launcher + serial-console-fd
   stdout so SELFHOST.BIN and the S5 program emit markers on serial;
   removes the keystroke fragility and the framebuffer-only limitation.
4. After S3: runner already extracts + boots + launches automatically (no
   new runner work for the real run); final acceptance expects
   `s4.boot_ok=true` and `s5.launch_line=true` (then `success` once the
   driver completes, `ACCEPTANCE-COMPLETE`, zero FAIL tokens).
5. Decide/trim: OUT.IMG content set (S1 user set incl. DESKTOP per item 2),
   and whether CONSOLE.BIN is strictly required or only DESKTOP.BIN + SH.BIN
   (not isolated in this pre-test).

---

## 9. Runbook — MODE=selfhost full acceptance (merged tip, w7)

The proven field recipe for the complete in-OS build (T4.3). Everything
below landed as a single long boot per stage-set; the S1/S2 boots are
3+ hours of QEMU wall time on a bare workstation.

### 9.1 Environment (lane worktree; never the main tree's disk.img)

```bash
# one QEMU per tree at a time; never `make clean`; the main tree may run
# its own VM concurrently — budget for the CPU split.
make MODE=selfhost disk.img   # host-side: rebuilds ALL .bin deps + stages
                              #   /TOOLS (from obj/selfhost/dist-staging) +
                              #   /SRC (src subset + Makefile.hobbyos) on
                              #   a fresh 1 GiB FAT16 disk.img (w6 pattern)
```

`obj/selfhost/dist-staging` (the wave-2 LLVM distribution, flat-converted by
`tools/selfhost-stage.sh --tools`) is the frozen `/TOOLS` source. Missing
staging warns + skips, so a fresh tree must re-run the dist install first.

### 9.2 Stage scripts — one drive file per boot

`/SRC/SELFHOST.RUN` is the driver's script (`<phase>|<tool>|<args>` lines).
The w6+w7 full loop uses stages of the form `sX|SH.BIN|/SRC/SELFHOST.SH <tgt>`
— `SELFHOST.SH` does `cd /SRC && /TOOLS/MAKE.BIN -f Makefile.hobbyos <tgt>`.
w7's combined script (staged over `::/SRC/SELFHOST.RUN` with `mcopy -o`):

```text
s0-check|SH.BIN|/SRC/SELFHOST.SH check
s1-user|SH.BIN|/SRC/SELFHOST.SH user
t25-run|SH.BIN|/SRC/T25.RUN          # OPTIONAL: in-OS run of the built
                                     # HELLO.BIN -> [HELLO] entering main on serial
s2-kernel|SH.BIN|/SRC/SELFHOST.SH kernel
s3-disk|SH.BIN|/SRC/SELFHOST.SH disk
```

`T25.RUN` = `cd /SRC/obj/arm` + `./HELLO.BIN` (the cwd note matters: HobbyOS
processes start with cwd=/ and the 8.3 name only resolves there).

### 9.3 Drive + poll (no tight loops)

```bash
# MODE=selfhost boots ONLY SELFHOST.BIN with argv[1]=/SRC/SELFHOST.RUN and
# relays its stdout + [SELFHOST] markers to the SERIAL console (w4-kernel).
python3 tools/w5_flat_boot.py --image disk.img --serial SERIAL-boot.log \
    --timeout 14400 --expect ACCEPTANCE-COMPLETE
# poll: bounded every 5-10 min (grep for phase=* -done / -fail in the serial;
# 3-4 h total). NO tight loops.
```

w5_flat_boot exit: 0 = expect-marker seen, 1 = fail-marker / phase=*-fail,
2 = timeout, 3 = harness error. The full capture is the §8.4 raw-serial
evidence; the driver prints `System halt.` (kernel) after `ACCEPTANCE-COMPLETE`.

### 9.4 Pull the artifacts

```bash
python3 fat16img.py list disk.img           # OUT.IMG sits at ::/OUT.IMG
python3 fat16img.py get disk.img OUT.IMG > out-loop.img   # ~1 s (linearized reader)
sha256sum out-loop.img obj/selfhost/w6-out/OUT.IMG        # compare vs w6
# per-file hashes: fat16img.py get disk.img SRC/obj/arm/HELLO.BIN, ... 
```

In-OS make is non-incremental, so the fresh OUT.IMG is a fresh 128 MiB FAT16
image: BPB geometry, layout classes and file BYTES (HELLO.BIN/OUTPROG.BIN
sha) match w6's — the image sha itself legitimately differs (fresh mkfs +
fresh copy order). Compare content classes, not raw image hashes.

### 9.5 S4/S5 — boot the extracted OUT.IMG

```bash
python3 tools/w7_accept_boot.py --image out-loop.img --serial S4-serial.log \
    --launch OUTPROG.BIN
```

Asserts `Booting AArch64 OS`, `Mode: DESKTOP`, `[MENU] …=OUTPROG.BIN`,
`[LAUNCH] OUTPROG.BIN`, `[KERNEL] Process … exited`. (A desktop boot shows
loader lines on serial; the program's own `[HELLO]` marker needs the
MODE=selfhost serial-console fd — see §8.3 — so S5's program-side serial
evidence is the loader lines + host file hashes.)

---

## 10. Verdict table vs §8.3 (filled by w7-accept on the merged tip 288995c)

Frozen criteria: `docs/selfhost/lanes/l5/selfhost.md` §8.3 checklist;
evidence standard §8.4 (raw serial markers, `System halt` / ACCEPTANCE-COMPLETE,
zero FAIL tokens, sizes + hashes for every artifact).

| §8.3 criterion | verdict | evidence |
|---|---|---|
| In-OS: kernel objects all compiled, `hobbyos.elf` linked, `hobbyos.bin` objcopy'd — transcript shows each phase | **PASS** (w6 S2, re-driven in w7 T4.3) | `[SELFHOST] phase=s2-kernel-done rc=0`, 59 object lines, lld link via response file, `hobbyos.elf` 1372912 B / `hobbyos.bin` 219564 B (ARM64 boot Image), raw serials `evidence-w6-run/s2.serial.txt` + w7 serial |
| In-OS: `OUT.IMG` assembled by the in-OS toolchain; host boots it successfully | **PASS** (w6 S3 + w7 boot of extracted image) | `phase=s3-disk-done rc=0` (365 s; MKFSFAT ≈ 284 s); extracted 128 MiB FAT16 (bps512/spc4/nfats2/root512/fatsz256), boot marker `Booting AArch64 OS` in 6.5–9 s |
| The booted OUT.IMG kernel was linked in-OS from in-OS-compiled objects; an in-OS-built user program runs on it | **PASS**(A) | OUT.IMG `[LAUNCH] OUTPROG.BIN` (37360 B), `[KERNEL] Process 3: OUTPROG.BIN exited`; OUTPROG.BIN sha == in-OS HELLO.BIN sha == host hello.bin (byte-identical, w6) |
| Full raw logs + artifact hashes archived (evidence dir + §11 records) | **PASS** | `evidence-w6-run/` + `evidence-w7/` (raw serials + pulled artifacts + sha256s); this doc + lane reports |
| x64: stretch only (M5) — not required for v1 done | n/a (not attempted) | — |

`[SELFHOST] ACCEPTANCE-COMPLETE` is the driver's completion marker
(acceptance.md §1); `System halt.` is the kernel's own close (§8.4 of
`selfhost.md`), both present in the w6 serials. Full final row (B: the
complete T4.3 loop S0→S3 on a fresh disk.img) fills in from
`docs/selfhost/lanes/w7-accept-report.json` + `w7-accept-evidence.md`.
