# In-OS kernel build: `build_kernel.sh` + `SRC.TAR` (inos-build-kernel)

Status: implemented 2026-10-15.  This is the user-facing self-hosting loop:
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

At the desktop shell (or any console):

    SH.BIN /BUILD_KERNEL.SH            # all: extract-if-needed + make all + install
    SH.BIN /BUILD_KERNEL.SH untar      # just re-extract /SRC.TAR into /SRC
    SH.BIN /BUILD_KERNEL.SH check      # one-TU compile smoke
    SH.BIN /BUILD_KERNEL.SH user       # HELLO SH LS CAT
    SH.BIN /BUILD_KERNEL.SH kernel     # 59 TUs -> /SRC/hobbyos.elf + /SRC/hobbyos.bin
    SH.BIN /BUILD_KERNEL.SH disk       # assemble the bootable /SRC/OUT.IMG
    SH.BIN /BUILD_KERNEL.SH install    # /SRC/hobbyos.bin -> /boot/hobbyos.bin

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

| step | fresh | incremental (objects exist) |
|---|---|---|
| extract `/SRC.TAR` (~900 entries, 27 MB) | ~5 min | n/a (force only) |
| `check` | ~1 min | seconds |
| `user` (dominated by `sh.c` -O2) | ~33–65 min | links only |
| `kernel` (59 TUs) | ~77 min serial | link + objcopy only |
| `disk` (MKFSFAT 128 MiB + MCOPY) | ~6 min | ~6 min |
| full default loop | **~2.5–4 h, one boot** | minutes |

`make` re-runs are incremental (FAT timestamps are real since w10); the
`-j8` caveat for real kernel TUs from `acceptance.md` §9.6 still applies —
use serial for `kernel` until the FAT dirent race is fixed.

## Evidence markers (never exit codes)

    [BUILD] ...                the driver script's own trace
    [TAR] done: files=… errors=…   extraction summary (errors=0 required)
    [SELFHOST] phase=…         make-recipe/driver stage markers
    ACCEPTANCE-COMPLETE        driver success marker (MODE=selfhost runs)
    Mode: DESKTOP              a boot of the installed (in-OS-built) kernel

## Related files

- `tools/selfhost-src-tar.sh` — assembles SRC.TAR (self-checking).
- `src/user/selfhost/build_kernel.sh` — the in-OS driver script.
- `src/user/tar.c` + `src/host/tar_test.c` — the extractor + its host test
  (synthetic edge cases + real GNU-tar archives, byte-compared).
- `bootloader/limine-arm.conf` — the canonical ARM boot config (disk
  recipe, `--src` staging and SRC.TAR all copy this one file).
- `docs/selfhost/acceptance.md` — the S0–S5 acceptance the toolchain
  itself was proven against (this feature sits on top of it).
