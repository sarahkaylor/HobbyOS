# In-OS kernel build: `build_kernel.sh` + `SRC.TAR` (inos-build-kernel)

Status: implemented + **verified 2026-10-15** (full in-OS loop passes; receipts in
`docs/selfhost/lanes/evidence-inos-bk/`).  This is the user-facing self-hosting loop:
boot HobbyOS from its own disk image and rebuild the kernel **inside
HobbyOS** from a source tarball carried on that same disk — no host
toolchain involved in the in-OS steps.

## What the disk image carries

The startup disk (`disk.img`, built by the root `Makefile`) has always
booted from the disk itself:

    AAVMF firmware -> /EFI/BOOT/BOOTAA64.EFI (Limine)
                   -> /boot/limine.conf
                   -> /boot/hobbyos.bin        (the kernel flat image)

i.e. the bootloader and the kernel already live on the FAT16 image; QEMU
boots it with `-bios AAVMF` + `-drive disk.img`, never `-kernel`.

`inos-build-kernel` adds three things to that image:

| path | what | produced by |
|---|---|---|
| `/SRC.TAR` | ustar tarball with the whole source payload (see below) | `tools/selfhost-src-tar.sh` |
| `/BUILD_KERNEL.SH` | the in-OS driver script | staged from `src/user/selfhost/build_kernel.sh` |
| `/TOOLS/TAR.BIN` | the in-OS tarball extractor (`src/user/tar.c`) | host build, staged by `tools/selfhost-stage.sh --tools` |

### SRC.TAR contents

Exactly the paths the in-OS build expects after extraction into `/SRC`
(frozen F2 flavor, `selfhost.md` §7.3):

    src/**                    the real source tree (real names, AD-11)
    tools/**                  repo tools (small; useful in-OS)
    Makefile.hobbyos          the in-OS build driver (F3 targets)
    linker.ld                 kernel linker script (repo root)
    bootloader/BOOTAA64.EFI   bootloader for the in-OS-assembled OUT.IMG
    limine.conf               = bootloader/limine-arm.conf
    bin/{DESKTOP,CONSOLE,SH}.BIN + OUTMARK.TXT
                              carried host build products (§8.1 layout)
    obj/arm/libc.a            carried libc archive (the S1 link)
    build_kernel.sh           the in-OS build script

The tarball is created with `tar --format=ustar --hard-dereference`
(no hardlink entries) and self-checked by re-reading it with the host tar
+ byte-comparing a sample (a broken tarball is never staged silently).

## Using it inside HobbyOS

At the desktop shell or any console — preferred form first (the shell
executes `.sh` files directly; this is the shebang equivalent):

    ./build_kernel.sh              # all: extract-if-needed + make all + install
    ./build_kernel.sh untar        # just re-extract /SRC.TAR into /SRC
    ./build_kernel.sh check        # one-TU compile smoke
    ./build_kernel.sh user         # HELLO SH LS CAT
    ./build_kernel.sh kernel       # all kernel TUs -> /SRC/hobbyos.elf + .bin
    ./build_kernel.sh disk         # assemble the bootable /SRC/OUT.IMG
    ./build_kernel.sh install      # /SRC/hobbyos.bin -> /boot/hobbyos.bin
    SH /BUILD_KERNEL.SH all        # equivalent, explicit
    SH.BIN /BUILD_KERNEL.SH all    # also resolves: bare names try the name
                                   # as typed (cwd, /) before appending .BIN

In a staged `SELFHOST.RUN` (the driver context) the stage line is:

    <phase>|SH.BIN|/BUILD_KERNEL.SH <mode>

**Toolchain preflight:** the script checks `/TOOLS/{CLANG,LLD,OBJCOPY,MAKE,
TAR}.BIN` up front and exits 2 with instructions when they are absent —
which happens when `disk.img` was built without the LLVM dist staging
(`obj/selfhost/dist-staging`; `docs/selfhost/llvm-bootstrap.md` §6 — the
`selfhost-stage.sh --tools` step then warns and skips).  Stage onto an
existing image with `tools/selfhost-stage.sh --tools disk.img`, or rebuild
`disk.img` where the staging exists.

The pieces the script drives (all in-OS):

    /TOOLS/TAR.BIN xf /SRC.TAR /SRC          # extract the source payload
    cd /SRC
    /TOOLS/MAKE.BIN -f Makefile.hobbyos all  # check+user+kernel+disk
    /CP.BIN /SRC/hobbyos.bin /boot/hobbyos.bin

`install` (also the last step of the default mode) makes the *next boot of
the same disk* run the freshly built kernel: the previous `/boot/hobbyos.bin`
is kept at `/boot/hobbyos.old`, and the limine menu's second entry
("previous kernel") boots it — the recovery path if an in-OS-built kernel
ever misbehaves.

## Bootability semantics

- `make disk` (S3) assembles `/SRC/OUT.IMG` — a standalone 128 MiB FAT16
  bootable image (kernel + limine.conf + BOOTAA64.EFI + user set).  The
  host verify loop extracts it and boots it (`tools/w7_accept_boot.py`).
- `install` additionally makes the *running disk* bootable-from-itself
  with the new kernel.  After a successful `all` + `install` this disk's
  next boot IS the in-OS-built kernel (`/boot/hobbyos.bin`).

## Budgets (ARM TCG, 8 vCPU / 16 GiB guest)

Measured in-OS 2026-10-15 (`MODE=selfhost` boot, PROBE: 868 files / 53 dirs,
28,298,626 bytes extracted):

| step | fresh | incremental (objects exist) |
|---|---|---|
| extract `/SRC.TAR` (~920 entries, 29 MB) | **~5 min** (measured) | n/a (force only) |
| `check` | **~1.5 min** (measured) | seconds |
| `user` (dominated by `sh.c` -O2) | ~33–65 min | links only |
| `kernel` (59 TUs) | ~77 min serial | link + objcopy only |
| `disk` (MKFSFAT 128 MiB + MCOPY) | ~6 min | ~6 min |
| full default loop | **~2.5–4 h, one boot** | ~23 min measured (34 TUs + link + disk + install, attempt 4) |

`make` re-runs are incremental (FAT timestamps are real since w10); the
`-j8` caveat for real kernel TUs from `acceptance.md` §9.6 still applies —
use serial for `kernel` until the FAT dirent race is fixed.

**Reading the serial log:** this kernel build traces every successful
`fat16_open()` with a bare absolute path line (`src/kernel/fat16.c`, the
unconditional `uart_puts(abs_path)` in `fat16_open`) — so extraction and
every in-OS clang header open print path-only lines.  They are ambient
kernel debug chatter, not extractor output; the `[TAR]`/`[BUILD]`/
`[SELFHOST]` markers are the evidence.  (Removing the trace is a kernel
change with its own battery cost — deliberately not done here.)

## Evidence markers (never exit codes)

    [BUILD] ...                the driver script's own trace
    [TAR] done: files=… errors=…   extraction summary (errors=0 required)
    [SELFHOST] phase=…         make-recipe/driver stage markers
    ACCEPTANCE-COMPLETE        driver success marker (MODE=selfhost runs)
    Mode: DESKTOP              a boot of the installed (in-OS-built) kernel

## Verification — 2026-10-15 (full loop, receipts)

`SH.BIN /BUILD_KERNEL.SH all`, attempt 4: `ACCEPTANCE-COMPLETE`, outcome=0,
1368.6 s (incremental resume; the first attempts' wedges are analysed in
`lanes/evidence-inos-bk/bk-wedge-report.md`).  Evidence in that directory:

| receipt | file |
|---|---|
| full loop serial (the pass) | `bk-full-attempt4-pass.serial.txt` |
| standalone boot of in-OS OUT.IMG (`Mode: DESKTOP`) | `bk-outimg-boot.serial.txt` |
| post-install disk reboot (`Mode: DESKTOP` from the in-OS kernel) | `bk-disk-postinstall-boot.serial.txt` |
| probe serial (extract + check; 868 files / 0 err) | `bk-probe.serial.txt` |
| attempt 1-3 wedge serials + report | `bk-full-attempt{1,2,3}*.serial.txt`, `bk-wedge-report.md` |
| hashes / sizes / boots | `bk-receipts.txt` |

Kernel flat images: in-OS `00bad4a7…` (218,356 B) at `/SRC/hobbyos.bin` ==
`/boot/hobbyos.bin` after install (cmp-verified); previous (selfhost-mode
host build, also what the disk shipped with) `7dc47c2e…` (218,396 B) kept
at `/boot/hobbyos.old`.  The in-OS kernel is **not byte-identical** to a
host build of the same tree: same size, but the link order differs
(Makefile.hobbyos lists the ASM objects last, the host Makefile first) and
host mode builds add a `-DKERNEL_MODE_*` define — so the flat images differ
byte-wise by design.  In-OS-vs-in-OS reproducibility is a separate question
(not measured here).

## Retry policy

The driver retries each make invocation up to 3× on a non-zero exit.
Rationale: the known stochastic exec-wedge class (see the wedge report)
kills or hangs one child per occurrence; a re-run compiles only the
missing TU, on a fresh process / fresh address space — per the kernel's
own refork-on-rejection recovery design.  Progress is monotone; only 3
consecutive failures on the same command are terminal.  (Not exercised in
the passing attempt — attempts 1-2 died before the retry existed, and
attempt 4 had no wedge.  A future wedge will exercise it naturally.)

## Related files

- `tools/selfhost-src-tar.sh` — assembles SRC.TAR (self-checking).
- `src/user/selfhost/build_kernel.sh` — the in-OS driver script.
- `src/user/tar.c` + `src/host/tar_test.c` — the extractor + its host test
  (synthetic edge cases + real GNU-tar archives, byte-compared).
- `bootloader/limine-arm.conf` — the canonical ARM boot config (disk
  recipe, `--src` staging and SRC.TAR all copy this one file).
- `docs/selfhost/acceptance.md` — the S0–S5 acceptance the toolchain
  itself was proven against (this feature sits on top of it).
