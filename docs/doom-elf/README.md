# Doom as an unmodified Linux ELF on HobbyOS — evidence

Two binaries are in play (see `docs/doom-linux-elf-plan.md`):

- **`doomgen.elf`** — doomgeneric's *stock* `doomgeneric_xlib.c` backend built for
  x86_64 Linux (static-PIE musl). It uses `DefaultVisual`/`DefaultDepth`, so it
  runs on HobbyOS's default depth-24 screen. **Working: the screenshots below.**
- **`doom10.elf`** — id Software's `linuxdoom-1.10` (github.com/id-software/doom)
  built for x86_64 Linux. It requires a depth-8 PseudoColor screen
  (`XMatchVisualInfo(..., 8, PseudoColor, ...)`) and never calls
  XInstallColormap. **Working:** `HB_XDEPTH=8` plus the DDX's map-time colormap
  install; `doom10-title-*.png` is its title screen captured from the guest.

**Read `REPORT.md` first** — it is the complete account (what had to be built,
what was measured, what the gaps are).  This file is the early run log.

## Files

| file | what |
|---|---|
| `REPORT.md` | the full account: architecture, measurements, gaps |
| `doom10-title-660x400.png` | id Doom title screen, guest framebuffer, 320×200 client @ 2× |
| `doom10-fullscreen.png` | the same, whole 1920×1080 HobbyOS screen |
| `doom10-e1m1-start-room.png` | E1M1's first room: barrel, corpse, pistol, HUD 50/100 %/0 % |
| `doom10-e1m1-outdoors-firing.png` | walked out to the blue-floored yard — rocky ridge, grey sky — **shotgun mid-fire** with the muzzle flash, AMMO 48 |
| `doom10-e1m1-corridor.png` | further in: rusted corridor, pistol, AMMO 50 |
| `doom10-e1m1-desktop-context.png` | the whole 1920×1080 framebuffer: the 320×200 client window at (0,0), no WM, no root background |
| `doomgen-e1m1-640x400.png` | doomgeneric E1M1 gameplay frame, guest framebuffer |
| `doomgen-e1m1-fullscreen.png` | the same, whole screen |
| `serial-doom10.txt` / `serial-doomgen.txt` | unedited guest consoles for those runs |
| `run-report-doom10.txt` / `run-report.txt` | harness reports (frame stats, ENOSYS list) |

## Run that produced these files

```
cd HobbyOS
TOOLS_SKIP=1 python3 tools/doom_elf/run_doom_elf_test.py \
    --elf /home/sarah/hobbyos-doom/build/out/doomgen.elf \
    --wad /home/sarah/hobbyos-doom/build/run-doom10/doom1.wad \
    --start-line "/DOOM.ELF -iwad /DOOM1.WAD" --seconds 25 --keys ""
```

The harness builds a `DESKTOP=icewm` disk, copies it (the repo's `disk.img` is
never mutated), stages `DOOM.ELF` + `DOOM1.WAD` + `/XSTART.TXT` +
`/X11/XSESSION.ENV` (`HB_XDEPTH=24`, `XSESS_NO_WM=1`), boots the intel guest
under KVM, drives QMP, and grades the screendumps.

Verdict: `PASS: Doom ELF ran under X and produced a frame`.

## What the guest console proves (serial-doomgen.txt)

```
[XSESS] spawned /XHOBBY.BIN pid=3
[XSESS] XSTART: /DOOM.ELF            <- args follow, the log line shows argv[0]
[XSESS] spawned /DOOM.ELF pid=5
[XSESS] session running
[X] zone memory: 0x108010c080, 600000 allocated for zone
[X] Using . for configuration and saves
[X] W_Init: Init WADfiles.
[X]  adding /DOOM1.WAD
[X] ===========================================================================
[X]  Doom Generic is free software, covered by the GNU General Public
[X] I_Init: Setting up machine state.
[X] M_Init: Init miscellaneous info.
```

i.e. a static musl x86_64 Linux binary, loaded by the HobbyOS compat ELF loader
(`abi=0x1`), found its IWAD on the FAT16 volume, opened the X display, and drew.

## The frame (doomgen-e1m1-640x400.png)

`shot-12.ppm` cropped to the 640x400 client window. Content: an E1M1 corridor
(textured walls/ceiling, first-person perspective), the HUD with
`AMMO 34`, `HEALTH 82%`, `ARMOR 94%`, the Doomguy face and the weapon grid
`2 3 4 5 6 7`, ammo `BULL 34/200  SHEL 0/50  ROKT 0/50  CELL 0/300`, and the
pickup message `YOU GOT THE SHOTGUN!`.

Measured on the whole 1920x1080 framebuffer: 12.3% non-black, 185 distinct
colours; the 640x400 window region is fully covered. `doomgen-e1m1-fullscreen.png`
is the whole screen, scaled.

## sha256

```
8711f41f6b49585ce5006bcf67a1b215f5b25e423b20a83c31ae3e5744fea611  doomgen-e1m1-640x400.png
5eae89806744d538bdb7da8e26794ded9ec76a27923af11bdeaabfa1827a31a6  doomgen-e1m1-fullscreen.png
81283537ed2ff72199a4d9277b6c200238c53293914e5a9993404774eea148b2  run-report.txt
9e757b338e0b203e8e06f96d8ccc6964e262f1d54b2ace60d474b6ec2e67cd0f  serial-doomgen.txt
```

`run-report.txt` is the harness's full report: the verdict, the frame statistics,
the unhandled-syscall list (empty for this run) and the complete serial console.
