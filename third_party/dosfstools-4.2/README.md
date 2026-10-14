# dosfstools 4.2 — vendored pristine upstream (mkfs.fat port source)

Pristine upstream tree for the HobbyOS **MKFSFAT.BIN** port (wave-2 lane W2-MKFS,
selfhost.md). This directory is the byte-identical tarball extraction, never
edited — ported/transcribed files live in `src/user/mkfs/` per house style
(docs/gnu-ports.md §1).

| field | value |
|---|---|
| tool | dosfstools (`mkfs.fat`) |
| version | 4.2 |
| upstream | https://github.com/dosfstools/dosfstools |
| tarball | `dosfstools-4.2.tar.gz` (this dir) |
| sha256 | `64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527` |
| tag→commit | `v4.2` → `bba5a12447991cc1be9c33855c6c68e89b9ab29c` |
| release date | 2021-01-31 |
| license | GPL-3+ |

Pin source: `docs/selfhost/buildtools-audit.md` §1 (lane L4). Hash verified at
lane time against the locally-downloaded tarball.

Port transcription set (upstream src/Makefile.am `mkfs_fat_SOURCES` +
`devinfo_common_sources`): `mkfs.fat.c`, `common.c`, `charconv.c`,
`device_info.c` + headers `common.h charconv.h device_info.h msdos_fs.h
endian_compat.h`, the generated `version.h`, and the `blkdev_*` geometry
surface. See `src/user/mkfs/config.h` for the pinned port decisions.
