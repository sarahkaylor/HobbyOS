# DOOM on the HobbyOS Linux ABI — build lane (`tools/linux-doom`)

Two static-PIE x86_64 Linux ELFs, built with musl and X11, that the HobbyOS
compat track can exec and that draw through HobbyOS's X.Org port:

| binary | what it is | size | sha256 |
|---|---|---|---|
| `doom10.elf`  | id Software `linuxdoom-1.10` (github.com/id-software/doom), patched for LP64 | 1 569 504 B | `eff329853a29d43877f2041273431e0e182b3bacc15ad9443cee7b1964926dda` |
| `doomgen.elf` | doomgeneric (ozkl/doomgeneric @ `e257a095`) **stock X11 backend** | 1 555 424 B | `1916d2f810250bfad742799a16b3646aa22360e8f7920c83ce8db09d294b11dc` |
| `DOOM1.WAD`   | DOOM Shareware v1.9 IWAD, 4 196 020 B, 1264 lumps | — | `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771` |

Installed copies live in `~/hobbyos-doom/artifacts/`.  Both ELFs report:

    ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), static-pie linked, stripped
    Type: DYN (Position-Independent Executable file)      # static-PIE
    readelf -d | grep -c NEEDED -> 0                      # no shared libs

## Toolchain path: musl (not the glibc fallback)

`docs/linux-abi.md` makes static-PIE **musl** the contract, so that is what
these are.  There is no musl libX11 on this host, so
`build-x11-musl-sysroot.sh` assembles one:

* Alpine **v3.24** static archives and headers (Alpine is a musl distro):
  `libx11-static`, `libx11-dev`, `libxext-static`, `libxext-dev`,
  `libxcb-static`, `libxcb-dev`, `libxau-dev`, `libxdmcp-dev`, `xorgproto`,
  `xtrans` — every package pinned by version **and** sha256 in the script.
* `libXau.a` is built from the X.Org `libXau-1.0.12` tarball with `musl-gcc`
  (Alpine ships libXau only as a `.so`, and `libxcb.a` needs
  `XauGetBestAuthByAddr` for the MIT-MAGIC-COOKIE path).

Everything lands in `~/.cache/hobbyos-doom/x11-musl-sysroot`, with a
`x11-musl.env` holding `X11_MUSL_SYSROOT/CFLAGS/LIBS`.  All downloads are
idempotent and hash-checked.

## Build (one command each, from a pristine clone)

```sh
tools/linux-doom/build-doom10.sh     # -> $HOME/hobbyos-doom/build/out/doom10.elf
tools/linux-doom/build-doomgen.sh    # -> $HOME/hobbyos-doom/build/out/doomgen.elf
```

Environment knobs: `DOOM10_SRC` (default `~/hobbyos-doom/doom-src/linuxdoom-1.10`),
`DOOMGEN_SRC` (default `third_party/doomgeneric` in this worktree),
`HOBBYOS_DOOM_BUILD`, `HOBBYOS_DOOM_CACHE`, `STRIP=0`, `DEBUG=1`.

### linuxdoom-1.10

The script copies the sources to `$BUILD/doom10-src`, applies
`patches/doom10-lp64-toolchain.patch`, adds two header shims, compiles every
`.c` with `musl-gcc` and links `-static-pie`.

Flags that are load-bearing:

| flag | why |
|---|---|
| `-std=gnu89` | GCC 15 defaults to C23; `typedef enum {false,true} boolean` in `doomdef.h` is a hard error there |
| `-fcommon` | `doomstat.h` has tentative definitions that need common symbols |
| `-DIPPORT_USERRESERVED=5000` | BSD-ism musl's `<netinet/in.h>` lacks; `i_net.c` builds `DOOMPORT` from it |
| `-Ishim` | `i_video.c` says `#include <errnos.h>` (upstream typo) → `shim/errnos.h` includes `<errno.h>`; `i_sound.c` includes `<linux/soundcard.h>`, which OSS removed from the UAPI → `shim/linux/soundcard.h` is an empty placeholder (the OSS code is compiled out under `SNDSERV`) |
| `-fno-strict-aliasing` | Doom 1.10 type-puns everywhere; GCC's aliasing assumptions are invalid for it |

**The source is not buildable-as-published on x86_64.**  The patch file
documents each hunk; in short: `maptexture_t`'s `void**` field is a 32-bit
on-disk field (every texture patch offset read garbage → SIGSEGV), pointer
arrays sized `*4`, `int` casts on pointers (colormaps/translation-table
alignment, save-game padding), a `char*`/`int` config default table, and a
missing NULL sentinel on `sprnames[]` that only worked because ILP32 memory
layout happened to place a zero word after the array.  No game logic changes.

### doomgeneric

The vendored `third_party/doomgeneric` carries one HobbyOS VirtIO-GPU edit in
`i_video.c` (static 32bpp `FB_ScreenInfo`, hardcoded `fb_scaling = 2`).
`patches/doomgen-stock-ivideo.patch` reverts that hunk to upstream
(`static struct FB_ScreenInfo s_Fb; int fb_scaling = 1;`) so the delivered
binary is the *stock* X11 backend.  Behaviour is the same in practice:
upstream's `I_InitGraphics` computes `fb_scaling = s_Fb.xres / SCREENWIDTH`
(= 2 for the 640x400 `DG_ScreenBuffer`) itself.

The script then drives doomgeneric's own `Makefile` (its `SRC_DOOM` list is
authoritative) with `CC=musl-gcc -static-pie`.

## Host verification (Xvfb + real screenshots)

`evidence_capture.py` runs a binary against Xvfb, waits, grabs the **game
window** with `import`, and parses the PPM itself (no PIL) to report the
non-black pixel fraction and distinct-colour count.  It starts/stops Xvfb if
needed.  Re-run:

```sh
python3 tools/linux-doom/evidence_capture.py --binary ~/hobbyos-doom/artifacts/doom10.elf \
  --outdir /tmp/ev --display :99 --screen 640x480x8 \
  --cwd <dir with doom1.wad> --capture 9:e1m1 -- -warp 1 1 -skill 3
```

Measured on this workstation (all frames 100 % non-black — the software
renderer paints every pixel):

| run | screen | window | distinct colours |
|---|---|---|---|
| doom10, title/credits | `Xvfb :99 -screen 0 640x480x8` | 320x200 | 42 |
| doom10, `-warp 1 1 -skill 3` | same | 320x200 | **158** |
| doomgen, attract demo | `Xvfb :98 -screen 0 640x480x24` | 640x400 (`"DOOM Shareware"`) | 118 |
| doomgen, `-warp 1 1 -skill 3` | same | 640x400 | **155** |

PNG + PPM + `stats.json` + the engine's stdout are in `evidence/`:

* `evidence/doom10/title/`, `evidence/doom10/e1m1/` — the shareware
  credits/title page and E1M1 with the full status bar.
* `evidence/doomgen/attract/`, `evidence/doomgen/e1m1/` — attract-mode demo and
  E1M1 at 640x400.
* `evidence/depth-compat/` — what happens on the *wrong* screen depth (below).

Why the window grab and not `import -window root`: on an 8-bit PseudoColor
root, Xvfb returns child-window pixels without colour-translating them through
the child's colormap, so a root grab of DOOM shows 11 colours from the server's
6x6x6 cube instead of DOOM's palette.  Grabbing the window makes the server
hand back that window's pixels *and* colormap (42/158 colours above).

## Screen-depth requirements — read this before wiring it into HobbyOS

The two binaries want **opposite** screens:

* **doom10.elf needs an 8-bit PseudoColor screen.**  `i_video.c` does
  `XMatchVisualInfo(dpy, screen, 8, PseudoColor, &X_visualinfo)` and calls
  `I_Error` otherwise.  On a depth-24 server it prints, verbatim:

      Error: xdoom currently only supports 256-color PseudoColor screens

  (`evidence/depth-compat/doom10-on-depth24.log`.)
* **doomgen.elf needs depth >= 24.**  Its stock backend blits a 32-bpp buffer
  with `XCreateImage(..., depth = DefaultDepth(...), ...)`; on a depth-8 root
  that is interpreted as 8 bpp and the image collapses to 19 colours
  (`evidence/depth-compat/doomgen-on-depth8.stats.json`,
  `doomgen-on-depth8.png`).

So HobbyOS's X server must expose a depth-8 PseudoColor screen for `doom10`
(a second screen, or an 8-bit mode) while `doomgen` wants a 24-bit TrueColor
screen.  If HobbyOS only offers depth 24, `doom10.elf` will refuse to start
until either the server grows an 8-bit screen or `i_video.c` is taught a
32-bpp blit path (a real porting job — say the word and it becomes a follow-up
lane; it is *not* included here because the brief was the unmodified engine).

## Known limitations

* **No sound.**  `doomdef.h` forces `SNDSERV`; the engine looks for an external
  `sndserver` helper, prints `Could not start sound server [sndserver]` and
  continues silently.  No OSS/ALSA dependency is linked in.
* **Title-screen demos are skipped** by `doom10`: the shareware 1.9 IWAD's
  `DEMO1..3` lumps carry a version byte that `VERSION` (109) rejects, so the
  log shows `Demo is from a different game version!` and the attract loop falls
  back to the title.  Use `-warp`/`-skill` or real input to see gameplay.
  `doomgen` plays the same demos fine (it does not gate on the byte).
* **Remaining 32-bit assumptions**, deliberately untouched because they are
  netgame/deathmatch-only: `d_net.c` `NetbufferChecksum` (`(int)&field`) and
  `d_main.c` `-statcopy` (`(void*)atoi(...)`).  Single-player is unaffected.
* Stripping (`STRIP=1`, the default) removes the ~6 MB of `.debug_info` that
  Alpine's static archives drag in; `STRIP=0` keeps symbols for gdb.

## Layout

```
tools/linux-doom/
  build-x11-musl-sysroot.sh   pinned Alpine static X11 + libXau-from-source
  build-doom10.sh             linuxdoom-1.10 -> static-PIE musl ELF
  build-doomgen.sh            doomgeneric stock X11 -> static-PIE musl ELF
  evidence_capture.py         Xvfb run + window screenshot + frame statistics
  patches/doom10-lp64-toolchain.patch
  patches/doomgen-stock-ivideo.patch
  shim/errnos.h               <errnos.h> typo shim
  shim/linux/soundcard.h      empty OSS header placeholder
  evidence/                   screenshots + stats + runtime logs
```
