# SWARM-REPORT — WP-D (DMA facility)

Branch `nv/wp-d-dma`, worktree `wp-d`, cut from integration tip `ce4c44af`.
Commit: `feat(nv-dma): a bus address is a value, not a synonym for phys`.

## What changed

| file | change |
|---|---|
| `src/include/nv_dma.h` | new. The facility's contract: `struct nv_dma_map { phys, bus, len, pages }`, `nv_dma_alloc/free/translation_enabled/bus/guard`, `nv_dma_test_set_translation`, `NV_DMA_BUS_INVALID`. States why bus ≠ phys, the current translation state and its source, and the coherence assumption (no cache maintenance; snooped sysmem DMA). |
| `src/kernel/nv_dma.c` | new. The identity statement in one static function (`nv_dma_resolve_bus`), allocation with pool/alignment/ownership/round-trip proofs, zeroing, the guard, the accessor, and the documented test seam. |
| `src/kernel/nv_dma_test.c` | new. 13 cases, kernel unit tier, registered unconditionally (the facility is arch-independent). |
| `src/kernel/unit_test.c`, `src/kernel/unit_test.h` | one prototype + one call for `nv_dma_test_suite()`. |
| `src/include/iommu_vtd.h`, `src/kernel/iommu_vtd.c` | new `iommu_vtd_translation_enabled()` reading `GSTS.TES` — the bit the enable path polls — instead of `iommu_vtd_is_active()`, which only reports "initialised". Header now says the two questions are different. |

## The interface as implemented

```c
struct nv_dma_map { uint64_t phys, bus, len; uint32_t pages; };
int      nv_dma_alloc(uint64_t len, struct nv_dma_map *out);
void     nv_dma_free(struct nv_dma_map *m);
int      nv_dma_translation_enabled(void);
uint64_t nv_dma_bus(const struct nv_dma_map *m);
int      nv_dma_guard(uint64_t phys, uint64_t bus);
void     nv_dma_test_set_translation(int on);
```

Behavior, and why each choice is the one that cannot lie:

- **`nv_dma_alloc`**: `len` must be non-zero and page-multiple (else `-EINVAL`), ≤ 8 GiB of pages (`-E2BIG`). With translation live it returns `-ENOTSUP` **before taking a frame** — a mapping that cannot be published is not a mapping, and an IOVA allocator does not exist yet. Then: `frame_alloc_contig`, alignment + `frame_phys_in_pool` at both ends, `frame_is_alloced` on **every** page (a hole in the run is caught, not assumed away), `virt_to_phys(phys_to_virt(phys)) == phys`, zero the range (`frame_alloc_contig` does not), resolve the bus, and run the finished mapping through `nv_dma_guard` before returning it. On **any** failure `*out` is left zeroed, so a caller that ignores the return cannot act on a half-filled mapping.
- **`nv_dma_bus`**: the only way a call site gets a bus address. Returns `NV_DMA_BUS_INVALID` (never `phys`) while translation is live, when the map is empty, or when `bus != phys` with translation off. `NV_DMA_BUS_INVALID` is `0xFFFFFFFFFFFFFFFF` — page-unaligned on purpose, so it cannot pass as "aligned enough".
- **`nv_dma_guard`**: `0` publishable; `-EINVAL` unaligned (including the poison); `-EIO` translation live (**both** a mismatch and an identity pair) or `bus != phys` with translation off.
- **`nv_dma_test_set_translation`**: tri-state seam (`on < 0` restores the hardware answer). Documented as test-only; the real code paths read `GSTS.TES` live on every call, so a state change after a mapping exists cannot be missed.

Identity in one place, verbatim from `nv_dma.c`:

```c
static int nv_dma_resolve_bus(uint64_t phys, uint64_t *bus_out) {
  if (nv_dma_translation_enabled()) { ...; return -ENOTSUP; }
  *bus_out = phys;      /* bus = phys ONLY because GSTS.TES is clear */
  return 0;
}
```

## The gate, and the red/green pair

The gate is `test_nv_dma_guard_translation_rejects_nonidentity`
(`nv_dma_test.c`):

```c
nv_dma_test_set_translation(1);
EXPECT_EQ(nv_dma_guard(GUARD_PHYS, GUARD_PHYS + 0x1000), -EIO);
EXPECT_EQ(nv_dma_guard(0x5F8700000ULL, 0x5FE400000ULL), -EIO);  /* VRAM-shaped */
```

Its companion `test_nv_dma_guard_translation_rejects_identity_too` asserts the
same refusal for `guard(P, P)` under translation, with a positive control that
`guard(P, P)` returns 0 with translation off, plus
`test_nv_dma_bus_never_returns_phys_under_translation` asserting
`nv_dma_bus(&m) != m.phys` and `== NV_DMA_BUS_INVALID` under translation.

**Finding worth keeping:** the gate as worded ("rejects a mapping whose bus !=
phys under translation") is satisfied by an identity-only guard. That is exactly
what mutation 1 showed — so the companion case is not redundant, it is the one
that pins the property the facility exists for. A future edit that "simplifies"
the guard to `bus == phys ? 0 : -EIO` passes the literal gate and fails here.

### Green (final state)

```
Tests run:    526
Tests failed: 0
UNIT TESTS PASSED
```

Baseline on the same tree before this commit: `Tests run: 513 / Tests failed: 0
/ UNIT TESTS PASSED`. Delta `+13`, all from `nv_dma_test_suite()`.

### Red 1 — guard made identity-only (translation branch deleted from `nv_dma_guard`)

```
  Running test_nv_dma_guard_translation_rejects_nonidentity...
  Running test_nv_dma_guard_translation_rejects_identity_too...
EXPECT_EQ FAILED: nv_dma_guard(GUARD_PHYS, GUARD_PHYS) == -EIO at src/kernel/nv_dma_test.c:225
Tests run:    526
Tests failed: 1
UNIT TESTS FAILED
```

Note which test stayed green: the gate case itself. That is the finding above.

### Red 2 — accessor ignores translation (branch deleted from `nv_dma_bus`)

```
ASSERTION FAILED: nv_dma_bus(&m) != m.phys at src/kernel/nv_dma_test.c:250
Tests run:    526
Tests failed: 1
UNIT TESTS FAILED
```

Both mutations were reverted byte-for-byte (`sha256 ea27c906…ff65`) before the
final green run above.

## Builds

- `nvswarm build wp-d` (intel, `MODE=unit_tests`): green, no warnings from
  `nv_dma.c`, `nv_dma_test.c`, `iommu_vtd.c` or `unit_test.c`.
- ARM gate replaced per instruction: the full ARM link is broken at `ce4c44af`
  for an unrelated, pre-existing reason (`ld.lld: error: kernel .bss exceeds the
  limine placement window`), reproduced on an untouched tree and not touched
  here. Per-TU check instead:

```
clang -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding
  -mcpu=cortex-a53 -mgeneral-regs-only ... -c src/kernel/nv_dma.c -o obj/arm/nv_dma.o
make: 'obj/arm/nv_dma_test.o' is up to date.
```

  and the two properties that matter for the AArch64 port:

```
$ llvm-nm -u obj/arm/nv_dma.o
 U frame_alloc_contig   U frame_free   U frame_is_alloced
 U frame_phys_in_pool   U print_int    U uart_print_hex   U uart_puts
$ llvm-nm obj/arm/iommu_vtd.o | wc -l
0
```

  No VT-d symbol crosses into the ARM object; `iommu_vtd.o` is a genuinely empty
  TU there, so `nv_dma_translation_enabled()` answers 0 from a fact about the
  platform (`nv_dma_hw_translation()`'s `#else` branch), not from a stub that
  hides a missing include. Because the `make` target compiles with
  `KERNEL_MODE_DESKTOP` (which makes `nv_dma_test.c` an empty TU), the test body
  was additionally compiled for AArch64 with `-DKERNEL_MODE_UNIT_TEST
  -fsyntax-only`: `ARM_UNIT_TU_OK`, `ARM_DMA_UNIT_TU_OK`.

## MEASURED / SOURCE / INFERRED

- MEASURED: 526/0/`UNIT TESTS PASSED`; the two red runs above; `[NV-DMA] map
  phys=0x00000002090A1000 bus=0x00000002090A1000 pages=33 identity=1` and the
  129-page block at `phys=0x00000002090C0000`; `iommu_vtd.o` (arm) has 0 symbols;
  `nv_dma.o` (arm) has no undefined VT-d symbol.
- SOURCE: `GSTS.TES` is the translation-enable status bit and the state this tree
  boots in (initialised, passthrough entry installed, `enable_translation()` has
  no caller) — `net_rdma.c:1243-1266`, `iommu_vtd.c:583-617`,
  `docs/nv-rpc-implementation-spec.md` §6.1.
- INFERRED: that a bus address produced here is the one the GPU dereferences.
  Today it is the same number, and that is all this facility can claim.

## What is NOT verified

- **Real IOMMU translation.** `GSTS.TES` has never been observed set anywhere in
  this work. Every translated-path assertion goes through the test seam
  (`nv_dma_test_set_translation`), which proves the *logic* switches, not that an
  IOMMU would honour a mapping. Proving that needs a card, a boot with
  translation enabled and an IOVA allocator — none of which exist.
- **Actual GPU dereference of `bus`.** Needs GSP firmware walking the PTE table;
  that is WP-C's link row, not this tier.
- **Non-identity mappings.** Nothing in the tree can produce `bus != phys`, so the
  whole translated path is refusal-only. There is no positive test of a working
  translated mapping because there is nothing to make one with.
- **Cache coherence.** No maintenance is performed anywhere by design (snooped
  sysmem DMA, `docs/nv-rpc-implementation-spec.md` §6.2 last note). I did not
  measure coherency on hardware in this lane; the assumption is documented, not
  proven.
- **Contiguity beyond `frame_alloc_contig`.** The per-page `frame_is_alloced`
  sweep proves ownership of each page, not that an external observer sees one
  run. That is the allocator's contract, restated, not independently verified.
- **The `GSTS.TES` read itself.** The unit tier compares
  `nv_dma_translation_enabled()` against `iommu_vtd_translation_enabled()`, which
  is the same function — so a wrong register offset would not be caught here. It
  would be caught by a boot log with translation enabled, which has never
  happened.

## Next step

WP-C consumes `nv_dma_bus()` for `PTE[0]` and re-asserts `pte[0] ==
nv_dma_bus(&block)` before every send. The day an IOVA allocator exists, the only
function that changes is `nv_dma_resolve_bus()`: allocate an IOVA and call
`iommu_vtd_map(bus, devfn, iova, phys, len)`, drop the `-ENOTSUP` early-returns,
and re-run this suite — `test_nv_dma_guard_translation_rejects_identity_too` and
`test_nv_dma_bus_never_returns_phys_under_translation` are the two cases that
must be rewritten deliberately rather than deleted.
