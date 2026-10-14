# MKFSFAT.BIN — dosfstools 4.2 mkfs.fat port (lane W2-MKFS)

In-OS FAT16/FAT12/FAT32 image creation for the HobbyOS self-hosting build
(acceptance stage S3 / `Makefile.hobbyos` `disk` target: `MKFSFAT -F 16 -C
OUT.IMG BLOCKS`).  Also usable interactively: `MKFSFAT -F 16 OUT.IMG`.

## Layout

| file | origin |
|---|---|
| `mkfs.fat.c` `common.c` `charconv.c` `device_info.c` | transcribed from `third_party/dosfstools-4.2/src/` (GPL-3+ header kept), byte-identical **except** the `HobbyOS port (W2-MKFS)` marked patches |
| `common.h` `charconv.h` `device_info.h` `msdos_fs.h` `endian_compat.h` | byte-identical transcriptions |
| `blkdev.h` | trimmed transcription (ioctl/sysfs plumbing dropped — see header) |
| `version.h` | generated version stamp, pinned to 4.2 / 2021-01-31 |
| `config.h` | port's pinned feature set (no autotools; `-include`'d by the Makefile rule).  Every `HAVE_*` here is a sysroot-gap decision from L4's audit (`docs/selfhost/buildtools-audit.md` §5) |
| `hb_termios.h` `hb_blkdev.c` | first-party glue: termios + blkdev geometry stubs the audit prescribes |

## The `-C` (create) path

Upstream 4.2 already implements `-C` as `open(O_EXCL|O_RDWR|O_CREAT,0666)` +
`ftruncate(dev, part_sector*sector_size + blocks*BLOCK_SIZE)` (mkfs.fat.c
`main()`, lines ~1869-1881) — **no dd, no sparse/SEEK_HOLE reliance** (4.2 has
no SEEK_HOLE anywhere).  The port changes nothing there; in-OS the target file
lives on the FAT filesystem itself and sizing is one `ftruncate()` syscall
(SYS_FTRUNCATE is FULL per the audit; the kernel resizes FAT16 files).  The
pre-FAT metadata (boot sector, both FATs, root dir) is then written and the
whole image `fsync()`ed.  `-C` requires the explicit block count
(`blocks_specified` → `die("Need intended size with -C.")` otherwise).

## What wave 3 must test in-OS (the deferred in-OS gate)

1. Boot the staged disk (acceptance S0-S3 harness), then on the console:
   `cd /SRC && /TOOLS/MKFSFAT.BIN -F 16 -C OUT.IMG 262144` — expect exit 0,
   banner `mkfs.fat 4.2 (2021-01-31)` on stdout, and OUT.IMG materialized at
   128 MiB.
2. `-C` on an existing file must fail (`file ... already exists`); the
   `Makefile.hobbyos` disk recipe `rm -f` first — verify that ordering.
3. Verify the image's BPB the kernel accepts: extract with `fat16img.py get`
   and re-mount/`mdir -i`, or boot it (Limine must find `BOOTAA64.EFI`).
4. `-F 16` sizing arms: 128 MiB → 2 KiB clusters (spc=4); QEMU/AAVMF boot is
   the acceptance S4 gate anyway.
5. `-n` volume label (CP850 path, `setlocale(LC_CTYPE,"")` in the C locale)
   and default `NO NAME    ` label.

## Host-native build (how gate 4 was validated)

`tools/build_mkfs_host.sh` compiles these same sources natively
(`-include config.h`, HAVE_ALARM=1) → `obj/mkfs_host/mkfs.fat` and races
against the system `mkfs.fat` on scratch images (BPB signature, FAT zones,
root entries).  See `docs/selfhost/lanes/w2-mkfs-report.json` for the run.
