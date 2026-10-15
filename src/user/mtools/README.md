# GNU mtools 4.0.49 — HobbyOS port (selfhost W2, lane w2-mtools)

Ported binaries: **MCOPY.BIN, MDEL.BIN, MMD.BIN** — the acceptance build
tools that stage the source tree onto the boot disk (`mcopy -s dir ::/SRC`,
`mmd ::/BOOT…`, and the disk recipe's copy-ins), per L4's
`docs/selfhost/buildtools-audit.md` §4 and the wave-2 `Makefile.hobbyos`
sketch.

## Layout (house style, see docs/gnu-ports.md)

| piece | where | what |
|---|---|---|
| pristine upstream | `third_party/mtools-4.0.49/` | full 4.0.49 tree from the pinned tarball (SVN upstream — no git tag; integrity = `mtools-4.0.49.tar.gz` + `.sha256`) |
| pinned config | `src/user/mtools/config.h` | hand-written; feature gates + HobbyOS-glue declarations (guarded by `HOBBYOS_TARGET`); host-parity retarget under `HOST_TEST` |
| port glue | `src/user/mtools/mtools_port.c` | one tiny `main()` per tool (`-DMTOOLS_TOOL_*`) + sysroot fallbacks (`getlogin`, `alarm`, `setuid/gid`, `helpFlag`, minimal `expand`/`precmd`/`postcmd`) |
| transcribed engine | `src/user/mtools/*.c` | byte-identical to upstream (verified with `cmp` over the whole tree) |

Upstream mtools builds ONE multicall `mtools` binary that dispatches on
`argv[0]`; HobbyOS loads one flat `.BIN` per program, so the port instead
links each command as its own binary (shared engine + one per-tool main from
`mtools_port.c`).  `mtools.c` (the dispatcher), `expand.c` and `precmd.c`
are therefore NOT transcribed — `mtools_port.c` replaces them (the audit's
§4.1 subset table listed mcopy/mmd+shared libs and allowed stub/minimal
replacements; documented deviation: `expand()` never runs `$(...)` shell
expansion and `precmd/postcmd` are no-ops, both unreachable for `-i IMG`).

## Build

```
make obj/arm/mcopy.bin obj/arm/mdel.bin obj/arm/mmd.bin   # ARM (default)
make ARCH=intel obj/intel/mcopy.bin …                      # x64 also builds
# disk.img now stages ::/TOOLS/MCOPY.BIN, /TOOLS/MDEL.BIN, /TOOLS/MMD.BIN
```

Per-object (`mtools_*`), per-main (`mtoolsm_*`) Makefile rules follow the
nano/sed multi-object port shape; the whole block is one tightly-commented
section in the user-ports area of the top Makefile.

## What is and isn't ported

Ported: mcopy, mdel, mmd, and the shared engine they pull — the stream /
image (plain_io) / FAT (fat, vfat, dirCache, mk_direntry) / charset (CP4xx/8xx
codepage tables, no iconv) / config (no mtools.conf — `-i` drive) layers,
VFAT **LFN writing kept** (byte-identical to host mtools, proven by parity).

Not ported (as L4 scoped): floppyd, xdf, scsi, mzip, mpartition, mbadblocks,
minfo, mformat (that is the dosfstools/mkfs lane), and every other tool main.

Sysroot gaps closed in the port (per audit §4.3): ioctl (compiled out —
`BLKFLSBUF` undefined, `devices.c` INIT_NOOP arm), termios/tcgetattr (never
compiled — `USE_RAWTERM` off, prompts fall back to plain stdio),
opendir/readdir (`-s` recursion NOT ported: `HAVE_READDIR` left off for the
target; the acceptance stages individual files, no `-s`), `strndup`
(missFuncs provides its own), `getlogin`/`alarm`/`setuid`/`setgid`/`dup`
(fallback implementations in the glue).

## Evidence

- ARM + x64 builds green; `.BIN` sizes recorded in the lane report.
- `make obj/arm/hello.bin` unchanged (user build intact).
- Host-side parity: the transcribed subset + port mains built for the host
  (`HOST_TEST` config retarget) and raced against the pristine reference on
  byte-identical FAT images — 8/8 byte-identical results (mmd dirs, single/
  LFN/binary copy-in, copy-out round-trip, mdel, `mcopy -s` tree, `--help`).
- In-OS smoke: deferred to wave 3 (no AUTORUN; MODE=selfhost + SELFHOST.BIN
  is the harness lane's kernel change, acceptance.md §5.2).  Exact steps in
  the lane report.
