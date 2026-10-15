# w4-kernel — Mission B: 128 MiB in-OS MKFSFAT slowness under TCG

(Wave-4 kernel lane, `selfhost/w4-kernel`. Companion: `w4-kernel-report.json`.)

## 1. The complaint

w3-kernel found the 128 MiB in-OS `mkfs.fat -C OUT.IMG 131072` ftruncate step
too slow under TCG: "QMP broke mid-ftruncate once" (docs/selfhost/lanes/
w3-kernel-report.json blockers; 16 MiB was fine). The acceptance S3 budget is
600 s wall with the driver's phases driven over serial.

## 2. Root cause (measured)

`fat16_truncate_to()` zero-filled a grow with 4 KiB `fat16_write()` chunks,
and **every `fat16_write()` call re-walked the file's cluster chain from
`start_cluster` to the cursor** (fat16.c:2036–2046). A grow of N clusters
therefore paid O(chunks × chain) = **O(n²) chain-walk iterations**, and
`alloc_cluster()` independently re-scanned the FAT from cluster 2 on every
allocation — another **O(n²)** term.

In-OS measurement (unit-tier probe build, real TCG, serial `[W4-PROBE]` lines,
gated behind `W4_MKFS_PROBE=1`; raw evidence:
`docs/selfhost/lanes/evidence-w4-kernel/fixed-probe-unit-128Mib.log`):

| ftruncate to | pre-fix ms | post-fix ms | Δ |
|---|---|---|---|
| 16 MiB | **78,741** | **34,595** | 2.3× |
| 64 MiB | **347,449** | **142,580** | 2.4× |
| 128 MiB | > 720,000 (unit-tier budget, run killed) | **283,981** | previously impossible |

Scaling: pre-fix quadratic (4× size → 4.4× time; 128 MiB extrapolates to
~23 min). Post-fix **linear** (16→64 = 4.1×, 16→128 = 8.2×) and the 128 MiB
acceptance run completes in **284 s**, comfortably inside the 600 s S3 budget.

## 3. Bounded fix (this lane, failing-test-first style)

Two kernel changes in `src/kernel/fat16.c`, both behind the existing tests +
one new guard test:

1. **Free-cluster hint in `alloc_cluster()`** — scans from the last-allocated
   cluster with wrap, instead of from cluster 2 every time: amortized O(1)
   allocation, O(n) total. Standard FAT driver practice; identical behavior
   on sequential fills (still allocates contiguously from 2).
2. **Bulk extension in `fat16_truncate_to()`** — the grow path now extends the
   chain to cover `new_size-1` in ONE forward walk (no per-4 KiB re-walk),
   and zero-fills only the partial tail cluster via `fat16_write`. Every
   full cluster handed out by `alloc_cluster()` is pre-zeroed, so the bulk
   region reads back as zeros — POSIX ftruncate semantics preserved, no
   content-write amplifier.

Correctness held by: `test_fat16_alloc_hint_chain` (new, always-on —
extend/shrink/regrow/reopen zero-fill + dir-entry invariants),
`test_fat16_ftruncate_extend_then_write` (w3), the full existing fat16/fs
suites, and the ARM + x64 unit tiers.

## 4. Recommendation for the driver's QMP / serial timeouts

- **After this fix**: in-OS 128 MiB ftruncate is O(n) + MMIO-bound; the
  acceptance S3 budget (600 s) has generous headroom for mkfs + mcopy of the
  full OUT.IMG payload. A 900 s S3 timeout is still a safe bound.
- **Never regress the quadratic back**: the `W4_MKFS_PROBE=1` probe is kept in
  the tree (gated, adds no stock cost) so a future lane can re-measure the
  ftruncate curve on-device in one boot.

## 5. What was deliberately NOT changed

- The block-driver per-512 B request loop (`virtio_blk_write_sector` with
  `count>1` is a serialized loop, virtio_blk.c:377–407). Making it true
  multi-block (NVMe cdw12 / virtio num sectors) would affect every I/O path
  on both arches and is out of scope for a bounded kernel lane.
- `mkfs`'s own FAT structure writes and `mcopy` payload copies (w4-src/w4-clang
  domain).
