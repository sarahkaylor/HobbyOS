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

- SEC2 helper parameters are still spelled `meta_phys`
  (`nv_gsp_sec2_start_booter_ex`, `sec2_attempt`, `struct sec2_img`). The value
  is already `nv_dma_bus()`'s; the name is a naming defect only, and those
  functions are the ones the r1 rig branch rewrites (15 hunks) — renamed there,
  not here, to avoid a conflict the merge does not need.
- `struct nv_info`'s five boot-path fields are dead ABI space (never filled by
  `nv_fill_info()`); marked legacy, not moved.
- `nv_fw_load()` (pointer-only form) does not keep its mapping and is released
  with `nv_fw_release_buf()`; documented in the header as parse-only.
- `nv_fw_load_blob()` now zeroes the frames it allocates (an `nv_dma_alloc()`
  property) before the FAT read overwrites them: ~38 MB of byte stores per boot
  for the RM image. That is the cost of one allocation path; measured build and
  tier times are unchanged in the noise.
