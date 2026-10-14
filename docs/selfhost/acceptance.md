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
the compile line + `[SELFHOST] phase=s0-check-done`. Timeout: 300 s wall.
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
compile line, link lines, `[SELFHOST] phase=s1-user-done`. Timeout: 900 s.
Typical failure: a ported tool's runtime gap (linker not finding a symbol,
libc missing a syscall the C++ runtime wants) → L2's audit items are the
first places to look; the FAIL-token scan catches the compiler's own errors
textually.

### S2 — the kernel, in-OS

In-OS command:

```text
make -f Makefile.hobbyos kernel
```

Compiles all 71 kernel TUs (66 C + 5 asm; measured: 18–28 s of host clang
serial — §4), links `hobbyos.elf` with lld, `llvm-objcopy -O binary` →
`hobbyos.bin`. Expected: 71 object lines, link line with no errors,
`[SELFHOST] phase=s2-kernel-done`. `obj/` and the `.elf`/`.bin` sizes are
recorded into the log (evidence). Timeout: 3600 s (TCG — see §4).
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
`[SELFHOST] phase=s3-disk-done`. Timeout: 600 s. Typical failure: MCOPY
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

### Retry-cost table (what each stage costs to re-run)

| stage | re-run cost | notes |
|---|---|---|
| S0 | ~1 min (mostly QEMU boot+teardown) | no disk rebuild |
| S1 | ~2–5 min + boot | reuses `/SRC/obj`; make recompiles only changed TUs |
| S2 | ~5–15 min + boot | reuses `/SRC/obj`; failed TUs recompile only |
| S3 | ~2–5 min + boot | rebuilds OUT.IMG from existing objs — cheap |
| S4 | ~1–2 min | reuses `out.img` already extracted — cheapest repeat |
| S5 | ~1–3 min | reuses `out.img` |

Re-running is cheap after S2: only a failed stage and the boot around it.
Never rebuild the boot disk to iterate on S4/S5 — extract `OUT.IMG` again
with `fat16img.py get`.

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

- **libcxx recommendation: stage by default, skippable (`STAGE_LIBCXX=0`).**
  Cost 11.7 MiB (×2 FAT slack ≈ 24 MiB on the 1 GiB disk). Rationale: the C++
  acceptance set is provisional (AD-9 "if it links cheaply"); including the
  headers keeps the `user` target's C++ path runnable without a re-stage.
  If the M3 freeze drops all C++ targets, flip the default off.
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

## 4. Budget guidance (TCG reality)

- **Boot:** measured ≈ 5–10 s to kernel + user markers under MTTCG on this
  host (2 boots/run ≈ 10–20 s). Cheap — never optimize here.
- **Kernel compile (the dominant term):** host-clang time for one kernel TU
  measured 0.06–0.59 s (main/program_loader/fat16); serial whole-kernel ≈
  18–28 s on the host. In-OS under TCG: **multiplier VERIFY until M2** — the
  documented x64-TCG experience (5–10×, run-tests skill) is the placeholder,
  and in-OS clang adds per-TU process-spawn cost (driver+cc1 per TU, R3).
  Working number for the harness: 71 TUs × 0.3 s × 8× (VERIFY) ≈ 3 min single
  thread; budget generously.
- **Wall-clock guidance:** workstation full acceptance ≈ **15–25 min
  dominated by S2**; run long/parallel tiers on the CI VM `hobytest`
  (192.168.0.20, 64 cores — reachable, verified this wave).
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

1. Freeze AD-9 (LIBLLM-hello set incl. C++? → flips STAGE_LIBCXX default).
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
