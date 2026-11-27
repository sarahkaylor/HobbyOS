# SWARM-REPORT-WP-WPRBUS — the WPR/booter hand-off publishes bus addresses only

Card: `t_ac4c7697` (follow-up of `t_b6fc285d`). Branch `nv/wp-booter-bus`,
base `nv/integ-swarm 2fc2d17c`.

## Verdict

The four addresses `nv_gsp_firmware_check()` hands to SEC2/the booter — the
radix3 root, the radix3 PTEs, the bootloader's data region and the ad10x
signature — were built from `.phys`. They are now bus values:

- the radix3 root is `nv_dma_bus()` of the tree's own `nv_dma_alloc()` mapping;
- every tree entry is `image_bus + page offset` or `tree_bus + page offset`;
- the bootloader field is `loader_bus + 0x6C` and the signature
  `img_bus + sig_off` — one addition on a bus base with a file offset;

plus a fifth one the card did not list: the SEC2 xfer engine's **source**
address (`d->booter_phys` → `d->booter_bus`), which is how the booter image is
DMA-read out of system memory.

`make wpr_bus_provenance` (new, a `host_tests` prerequisite) is green; the two
mutations it exists for turn it red (5 violations) and back (transcript below).
Unit tiers on the final tree: **intel 607 run / 0 failed**, **ARM 485 / 0**
(the +1 over the 606 baseline is the new `test_nv_radix3_build_alloc` case).

No boot was run — this card changes address provenance only, and today
`bus == phys`, so a boot cannot distinguish the two. The runtime rows that would
show the change are named under "What a boot would print".

## What was walked

`src/kernel/nv_gsp.c` `nv_gsp_firmware_check()` (blob loads, ELF section map,
the `.fwimage` slide, the layout, the radix3 build, the 256-byte metadata block,
SEC2 stage 5 and the teardown) and the helpers it drives:

- `src/kernel/nv_fw.c`: `nv_fw_load_blob()`, `nv_radix3_build_into()`,
  `nv_radix3_build()`, `nv_fw_release()`;
- `src/include/nv_fw.h`, `src/include/nv.h`: the blob/tree/boot-path types.

`nv_gsp_sec2_start_booter_ex()` was read for its DMA source (`base`), which is
the only other place a booter byte reaches the device.

## Changes

| File | Change |
|---|---|
| `src/include/nv_fw.h` | `struct nv_fw_blob` carries `struct nv_dma_map map` instead of `phys`; `nv_fw_blob_bus()` is the one accessor (`nv_dma_bus(&b->map)`). `nv_radix3_build_into()` gains `tree_bus`/`image_bus`; new `struct nv_radix3_tree { map; root_bus; }`; `nv_radix3_build()`/`nv_radix3_release()` retyped around it. |
| `src/kernel/nv_fw.c` | Tree entries written from `image_bus`/`tree_bus`; `nv_radix3_build()` allocates the tree with `nv_dma_alloc()` and returns `root_bus`; `nv_fw_load_blob()` allocates the file's frames with `nv_dma_alloc()`; `nv_fw_release()` frees the mapping. |
| `src/kernel/nv_gsp.c` | `img_bus`/`loader_bus` from `nv_fw_blob_bus()`; the three `m.sysmem_addr_*` fields and the radix3 build use them; `d->booter_bus` replaces `d->booter_phys` and feeds the xfer engine's source; the tree is owned from the build to the teardown (a failed layout used to leak it). |
| `src/include/nv.h` | `nv_dev`: `booter_bus`, `struct nv_radix3_tree radix3`. The five never-populated `nv_info` boot-path fields are marked legacy instead of moving the ABI. |
| `src/kernel/nv_fw_test.c` | `test_nv_radix3_build` now uses `tree_bus`/`img_bus` deliberately unequal to the phys bases and asserts no entry is a phys value; new `test_nv_radix3_build_alloc` (allocation, `root_bus == nv_dma_bus(&map)`, `-ENOTSUP` under translation); the failed-load case asserts the mapping is empty. |
| `tools/nv/check_wpr_bus_provenance.py` | New guard (sibling of `check_rpc_bus_provenance.py`), 6 rules, `--gate`. |
| `tools/nv/demo-wpr-bus-guard.sh` | Break/revert demonstration, reproducible. |
| `Makefile` | `wpr_bus_provenance` target, `host_tests` prerequisite (placed with the other host-tier run targets, not beside the RPC guard's stanza, so the two branches do not conflict in one line). |
| `docs/nv-wpr-bus-provenance.md` | The address table, the fix shape, the guard rules, the residual items. |
| `evidence/nv/wpr-bus-guard-demo.txt`, `evidence/nv/wpr-bus-unit-tier-{intel,arm}.log` | Evidence. |

## Gates

`tools/nv/check_wpr_bus_provenance.py --gate` (green):

```
$ make wpr_bus_provenance
python3 tools/nv/check_wpr_bus_provenance.py --gate
nv wpr bus provenance: ok -- 3 metadata field(s), 3 tree store(s), 5 device-facing call(s), 6 allowed CPU-side .phys use(s), 0 raw phys
```

The six rules: (A) no `m.sysmem_addr_*` assignment built from `.phys` and every
one spells a bus value; (B) every `.phys` in the region is one of three CPU-side
forms (`phys_to_virt`, a log row, the `img_phys` local the log uses); (C) no
device-facing call takes a `.phys` argument; (D) the seam still reads
`nv_fw_blob_bus(&rm)`/`(&loader)`, publishes `tree.root_bus`, `loader_bus + 0x6C`,
`img_bus + sig_off`, and the SEC2 source is `d->booter_bus`; (E) no `fw_put64()`
writes a `phys` expression into the tree; (F) `nv_fw_load_blob()` allocates
through `nv_dma_alloc()`, `struct nv_fw_blob` has no bare `phys` member, and
`struct nv_dev` carries `booter_bus` / the tree mapping.

### The guard fails when a raw phys is reintroduced (demonstrated)

`bash tools/nv/demo-wpr-bus-guard.sh` mutates two lines — the signature field
built from `rm.map.phys`, and a level-2 PTE built from `image_phys` — runs the
gate, restores both files and runs it again. Real transcript
(`evidence/nv/wpr-bus-guard-demo.txt`):

```
$ make wpr_bus_provenance   # expect FAIL
python3 tools/nv/check_wpr_bus_provenance.py --gate
nv wpr bus provenance: FAIL (5 violations)
  src/kernel/nv_gsp.c:4889: m.sysmem_addr_signature = sig_off ? (rm.map.phys + sig_off) : 0; -- a metadata field the BOOTER reads is built from phys; it must be a bus value (nv_dma_bus) plus an offset inside that mapping
  src/kernel/nv_gsp.c: seam expression not found: 'm.sysmem_addr_signature = sig_off ? (img_bus + sig_off) : 0;' -- the WPR/booter glue must publish the nv_dma_bus() answer; update this guard with the new expression
  src/kernel/nv_gsp.c:4889: m.sysmem_addr_signature = sig_off ? (rm.map.phys + sig_off) : 0; -- .phys outside the three CPU-side uses; a device-visible address must come from nv_dma_bus()
  src/kernel/nv_fw.c: nv_radix3_build_into(): fw_put64(... phys ...) -- a tree entry is an address the DEVICE walks; it must be image_bus/tree_bus plus a page offset
  src/kernel/nv_fw.c: seam expression not found: 'image_bus + i * NV_RADIX3_PAGE' -- the tree must hold bus addresses and the blobs must be nv_dma mappings; update this guard with the new expression
make: *** [Makefile:4163: wpr_bus_provenance] Error 1
exit=2
$ # both files restored
$ make wpr_bus_provenance   # expect ok
nv wpr bus provenance: ok -- 3 metadata field(s), 3 tree store(s), 5 device-facing call(s), 6 allowed CPU-side .phys use(s), 0 raw phys
exit=0
```

### Kernel unit tiers

Both run with the tree's own scripts, `TOOLS_SKIP=1` (`run_unit_tests_intel.sh`
also with `SKIP_KVM=1`, i.e. TCG, because `/dev/kvm` is root:kvm 660 and this
uid is not in the group):

- intel: `Tests run: 607`, `Tests failed: 0`, `UNIT TESTS PASSED`
  (`evidence/nv/wpr-bus-unit-tier-intel.log:3036-3038`);
- ARM: `Tests run: 485`, `Tests failed: 0`, `UNIT TESTS PASSED`
  (`evidence/nv/wpr-bus-unit-tier-arm.log:2453-2455`).

The new rows in the intel log (`evidence/nv/wpr-bus-unit-tier-intel.log:2073`, `:2087-2088`):

```
[NVFW] firmware parser suite
  Running test_nv_radix3_build...
  Running test_nv_radix3_build_alloc...
  Running test_nv_fw_load_missing...
```

`test_nv_radix3_build` would fail on the pre-card code: it passes
`tree_bus = 0x40000000` / `img_bus = 0x50000000` against
`tree_phys = 0x20000000` / `img_phys = 0x30000000` and asserts the entries hold
the bus values (and, explicitly, that they are not the phys ones).

## What a boot would print

Not run here. A boot on this branch would show, per blob,
`[NV-DMA] map phys=… bus=… pages=… identity=1` (three blobs + the tree), the
`[NV-GSP] radix3 root_bus=… pages=… image pages at bus=… phys=…` row (replacing
`root=…`), `[NV-GSP] fw … phys=… bus=…`, and the unchanged
`… block_phys=… block_bus=…` pair. With translation off, `bus` equals `phys` in
every one of them — which is exactly why this card's gates are structural.

## Residual items

- SEC2 helper parameters were still spelled `meta_phys` on the lane this branch
  was cut from (`nv_gsp_sec2_start_booter_ex`, `sec2_attempt`,
  `struct sec2_img`). **Resolved by the fold**: `nv/wp-rpcpub`'s `e55a922d`
  renamed them, and the merged tree spells the parameter
  `const struct nv_dma_map *meta_map` and the field `uint64_t meta_bus`
  (`src/kernel/nv_gsp.c:3893`, `:4224`). `grep -rn meta_phys src/kernel/` is
  empty; the only survivor is the legacy `gsp_meta_phys` ABI word in `nv.h`.
  The report's original reason to defer — a 15-hunk conflict with the r1 rig
  branch — described a tree this card no longer merges.
- `struct nv_info`'s five boot-path fields are dead ABI space (never filled by
  `nv_fill_info()`); marked legacy, not moved (`src/include/nv.h:317-325`).
- `nv_fw_load()` (pointer-only form) does not keep its mapping and is released
  with `nv_fw_release_buf()`; documented in the header as parse-only
  (`src/include/nv_fw.h:449-457`).
- `nv_fw_load_blob()` now zeroes the frames it allocates (an `nv_dma_alloc()`
  property) before the FAT read overwrites them: ~38 MB of byte stores per boot
  for the RM image. That is the cost of one allocation path; measured build and
  tier times are unchanged in the noise.

## Fold and re-apply (t_48c2e9f8, 2026-11-27)

This change was written on `nv/wp-booter-bus` (tip `00cabf91`, based on
`nv/integ-swarm 2fc2d17c`). That lane's scratch tree was removed when
`t_ac4c7697` closed and no remote ref carried the code — but the branch object
survived in the `/home/nvcard/work/HobbyOS` clone, so this re-apply is the
original commit's diff moved onto the merged integration tip, not a
re-derivation from this document. Only one file conflicted.

The integration history it lands on:

```
d9509877 fix(nv-arm-reloc): record arg_names 10 -> 12, the RPC args table nv/wp-rpcpub grew
32a4f8a3 feat(nv-gsp): WPR/booter metadata and radix3 publish bus addresses, not phys
fbfc903b Merge branch nv/wp-rpcpub into nv/integ-swarm      (2fc2d17c + 32cf0ea1)
```

### The six hunks, and how they resolved

All six were in `nv_gsp.c`, and all six are the same seam: `nv/wp-rpcpub`
(`e55a922d`, `32cf0ea1`) had replaced the same four values with its own
run-accessor arithmetic, so the two sweeps met on identical lines.

| Hunk | `nv/wp-rpcpub` side | kept |
|---|---|---|
| 1 | `base = d->booter_phys; base_bus = nv_dma_bus_of_run(base, len)` | `base = d->booter_bus;` — the blob owns a mapping, so there is no run to resolve. The two `falcon_dma_block()` call sites now pass `base`, and the `NV_DMA_BUS_INVALID` guard stays with its message corrected to name `nv_fw_blob_bus()`. |
| 2 | `img_bus = nv_dma_bus_of_run(rm.phys, rm.pages * PAGE)`, plus a second `loader_bus` declaration and a "bus FAIL" suppression | `img_phys = rm.map.phys; img_bus = nv_fw_blob_bus(&rm);` — `loader_bus` already comes from `nv_fw_blob_bus(&loader)` in an earlier hunk; the duplicate declaration is gone. A refused bus now surfaces as `nv_radix3_build()`'s `-ENOTSUP`, so nothing is published. |
| 3 | `nv_radix3_build(img_bus, size, &root, &tpages)` | `nv_radix3_build(img_phys, img_bus, size, &tree)` — both ends of both mappings; the entries hold `tree_bus`/`image_bus`. |
| 4 | `root_bus = nv_dma_bus_of_run(root, tpages * PAGE)` | `tree.root_bus` — resolved once inside `nv_radix3_build()` from its own `nv_dma_alloc()` run. |
| 5 | `" image pages at "` | `" image pages at bus=… phys=…"` — the row prints both views. |
| 6 | `m.sysmem_addr_radix3_elf = root_bus`, `m.sysmem_addr_bootloader = loader_bus + 0x6C` | `tree.root_bus`, and `0x6C` published only when the loader mapping exists. |

The teardown (`d->radix3 = tree` … `nv_radix3_release(&d->radix3)`) merged
without a conflict and replaces the lane's `radix3_root`/`radix3_pages` pair.

`nv_dma_bus_of_run()` is **not** removed by this resolution: eight call sites
remain for the raw frame runs that have no `struct nv_dma_map` (FWSEC staging,
IMEM/DMEM scratch, landing pages) — it is the accessor for those, exactly as
`nv_fw_blob_bus()` is for blobs. Both are `nv_dma_bus()` in the end.

### The AArch64 link gate caught the fold

`nv/wp-rpcpub`'s own gate ran the intel tier only. The first ARM link of the
merged tree failed the folded-table inventory:

```
hobbyos.elf: FAIL -- folded table(s) not in the audited inventory [MODE=unit_tests]:
  GREW  arg_names   10 -> 12 word(s)  .rodata
```

`e55a922d`'s args page added the `magic` and `size` fields to `nv_rpc.c`'s
`arg_names[]`. That is the gate's documented case (b): the table is unchanged
in kind, two names were added to the population. The baseline was regenerated
from **each mode's own linked image** with `--update-baseline` (unit_tests
21 tables / 426 words / 431 pointer words; desktop and test 19 / 420 / 425) and
`docs/nv-arm-reloc.md` §4 carries the `12 / 12` row and the new totals with the
reason. `d9509877`.

### Tiers on the final tree (`d9509877`)

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=1800 ./run_unit_tests_intel.sh
  Tests run:    611   Tests failed: 0   UNIT TESTS PASSED

TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh      # ARCH=arm
  Tests run:    486   Tests failed: 0   UNIT TESTS PASSED
```

Intel's 611 is `nv/wp-rpcpub`'s 610 plus this branch's
`test_nv_radix3_build_alloc`; ARM's 486 is `nv/wp-rpcpub`'s 485 plus
`test_nv_dma_bus_of_run_for_borrowed_frames` (the only test name the merged ARM
log adds over this branch's evidence log). `make wpr_bus_provenance` is green
on the merged tree:

```
nv wpr bus provenance: ok -- 3 metadata field(s), 3 tree store(s), 5 device-facing call(s), 6 allowed CPU-side .phys use(s), 0 raw phys
```

`bus == phys` on this hardware, so none of this is a boot result: the tiers and
the guard are structural gates, and the runtime row that would distinguish the
two is still the boot log's `[NV-DMA] map phys=… bus=… identity=1` plus
`radix3 root_bus=…`.
