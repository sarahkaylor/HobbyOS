# GB-122 — five complete waves on the resume-side commit; 4 of 5 at `Vector: 14 = 0`

Head `215b79b` + `93-both-site-commit.patch` (commit-on-absent at both resume sites, gated on the
branch the frame will take). Intel, this VM. **All five waves completed** (`halted=1`).

| wave | console | FAIL | fault banners | programs exited | `Vector: 14` | commits | distinct (pid,page) | ratio |
|---|---|---|---|---|---|---|---|---|
| 91 | 35 966 | 372 | 2 | 88 | **0** | 142 | 68 | 2.09 |
| 92 | 33 101 | 186 | 3 | 86 | **1** | 139 | 59 | 2.36 |
| A1 | 18 576 | 132 | 0 | 86 | **0** | 69 | 38 | 1.82 |
| C4 | 10 133 | 85 | 0 | 85 | **0** | 49 | 20 | 2.45 |
| C5 | 11 087 | 86 | 0 | 85 | **0** | 30 | 22 | 1.36 |

Baselines on this same host, unmodified `215b79b`: `Vector: 14` = **10, 17, 21, 26** across four
waves; fault banners 3-25.

## Clause status, against the 01:45Z DoD

| # | clause | status |
|---|---|---|
| 1 | five halting waves, `Vector: 14 = 0`, distinct/total ratio per wave | **4 of 5** — wave 92 carries one `Vector: 14`, unattributed; ratios above |
| 2 | the unmap named (or repeats explained) | **named**: not an unmap — exec installs a **new AS** at the same pid/VA (`8/8` flips were root changes, `cr3 == root`). The exec-prefault fix is GB-134 and is **blocked** on the `uart_lock` wedge |
| 3 | fatal count vs baseline on this host | 2 / 3 / 0 / 0 / 0 banners vs 3-25; 0 / 1 / 0 / 0 / 0 `Vector: 14` vs 10-26 |
| 4 | both arches build; `host_tests` with count | **intel exit 0, arm exit 0** (arm needed the `spsr` branch guard, `C8`); `host_tests` **1096 PASS / 0 FAIL** (`C7`) |
| 5 | a test that fails before the fix | **not built** — the pre-fix behaviour is measured (10-26 faults per wave) but not encoded as a tree test |

## What the patch is and is not

It is a **mask**: it commits the destination page at the first resume of each agent's new AS, so the
copy never faults, at a cost of 30-142 pages per wave (one per new AS). It does **not** fix the cause;
GB-134 owns that and is blocked on the console-lock wedge (my last measurement there: `uart_lock`,
resolving the wave's stall).

## The one unmet number

Wave 92's single `Vector: 14` is not attributed. Every other wave is clean, and the count is 1 in
~28 000 console lines across the five runs. I am reporting it rather than dropping the wave.

## Disclosures

- Waves C4/C5 were taken on the build before the arm-conditional (`spsr`) tweak; that tweak changes
  only the `#else` arm branch, so the intel binary is identical — but it is stated rather than assumed.
- `host_tests` is a host-side suite; it exercises neither the resume path nor the commit.
- Earlier partial waves (61, 71, 81) are **not** counted here; only the five above are complete.
- `main` untouched; the patch is `93-both-site-commit.patch` in this directory.