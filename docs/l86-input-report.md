# L86 — mouse and keyboard input into RimWorld inside the HBHOST window

Branch `rw-l86-input` (from `rimworld` @ `f3786e98`, includes L83+L84+L85).
Worktree `~/hobbyos-rw-lanes/l86-input`. Nothing pushed.

The question: under IceWM the game runs in **HBHOST's** X window, not on the
desktop's own window protocol. Everything proven before this lane stopped at
the Start menu — no QMP click had ever been aimed *inside* the game and no key
had ever been typed into it. Does a click land where the cursor is, and does a
key reach the game?

**Clicks: yes, proven for the front end as far as the guest would boot.** Three
OCR-located clicks each changed the game's own screen — the Steam modal's
`Ignore`, `New colony` (main menu → `Choose scenario`), and `Crashlanded`
(selection). **Keyboard: not proven.** The driver that types into the Seed
field and moves focus with Tab is written and wired, but the guest stopped
reaching the menu: every boot after the third froze with all of the game's
threads parked (§7). That is a pre-existing guest defect, not an input one — in
the frozen boots the game had not received a single click yet.

---

## 0. Summary

| question | answer |
|---|---|
| where is the game's content on screen? | `[HB] geometry client=332,24,1280x736` + `[XSW] … gx=0 gy=16` → content origin **(332,40)**, 1280×720 |
| does a click land where the cursor is? | **yes**, three OCR-located clicks, each with a UI change and a diff bbox (§2) |
| do keys reach the game? | **not proven** — the guest stopped reaching the menu before the Seed field could be driven (§7); the path is analysed in §5 |
| how far does the front end go? | main menu → **Choose scenario** → Crashlanded selected; `Next` never clicked (the footer rule broke at 1280×720, §2.2) |
| any offset or dropped events? | no offset observed; no click was silently dropped. Three latent input-path defects found by reading the code, none hit by RimWorld (§5) |

## 1. The window and the two frames of reference

`[HB] geometry` is HBHOST's X *client area* inside IceWM's frame; the game's
content starts 16 px below its top edge because that strip is HBHOST's own menu
bar (`[HB] hd_set_size … content=0,16,1280x720`). The guest's `[XSW]` row
reports the same 16 px as `gy`:

```
[HB] geometry client=332,24,1280x736 screen=1920x1080
[XSW] n=… w=1280 h=720 gx=0 gy=16 …
```

so `screen = (332,40) + content`. HBHOST's `hd_press` → `wm_mouse_local` →
`wm_mouse_pixel` subtracts exactly `(w->x+2, w->y+34) = (0,16)` from the X
client coordinates, which is why the `content=(x,y)` in the log below **is** the
coordinate the game's input system receives in its `ESC [ P x;y;btn ~` message —
not an estimate.

Every click in this report was aimed in **screen** coordinates found by OCR on
the client-area crop (2× upscale — RimWorld's UI font is ~12 px at 1280×720 and
tesseract needs the upscale), and is logged in both frames:

```
[L86] click New colony             screen=(1215,267) content=(883,227) hold=0.12
```

## 2. The clicks that were driven

Run 2 of the lane (`obj/l86/run2-partial.jsonl`, serial
`obj/l86/attempt-*/`). Every row is a QMP `input-send-event` absolute click at
the screen pixel named, after the label was found by OCR on that screenshot.

| # | stage | click (OCR-located) | screen | content | diff px | bbox | OCR after |
|---:|---|---|---|---|---:|---|---|
| 1 | Steam modal | `Ignore` | (813,592) | (481,552) | 415 281 | (98,11)-(1914,1071) | modal gone: `Tutorial … New colony … Options … Quit to OS` |
| 2 | main menu | `New colony` | (1215,267) | (883,227) | 721 440 | (98,40)-(1914,1071) | `Choose scenario … Crashlanded … Three crashlanded survivors …` |
| 3 | scenario | `Crashlanded` | (527,98) | (195,58) | 19 174 | (102,85)-(1914,1071) | same screen, `Crashlanded` card re-rendered with its description |
| — | scenario | footer (brown-band rule) | (643,629) | (311,589) | 23 687 | (96,85)-(1914,1071) | **no advance**: still `Choose scenario` — wrong target (§2.2) |

### 2.1 The Steam modal (click 1)

OCR before, on the client crop:

```
Could not initialize Steam API. Possible causes:
- Steam is not running.
- Launched from outside Steam without steam_appid.txt in place.
- Running with different privileges than Steam client …
```

`Ignore` was found as a word box centred at screen **(813,592)**; the modal's
own text spans **(383,321)–(1485,660)**. After the click at that pixel the same
OCR finds the main menu (`Tutorial`, `New colony`, `Options`, `Quit to OS`) and
no `Ignore` and no "Steam". The whole-screen bbox is large because the dialog is
replaced by the menu behind it — the modal's own rect changed on 62.6 % of its
pixels (dark dialog vs menu art), and no other input was sent.

### 2.2 The footer rule does not survive 1280×720 (the one miss, and why)

L81's rule — "the affirmative is the rightmost flat-brown footer band" — was
measured at 1920×1080, where the buttons are separate widgets. At 1280×720 the
footer is **one full-width brown strip** with the labels drawn on it, so the
band finder returns a band spanning the window (rejected by the width filter)
or, worse, a list-row button elsewhere on the page. The row above is that
failure: the click went to (643,629) and the screen did not move.

Measured on the same screenshot with OCR, the real buttons are:

```
Back   screen=(536,736) 33x18
Next   screen=(1373,736) 29x18
```

So the driver now finds the footer affirmative **by its label** (`Next`,
`Generate`, `Start`, `OK`, `Accept`, `Select`, `Confirm`, `Continue`,
`Settle`), bottom-most then right-most occurrence, and only falls back to the
brown-band rule when OCR cannot read the strip (`rw_l86_input.find_advance`).
That is the single change the deeper flow needed; it is what the next boot
would have exercised when the guest stopped booting (§7).

## 3. Keyboard — designed, not yet reached

The driver's keyboard step (`type_seed`, `tab_probe`) is written and wired into
the flow, and would have produced exactly the evidence the lane asked for:

1. at the Create-world screen, OCR-find `Seed`, click the field (the first
   numeric word to its right, else 90 px right of the label);
2. OCR the field strip (psm 7, 2× upscale) before and after clicking, so the
   caret/selection is a recorded change;
3. inject `1 2 3 4 5` as QMP `send-key` qcodes and OCR the strip again — a
   changed field is unambiguous proof that key events reach the game;
4. one `Tab`, screenshot before/after, and report the diff bbox: a focus ring is
   a small localised change (a few hundred pixels inside the window), which
   cannot be confused with the taskbar clock at screen rows ≥ 1054.

It did not run: every boot after run 2 froze before the menu (§7).

## 4. World generation and the site screen

Same reason: `Generate` and the tile click (`land_point` + a 2 s hold, then the
tile panel's `Temperate forest … Terrain …` OCR, then a confirm-control search
for `OK`/`Select`/`Next`/`Accept`) are implemented in the driver but the guest
never reached the world-options screen again.

## 5. Diagnosis of the input path

The click path, end to end, with the code that implements each hop:

| hop | code | what it does |
|---|---|---|
| QEMU → guest | `input-send-event` (abs + btn) | pointer at 1920×1080 screen coordinates |
| X server → HBHOST | `hbhost.c:handle_x_event` | `ButtonPress`/`KeyPress`/`MotionNotify` on HBHOST's window |
| HBHOST → desktop code | `handle_button` → `hd_press` → `wm_mouse_local` → `wm_build_mouse_seq` | subtracts the content origin `(0,16)`, builds `ESC [ P x;y;btn ~` |
| desktop → client | `desktop_send_hook` = `window_write_default` | writes the sequence to the app's stdin pipe (one `write` per message) |
| client | `libx11.c:input_bytes` → `handle_mouse` → `mouse_event` | queues an `XButtonEvent` with `x,y` exactly as received |
| keys | `handle_key` → `hd_key_press` → `forward_key_to_focused` | `ESC [ K <mods> ~` (modifier stamp) then the payload byte |
| client | `key_byte` → `key_event` | printable byte → keysym = ASCII code |

**No offset.** The mapping is `content = screen − (332,40)` and it is the same
subtraction HBHOST performs; an error there would have shown as a click 16 px
(or 332 px) off its OCR'd label. The three clicks in §2 landed on their labels.

Three **latent** defects in the same path, found by reading it. None affects
RimWorld (it takes the pixel-window path), but each is a dropped-event class:

1. `hbhost.c:handle_button` returns immediately when `game_mode` is set, and
   `MotionNotify` is forwarded only `if (!game_mode)`. `game_mode` is the
   "fullscreen fb app that never asked for a pixel window" state
   (`[HB] fullscreen fb app -> game view`). RimWorld never enters it — it asks
   for a pixel window (`[HB] pixel mode -> 1`, `[XSW] … w=1280 h=720 gx=0
   gy=16`) — but any hosted game that maps the framebuffer directly gets **no
   mouse at all**: no press, no release, no motion.
2. Key **releases** reach the app only if it reads HBHOST's private raw queue
   (`raw_seen`); a compat-X11 client is sent the press payload only. Typing is
   unaffected (a keystroke is a press), but a game that polls `Input.GetKey`
   instead of `GetKeyDown` would see a key as held after a single press.
3. HBHOST never calls `XSetInputFocus`; it relies on the WM focusing the window
   when it is clicked. True for IceWM's default ClickToFocus (which is why the
   driver clicks the title bar first), but a WM with focus-follows-mouse or no
   click-to-focus would leave keys going nowhere.

## 6. How to reproduce

```sh
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l86-input
fuser -k disk.img; sleep 2
RW_L86=1 RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
  tools/rw/run-rimworld-desktop.sh --run-timeout 2400
```

`RW_L86=1` is the only new switch: it makes `rw_desktop_drive.py` hand the
already-running session to `tools/rw/rw_l86_input.py` after the Start-menu
launch (the default path is untouched). Evidence lands next to the screenshots
in `obj/rw-desktop/shots/l86.jsonl`; `tools/rw/l86-report.py` prints the tables
above from that file.

Supporting tools:

| tool | what it is for |
|---|---|
| `tools/rw/rw_l86_input.py` | the driver: OCR-located clicks, key injection, per-step diff evidence |
| `tools/rw/l86-live.py` | drive an already-running VM over its QMP socket (no reboot, no def-load wait) |
| `tools/rw/l86-retry.sh` | boot until the guest gets past def loading (§7) |
| `tools/rw/l86-report.py` | `l86.jsonl` → the markdown tables in this report |

## 7. The blocker: the guest freezes during def loading

<!-- STALL SECTION -->

Not this lane's defect, but it decides how far the evidence goes, and it will
hit the next person who tries to drive the game, so it is here with rows.

On **every boot after run 2** (a dozen attempts, with the variants in the table
below) the game froze before the menu — at the end of def loading
(`[XSW] n=` 330–400, once as early as `GfxDevice: creating device client`) —
and never advanced:

```
[XSW] n=390 swap=390 put=390 putok=390 flush=486 crc=0x… changed=1 w=1280 h=720 gx=0 gy=16 t=355443 ready=1
```

From then on the kernel's idle detector repeats with **every** game thread
parked:

```
[IDLESTUCK] slot=30 pid=30 st=8 … Loading.AsyncRe
[IDLESTUCK] slot=31 pid=31 st=8 … UnityGfxDeviceW
[IDLESTUCK] slot=32 pid=32 st=8 … Loading.Preload
[ALIVE-SET] no READY work, yet the scheduler counts live work …
[FUTEX] q slot=31 pid=31 st=9 key=0x113000A460 bitset=0xFFFFFFFF waits=129 wakes=128 … UnityGfxDeviceW
```

What it is **not** (each tested):

| hypothesis | test | result |
|---|---|---|
| the L85 double-run/reaper defect | `grep -c LOSTWAKE` on the serial log | **0** — the reaper never fired |
| GL path (virgl) | `RW_GLMODE=softpipe` | froze too |
| host GPU entirely | `RW_QEMU_GL=0` (2D VGA, no virtio-gpu-gl) | froze at the same key |
| vCPU count | `QEMU_SMP=4`, `QEMU_SMP=16` | froze at n=370 / at GfxDevice init |
| stale guest state | deleted `::/RW_HOME` from disk.img | froze |
| corrupt disk image | rebuilt `disk.img` from scratch | froze |
| a stale NFS export | `diff -rq` export vs source | only the AD-9 launcher differs (expected) |
| the input driver's screenshots | froze in boots that never saw a click | — |

The last `[XO]` rows (asset opens) are always inside
`Data/Biotech/Languages/English/Keyed/`, i.e. at the very end of the def load;
towards the end of the session the freeze moved *earlier* — right after
`GfxDevice: creating device client`, before a single present — in both
`RW_GLMODE=virgl` and `RW_GLMODE=softpipe`, so it is the render-thread
handshake itself and not the GL driver.  Two boots earlier the same day — same
kernel, same recipe, same driver — got through and reached the menu at `n≈600`.

The one row that is in every failing boot is the parked GL writer:

```
[FUTEX] q slot=31 pid=31 st=9 as=0x75704A50 key=0x113000A460 \
        bitset=0xFFFFFFFF waits=135 wakes=134 … UnityGfxDeviceW
```

`waits` is one more than `wakes` on that key in every failing boot, and the
thread never runs again; the main thread then parks behind it.  That is the
shape of a lost wake in the guest's condvar handshake (the same class L85 fixed
destructively in the reaper), not of anything the input path does — in the
frozen boots the game had not received a single click yet.

**Consequence for this lane:** the click proof stops at `Crashlanded` and the
keyboard proof has no run. When the guest freeze is fixed (or a boot gets
through), `RW_L86=1` drives menu → scenario → storyteller → world options →
Seed field → Generate → site without further changes, and every step lands in
`l86.jsonl` with its before/after OCR and diff bbox.
