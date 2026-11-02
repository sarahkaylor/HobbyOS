# XDEPTH8 — depth 8 PseudoColor probe for HobbyOS's X.Org server

`XDEPTH8.BIN` is the client half of the depth-8 lane. It proves, through the
real X protocol, that `XHOBBY.BIN` with `HB_XDEPTH=8` in its environment is a
depth 8 PseudoColor X screen — the only kind of screen Doom 1.10 will run on
(`i_video.c` calls `XMatchVisualInfo(dpy, screen, 8, PseudoColor, &info)` and
otherwise `I_Error("xdoom currently only supports 256-color PseudoColor
screens")`).

What it does, logging one `[XD8] ...` line per step to stderr (the session
relays client stderr to the serial console):

1. `XOpenDisplay(":0")`.
2. `XMatchVisualInfo(dpy, screen, 8, PseudoColor, &vi)` — logged with the
   visual id, class and colormap size.
3. `XCreateColormap(root, vi.visual, AllocAll)` and `XStoreColors` of 256
   known cells: `0x10` red, `0x50` green, `0xA0` blue, `0x00` black, `0xFF`
   white, everything else a grey ramp.
4. `XCreateWindow(depth 8, InputOutput, vi.visual, colormap)` at 0,0, 640x400,
   `XMapWindow`, `XInstallColormap`.
5. `XPutImage` of a depth 8 `ZPixmap`: a white 4-row strip on top, a black
   4-row strip at the bottom, and three 640x400 vertical bands of indexes
   `0x10`/`0x50`/`0xA0` in between (edges at x = 213 and x = 426).
6. `XGetImage` read-back of the bands, the top and the bottom strip, so the
   console distinguishes "the client never sent the image" from "the DDX
   expanded the palette wrongly".
7. Then it idles (redrawing on `Expose`) until the session tears it down.

## Running it

```sh
# server + client (this tree's X11 build dir and the shared client prefix)
bash third_party/x11/build.sh --arch intel \
    --prefix ~/hobbyos-icewm-lanes/prefix/intel --stage server -j8
bash third_party/x11/build.sh --arch intel \
    --prefix ~/hobbyos-icewm-lanes/prefix/intel --stage clients -j8

# boot, stage /XSTART.TXT + HB_XDEPTH=8 + XSESS_NO_WM=1 on a disk COPY,
# then assert the console markers and the screendump pixels
python3 src/user/x11/apps/xdepth8/run_xdepth8_test.py
```

`-noinstall` as the client's argv skips its explicit `XInstallColormap`, which
is exactly what linuxdoom-1.10 does (it creates a private AllocAll colormap,
sets it on the window and only ever calls `XStoreColors`).  The DDX installs a
mapped window's colormap itself (`hbRealizeWindow`), so the bands must come out
identical either way:

```sh
python3 src/user/x11/apps/xdepth8/run_xdepth8_test.py --no-build \
    --client-args=-noinstall
```

`run_xdepth8_test.py` builds `disk.img` (`TOOLS_SKIP=1`), copies it, stages
`/XDEPTH8.BIN` as the session autostart and `/X11/XSESSION.ENV` with
`HB_XDEPTH=8` + `XSESS_NO_WM=1` (IceWM cannot run on an 8-bit screen), boots
QEMU with the same device set as `run_icewm_test.py`, and checks:

- the server's `hobbyos: screen 640x400 depth 8 PseudoColor` line,
- `[XD8] XMatchVisualInfo depth 8 PseudoColor ok`,
- `[XD8] XPutImage 640x400 ZPixmap depth 8 done`,
- `[XD8] XGetImage green band (320,200) = index 0x50`,
- the screendump: white at (320,2), black at (320,398), red at (100,200),
  green at (320,200), blue at (600,200), both band edges exactly one pixel
  wide, and (700,500) — outside the 640 px screen — still black.

Artifacts land in `/tmp/hobbyos_xdepth8/` (`xdepth8.ppm`, `serial.log`,
`REPORT.txt`); exit 0 only when every check passes.

## Negative control

`--control24` boots the same disk and the same client with `XSESS_NO_WM=1`
but **no** `HB_XDEPTH`.  It must then see a depth 24 root and log
`[XD8] FAIL XMatchVisualInfo(depth 8, PseudoColor): no such visual` — the
exact call Doom 1.10 turns into `I_Error`.  That pins the pixel evidence above
on `HB_XDEPTH=8` rather than on anything else in the image:

```sh
python3 src/user/x11/apps/xdepth8/run_xdepth8_test.py --no-build --control24 \
    --out /tmp/hobbyos_xdepth8_control
```
