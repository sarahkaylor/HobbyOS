# Evidence — present's share of the frame on the venus path (t_0552cbd5)

Everything here is derived from counters that committed runs already wrote. **No new boot**,
no host clock, `renderer=venus` on every row. The report is `docs/nv-n9v-present-split.md`;
the parent analysis is `docs/nv-n9v-tail.md` (frame-time distribution, same 720p run).

## Inputs (tracked elsewhere, not duplicated)

| file | what it is | resolution |
|---|---|---|
| `docs/nv-n9v-tail-evidence/present-windows.csv` | 40 world-rate `[HBP]` windows of the 2026-11-24 N9v baseline run (29.19 avg / 18.2 1 %-low, tree 0ac70a7b) | 1280×720 |
| `docs/cp-bench-evidence/hobbyos-venus-720p/hbp.csv` | the same run's 152 `[HBP]` windows, all phases (launch/menu/bench/quit) | 1280×720 |
| `docs/cp-p7-evidence/game-venus2-menu.txt` | the 2026-11-17 p7-venus2 boot: title → main menu, `[X] [HBP]` rows with `set=`/`flush=` | 1920×1080 |
| `docs/cp-p7-evidence/f5-handback.txt` | the f5a boot: vkcube fullscreen, FIFO (a second 1080p surface through the same instrument) | 1920×1080 |
| `docs/cp-k7-evidence/thrw-hbv-world-strix.txt`, `docs/cp-present-evidence/*.txt` | 17 world-phase `[HBP]` summaries across boots — the spread of the flip term at a *fixed* size | 1280×720 |
| `docs/cp-hunt.md` (H1-09 + boot tables) | in-world `fpsprobe` medians at both resolutions (no `[HBP]` rows survive) | both |
| `src/kernel/virtio_gpu_x64.c` (`virtio_gpu_x64_scanout_flip`) | what the flip actually is: `SET_SCANOUT_BLOB` + `RESOURCE_FLUSH`, both waited for | — |

## Derived (this directory)

| file | what it is |
|---|---|
| `analysis.txt` | the console transcript: every table in the report's §2–§4 is a copy of a block here |
| `present-windows-720p-flight.csv` | per-window dump, 40 rows: `n fps path period_ms sub_ms wait_ms flip_ms present_ms path_ms set_ms flush_ms idle_ms` |
| `present-windows-1080p-menu.csv` | per-window dump, 19 rows, same columns (incl. the two 17 fps title windows) |
| `reproduce.sh` | regenerates `analysis.txt` and both CSVs from the inputs above |

## Reproduce

```
bash docs/nv-n9v-present-split-evidence/reproduce.sh \
  > docs/nv-n9v-present-split-evidence/analysis.txt
```

which is `tools/cp/bench/present-split.py` over the inputs. The tool's own checks:

```
python3 tools/cp/bench/test_present_split.py          # parsers + overlap arithmetic, synthetic
python3 tools/mesa/venus/test_hb_present_each.py      # the [HBPP] counter, compiled and driven
```

## What is *not* here

- **A per-frame sample dump.** None exists: `[HBP]` is a 2 s average, and no committed run
  set `HB_PRESENT_EACH=1`. The counter that produces one (one `[HBPP]` row per present,
  guest clock) is landed and tested; `present-split.py --hbpp` is its reader. The boot that
  writes it needs strix (this analysis box has no render node and no ssh credential).
- **Any 1080p *world* present number.** The only 1920×1080 `[HBP]` data is p7-venus2's menu
  at 44 fps; the 1080p world boots in `docs/cp-hunt.md` logged fps only.
- **Any host-native figure.** Nothing in this directory is a host measurement.
