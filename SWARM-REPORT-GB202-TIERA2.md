# GB202 Tier A, second pass — A6 and A8 (0 boots)

Card: `t_3e121087`. Lane `nv-card`, branch `nv/gb202-tier-a2`, based on
`nv/integ-swarm` `eb3bbe69`. **Boots spent: 0.** No `qm`, no VM start, no GPU
access, nothing written on `r1`; the vendor source came from the upstream
tarball at the tag (`sha256 f30d4c20…a9e01`, verified before extraction), not
from a guest.

`docs/gb202-vendor-diff.md` §5 Tier A lists eleven rows. The first pass landed
A3/A4/A5/A7/A9/A10; this one lands the two that were never carded, **A6** and
**A8**, and extends the executable gate from 30 to 49 assertions.

---

## 1. A6 — the vendor's DMA-target pair (+8 / +40)

**Row.** "make the default args-page target NONCOHERENT (2) and the descriptor
page 1, and make the buffer's cache attribute match"; vendor
`kernel_gsp_gh100.c:453-461`, `:390-405`; ours `nv_fsp_cot.c:1997-1998`.

**What the vendor does (measured from the 580.178.04 tree, not inferred).**
`_kgspMemdescToDmaTarget` (`kernel_gsp_gh100.c:390-405`) answers from the
*memdesc's cache attribute*: `ADDR_SYSMEM + NV_MEMORY_CACHED` → 1 (COHERENT),
anything-else-SYSMEM → 2 (NONCOHERENT). The two pages are allocated with
different attributes:

| page | allocation | attribute | target sent |
|---|---|---|---|
| WPR-meta descriptor (`+8`) | `kernel_gsp_tu102.c:121` | `NV_MEMORY_CACHED` | **1** |
| libos init-args (`+40`) | `kernel_gsp_tu102.c:148` | `NV_MEMORY_UNCACHED` | **2** |

so the vendor's own pair is 1/2 (`kernel_gsp_gh100.c:453-461` fills both
fields from those two calls). Ours sent 1/1 on every mode except the 7/67/50
probes.

**What landed.** A new pure TU, `src/include/nv_fmc_args.h` +
`src/kernel/nv_fmc_args.c`, holds the vendor's `GSP_FMC_BOOT_PARAMS` offsets
(measured by compiling `gspifpub.h:113-120` at that tag: sizeof 88, targets at
+8 and +40) and derives the target from an explicit cache-attribute enum:

```
nv_fmc_args_dma_target(NV_FMC_CACHE_CACHED)   == 1
nv_fmc_args_dma_target(NV_FMC_CACHE_UNCACHED) == 2
```

`nv_fmc_args_targets_encode()` writes those two words and nothing else;
`nv_fmc_args_targets_check()` reads them back off the page and returns `-EIO`
for the old 1/1 pair. The FSP RM rung calls the encoder for its default pair,
keeps the mode-7/67 branch as the row's provenance (it now agrees with the
default), and prints an `a6 targets desc=…  args=…  pair=vendor(1/2)` row
built from the page's own bytes. The two sweep/park base pages that rebuild
the same args page now use the same helper. Mode 50 still overrides both
words to 2 on purpose (its own card) and the row says so.

**The cache-attribute half.** `nv_dma`'s frames are coherent-by-construction
(snooped sysmem; `src/include/nv_dma.h`, "COHERENCE"), so no cache maintenance
is added. What "match" means in code is that the *declared* pair is a function
of the attribute the vendor declares for each page, so a page claiming a
different pairing cannot be produced by this file. The FMC-only rung
(`nv_fsp_cot.c`, the `/COT.CFG` instrument) is unchanged: it sends `RMARGS=0`
and no libos args page, so +40 has nothing to describe.

---

## 2. A8 — real LIBOS log regions and the nvlog header

**Row.** "build real log regions (D12)"; vendor `kernel_gsp.c:3141,5179`;
ours "mode 4 only, zeroed"; change "nvlog headers + `LOG*` entries in the args
table"; gate "header test against the vendor's `nvlog` layout".

### 2.1 The two objects the vendor has (this is the fact the mode-4 probe missed)

Reading the vendor's own code rather than the row's shorthand:

1. **the region handed to firmware** — `_setupLogBufferBaremetal`
   (`kernel_gsp.c:3052-3134`) allocates one sysmem memdesc per task, zeroes
   it, and hands libos the region's own physical address (the args entry's
   `pa` is `pTaskLogBuffer[1]`, i.e. the PTE[0] `memdescGetPhysAddrs` just
   filled in — `:3104`, `:5199`). Its layout is **one NvU64 put pointer at
   +0, records from +8** (`liblogdecode.c:1281-1282`, record reads at
   `physicLogBuffer[1]`, `:1308`/`:1365`).
2. **the NVLOG container a reader validates** — `libosLogInitHeader`
   (`liblogdecode.c:1985-2013`) writes `LIBOS_LOG_NVLOG_BUFFER_V2`
   (`liblogdecode.h:123-135`: gpuArch, gpuImpl, version, buildIdLength,
   taskPrefix[8], localToGlobalTimerDelta, buildId[32], flags, reserved) and
   the put pointer at `offsetof(data) + 8` (`:2086`). The header is **72 bytes**
   (`0x48`), put at `0x48`, records at `0x50` — verified by compiling the
   vendor's field list; the same numbers are `_Static_assert`ed in our TU.

So a region is *not* a container. The vendor zeroes (1) and wraps it into (2)
at read time. Mode 4 zeroed 1 MiB under three names, published RMARGS first
(contradicting the vendor's own "LOGINIT must be first", `kernel_gsp.c:5191`),
and had no reader at all.

### 2.2 What landed

* `src/include/nv_libos_log.h` + `src/kernel/nv_libos_log.c`:
  * the V2 header, field for field, with every field's `file:line`;
  * `nv_libos_nvlog_encode()` — a valid, empty container (version 2,
    prefix, timer delta, build id, flags, reserved 0, put 0);
  * `nv_libos_nvlog_decode()` — the reader's check: `version == 2`,
    `reserved == 0`, `buildIdLength <= 32`, a NUL-terminated non-empty
    prefix, and a put pointer that fits the record area. **A zeroed region is
    `-EINVAL` (version 0 ≠ 2)** — the mode-4 defect, executable;
  * `nv_libos_nvlog_snapshot()` — the wrap `libosLogAddLogEx` performs
    (`liblogdecode.c:1514-1523`), region → container, so a dump reader can
    validate a boot's bytes;
  * the vendor's task table, in `logInitValues` order
    (`kernel_gsp.c:3152-3164`) with the ids/prefixes of
    `nv-firmware-chip-family-select.h:107-113`:

    | # | id | prefix | bytes | nvlog |
    |---|---|---|---:|---:|
    | 0 | `LOGINIT` | `INIT` | 0x10000 | 1 |
    | 1 | `LOGINTR` | `INTR` | 0x10000 | 1 |
    | 2 | `LOGRM` | `RM` | 0x10000 | 1 |
    | 3 | `LOGMNOC` | `MNOC` | 0x10000 | 1 |
    | 4 | `LOGDBG` | `DBG` | 0x10000 | 0 |
    | 5 | `LOGROOT` | `ROOT` | 0x1000 | 1 |

    total 0x51000 bytes, each region page-aligned in one contiguous run
    (matching `memdescCreate`'s per-region alignment), `LOGINIT` first.
* **FSP RM rung** (`nv_fsp_cot.c`): allocates the six regions, publishes six
  `LOG*` entries followed by `RMARGS` (the vendor's order, `:5194-5206`), and
  prints, before and after the COT, one row per task:
  `log[LOGRM] pa=… size=65536 put=0 records=0 nz_dwords=0 hdr=valid
  prefix=RM version=2 flags=1 nvlog=1`. A `log hdr zeroed=rejected
  (invalid nvlog header)` row shows the old probe's exact defect is now
  detected. Mode 4 is now "same as 1 but WITHOUT the LOG* entries" (the
  missing-region control); the zeroed variant no longer exists.
* **Kernel stage 8** (`nv_gsp.c`): the three 4 KiB log pages its table names
  get the same per-page header rows (`[NV-ARG] stage=log[i] … hdr=valid
  version=2`), so both boot paths describe the channel the same way.
* **ARM reloc inventory**: `nv_libos_log_tasks` is a folded
  `const char *` table (12 words). The AArch64 gate caught it at the first
  ARM link — recorded with `--update-baseline` for all three modes and a row
  in `docs/nv-arm-reloc.md` §4 (unit 426 → 438 words, desktop/test 420 → 432;
  `.rela.dyn` 443/437, every word applied).

### 2.3 What is unit-model, and what the next boot will read

Nothing above asks the GPU anything. The rows are the unit model: the regions
are real sysmem the RM can write, and the reader validates them the way the
vendor's dump reader does. On the next **authorised** boot (budget decision on
`t_a769beaa`, not this card), the observable is `log-after`: per task, `put`,
`records`, `nz_dwords` and `hdr=valid`. `put=0` for every task then means "the
channel existed and the task wrote nothing" — a different, and citable, answer
from `statq_written=0` and a register that reads 0. If the task does write,
`put`/`records`/`nz_dwords` move and the same container decode gives the
header a dump reader would trust.

Honest limitation: the firmware-side layout is the put-pointer form (object 1),
not the V2 header. The V2 container is built by the *reader* — which is what
the vendor's RM does — so this card does not claim the FSP writes a header it
does not write. What it does claim, and tests, is that our regions are the
vendor's regions and that our reader is the vendor's header layout.

---

## 3. Gates (all run from this tree)

```
python3 tools/nv/check_tiera.py   -> assertions 49 failures 0   (was 30/0)
python3 tools/nv/check67.py       -> assertions 25 failures 0   (unchanged)
```

`check_tiera.py` gained 19 assertions: A6 (offsets, attribute→target mapping,
encoder bytes, the rung's read-back gate and row) and A8 (V2 constants and
field order, the 72-byte static assert, the version/reserved refusal, the six
tasks in vendor order and sizes, the rung's per-task entries and pre/post
rows, the kernel stage-8 rows), plus the five new unit cases in its
exists-and-is-registered lists.

## 4. Tiers from the final source

```
SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 bash run_unit_tests_intel.sh
  Tests run: 621   Tests failed: 0   UNIT TESTS PASSED        (baseline 616/0)

TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 bash run_unit_tests.sh
  Tests run: 486   Tests failed: 0   UNIT TESTS PASSED        (baseline 486/0)
  reloc gate OK -- 443 kernel pointer word(s), every one applied at boot
  folded-table inventory matches -- 22 table(s) / 438 word(s) as audited
```

The five new cases (x86-64 only, so ARM stays 486):

| case | asserts |
|---|---|
| `test_nv_gsp_boot_args_targets` | +8 = 1, +40 = 2 from the attribute; the old 1/1 pair is `-EIO`; nothing else on the page moves; short page refused |
| `test_nv_gsp_nvlog_header_round_trip` | encode → bytes at 0x00/0x04/0x08/0x0C/0x10/0x18/0x20/0x40/0x44/0x48 → decode, every field equal; put past the record area is `-E2BIG`; bad prefix/build-id/length refused |
| `test_nv_gsp_nvlog_header_rejects_zeroed` | a zeroed region is `-EINVAL`; version 1 is `-EINVAL`; a nonzero reserved word is `-EINVAL` |
| `test_nv_gsp_nvlog_region_snapshot` | an empty region wraps to a valid empty container (put 0); one written record lands after the put pointer and `put`/`records`/`nz_dwords` agree; a too-small container is `-E2BIG` |
| `test_nv_gsp_libos_log_set_matches_the_vendor` | six ids/prefixes in order, vendor sizes, 0x51000 total, LOGDBG nvlog 0, page-aligned non-overlapping offsets, LOGINIT at 0, per-task container prefix |

## 5. Files changed

```
src/include/nv_fmc_args.h        (new) vendor GSP_FMC_BOOT_PARAMS offsets + attribute→target
src/kernel/nv_fmc_args.c         (new) encoder / read-back check
src/include/nv_libos_log.h       (new) V2 header + the two vendor objects, citations
src/kernel/nv_libos_log.c        (new) encode / decode / snapshot / task table
src/kernel/nv_fsp_cot.c          A6 default pair, A8 six regions + entries + rows
src/kernel/nv_gsp.c              A8 stage-8 per-region header rows
src/kernel/nv_gsp_glue_test.c    5 new unit cases
tools/nv/check_tiera.py          30 -> 49 assertions
tools/reloc_tables_baseline.json nv_libos_log_tasks recorded (all three modes)
docs/nv-arm-reloc.md             the audit-list row and the totals
SWARM-REPORT-GB202-TIERA2.md     this report
```

## 6. Not done, on purpose

* **No boot.** This card authorises none; the first boot with a live RM log
  channel stays the budget decision on `t_a769beaa`.
* The four B-row values, mode 67's shape, A1/A2/A3/A4/A5/A7/A9/A10, the FMC
  classes and the RM-object class point were not re-spent or re-derived.
* LOGKRNL (`kernel_gsp.c:3214-3245`, id at
  `nv-firmware-chip-family-select.h:151`, the libos-kernel log) is not built:
  it is conditional on `kgspHasLibosKernelLogging_HAL`, which this tree does
  not model. The six task logs are the set the release-build `logInitValues`
  carries.
