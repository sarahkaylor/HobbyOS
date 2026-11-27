# SWARM-REPORT-GB202-FMCHARVEST — the FMC-abort harness (modes 63–66) reaches `nv/integ-swarm`

Card: `t_b93aa001`. Branch: `nv/gb202-fmc-harness`, cut from `origin/nv/integ-swarm`
(`070900b4`, 2026-11-27 20:15Z) and folded as a merge commit. **Zero boots**: every number
below is a file measurement or a unit-tier run on this box, not a new card measurement.

## 0. What this card found on disk (and why the fold is wider than the card text)

The harness was a **non-git copy** at
`/srv/hermes-kanban/boards/hobbyos-nv/workspaces/t_a769beaa/tree` (`git status` there: `fatal:
not a git repository`). The card text named four files. Measured against `070900b4`, the copy is
a *stale-base* working tree (~48 tracked files differ from the tip), so "copy the four files" would
have reverted the WPR/booter-bus work (`src/kernel/nv_gsp.c` at the tip is 5,564 lines with the
per-instance transport; the copy is 6,877 lines on an older lineage).

Worse: the harness is not four files. The whole FSP rung it sits on **also exists on no ref** —a
check of every local and remote branch for the symbols below returns nothing:

```
$ git grep -l "nv_fsp_cot_probe\|int nv_pfsp_probe\|enum nv_falcon_state\|sha384" \
    $(git for-each-ref --format='%(refname)' refs/heads refs/remotes) -- src
(no output)
```

So the fold carries the harness *and* the minimum support it needs to compile and run, ported onto
the tip's APIs. Every adaptation is listed in §4 so the next reader can tell harness code from
fold glue.

## 1. What landed

| file | state | what it is |
|---|---|---|
| `src/kernel/nv_fsp_cot.c` | new, 3,102 lines | the COT rung: modes 1…66; blob table; `rm_cms2_log()`; `fmcerr` rows. Copied from the lane tree's `nv_fsp_cot.c` (`bc7c09eb`) with the mode-66 delta from `t_2e98aaee` applied (result `752f088d`, the parent's `kernel_source_sha256`). |
| `src/kernel/nv_fsp.c` | new, 751 lines | PFSP channel probe (EMEMC/EMEMD/queue registers, caps-query packet) and the pure helpers the unit tier uses. |
| `src/kernel/sha384.c`, `src/include/sha384.h` | new | SHA-384 for the COT's hash check. No other ref had one. |
| `src/include/nv.h` | +61 lines | `enum nv_falcon_state` + `nv_falcon_state_name()`; `nv_dev` PFSP fields (`pfsp_base`, caps/ctl/busy, scratch groups 2/3, ucode, state); declarations for `nv_pfsp_probe`, `nv_fsp_channel_probe`, `nv_fsp_cot_probe` and the four pure helpers. |
| `src/kernel/nv.c` | +130 lines | `nv_falcon_state_name()`, `nv_falcon_classify()`, `nv_pfsp_probe()`; the PFSP probe called from `nv_falcon_probe_all()`; the harness hooks in `nv_bringup()` (both the falcon-failure path and the success path call `nv_fsp_channel_probe()`/`nv_fsp_cot_probe()`). |
| `src/kernel/nv_regs.h` | +23 lines | `NV_FALCON_FSP_BASE` (0x8F0000), `NV_PFSP_SCRATCH_GROUP_2/3`, `NV_PFSP_UCODE_VERSION/MAJOR/MINOR`, `NV_REG_BLOCK_SENTINEL` + `nv_is_block_sentinel()`. |
| `src/kernel/nv_gsp.c` | +219 lines | the peer side the boot log is read with: `nv_gsp_rpc_peer_row/link/take/written`, `peer_ld32/st32`, and the address-only publish `nv_gsp_rpc_publish_bus()`. |
| `src/include/nv_gsp_glue.h` | +16 lines | declarations for the above. |
| `docs/gb202-rm-objects.md` | new on this ref, 1,072 lines | the written record, taken from `origin/nv/gb202-rm-615` (`c90584ec`): survey + §11–§13. |
| `docs/gb202-dmem-log.md` | new on this ref, 284 lines | the mode-66 report from `t_2e98aaee` (FSP returns 0 bytes for the vendor MMT VDM). |
| `evidence/nv/fmc-harness-unit-tier-{intel,arm}.log` | new | the two tier runs quoted in §5, committed as the repo's own evidence shape (`evidence/nv/wpr-bus-*`). |

Firmware images are **not** committed: `.gitignore` line 6 is `*.BIN`, deliberately. What is
preserved is the *name → bytes* provenance below.

## 2. Acceptance 1 — modes 63, 64, 65, 66 are on the ref

In the merged tree:

```
$ grep -c 'mode == 6[3-6]' src/kernel/nv_fsp_cot.c
12
$ for m in 63 64 65 66; do printf 'mode %s: ' $m; grep -c "mode == $m\b" src/kernel/nv_fsp_cot.c; done
mode 63: 8
mode 64: 7
mode 65: 6
mode 66: 6
```

Mode 66 is the `t_2e98aaee` delta: `rm_dmem_log()`, `cot_send_words()`, `cot_recv_quiet()` and the
`NV_FSPLOG_*` frame constants (the vendor's raw MCTP VDM, 4 dwords). It is gated on the same
`RM.CFG`/`COT.CFG` files as the rest, so an ordinary boot still does nothing.

The blob (`git ls-tree origin/nv/integ-swarm src/kernel/nv_fsp_cot.c` after the push) is
`ba88be3f539aac9743cde0e12153bdaac18370cd` — the tree's version as measured on this branch
(`git hash-object src/kernel/nv_fsp_cot.c`).

## 3. Acceptance 2 — every blob name resolves

The table (`nv_fsp_cot.c`, `rm_boot()` step 1) loads exactly these names from `/NVIDIA/GSP/`:

| name | bytes | sha256 | where the bytes come from / where they are staged |
|---|---|---|---|
| `FMC.BIN` | 199,820 | `cb59a35c1d4bd1274d7267fd10243c29f843ff41c851b9cbd59f5af2ddd7fece` | linux-firmware `gb202/gsp/fmc-570.144.bin`; in `base.img` (`fw/FMC.BIN`) |
| `FMCV.BIN` | 216,204 | `6adbd92d63c375aa3dc40ea11af35cbc4a03957e3b2c9ae3e778cc5d90ab3cbc` | 580.178.04's `GspRmFmcGfwProdSigned_GB202` **container** (its image section is 215,624 B, `cea2190c…` = `artifacts/fmc-580.178.04.bin`); in `base.img`. Modes 13 and 65. |
| `FMC615.BIN` | 287,884 | `ae4e70559f14d9becf7f0c4014a990d0873d174f3af0cc4913484e2cfb3238d5` | 615.78.08's container: image 287,304 B `5b74fefd42fc7b5f2e30c03e7558631d76899d9c6237ee9115c4e0829c63548b` (= `x615/gb202-rm-survey/artifacts/fmc-615.78.08.bin`) + that release's own hash (48 B `a97fa737…`), signature (96 B `512b52f7…`) and public key (97 B `af870e5d…`), wrapped by `fw/mkfmc.py` over the 620 ELF template `ec87ba35…`. Staged in `rm63.img`/`rm64.img` (and by `boot66.sh`); `fw/FMC615.BIN` now carries the same bytes. Modes 63, 64, 66. |
| `BOOT615.BIN` | 290,816 | `37159b4b7ce28066bbf83d11d50abff8e26722eb6bc2825290c14944a7c6aef0` | 615.78.08 `GspRmBoot` `UCODE_IMAGE_PROD` (decoded from the cached 615 bindata); staged in `rm63.img`/`rm64.img`. |
| `BOOTIMG.BIN` | 217,088 | `ac0688326c1ea8aae2da26d785e7091bedb9a45565b03543a479ed05db0ba367` | 580.178.04 `GspRmBoot`; in `base.img`. |
| `GSPRM.BIN` / `GSPRM.SIG` | 63,541,248 / 4,096 | in `base.img` (not in `fw/`) | the GSP-RM image and signature the COT rung boots. |
| `BLB.BIN` | 200,812 | `d40b48e431d1707dc77af3605db358ed7a32ebfc2830eb74de2eddb4d3025071` | the booter/load block; in `base.img`. |

Two corrections this card makes:

1. **`FMC580C.BIN` is gone.** Mode 65 named a blob that existed nowhere: `build65.sh` copied
   `fw/FMCV.BIN` onto that name *inside the image*. The code now asks for `FMCV.BIN`, the blob that
   exists (216,204 B, `6adbd92d…`), and the size gate (`RM_FMC580_BYTES = 216204`) is unchanged.
2. **`FMC615.BIN` is a real name with a reproducible recipe.** Rebuilt this run from its four
   inputs with `fw/mkfmc.py` and compared:

   ```
   $ python3 fw/mkfmc.py x615/FMC620.tpl x615/fmc615_image.bin x615/fmc615_hash.bin \
       x615/fmc615_sig.bin x615/fmc615_pkey.bin FMC615.rebuilt.BIN
   wrote FMC615.rebuilt.BIN 287884 bytes; image 287304
   $ sha256sum FMC615.rebuilt.BIN fw/FMC615.BIN
   ae4e7055…  FMC615.rebuilt.BIN
   ae4e7055…  fw/FMC615.BIN        ← byte-identical
   ```

   The card's warning is kept: **size does not identify the release.** The 615/620/610/595 FMC
   *images* are all 287,304 B with four different sha256s (`5b74fefd`, `55042efa`, `17a15ce8`,
   `d7174df2`), and `fw/FMC595.BIN` (287,884 B, `17057676…`) is the **595** container — same size
   as the 615 container, different bytes, not interchangeable.

Resolution checked against the actual lane images (`mdir -i <img> ::/NVIDIA/GSP`): `base.img` has
FMC/BLB/BOOTIMG/GSPRM/GSPRM.SIG/FMCV; `rm63.img` adds FMC615+BOOT615; `rm65.img` had
`FMC580C.BIN` (216,204) and **also** `FMCV.BIN` from base, so mode 65 resolves as renamed without
re-staging. No name in the table now points at a file that does not exist.

## 4. Fold adaptations (harness code vs glue)

The harness was written against an older tree. The tip's APIs differ in five places; all five are
mechanical and none touches a mode's numbers:

1. `nv_gsp_rpc_publish(d, &bus, &ptes)` → **`nv_gsp_rpc_publish_bus(d, &bus, &ptes)`**. The tip's
   `nv_gsp_rpc_publish(struct nv_gsp_rpc *r, struct nv_dev *d)` also programs MAILBOX0/1; the
   harness's call only needs the RMARGS address, and adding mailbox writes the harness never made
   would change the measured host state. One call site (`nv_fsp_cot.c:1930`).
2. `struct nv_fw_blob` lost its bare `phys` member (it now owns an `nv_dma_map`) — the two blob
   resets use `map.phys/bus/len/pages`, and the `phys=` log rows became `bus=` via
   `nv_fw_blob_bus()`. This is the tip's own provenance rule (device-visible addresses are bus
   addresses), applied to two rows; no other log text changed.
3. `nv_radix3_build_into()` grew `tree_bus`/`image_bus` arguments: the call now passes
   `nv_dma_bus(&m_tree)` and `nv_dma_bus(&m_rmimg)`.
4. The PFSP probe (`nv_pfsp_probe`) and the falcon-state classifier were not on the tip; they are
   ported verbatim from the lane's tree (`nv.c`) with the enum moved to `nv.h`. The tip's existing
   `struct nv_falcon` (`present`/`fenced`) is left untouched so `nv_info` and the existing tests are
   unchanged; the new state word lives on `nv_dev.pfsp_state` only.
5. `nv_fsp.c`/`nv_fsp_cot.c` non-x86 stubs needed `<stdint.h>`/`errno.h`/`struct nv_dev` when the
   file is compiled for ARM (they were previously only ever built for intel).

## 5. Acceptance 4 — both unit tiers, in the tier's own mode

Both tiers were run **before and after** the fold, on this box, same scripts, same environment
(`TOOLS_SKIP=1 BROWSER_SKIP=1`, `SKIP_KVM=1` for intel because this uid is not in `kvm`, TCG
otherwise; `UNIT_TESTS_TIMEOUT=900`). Baseline = a clean worktree at `070900b4`.

| tier | command | pre-fold `070900b4` | merged tree | verdict |
|---|---|---|---|---|
| intel | `SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh` | `Tests run: 611 / Tests failed: 0 / UNIT TESTS PASSED` | `Tests run: 611 / Tests failed: 0 / UNIT TESTS PASSED` | green, count unchanged |
| arm | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` (ARCH=arm) | `Tests run: 486 / Tests failed: 0 / UNIT TESTS PASSED` | `Tests run: 486 / Tests failed: 0 / UNIT TESTS PASSED` | green, count unchanged |

The tier logs are `evidence/nv/fmc-harness-unit-tier-{intel,arm}.log` in this commit. `make -j4`
throughout. The default (non-unit) kernel build was also checked clean for **both** arches
(`make ARCH=intel hobbyos.elf -j4`, `make ARCH=arm hobbyos.elf -j4`) because the unit mode defines
`KERNEL_MODE_UNIT_TEST` and would not have compiled the hardware paths.

Style, for completeness: `make check-format` is **already red at `070900b4`** (165 files, including
`src/kernel/nv_gsp.c`), so it is not a gate this fold can turn green. The copied `nv_fsp_cot.c` was
run through the repo's own formatter (`python3 tools/cstyle.py format`), which changed 66 lines and
**zero** non-whitespace tokens (`diff -w` empty) — the tier counts above were re-measured after
that, so they describe the committed bytes.

## 6. Acceptance 3 — the written record

`docs/gb202-rm-objects.md` on the ref, read off the ref as the card requires:

```
$ git show origin/nv/integ-swarm:docs/gb202-rm-objects.md | wc -l
1072
$ git show origin/nv/integ-swarm:docs/gb202-rm-objects.md | sha256sum
7b093c3e5f28f067a699a73f7a84a461f6ad6b66d69f9eca2b69c722b719847a
```

That is the `origin/nv/gb202-rm-615` (`c90584ec`) blob, byte for byte (`git hash-object` =
`108f6e749e535298b1bfd9c280cb93c1c770c0f8`); the `nv/gb202-rm-survey` copy is the 491-line
pre-§11 version and was **not** used. Mode 66's own report is carried alongside as
`docs/gb202-dmem-log.md` (284 lines, sha256 `bbf86411…`).

## 7. What the next reader should know

- The blob names are only resolvable with the lane's `fw/` staging dir (untracked, `*.BIN`) plus
  `base.img`. A fresh clone has the *code*; reproducing rm63/64/65/66 needs the five images in `fw/`
  and the staging scripts, which is why §3 lists bytes rather than paths.
- No branch has ever contained this harness before this commit; `git log --all -- src/kernel/nv_fsp_cot.c`
  was empty. The provenance of every line is the lane tree named in §0, not a rewrite from reports.
- The merge is doc + code only. No boot, no GPU touched, no `docs/nv-plan.md` edit.
