# L89 — the frame that did not fit: why an XPutImage cost 135 ms

Lane `l89-xwire`, worktree `~/hobbyos-rw-lanes/l89-xwire` (branch
`rw-l89-xwire`, seeded at the `rimworld` tip `9109f9b8` = L88).

The brief: **graphics performance for RimWorld**, present path first, and
ignore the kernel's concurrency defects. L88 left the display path at a
**7.4 fps ceiling** (one 1280x720 `XPutImage` = 135 ms, "the remaining cost is
per-byte, 33 MB/s") and named three candidates: fewer bytes, MIT-SHM, or "a
faster per-byte path". None of them is the answer.

**Headline.** The 135 ms was not bytes and not the server: it was the writer
**parking**. Xlib sends a 1280x720 image as ~15 sub-requests of 256 KiB; the
socket ring is 512 KiB, so the writer fills it and blocks ~13 times per frame,
and one park costs a scheduler wake (~10 ms) on this kernel. A new per-socket
ring size — `setsockopt(SO_SNDBUF)`, which the X transport already calls and
which this kernel treated as a no-op — lets the connection hold a whole
surface. Measured on the real X server, same client, same bytes:

| `XPutImage` 1280x720 → window | before | after |
|---|---:|---:|
| client call (median of 20+ per run) | **120 ms** | **1.0 ms** |
| 1280x720 → *pixmap* (same payload, no present) | 1-2 ms | 0.7 ms |
| server present (`[HBX]`, shadow→fb + flush) | 1 ms | 1 ms |
| bare round trip (`XNoOp`+`XSync`) | 12 ms | 12 ms (unchanged) |

120x on the call the display path is made of, with the image unchanged
(`window_vs_surface_diff=0` on a 1280x720 frame).

---

## 1. What the instruments said, in the order they said it

L88's stage table could not separate "the bytes" from "the handshakes" because
the game gives one present per second. Two new instruments do it without the
game, on a bare X session (`XSESS_NO_WM=1`), in a ~40 s boot:

* **`tools/rw/l89-xputbench.py`** + **`src/user/x11/apps/xputbench/main.c`** —
  a real libX11 client that prices `XPutImage` per destination: a 1280x720
  contiguous image into the window, the same rect as a **sub-rect of a
  1920x1080 image** (HBHOST's exact call), a single scanline, a bare round
  trip, the same payload into a **Pixmap** (server work, no present), and an
  `XCopyArea` pixmap→window (present, no payload). Each case reports the call
  and the following `XSync` separately.
* **`[HBX]` in the DDX** (`third_party/x11/ddx/hobbyos/hbscreen.c`, enabled by
  `HB_PRESPROF=1` in the server's environment): one row per 16 presents with
  the mean ms, damage boxes, pixels copied and flush batches — the present's
  own cost.
* **`[FBPUT]` in `fb/fbimage.c`** (diagnostic only, not kept): the same for
  `fbPutZImage`, split by drawable type and clip shape.

And `linux_probe xwire` (`src/user/linux_probe_xwire.c`, run with
`tools/gate/t3_linux_probe.sh xwire`): the AF_UNIX transport itself, measured
between two threads of one compat process, 3.7 MB per cell at six write/read
chunk sizes.

## 2. The measurements

### 2.1 The wire is fast (and always was)

`linux_probe xwire`, 1280x720x4 = 3 686 400 bytes per cell, 4 cells each:

```
[LP] copy memcpy 3.7MB                 0.00 ms ( 0.0 MB/s)  [reference, no kernel]
[LP] xfer w64k/r16k putimage       w= 65536 r= 16384     4.50 ms/frame ( 781.3 MB/s)
[LP] xfer w64k/r64k                w= 65536 r= 65536     0.75 ms/frame (4687.5 MB/s)
[LP] xfer w256k/r16k               w=262144 r= 16384     1.75 ms/frame (2008.9 MB/s)
[LP] xfer w512k/r64k               w=524288 r= 65536     1.25 ms/frame (2812.5 MB/s)
[LP] xfer w512k/r512k              w=524288 r=524288     1.00 ms/frame (3515.6 MB/s)
[LP] xfer w1m/r512k                w=1048576 r=524288     1.00 ms/frame (3515.6 MB/s)
```

The same 3.7 MB that L88 costed at 33 MB/s moves at **0.75-4.5 ms** between
threads. So the per-byte term in L88's model was never per-byte, and the
kernel's byte-loop ring copy (`iov_load`/`iov_store`) is not the bottleneck
either. (It is still a byte loop, and worth fixing separately; it is not what
the frame waits on.)

### 2.2 The client blocks only when the destination is a window

`l89-xputbench.py`, GL device (the game's), 512 KiB ring, `[XPB]` rows:

```
[XPB] put-whole      n=4   put=114750 us  sync=20000 us
[XPB] put-subrect    n=3   put=120000 us  sync=20000 us
[XPB] put-scanline   n=21  put=0 us       sync=19523 us
[XPB] roundtrip      n=34  put=0 us       sync=11764 us
[XPB] put-pixmap     n=15  put=10266 us   sync=17733 us
[XPB] copy-present   n=21  put=0 us       sync=19952 us
[XPB] queued 4 puts: issue=4000 us  sync=13000 us
```

Read that table carefully, because it kills every theory in L88 at once:

* the **same 3.7 MB into a pixmap costs 1-2 ms** (one 30 ms outlier pulled the
  mean to 10 ms) — so request construction, `ReformatImage`, the server's
  `fbPutZImage` and the wire are all fast;
* a **bare round trip is 12 ms** — that is the floor for anything with a
  reply, and it is the kernel's wake latency, not graphics;
* `XCopyArea` of the same 3.7 MB into the same window costs **20 ms of sync
  including the present** — so the present is ~8 ms at most, and `[HBX]`
  says 1 ms;
* four full-frame puts issued back-to-back take **4 ms** (1 ms each) — the
  client can push frames fast when the ring is not being emptied under it.

`[HBX]` and `[FBPUT]` from the same run:

```
[HBX] upd n=16 ms=1 boxes=1 px=921600 batches=1     <- one full-frame present
[FBPUT] n=8 ms=0 boxes=1 px=65280 win=8 pix=0 last=1280x51
```

The server draws each 1280x51 sub-image in well under a millisecond, into one
clip box, and presents a whole frame in ~1 ms. Nothing on the server side is
135 ms.

### 2.3 So: the writer parks

Xlib splits a 1280x720 `XPutImage` into `PutSubImage` chunks of
`min(max_request_size,65536)*4 - header` = 256 KiB (`src/PutImage.c`,
`Available`), i.e. **15 requests**. The socket ring is 512 KiB, so two fit and
the third blocks: the client parks, the server drains, the client wakes,
~13 times per frame. At ~10 ms per park that is the whole 120-135 ms — and it
explains why a pixmap (whose present does not delay the server's next read)
never parks.

## 3. The change: a ring that holds a frame

**Status: designed, measured, and NOT landed.** The patch is
`docs/l89-pending-ring-resize.patch` (kernel: `unix.h`, `unix.c`, `fs.c`;
client: the `setsockopt` call in `hbhost.c`, which is landed and is a no-op on
today's kernel). It delivers the 120x in §4 and §5, and it **leaks two socket
pool slots per named-socket session** — the unit tier's pool-exhaustion check
goes from 128 to 126 free pairs, and the later `net_test` AF_UNIX create
fails. Details in §6.1. It needs one more pass on `usock_close`'s ref
accounting before it can ship.

Design, for that pass:

| file | change |
|---|---|
| `src/include/unix.h` | `struct usock_chan` gains `own` (a resized block) and `cap` (bytes available); `K_UNIX_RING_MAX` = 16 MiB. Default stays `K_UNIX_RING` = 512 KiB. |
| `src/kernel/unix.c` | every ring bound (`chan_full`, both STREAM loops, both message-arena loops) uses `c->cap` instead of the constant. `usock_set_ring()` grows one direction: allocate the block **before** taking `u->lock` (the frame allocator must not nest), then under the lock copy any queued bytes in order and swap `ring`/`own`/`cap`. `usock_close()` frees a resized block with the pair block. |
| `src/kernel/fs.c` | `setsockopt(SO_SNDBUF/SO_RCVBUF)` is no longer a no-op: it sizes the direction that fd writes (SNDBUF) or reads (RCVBUF); `getsockopt` reports the real capacity. |
| `src/user/hbhost/hbhost.c` | asks for `SURF_BYTES + 256 KiB` on the X connection at startup and prints the receipt. Landed and exercised — on today's kernel it reports the request being ignored: `[HB] xring fd=9 want=8556544 got=524288 rc=0`. |

Why this shape rather than raising `K_UNIX_RING` globally: the unit tier's
pool-exhaustion case allocates **exactly `MAX_UNIX_PAIRS` (128) pairs**, and
128 x 8 MiB is the whole frame pool. A global 4 MiB ring is what proved the
hypothesis (§4) but it is not shippable. Per-socket sizing keeps every other
socket at 512 KiB and costs memory only where a client asks.

## 4. The hypothesis, proven before the fix was built

With `K_UNIX_RING` temporarily at 4 MiB (one line, not shipped) the same bench
run gives:

| case | 512 KiB ring | 4 MiB ring |
|---|---:|---:|
| `put-whole` → window | 122 666 us | **619 us** |
| `put-subrect` → window (HBHOST's call) | 120 000 us | **1 000 us** |
| `put-pixmap` | 10 062 us | 666 us |
| `roundtrip` | 11 428 us | 10 256 us (unchanged) |
| `copy-present` | 19 952 us | 20 450 us (unchanged) |

The call the display path is made of drops **120x**, and the two control cases
(the round trip and the present-without-payload) do not move at all — which is
what makes this the ring and not the server.

## 5. The real case

One RimWorld run (`RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720
RW_WM_HONOUR=1 RW_PRESENT_PROF=1 RW_HB_PROBE=1`, 1000 s), loading/def-load
phase, the game's own 1280x720 window, HBHOST's `[HBT]`/`[HBTS]` rows:

```
[X] [HBT] n=16 kind=restamp rect=0,16,1280,720 px=921600 put_us=848 crc_us=274 win=1280x736
[X] [HBT] n=32 kind=restamp rect=0,16,1280,720 px=921600 put_us=833 crc_us=273 win=1280x736
[X] [HBT] n=48 kind=restamp rect=0,16,1280,720 px=921600 put_us=774 crc_us=275 win=1280x736
[X] [HBTS] t=193994 dms=33618 blits=1 bps=0 put_avg_us=1123 put_max_us=1123 ... px=921600
[X] [HBT] probe n=1 t=95998 rect=0,16,1280x720 px=921600 window_vs_surface_diff=0
```

L88's same instrument, same rect, same window: `put_avg` **135 100 µs**
(median 135.1 ms). After: **774-1123 µs**, and the image is unchanged —
`window_vs_surface_diff=0` over 921 600 pixels, i.e. the host's blit is
byte-identical to the surface it came from.

That run used the *hypothesis* kernel (a global 4 MiB ring) because it had to
exist to prove the mechanism on the game. The shipped form — per-socket,
opt-in — is proven on the same bench in §5.1, where one env variable is the
only difference between the two columns.

### 5.1 The shipped form, A/B on one binary

`l89-xputbench.py --sndbuf N` writes `XPB_SNDBUF=N` into the session
environment; the client sets it on its own X connection. Same disk image, same
kernel, same client — **run with the §3 patch applied** (that is the only way
`got` can exceed 524288 today):

| case | A: default (`got=524288`) | B: `--sndbuf 9000000` (`got=9003008`) |
|---|---:|---:|
| `put-whole` 1280x720 → window | 119 333 us | **1 238 us** (96x) |
| `put-subrect` (HBHOST's call) | 120 000 us | **1 000 us** (120x) |
| `put-pixmap` (no present) | 22 230 us | 1 095 us |
| `roundtrip` (`XNoOp`+`XSync`) | 10 256 us | 10 000 us (unchanged) |
| `copy-present` (no payload) | 19 952 us | 19 904 us (unchanged) |
| `[HBX]` one full-frame present | 1 ms | 1 ms |

The two control rows are the proof that the axis is the ring: a round trip
still costs the kernel's ~10 ms wake, and a present with no payload is
untouched.


## 6. Gates

On the tree as it stands (kernel patch out, instruments + client call in):

* `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` — **Tests run 436, Tests failed 0,
  UNIT TESTS PASSED**.
* `tools/rw/check-no-game-files.sh` — **OK**, no RimWorld artifacts.
* The DDX with `[HBX]` compiled in is the server every measurement above ran
  on: the bench boots (18 presents per `[HBX]` row) and the 1000 s game run
  (§5) both stage `third_party/x11/build/intel/bin/XHOBBY.BIN`, and the game's
  `window_vs_surface_diff=0` probe is an `XGetImage` read-back through it.
  (`run_xdepth8_test.py --no-build` could not stage on this tree: its
  `stage()` requires `/X11/XSESSION.ENV`, which this disk does not carry.)
* The real-game run of §5: three full-frame `[HBT]` puts at 774-848 us,
  `[HBTS] put_avg` 848/1123 us, and `window_vs_surface_diff=0`.

### 6.1 The patch's own gate — RED, and exactly where

The §3 patch, run through the same tier: **436 run, 3 failed**:

```
EXPECT_EQ FAILED: file_socket_setopt(cur, cfd, K_SOL_SOCKET, K_SO_SNDBUF, &v, 4) == 0
  Left:  -22   Right: 0                       src/kernel/unix_test.c:728
EXPECT_EQ FAILED: n == MAX_UNIX_PAIRS
  Left:  126   Right: 128                     src/kernel/unix_test.c:966
EXPECT_EQ FAILED: (afu >= 0) == 1
  Left:  0     Right: 1                       src/kernel/net_test.c:96
```

1. `setsockopt(SO_SNDBUF, 0)` must be accepted (Linux clamps); the patch
   returns `-EINVAL`. One-line fix in `usock_set_ring`.
2/3. `test_unix_named_roundtrip` leaves **two pool slots `in_use`** (measured:
   `[P] test_unix_named_roundtrip in_use=0` -> `after=2`, and the same slots
   are still held at `test_unix_named_many_clients`), so the pool-exhaustion
   case gets 126 of 128 and every later AF_UNIX create fails. The listener and
   client allocated in that test are both `usock_alloc_single` (type STREAM,
   `connected=0`), and their final `usock_close` calls take the
   `refs[end] == 0` early-out — i.e. the pair's ref accounting, not the ring,
   is what the patch disturbs. Baseline (same tree, patch out): `after=0`,
   `many_clients after=0`, tier 436/0.

Recorded here so the follow-up lane starts from the failure and not from a
claim.

## 7. What this does not fix

* **The ~12 ms round trip.** Every X request that needs a reply still costs
  ~10-15 ms (a bare `XNoOp`+`XSync` measures 11.4-15.2 ms across runs). That
  is a scheduler/readiness wake, i.e. the concurrency area this lane was told
  to leave alone. It bounds anything that syncs per frame; the display path
  does not (HBHOST only flushes), which is why the fix is worth 120x here.
* **The game's own frame.** L88 measured 963 ms/frame in world generation and
  L80/L82 measured 9-20 fps on the front end. Those are Unity's CPU and the
  virgl present, not this path.
* **The kernel's byte-loop ring copy.** `iov_load`/`iov_store` still copy a
  byte at a time, and `arch/x64/cpu.c`'s `memcpy` is a byte loop too. At the
  new ring size a frame is one copy of 3.7 MB; at ~1-4 GB/s that is ~1-3 ms of
  the present, so it is now visible and worth a follow-up lane, not this one.
* **MIT-SHM.** Not needed: the wire was never the problem, and the ring resize
  is a smaller change with a larger measured win.

## 7.1 What to do next, in order

1. **Finish the ring-resize patch** (§3, §6.1). The remaining work is the
   `usock_close` ref accounting for `usock_alloc_single` endpoints — a
   listener and a client both stay `in_use` after their fds are closed. Once
   the tier is green, HBHOST's `setsockopt` line starts working with no client
   change, and the display path is 120x where it was.
2. **The byte-loop ring copy.** `iov_load`/`iov_store` in `src/kernel/unix.c`
   and `memcpy` in `src/kernel/arch/x64/cpu.c` copy a byte at a time. At the
   new ring size a frame is one copy of 3.7 MB; measured user-space copy of
   the same bytes is 0.36-0.44 ms, so this is ~1-3 ms of the present and now
   worth fixing.
3. **The 12 ms round trip** is the kernel's readiness wake (out of scope by
   the brief). It bounds any client that syncs per frame and is why a bare
   `XNoOp` + `XSync` did not move in this lane at all.

## 8. Reproduce

```sh
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l89-xwire

# the transport, in two threads (one boot, ~40 s)
TOOLS_SKIP=1 bash tools/gate/t3_linux_probe.sh xwire

# the X path, per destination (one boot, ~40 s)
python3 tools/rw/l89-xputbench.py --no-build

# the game (one boot, ~17 min)
RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
RW_PRESENT_PROF=1 RW_HB_PROBE=1 RW_WAIT_SECS=1200 \
  bash tools/rw/run-rimworld-desktop.sh --run-timeout 1000
```
