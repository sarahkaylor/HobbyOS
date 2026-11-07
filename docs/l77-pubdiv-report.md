# L77 — the probe→PUB divergence does not survive L75's fix; the publish path is now identity-asserted, and the game reaches the main menu

Lane `l77-pubdiv`, worktree `~/hobbyos-rw-lanes/l77-pubdiv` (branch
`rw-l77-pubdiv`, seeded at the merged tip `e6fd3500`). The question L76 handed
over: *the probe's own read of `/RWDSO/UnityPlayer.so` off `0x9AD000` held 3547
non-zero bytes, the probe answered 1, and the page published for that offset
was 4096 zeros (`[PUB] … zero=4096 ver=1`), then `#UD`.* Does that still happen
now that the frame pool can no longer alias the NVMe BAR (L75), and if it does,
where is the gap between "verified" and "published"?

## 0. Summary

| question | answer |
|---|---|
| does a zero page still get published as verified on the merged tree? | **no.** Two 1800 s windows: **0** `[PUB] … zero=4096 ver=1` rows for a file-backed exec page, 0 `[TXT] PROBE`, 0 `[TXT] REPLAY`, 0 `[TXT] PROBEGET`, 0 `[UDX]`, 0 user faults, `xunv=0`, and `hole=0` on every `[BLKD]` row |
| was the old trigger even reachable in those windows? | **yes.** Window 2 walked `fr_used` to **367 758** — past **327 680** (the old extent-1 base) and **327 936** (the NVMe BAR frame) — with `bar_in_pool=0` at boot and `hole_allocs=0`; nothing happened at either frame |
| was the probe path exercised? | **not in the field.** Every exec page verified on the first two reads (`[PUBS] … xunv=0`), so `fat16_page_probe()` was never called in either window. The field verdict is "does not reproduce", and the *class* is closed structurally below rather than by that absence alone |
| so what was wrong? | a real defect of exactly that class, one layer down: `fat16_page_probe()` read into a static `rawbuf[MAX_CPUS][4096]` and copied to the caller **after** a second (preemptible) 4 KiB read. It compared `a = rawbuf` (CPU 0's slot) against the second read while publishing `rb` (this CPU's slot). On any CPU but 0 the two-read rule was comparing two different buffers, and the `second_zero` replay escape then accepted whatever `rb` held at copy time |
| the fix | read 1 lands **in the caller's buffer**; the identity of what was verified (pointer, offset, nz, sum) is handed back in `struct fat16_page_id`; there is no copy after the verdict |
| the assertion | `vm_pub_identity()` on both publish paths (`probeget`, `verify`): pointer + offset + source-unchanged + destination-byte-identical, or **no publish**, a `[PBF]` alarm and the counters (`idc=` / `idf=`) on `[PUBS]` |
| proof it trips | `test_vm_publish_identity` corrupts the copy on purpose: `[PBF] probeget … dst_nz=3825 dst_sum=0x77880 same=0 buf=1 off=1 ver=1 pub=0` → refused → `[TXT] ZERO … published the device's bytes` → `[PUB] … zero=16 ver=1` |
| what the game does now | **it gets past `Initializing.` to the main menu.** L2/L3/L4: OCR reads `Initializing.` on the splash, then the screen changes to RimWorld's menu art with the **Steam API modal** (`Could not initialize Steam API … [Ignore] [Quit]`), which nothing clicks — 1 static present per ~12 s for the rest of each window |
| the one window that did not get there | L1 (merged tip): presents stop at `n=120` (`t=133941`), the loader still finishes (`[XO] 1209`), all threads parked, no fault. The same park L75 §5.2 recorded; it is not a page-in failure (no `[EIO]`, no `[TXT] ZERO`). L3/L4 on the fixed kernel do not show it |
| regression | **435/0 units** + **9/9 gates** |

## 1. The three places "verified" and "published" could be different bytes

L76 ended with the tuple below and named the gap but not the line:

```text
[TXT] PROBE off=0x9AD000 … | raw rc=0 r_nz=3547 r_sum=0x65E51 | dropped=8 refill n=4096 f_nz=0
[TXT] REPLAY off=0x9AD000 -> read 1 non-zero, read 2 all-zero (device replay): publishing read 1
[TXT] PROBEGET off=0x9AD000 -> … published the device's bytes
[PUB] who=fault_obj va=0x10824E2000 off=0x9AD000 prot=RX priv=1 src=file zero=4096 ver=1
[KERNEL] User process fault! Vector: 6 RIP: 0x10824e2ea0
```

On the merged tree the state to reproduce is: `dst` is a frame whose contents are
zeros at publish time while the probe's read held real bytes. There are exactly
three mechanisms that can produce that, and this lane checked all three:

1. **The destination frame is not RAM.** That was L75: pool extent 1 began at
   `0x80000000`, the 32-bit PCI hole, and QEMU's NVMe BAR is at `0x81000000`
   (frame 327 936). A 4 KiB write into that frame disables the controller and a
   read of it returns device state. Boot now prints `bar_in_pool=0` and
   `hole_allocs=0`, and `frame_test.c` pins it.
2. **The source buffer is not the one that was verified.** `fat16_page_probe()`
   used a static per-CPU buffer and copied to the caller after the verification
   read — a preemptible window (see §3).
3. **The publisher copies from a different frame than the verifier filled.**
   `vm_exec_page_verify()` copies `f[i]` → `dst`; `vm_file_obj_materialize()`
   then publishes `priv` (= `dst`) or, in the shared case, the cache frame after
   `vm_page_eq(priv, src)`. Both are the same bytes at the moment of the check.

(1) is L75's, (2) is fixed here, and (3) is now asserted rather than argued.

## 2. Two 1800 s windows on the merged tip: the divergence does not reproduce

`tools/rw/l77-ladder.sh 1 2`: `--timeout 600 --run-timeout 1800`,
`RW_WAIT_SECS=1900`, softpipe, the L75 NVMe trace enabled. Sequential; the unit
tier never ran at the same time.

| run | presents | `[XO]` | `res_mib` | max `fr_used` | `hole=` | resets | divergence rows | end state |
|---|---:|---:|---:|---:|---:|---:|---|---|
| L1 | 120 | 1209 | 667 | 209 932 | 0 | 0 | none | **parked**: presents stop at `n=120` (`t=133941`), loader still finishes, all threads parked; alive at 1800 s, no fault |
| L2 | **1270** | 1207 | 1329 | **367 758** (`fr_high` 495 015) | 0 | 1 (self-check) | none | **main menu**: `Initializing.` on screen, then the Steam API modal; alive at 1800 s, no fault |

`[FRAME] ram ext0=1 ext1=1 … ext1=0x100000000-0x200000000 reserved=16384` and
`[BLKQ] mmio win=0x81000000-0x81200000 bar_in_pool=0 alias_read=0x0F0107FF cap=0x0F0107FF hole_allocs=0`
on both windows.

The rows that would have carried the old failure are simply absent:

```text
L1: [PUBS] n=221184 exec=5000 xzero=9 file_xzero=9 anon_xzero=979 xunv=0 | fault_anon … | img xzero=9
L2: [PUBS] n=774144 exec=5357 xzero=9 file_xzero=9 anon_xzero=1255 xunv=0 | fault_anon … | img xzero=9
```

All nine `file_xzero` pages are `who=img` — the native image's own zero pages
(DESKTOP.BIN has 107 of them, L63), not a DSO text page. `xunv=0` says every
executable page was verified on the first pass in both windows, which is why
`[TXT] PROBE` never fires: **the field windows do not exercise the probe**, so
they are evidence that the L75 fix removed the *old* trigger (L2 crosses both
old hole frames), not that the probe path is safe. §3 is why the probe path is
safe anyway.

Window 1 is worth keeping in the record: it is the same early park L75's §5.2
described (present stop, loader finishes, 25 threads parked), and it shows the
park is not a consequence of the page-in gate — no `[EIO]`, no `[TXT] ZERO`, no
fault, and the loader still reaches `[XO] 1209`.

## 3. The defect in the probe: the verified buffer was not the published one

`src/kernel/fat16.c`, before this lane:

```c
static uint8_t rawbuf[MAX_CPUS][PROBE_PAGE];
uint8_t *rb = rawbuf[get_cpuid() % MAX_CPUS];
...read 1 -> rb...
uint32_t raw_nz = nz_bytes(rb, len, &raw_sum);
...
int n2 = fat16_read_direct_pos_raw(start_cluster, file_size, chk, len, pos);  /* preemptible */
const uint8_t *a = (const uint8_t *)rawbuf;   /* CPU 0's slot, not rb */
const uint8_t *b = chk;
for (...) { if (a[i] != b[i]) ok = 0; ... }    /* compare against CPU 0 */
if (!ok && second_zero) ok = 1;               /* replay escape */
if (ok) { for (...) out[i] = rb[i]; return 1; }   /* publish whatever rb holds NOW */
```

Three properties make this the same failure class as L73's, one layer down:

* **The window is preemptible.** A 4 KiB raw read is eight sector reads, each
  entering `blk_token_acquire()` → `safe_wfi()`; the tick schedules the thread
  out mid-lock (`src/kernel/lock_order.c` says so in as many words). A second
  probe on the **same** CPU therefore writes that CPU's slot between read 1 and
  the copy. L73 made the buffer per-CPU, which removed the cross-CPU collision
  and left the same-CPU one.
* **The comparison used the wrong slot.** `a = rawbuf` is `rawbuf[0]`; the
  publish used `rb = rawbuf[cpuid]`. On any CPU but 0 the "two reads must
  agree" rule was comparing the second read against another CPU's buffer, and
  the `second_zero` replay escape turned that into an unconditional pass
  whenever the second read came back all-zero.
* **The copy happened after the verdict.** Even with `a = rb`, the bytes
  published were re-read from the shared slot one whole device read later.

### The fix

* Read 1 lands in `out`, the caller's own frame (`pb` in `vm.c`). There is no
  static buffer, no per-CPU slot, and **no copy after the verdict**: the bytes
  verified are the bytes the caller holds.
* The verdict's identity is returned in `struct fat16_page_id { buf, off, nz,
  sum; }` (`src/include/fat16.h`), re-measured on `out` at the moment of the
  verdict. A diagnostic-only caller (`out == NULL`) gets a scratch frame and no
  identity.
* The `second_zero` replay escape stays (it is real: read 1 = the file's bytes,
  read 2 = 4096 zeros is the device's post-abort replay), but it now applies to
  the same buffer that is published, so it can no longer launder a different
  page.

## 4. The identity assertion: `verify(buf) == publish(buf)`

`src/kernel/vm.c`:

```c
static int vm_pub_identity(const char *tag, uint64_t src, uint64_t dst,
                           uint64_t off, const struct fat16_page_id *pid);
```

Four conditions, each a separate way to catch a divergence early:

| field | condition | catches |
|---|---|---|
| `buf=` | `pid->buf == src` | the publisher copying from a frame the probe did not fill |
| `off=` | `pid->off == off` | a verdict about another file offset |
| `ver=` | `pid->nz/sum == nz/sum(src)` | the source buffer changing after the verdict (the L73/L77 race) |
| `pub=` | `pid->nz/sum == nz/sum(dst) && vm_page_eq(src, dst)` | the published copy not being the verified bytes |

It is called on both exec publish paths:

* `probeget` — right after `vm_frame_copy(dst, pb, …)` in
  `vm_exec_page_verify()`. A failure leaves `have = 0`, so the caller falls
  through to the independent device re-read (and a `[TXT] ZERO … published the
  device's bytes` row) instead of publishing the diverged page.
* `verify` — at the end of `vm_exec_page_verify()`, before the source frames are
  freed, so every exec publish (fault, read-touch, shared) is covered.

Rows:

```text
[PBI] probeget off=0x0 probe_buf=0x24091000 src=0x24091000 dst=0x2408D000 frame=147601/147597 ver_nz=4080 ver_sum=0x7F800 src_nz=4080 dst_nz=4080 dst_sum=0x7F800 same=1 buf=1 off=1 ver=1 pub=1
[PBF] probeget off=0x0 … dst_nz=3825 dst_sum=0x77880 same=0 buf=1 off=1 ver=1 pub=0
```

`[PBI]` prints for the first 12 checks and every 2048th afterwards; `[PBF]` prints
on **every** failure. `[PUBS]` now carries `idc=<checks> idf=<failures>`.

Cost: two 4 KiB scans per asserted publish (`nz_sum` on both frames) plus one
`vm_page_eq`. At L2's 5357 exec pages that is ~44 MiB of linear reads over
1800 s — not measurable against the load.

## 5. The test that makes the assertion trip

`test_vm_publish_identity` (`src/kernel/vm_test.c`), called from
`vm_test_post_cache_suite()`. It builds the poisoned-cache state that reaches
the probe path and does both halves:

1. **the probe's identity describes the caller's buffer** — `id.buf == the frame
   passed in`, `id.off == 0`, `id.nz`/`id.sum` equal to a re-measure, and the
   buffer holds the fixture's pattern (`(i*7+3)&0xFF`), not zeros;
2. **a divergence is refused, not published** — the unit-only seam
   `vm_test_corrupt_next_publish(1)` zeroes 256 bytes of `dst` between the copy
   and the assertion. The test asserts `vm_test_id_fail()` grew by exactly 1 and
   that the page still ends up with the file's bytes (the device re-read);
3. **a clean publish counts a check and no failure** — second mapping, poisoned
   cache, no corruption: `idf` unchanged, `idc` grew, bytes correct.

```text
[VM-PBI] checks=6 fails=1 frames free0=1324007 now=1324007
```

## 6. Ladder

| run | tree | presents | `[XSW]` rows | max `fr_used` | `res_mib` | `[FUTR]` round rate | `Initializing.` | menu evidence | end |
|---|---|---:|---:|---:|---:|---|---|---|---|
| L1 | merged | 120 | 15 | 209 932 | 667 | 614 → 492 → **15/s** (t=329) | no | none: final frame static except the clock (`DIFF 75 px`, 100 % in the clock region) | parked, alive, no fault |
| L2 | merged | **1270** | 129 | **367 758** | 1329 | 2603 (t=94) … 2058 (t=431) → **8/s** (t=1778) | **yes** (OCR, shots 50-58) | **yes**: frame 60→70 changes 76.8 % of pixels, bbox `(2,34)-(1917,1069)`, 0.1 % in the clock region; OCR = the Steam API modal over the menu art; after that, clock-only | at the Steam modal, alive, no fault |
| L3 | fixed | **1180** | 121 | **465 513** | 1319 | 2603 (t≈94) … **8/s** (t=1763) | **yes** (OCR, shots 55-70) | **yes**: modal at 805,768 from shot ~72; frame 60→90 changes 76.7 % of pixels, 0.1 % in the clock region, OCR = the Steam API modal | at the Steam modal, alive, no fault |
| L4 | fixed | **1260** | 129 | **367 598** (`fr_high` 495 148) | 1330 | … **8/s** (t=1792) | **yes** | **yes**: modal at 805,768 in the final frame | at the Steam modal, alive, no fault |

### 6.1 The fixed tree: the same two shapes, plus the assertion rows

Runs 3 and 4 are the same harness as 1 and 2 on the fixed kernel. Neither parks
early (both reach `[XO] 1203`/`1208` and the menu), both cross both old hole
frames, and both carry the new rows:

```text
L3: [PUBS] n=778240 exec=5385 xzero=9 file_xzero=9 anon_xzero=1256 xunv=0 idc=4032 idf=0 | …
L4: [PUBS] n=774144 exec=5379 xzero=9 file_xzero=9 anon_xzero=1250 xunv=0 idc=4028 idf=0 | …
```

`idc` counts the publishes that went through the identity assertion
(`probeget` + `verify`); the page-cache shared path in
`vm_file_obj_materialize()` is checked by `vm_page_eq(priv, src)` instead, and
the anon/zero-fill paths have nothing to verify. `idf=0` in both windows: no
publish was ever refused because the bytes did not match the verdict.

`[PBI]` rows in the field (first 12, then every 2048th):

```text
[PBI] verify off=0x94000 probe_buf=0x21364000 src=0x21364000 dst=0x21362000
      frame=136036/136034 ver_nz=3442 ver_sum=0x5D9B6 src_nz=3442 dst_nz=3442
      dst_sum=0x5D9B6 same=1 buf=1 off=1 ver=1 pub=1
```

Both fixed-tree windows still show `[TXT] PROBE` = 0 and `xunv=0`: as in the
merged-tip windows, nothing needed rescuing, so the probe path was not entered.
The evidence that the probe path is now safe is the unit test (§5) and the
code shape (no shared buffer, no copy after the verdict), not a field
occurrence — stated here rather than implied.

### 6.2 The row that says "main menu"

```text
[XSW] n=1270 swap=1270 put=1270 putok=1270 flush=1377 crc=0x9bcb235cecf1c6d0
      changed=0 w=1920 h=1080 gx=2 gy=34 t=1719640 ready=1 nz=1947698/1950488
      box=0,0,1915,1017 copy=0 glflush=21
```

`nz=1947698/1950488` (2788 pixels zero) and a constant CRC: one static frame per
~12 s, with the modal's brown buttons on it
(`tools/rw/click-ignore.py` finds the left button at **805,768** from shot 70
onwards; it finds nothing in L1's final frame).

## 7. The remaining wall: the Steam API modal on the main menu

Window 2 gets further than any run in L73-L76: loader complete (`[XO] 1207`),
`Initializing.` on screen, then the **main menu** — and stops there because
RimWorld opens a modal Steam error and waits for a click:

```text
Could not initialize Steam API. Possible causes:
- Steam is not running.
- Launched from outside Steam without steam_appid.txt in place.
- Running with different privileges than Steam client (e.g. "as administrator").
In most cases, rebooting your computer will fix this problem. You can play
anyway, but no Steam stats will be recorded and Steam features won't be available.
                                                            [ Ignore ]   [ Quit ]
```

Rows that make it a wall rather than a pause:

* `[XSW] … changed=0` from `n=880` to `n=1270`, one present per ~12 s,
  `crc=0x9bcb235cecf1c6d0` constant — the client is idle, not loading.
* `[FUTR] t=886 rate=9/s … t=1778 rate=8/s` — the request/completion transport
  is idle (it was 2058/s at t=431 while the menu's data was still loading).
* `[THRS] pid=4 … live=25-40` — the process is healthy; nothing is missing.
* `tools/rw/click-ignore.py` locates the Ignore button (805,768) from shot 70
  on; `Enter` does not dismiss the box (measured when the tool was written).

The harness has no pointer injection (`rw_desktop_drive.py` sends keys through
the launcher's console protocol: F1/End/Up/Enter), so the click cannot be
delivered today. That is the next lane's first job, and it is a harness change,
not a kernel one: QMP `input-send-event` with an absolute pointer, or a keyboard
path that the modal accepts.

## 8. Regression

| gate | result | log |
|---|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **435 run, 0 failed — UNIT TESTS PASSED** (434 + `test_vm_publish_identity`) | `obj/l77-units2.log` |
| `bash tools/rw/l77-gates.sh` | **9/9 PASS** — redzone, mmap_race, m2_mono, m2_tp, m2_threads, m12, m3_window, m4_gl, rwnfs | `obj/l77/gates/*.log` |

The unit tier and the desktop harness were never run concurrently.

## 9. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l77-pubdiv
tools/rw/l77-ladder.sh 1 2 3 4          # 4 x 1800 s, sequential
python3 tools/rw/l77-summary.py obj/l77/serial-L2.log
python3 tools/rw/click-ignore.py obj/l77/shots-L2/99_final.png   # 805 768
tools/rw/l73-frame-diff.py obj/l77/shots-L2/60_running.png obj/l77/shots-L2/70_running.png
```

## 10. What the next lane should do first

1. **Click Ignore.** The game is at the main menu with a modal it cannot pass
   without a pointer event. `click-ignore.py` already finds the button; the
   harness needs a way to deliver the click (QMP absolute pointer).
2. **Watch the menu, not the loader.** Once the modal is gone the ladder's next
   rows are menu buttons (`New Colony` / `Load Game` / `Options`), which is a
   different present rate and a different input protocol from anything measured
   so far.
3. **Keep `idc=`/`idf=` on the `[PUBS]` row in the acceptance window.** `idf>0`
   is now the alarm; a window with `idf=0` and `xunv=0` is a window in which no
   executable page was ever published without its bytes being verified.
4. **One residual of the same class, not fixed here.** In
   `vm_file_obj_materialize()`, the *shared* exec path publishes the page-cache
   frame `src` after `vm_page_eq(priv, src)` and frees `priv`. The equality is
   taken immediately before `vm_arch_map()`, but the cache frame is not pinned
   across the two: an eviction between them would map a recycled frame. It did
   not fire in any window (`file_xzero` is 9 in every run, all `who=img`), but
   the assertion added here does not cover it — pinning `src` (refcount) or
   publishing the verified `priv` in that case is the clean fix.
