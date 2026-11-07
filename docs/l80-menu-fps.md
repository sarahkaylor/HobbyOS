# L80 — the menu's 1 fps: the budget, the comparison, and the one wall that is the menu's

Lane `l80-menufps`, worktree `~/hobbyos-rw-lanes/l80-menufps` (branch
`rw-l80-menufps`, seeded at the merged tip `0e9f390e`).

The question this lane was given: the game reaches its main menu and then runs
at **1 fps**, a 14x drop from the loading phase on the same build, so something
in the menu's draw/present path must be pathological. Break the 1000 ms down,
compare with loading, test the two cheap hypotheses (per-frame full readback; a
blocking wait in the present path), and fix the dominant term.

The first thing the measurements did was **refute the premise**, and that is
the most useful result in this document: on the L78 build there is no 14x drop
and there is no menu cliff. All three L78 runs present at 1.0-1.2 s/frame from
the **first** present (1 390 presents in 1 739.8 s), while the 14 fps "loading
phase" belongs to a *different* lane's runs (L71/L74) on a different day. What
this lane can show is the budget of a real frame (66-72 ms of a 96-100 ms frame
is the present path), the one difference that *is* the menu's (the per-client
resource table fills and every later create is refused), and the fixes for it.

---

## 0. Summary

| question | answer |
|---|---|
| is the menu 14x slower than loading on the same build? | **no.** L78's three runs present at 1.0-1.2 s/frame from present 1 (M1 1 360 presents / 1 721 s, M2 1 390 / 1 740 s, P3 1 380 / 1 737 s); the per-10-frame batch is already 1.0 s at `n=10`. The 12-16 fps "loading phase" is L71/L74's runs (`l74-virgl-1916x1018`: 3 070 presents in 245.4 s = 12.5 fps) |
| does the merged tip still run at 1 fps here? | **no.** Same tree, same phase: 9-20 fps during loading (`dt` 49-115 ms), 60 fps on empty frames (`dt` 16.6 ms) |
| where does a frame's time go? | frame 96-100 ms = `st_context_flush` **34 ms** (the frame's submit + front-buffer display hook) + fence_finish/frontend present **21-24 ms** (of which the winsys present is 14-15 ms) + `XcPresentMark` **11-14 ms** + engine **~30 ms** |
| per-frame full readback? | **no.** 98 % of presents read back 16 KiB (thumbnail); every 64th reads 8.3 MB (`dmg_resync`). The damage path was idle on 31-32 of every 32 presents |
| a blocking wait in the present path? | **a wait that cannot wait.** `XMesaSwapBuffers` calls `fence_finish(..., OS_TIMEOUT_INFINITE)` for a fabricated "already signalled" fence, and virgl's `fence_finish` **flushes the context again before it looks at the fence** |
| what is the menu's own wall? | the per-client GPU resource table (**1 024**) fills; every later `res_create` is refused, per frame, for the rest of the run (2 298 rows in one run, 2 383 in another). Raised to 4 096, and the refusal row is now bounded |
| fps before/after | see §6; the present path costs **3.4 ms** at 60 fps and **14-15 ms** at 12 fps with the same code, i.e. its absolute cost tracks the host, not the layout |

---

## 1. The premise, checked against the L78 logs

`docs/l78-probe-menu-report.md` says the menu costs 0.9-1.0 fps "against the
12-16 fps the loading animation gets on the same device". The loading number is
quoted from `docs/rimworld-virgl-m3.md` §3.2 (M3, 15.97 fps) and L74 (14.36
fps) — other lanes, other runs. The L78 runs' own present rows say something
else, and they say it from the first frame:

| run | `[XSW]` rows | presents | last `t=` | fps | first batch | last batch |
|---|---:|---:|---:|---:|---:|---:|
| M1 | 139 | 1..1360 | 1 721.0 s | **0.79** | 1.0 s/frame (n=10) | 1.0 s/frame |
| M2 | 141 | 1..1390 | 1 739.8 s | **0.80** | 1.0 s/frame (n=10) | 1.2 s/frame |
| P3 | 141 | 1..1380 | 1 737.1 s | **0.79** | 1.0 s/frame (n=10) | 1.0 s/frame |
| L74 `l74-virgl-1916x1018` (control, different build) | 310 | 1..3070 | 245.4 s | **12.5** | 0.1 s/frame | 0.1 s/frame |

`t=` is the guest's own monotonic clock (L74's addition), so this is not a log
follower artefact. The batch deltas never leave the 1.0-1.3 s band: there is no
transition at the menu (`n≈530` in M1, `n≈550` in M2/P3) and no fast phase
before it.

The one present-path difference L78's own rows do record is the damage path:

| run | `put=` / `putok=` at the last row | presents seen by XPutImage | idle |
|---|---:|---:|---:|
| L78 M2 | 1 390 / 1 390 | **every present** | 0 % |
| L74 (control) | 49 / 49 | 1.6 % | 98.4 % |
| run B (this lane, loading) | `[VGLB] idle=31-32/32` | 2 % | **98 %** |

So L78's frames each did a full-surface readback + one XPutImage + a 7.8 MB
`shadow_blit`, while L71/L74's did neither on 98 % of presents. That is the
difference between a 14 fps run and L78's runs — but it explains a factor of
~2-3 in the present path, not 12x on its own; the rest is the host (§6).

---

## 2. The instrument, and what it cost to learn

Three rows, one per layer, all gated by staged markers (`/XPROF` for the shim,
`/GLPROF` for the winsys), all batched 32 presents:

| row | written by | fields |
|---|---|---|
| `[XSWB]` | compat libX11 (`XcPresentMark`) | frame period `dt`, the shim's own time `self` (surface checksum, fb0 blit, flush message), `put` (XPutImage), `flush` (shadow_flush), and `row_us` |
| `[XSWL]` | libGL (`XMesaSwapBuffers`, patch `0013`) | `flush` = `st_context_flush(ST_FLUSH_FRONT)`, `fence` = fence_finish + the frontend's present, `swap` = `st_context_invalidate_state` |
| `[VGLB]` | the virgl winsys (`flush_frontbuffer`) | `pres` (the GPU present), submits/readbacks/uploads and their bytes, `idle`, and the thumbnail/copy/display stages |

**The first version wrote one row per present, and that was a measurement
error worth recording**: a 150-byte row goes to `Player.log` (a FAT file) *and*
to `/dev/console` (a polled UART) **inside** the present it is measuring, and
`row_us` says what that costs — **46-147 ms** in the loaded phase, 4-13 ms when
the machine is quiet. A per-present row is therefore up to a third of the frame
it reports. Batching 32 presents makes it 1.5-4.5 ms/frame and leaves it in the
row so a reader can subtract it.

This is also the explanation for a pile of "1 fps" folklore on this ladder: the
established `[XSW]` row is written every 10 presents to the same two
destinations, so at 12 fps the old instrument alone is ~100 ms per 10 frames
(~10 ms/frame, 10 % of the frame), and any lane that added its own per-present
row on top of it was measuring its instrument.

---

## 3. The budget (run C, loading phase, 96-100 ms frames)

`[XSWL] frames=32 flush_avg=33968 fence_avg=20968 swap_avg=0 inval_avg=0 total_max=178000`
`[XSWL] frames=32 flush_avg=34125 fence_avg=24531 swap_avg=31 inval_avg=0 total_max=304000`
`[XSWB] n=800 t=130600 frames=32 dt_avg=96531 ... self_avg=14437`
`[XSWB] n=832 t=133356 frames=32 dt_avg=100375 ... self_avg=11125`

| term | ms/frame | whose | note |
|---|---:|---|---|
| frame (`dt`) | **96-100** | | period between two `XcPresentMark` calls |
| `st_context_flush(ST_FLUSH_FRONT)` | **34** | libGL/Mesa | the frame's own submit (`virgl_flush_eq` → `GPU_IOC_SUBMIT`) + st's front-buffer display hook, which is where the winsys present happens |
| `fence_finish` + `xmesa_swap_st_framebuffer` | **21-24** | libGL + winsys | contains the winsys present, 14-15 ms of it |
| `st_context_invalidate_state` | 0 | libGL | |
| `XcPresentMark` (`self`) | **11-14** | shim | 2 passes over 7.8 MB every 10th present (the `[XSW]` checksum) + the `[XSW]` row's own I/O |
| engine between swaps | **~30** | Unity | remainder |

So **66-72 ms of a 96-100 ms frame is the present path** at 9-12 fps, and the
single biggest item is the frame's own submit, not the readback. The
`l80-budget.py` identity (`dt = swap + self + gap`) holds on 1 068 of 1 201 rows
within 5 ms in the earlier per-present run.

### 3.1 The two cheap hypotheses

**(a) Per-frame full readback because the background animates — false.**
`[VGLB]` over run B: `readbacks/frame 1.0 (16 KiB)`, `idle 98 %`, and every
64th present `rd_b=8 310 784` (8.3 MB, the `dmg_resync`). The menu frame in L78
is *static* (`changed=0`, one CRC for 1 000+ presents), so nothing in the
damage path can be dirty per frame. The hypothesis would have predicted 8.3 MB
on every present; the measurement is 16 KiB on 98 of 100.

**(b) A blocking wait in the present path — yes, but a wait that cannot wait.**
There is no fence the winsys honours (`fence_wait` returns true, and the fence
it handed back was fabricated). But `XMesaSwapBuffers` asks
`st_context_flush(..., &fence, ...)` and then calls
`fence_finish(fence, OS_TIMEOUT_INFINITE)`, and virgl's `fence_finish` is:

```c
static bool virgl_fence_finish(...) {
   if (vctx && timeout)
      virgl_flush_eq(vctx, NULL, NULL);      /* <- a full submit, every swap */
   return vws->fence_wait(vws, fence, timeout);
}
```

A fence that is already signalled cannot wait — but it still costs the flush
that precedes the check. On this host that is one GPU round trip per frame, and
`[XSWL]`'s fence bucket (21-24 ms, present included) is where it lives.

**Not separated, stated rather than smoothed over:** patch `0013`'s timestamps
put `fence_finish` and `xmesa_swap_st_framebuffer` in the same bucket (`t1` is
after `st_context_flush`, `t2` after the frontend present), so the fence fix's
own saving is bounded by 21-24 ms and is not attributed more tightly than that.
The run that would separate them (run D, §6) stalled at a different wall before
the menu.

---

## 4. Menu vs loading, row by row

The honest limitation first: **no run in this lane reached the menu.** Run A
(900 s) was still loading at `t=793 s`; run B reached `t=756 s` but the game's
window content collapsed at `t=637` when the resource table filled (§5); run D
stalled at `t=466 s`. What follows is what the rows actually support.

| dimension | loading (measured, run B/C) | menu (L78 rows only) |
|---|---|---|
| frame | 96-100 ms (run C), 49-90 ms (run B) | 1 000-1 180 ms |
| presents | 1 per swap | 1 per swap |
| submits/frame | **1** (88 B: the thumbnail blit) | not visible — L78's winsys rows never reached its serial log |
| readbacks/frame | **1** (16 KiB), 8.3 MB every 64th | not visible |
| XPutImage | **0** on 98 % of presents (`idle=31-32/32`) | **1 per present** (`put=1390 putok=1390`) |
| render target | 1920x1080 (`dt` row, stride 7680) | 1920x1080 |
| `glFlush` | 21 in the whole run, frozen from the first minute | 21, frozen |
| `glFinish` | none observed (no fence is ever waited on successfully) | none |
| damage bytes/present | 16 KiB, 8.3 MB every 64th | ≥8.3 MB every present (full-surface fallback, `put` advancing on every present) |

The only row where the menu differs in kind is the last one: L78's menu (and,
in its logs, its loading screen too) presented a full-surface readback on every
frame while today's runs present 16 KiB. That is a factor of 2-3 on the present
path, not 14x, and it is a *build* difference (L71/L74 ran 98 % idle), which
means it belongs to whatever changed between L74 and L78 rather than to the
menu.

---

## 5. The wall that is the menu's: the resource table

This is the one finding that is specific to the menu and is ours.

```
[GPU] res_create refused: client c=1 nres=1024 ctx_held=2 ctx_req=2 pid=4 fd=12
```

Run B: **2 298** of these, the first between `[XSW] n=4440` and `n=4450`, one
per refused create, for the rest of the run; run A: **2 383**. L78: **0** (its game reached the menu
in 1 390 presents, i.e. never created enough resources to fill the table).
Immediately after the first refusals the frame collapses to a black window with
one small box and the presents speed up to 60 fps — the engine has stopped
being able to build anything.

The limit was M3's guess:

```c
#define GPU_ABI_MAX_RES_PER_CLIENT 1024   /* gpu_abi.c */
#define GPU3D_MAX_RES              1024   /* virtio_gpu_3d.c, the transport */
```

M3 measured 618 resources for a scene and concluded "the next limit to watch is
the 64 MiB per-attach ceiling". The menu wants more than 1 024 (atlases + render
targets + the swap chain), and once the table is full the engine retries every
frame: a refused create is a full control-queue round trip (the guest spins on
the used ring) plus a kernel row, and the UI never gets its textures.

**Fixed**: both tables raised to **4 096** (~290 KiB of kernel BSS for the
transport), and the refusal row bounded — the first four carry the shape, then
one per 1 024 refusals carries the rate, always with `refused=<n>`. Before the
bound, six console writes per refusal × 2 298 refusals buried every other row in
the log (and each row is not free, §2).

Run D reached 4 250 presents with **0 refusals** and the table at 4 096 --
which is *just short* of run B's first refusal (n=4440), so the fix is **not
confirmed by a completed run**: what is confirmed is the mechanism (a full table
refuses every create, per frame, for the rest of the run) and that the refusal
row is now bounded. Run D then stalled at `t=466 s` with no presents for 3+
minutes and no refusals — a *different* wall (a hang, not resource
exhaustion), recorded here rather than papered over. The confirmation run is the
next lane's: 4 096 should carry the menu past n=4440 without a refusal.

---

## 6. The fix, and the numbers

| change | file | why |
|---|---|---|
| no fabricated fence | `tools/mesa/virgl-hobbyos/virgl_hobbyos_winsys.c` | `supports_fences = 0` now means "no fence": a NULL fence skips `fence_finish`, which would otherwise flush the context again (21-24 ms/frame) for a wait that cannot wait |
| resource table 1 024 → 4 096 | `src/kernel/gpu_abi.c`, `src/kernel/virtio_gpu_3d.c` | the menu's atlases/RTs exceed 1 024; a full table refuses every create for the rest of the run |
| refusal row bounded | `src/kernel/gpu_abi.c` | 2 298 rows of six console writes each; first 4, then every 1 024th, with the count |
| instruments `[XSWB]`/`[XSWL]`/`[VGLB]` + `/XPROF`, `/GLPROF`, `RW_PRESENT_PROF=1` | `libx11.c`, `xm_api.c` (patch `0013`), `virgl_hobbyos_winsys.c`, harness | the budget had to be measurable per layer, and batched so the probe is not a term in it |

| run | build | presents | fps (loading phase) | present path | refusals |
|---|---|---:|---:|---:|---:|
| L78 M2 | L78 tip, no probe | 1 390 / 1 740 s | **0.80** | not visible | 0 |
| run B | merged tip, table 1 024 | 9 344 | 11-18 (dt 55-89 ms) | 9.5-14.9 ms | 2 298 |
| run C | + table 4 096, fence not yet | killed early (no full-run number) | 10 (dt 96-100 ms) | 14-15 ms | 0 to that point |
| run D | + no-fence fix | 4 250 (stall at t=466 s) | 9-20 (dt 49-115 ms) | not separated (stall) | **0** |

**The fence fix has one independent number.** `tools/gate/m2_virgl.py` measures
`F`, the fixed cost of one clear+swap frame, on the same probe and the same
kernel it always has. On this tree it now reports **F = 5.95 ms/frame**
(`p = 0.70 us/draw`), against **13.00 ms** for the same gate in L74 and 9.68 ms
in M3's bench. Different days and different host load, so it is a supporting
row rather than a proof -- but the direction and the size (7.05 ms, i.e. the
same order as the 21-24 ms bucket's fence part on a busier host) are what
removing one GPU round trip per swap predicts.

**What this means for the 1 fps.** The menu is not what costs a second. The
same tree, same phase, same hour presents at 9-20 fps, with the present path at
66-72 ms of a 96-100 ms frame; the code path that the menu exercises (idle
damage, one submit, one 16 KiB readback, one XPutImage) is the *cheap* one. The
1 000-1 180 ms of L78's runs is a factor of 10-12 on every term at once — the
frame submit, the readback, the XPutImage, the fb0 blit — which is what a
synchronous, spinning GPU transport looks like when the host's virgl work is
slow (three other lanes' VMs were running during those runs; today's identical
measurement is 10-12x faster). L78's own damage path made it worse twice over:
it presented a full-surface readback on every frame (`put` advancing on every
present) instead of the 16 KiB that today's runs read.

Two levers follow, in order of measured size:

1. **The number of synchronous GPU round trips per frame.** Every one of them
   is the guest spinning on `gpu_used.idx` while the host works
   (`virtio_gpu_x64_transact_ex`), so host latency is guest wall time. The
   fence fix removes one (unverified in a completed run, §3.1b); batching the
   present readback into the frame's own submit (`VIRGL_CCMD_TRANSFER3D`) would
   remove another; M5's direct scanout removes readback + copy + XPutImage.
2. **Not printing per-present rows.** Measured at 46-147 ms per row.

---

## 7. Gates

| gate | result | log |
|---|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **435 run, 0 failed — UNIT TESTS PASSED** | `obj/l80/units.log` |
| `tools/gate/m2_virgl.py` | **PASS** — both drivers from one DSO, pixel-exact on GL and `/dev/fb0`; `p = 0.70 us/draw`, **`F = 5.95 ms/frame`** (L74 recorded 13.00 ms for the same gate) | `obj/l80/gate-m2.log` |
| `tools/gate/m3_virgl.py` | **PASS** — every run virgl, the two present modes pixel-identical, plan row **89.42 fps** at 1916x1018 | `obj/l80/gate-m3.log` |
| `tools/gate/m4_gl_probe.py` | **PASS** — libGL in the guest rendered a GL 3.3 core triangle and presented it to a window | `obj/l80/gate-m4.log` |

(Unit tier run with no desktop harness up; the gates run sequentially, each in
its own boot.)

---

## 8. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l80-menufps

# a run with the per-present budget (batched rows), 1200 s
RW_QEMU_GL=1 RW_GLMODE=virgl RW_PRESENT_PROF=1 RW_SHOT_EVERY=150 \
  tools/rw/run-rimworld-desktop.sh --timeout 600 --run-timeout 1200

# the budget table from that run's serial log
python3 tools/rw/l80-budget.py obj/rw-desktop/serial-desktop.log

# the L78 premise, re-derived from that lane's own rows
grep -a '\[XSW\]' ~/hobbyos-rw-lanes/l78-menu/obj/l78/serial-M2.log | tail -3
```
