# L16 — the M4 present path: a GL 3.3 frame on a real window

Status: **fixed and gated in the guest.** `glXSwapBuffers` puts a GL 3.3 core
frame on the HobbyOS desktop, `/dev/fb0` holds it pixel-for-pixel, teardown is
clean, and `tools/gate/m4_gl_probe.py` grades all of it in one boot.

## 1. What the recorded defect actually was

The plan carried: *"`glXSwapBuffers`' present path (`xlib_sw_display →
XPutImage`) faults with an instruction fetch into a non-executable libGL
page, so no window screenshot yet."* Reproducing it split that into two
independent facts:

1. **The swap path itself works** — but only if the client waits for the
   desktop's content rect before painting. The first version of this lane's
   probe painted straight after `XMapWindow`, `glXSwapBuffers` returned
   normally, and *nothing* was presented: `XPutImage` returns early while
   `d->win.content_ready` is 0, so the swap silently dropped every pixel and
   still reported success. Waiting for `ConfigureNotify`/`Expose` (what the M3
   client already does) made the frame appear.
2. **The fault was real, but it was not in the swap path.** With a fully
   working swap, a clean `glXMakeCurrent(None)`, `glXDestroyContext`,
   `XCloseDisplay` and `dlclose(libGL)`, the process died *after* `main`
   returned:

   ```
   [GL] teardown: clean
   [GL] done rc=0
   [KERNEL] pid=3 (GLPROBE.BIN) memory fault VA=0x0000001081ACFF50
            PC=0x0000001081ACFF50 rw=rx in=HOLE region=NONE ... -> killed
   [EXITPC] pid=3 last_user_pc=0x1080B88297 code=0x4000000B
   ```

   `PC == VA`, `rw=rx` (instruction fetch), `in=HOLE` — code jumped into a page
   that is not mapped at all, not into a mapped-but-non-executable one.

## 2. Attributing the PC

libGL's load bias is printed by the probe (`dump_link_map`): `0x1080c6e000`.
`0x1081ACFF50 − base = 0xe61f50`. The shipped DSO is stripped, so the offset
was resolved by **relinking the same objects unstripped** (same archives, same
order, same version script; stripping does not move code):

```
$ readelf -sW /tmp/libGL.dbg.so | ... nearest symbol <= 0xe61f50
0xe61f50 FUNC format_array_format_table_destroy   (delta 0x0)
```

`format_array_format_table_destroy` is registered by
**`src/mesa/main/formats.c:499`: `atexit(format_array_format_table_destroy)`**
inside `format_array_format_table_init()`, reached from
`_mesa_format_from_array_format()` on the first context draw.

## 3. Root cause

glibc's `atexit()` is process-global: a handler registered from inside a DSO is
**not** scoped to that DSO and is **not** run by `dlclose()`. Mesa calls plain
`atexit()` from several places that libGL builds
(`src/mesa/main/formats.c:499`, `src/mesa/main/extensions.c:235`,
`src/mesa/main/context.c:222`, `src/util/os_misc.c:316`,
`src/util/strtod.c:53`, `src/util/u_process.c:211`). So:

1. the first GL call registers Mesa's handlers in the *process* exit list;
2. the client `dlclose()`s libGL, and glibc unmaps its segments;
3. `exit()` walks the exit list and calls
   `format_array_format_table_destroy` at `libGL + 0xe61f50` — a hole;
4. instruction fetch → SIGSEGV → the process is killed *after* a successful
   render, which is why it looked like a graphics bug.

It is an **unload** defect, not a present defect. Any app that `dlclose()`s
this libGL crashes on exit the same way — Unity/SDL do dlclose graphics DSOs on
shutdown, so this had to be fixed rather than worked around in the probe.

Ruled out while attributing it:

| Hypothesis | Measurement | Verdict |
|---|---|---|
| compat ELF loader mishandles a DSO with separate RX/RW LOADs | `/proc/self/maps` inside the guest shows libGL as `r--p` / `r-xp 0xab3000` / `r--p` / `rw-p`, i.e. exactly the four LOADs with correct protections, and RELRO mprotected to `r--p` | not the bug |
| zeroed `XImage.f` table (`XDestroyImage` is a macro) | `f.destroy_image`/`get_pixel`/`put_pixel` are populated since L13; the swap calls `XPutImage`, never a NULL slot | already fixed |
| `XPutImage` needs a real GC/visual | `XCreateGC` returns a pooled GC, `XSetFunction(GXcopy)` succeeds, and `XPutImage` is a memory blit into the shadow | not the bug |
| text mapped without `PROT_EXEC` | the fault is `in=HOLE region=NONE`: no region at all, because `dlclose` unmapped it | wrong page class |

## 4. The fix

`tools/mesa/build-hobbyos.sh` links `libGL.so.1` with **`-z nodelete`**
(`DF_1_NODELETE`), and the build now *fails* if the flag disappears:

```
df_1:        NODELETE (dlclose keeps the DSO mapped; its atexit() handlers stay valid)
```

`dlclose` then drops the reference without unmapping, so the process-global
handlers registered from inside the DSO remain valid until `exit()`. This is
the same contract libglvnd uses for a vendor GL driver (`RTLD_NODELETE`), and
it is the honest property of a DSO that registers process-global exit hooks:
**a DSO that cannot be unloaded must say so.** Patching the six `atexit()`
call sites was the alternative; it would fix today's list and still leave any
future Mesa `atexit()` (or any other bundled library's) as a landmine.

## 5. The probe and the gate

`src/user/gl_probe.c` now does both readbacks:

* FBO 256×256 triangle, `fnv1a64=0x25476e955b66d136` — renderer identity
  (byte-identical to the host run of the same DSO);
* a real window through the compat libX11 (`XCreateWindow` on the
  `glXGetVisualFromFBConfig` visual, `XMapWindow`, wait for
  `ConfigureNotify`/`Expose`), context made current on it, triangle drawn at
  the window's own size, `glXSwapBuffers`, then the **desktop framebuffer**
  (`/dev/fb0`, mmapped by the probe itself) is read at the window's content
  rect and compared pixel-for-pixel with the GL back buffer.

The comparison decodes the fb's `0xAARRGGBB` word and walks rows bottom-up
(X is top-down, GL is bottom-up); both facts were measured, not assumed
(`GL (07,07,f1,ff)` at the blue vertex reads back as `0xff0707f1`). What
differs is only the desktop's re-stamped mouse pointer — `ESC ] F` asks the
desktop to draw it over the app's pixels.

Teardown is exercised and graded: `glXMakeCurrent(dpy, None, NULL)` →
`glXDestroyContext` → `XCloseDisplay` → `dlclose(libGL)` → `exit()`, all four
rows plus a clean `[EXITPC] code=0x0`.

## 6. Evidence (real boot, `tools/gate/m4_gl_probe.py`, exit 0)

```
[GL] GL_VERSION  : 3.3 (Core Profile) Mesa 26.2.6
[GL] GL_RENDERER : softpipe
[GL] GLSL        : 3.30
[GL] FBO 256x256 status=0x8cd5 (complete=0x8cd5)
[GL] readback 256x256: fnv1a64=0x25476e955b66d136 glGetError=0x0
[GL] non-background=22890/65536 (34%) colours=22891 bg=13,13,26,255
[GL] window content rect x=2 y=34 1916x1018 (laid out=1)
[GL] window: default framebuffer 1916x1018 drawn, glGetError=0x0
[GL] gl back buffer 1916x1018 fnv1a64=0x3e2e5ead7fc50121 glGetError=0x0
[GL] swap start drawable=0x1 ctx=0x1040001740
[GL] swap returned
[GL] desktop fb: /dev/fb0 mapped at 0x16f0000000 (1920x1080 b8g8r8a8)
[GL] desktop rect 2,34 1916x1018 fnv1a64=0x039795a9519bfdca nonbg=683570/1950488
[GL] pixel diff: 96 of 1950488 differ (0.0049%)
[GL] diff bbox: x 957..966 y 505..518 (10x14)
[GL] present check: desktop holds the frame (only the re-stamped cursor differs)
[GL] teardown: glXMakeCurrent(None) -> 1
[GL] teardown: glXDestroyContext returned
[GL] teardown: XCloseDisplay returned
[GL] teardown: dlclose -> 0
[GL] teardown: clean
[GL] done rc=0
[EXITPC] pid=3 last_user_pc=0x1080B5D146 code=0x0
```

`nonbg=683570/1950488 (35.0%)` is the triangle's coverage of the window — the
same 34–35 % the 256×256 FBO readback reports. The 96 differing pixels are a
10×14 rect at the pointer position, all `0xffffffff` (the white arrow).

Artifacts (regenerated by the gate, git-ignored):

| file | what |
|---|---|
| `docs/rimworld-m4-window.png` | the committed screenshot: the GLPROBE window on the HobbyOS desktop |
| `obj/m4-shots/03_window.ppm/.png` | QMP screendump the gate takes while the probe holds its window |
| `obj/m4/gldesk.ppm/.png` | the desktop rect read out of `/dev/fb0` by the probe (1:4) |
| `obj/m4/glprobe.ppm/.png` | the FBO readback (the renderer-identity frame) |
| `obj/m4_l16_final.txt` | the gate's own log of the run above |

## 7. What remains

* The window is *maximised* by the desktop (1916×1018 content rect for a
  320×240 request) — the desktop's window policy, not a GLX issue; a real app
  gets resized the same way, and the probe renders at the size it is given.
* Frame rate at 1916×1018 on softpipe is not measured here; M4's fps target is
  the 1280×720 cube harness (`tools/mesa/cube.c`), not this gate.
* `libGL.so.1` now stays mapped until process exit by design. If a future
  build wants a truly unloadable DSO, Mesa's `atexit()` call sites have to be
  converted to `__cxa_atexit(..., __dso_handle)` with a DSO-local
  `__dso_handle` — that is a Mesa-side change, deliberately not taken here.

## 8. Plan amendment for L0 (`docs/rimworld-plan.md`, integrator-owned)

Replace the M4 "Still open" sentence with:

> **Present path closed (L16).** `glXSwapBuffers` presents a GL 3.3 frame to a
> real window: `xlib_sw_display → XPutImage → XFlush → /dev/fb0`, verified by
> reading the desktop framebuffer back inside the guest (96 of 1,950,488 pixels
> differ, all of them the desktop's re-stamped cursor) and by a screenshot
> (`docs/rimworld-m4-window.png`). Teardown
> (`glXMakeCurrent(None)`/`glXDestroyContext`/`XCloseDisplay`/`dlclose`) is
> clean and `EXITPC code=0x0`. The recorded "instruction fetch into a
> non-executable libGL page" was **not** in the swap path: Mesa registers
> process-global `atexit()` handlers from inside the DSO
> (`formats.c:499 format_array_format_table_destroy`), so after a client's
> `dlclose` the process's `exit()` jumped into the unmapped text
> (`VA=PC=0x1081ACFF50 = libGL+0xe61f50`, `in=HOLE`). `libGL.so.1` is now
> linked `-z nodelete` (DF_1_NODELETE, as libglvnd does for vendor drivers) and
> the build fails if the flag is lost. See `docs/rimworld-m4-present.md`.

and append to §11 (fix log):

> - 2026-11-04 — **M4 present path closes: a GL 3.3 frame reaches a real
>   window.** `libGL.so.1` gains `-z nodelete` (a dlclose()ed DSO holding
>   Mesa's process-global `atexit()` handlers crashed `exit()` in the unmapped
>   text — `format_array_format_table_destroy` at `libGL+0xe61f50`,
>   `in=HOLE`), `gl_probe` waits for the desktop's content rect before
>   painting (an early swap presents nothing and still returns), reads the
>   frame back out of `/dev/fb0` and tears the stack down cleanly. Receipts:
>   `[GL] pixel diff: 96 of 1950488 (0.0049%)`, `[GL] teardown: clean`,
>   `[EXITPC] code=0x0`, `docs/rimworld-m4-window.png`.
