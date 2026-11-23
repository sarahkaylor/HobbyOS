# SWARM-REPORT — WP-ARM2 (make the AArch64 unit tier run to completion)

Lane: `wp-arm2` · branch `nv/wp-arm2-tier` (cut from `756c8c9d`, the round-2 integration
tip) · box `sarah@192.168.0.250` (`/srv/nv/wp-arm2`) · worktree
`/Users/sarah/Documents/GitHub/hobbyos-nv/wp-arm2` · 2026-11-23.

Tags follow `PLAN.md` §6: **MEASURED** = I ran it on c4 and read the number; **SOURCE** =
read from the tree/doc; **INFERRED** = reasoning from those, not observed. All arithmetic
in this report was computed in code, not by hand.

## Outcome

The AArch64 unit tier now runs to completion on c4: **`Tests run: 463 / Tests failed: 0 /
UNIT TESTS PASSED`** (exit 0), from a `disk.img` that a plain
`make ARCH=arm MODE=unit_tests … disk.img` really builds — no harness stub anywhere. Intel
is unchanged at **578 / 0**. The full ARM link is still green (`__bss_end` `0x417AE1D4`,
62.32 MiB of headroom under `linker.ld`'s ceiling).

Two blockers, two commits:

| sha | what |
|---|---|
| `86ba64d8` | `fix(arm): build NVSMI only where the /dev/nvctl ABI exists` (Makefile) |
| `8d126e69` | `fix(nv-shim): the mock RM table is built at runtime -- a static one holds link addresses` (`nv_rm_mock.c`, `nv_rm.h`, `nv_shim.c`) |

## Blocker 1 — a fresh ARM `disk.img` did not build (pre-existing, round 0)

MEASURED, my own reproduction, same message round 2 reported:

```
$ clang … --target=aarch64-none-elf -c src/user/nvsmi.c
src/user/nvsmi.c:68:18: error: variable has incomplete type 'struct nv_info'
   68 |   struct nv_info info;
      |                  ^
src/user/nvsmi.c:68:10: note: forward declaration of 'struct nv_info'
```

`struct nv_info`, `NV_IOC_*` and the whole kernel nv API sit inside `#ifdef __x86_64__`
in `src/include/nv.h` (1–314), because the driver needs PCIe config space, MSI-X and a
64-bit BAR window the aarch64 port does not have (SOURCE: the header's own comment, and
nv.c/nv_rpc.c, which are empty TUs on ARM). So on ARM there is no `/dev/nvctl` and no
record to report on, and the tool cannot compile.

**Fix**: NVSMI is built and shipped where its ABI exists. `Makefile` now gates the
`nvsmi.o` / `nvsmi.bin` rules and the `disk.img: $(NVSMI_BIN)` dependency (line 1884) and
the `::/NVSMI.BIN` mcopy (line 3114-3121) on `ARCH=intel`. The ARM image carries no
`/NVSMI.BIN`; nothing on it reads one (SOURCE: the only reference to `NVSMI.BIN` in the
tree is that Makefile rule — x86 boot logs under `evidence/nv/` aside). The rejected alternative — a second
definition of `struct nv_info` for ARM — would be two sources of truth for a wire format
with no producer on that arch, and the task's "do not stub the kernel ABI away" cuts the
same way: nothing was stubbed, the tool simply is not built where its interface is absent.

MEASURED after the fix:

```
$ nvswarm build wp-arm2 ARCH=arm MODE=unit_tests LO_ON_DISK= hobbyos.elf disk.img   # exit 0
disk.img   1,073,741,824 B        hobbyos.elf   6,390,384 B
$ mdir -i disk.img ::/ | grep -c NVSMI   → 0        (UPTIME.BIN present, so the image is real)
```

## Blocker 2 — the EL1 synchronous exception in `test_nvshim_probe`

### What the tier did (MEASURED, one run, `/tmp/arm2-run1.txt`)

180 `Running test_…` rows, 0 `ASSERTION FAILED`, 0 `EXPECT_EQ FAILED`, then:

```
  Running test_nvshim_probe...
[KERNEL] FATAL: CPU 0x0 Synchronous Exception in EL1! ESR: 0x0000000002000000 EC: 0x0
         ISS: 0x0, FAR: 0x0, ELR: 0x00000000401485C0, PROC pid=42 name=,
         SPSR: 0x0000000060000345, LR: 0x000000043A552060, TFP: 0x000000043BC2DB80
[KERNEL] bytes@ELR: 16 × 0x0000000000000000
[KERNEL] bytes@uart_puts: 0x0000000012800001 0x0000000017FBD4DF 0x000000006361726F …
```

`0x401485C0` is exactly `mock_device_open` in that link (`llvm-nm`: `t mock_device_open`
= `0x401485c0`), which is why round 2 attributed the fault to "the entry of
mock_device_open".

### Root cause: the fault is a branch to the LINK address of the function, and the ARM image runs slid

Every row below is MEASURED on c4 from that build unless tagged.

| Observation | Value | What it rules in |
|---|---|---|
| `nv_rm_mock`'s `.rodata` table, slot 1 (`device_open`) | `0x401485c0` | the table stores `mock_device_open`'s **link** address (slot 0, `name`, is `0x401fe610`, also a link address) |
| `&uart_puts` at runtime | `0x43A5EA460` | the code really runs at link + slide |
| `uart_puts` link address | `0x401EA460` | slide = `0x43A5EA460 − 0x401EA460` = **`0x3FA400000`** |
| `LR` at fault − slide | `0x40152060` | the caller was executing in slid space, inside `nv_shim.c` — so the caller was right and the callee address was wrong |
| disassembly at `0x401485c0` in the ELF | `stp x29, x30, [sp, #-0x30]!` | the instruction at the link address is not what faulted; the CPU never fetched it |
| `bytes@ELR` in the handler | 16 zero words (`bytes@uart_puts` reads real code) | the runtime mapping at the link address holds zeros |
| `ESR = 0x2000000`, `EC = 0`, `ISS = 0`, `IL = 1`, `FAR = 0` | — | "unknown reason" for a 32-bit instruction = an **UNDEFINED encoding** (`0x00000000` = `udf #0`). Not an alignment fault (EC `0x21`/`0x25`), not an FP/SIMD trap (EC `0x07`), not a data abort (FAR would be set) |
| `SPSR = 0x60000345` | `EL1h`, D/A/F masked, I unmasked | ordinary process-context kernel code, not a handler-corruption artefact |

So no EL1 hardware rule was broken by the *code* that ran — no unaligned wide load, no
SIMD, no masked read treated as data, no bad pointer dereference. The bug is one level
up: **`const struct nv_rm_if nv_rm_mock = { "mock", mock_device_open, … };` is a static
initializer holding function addresses**. `ld.lld` resolves those to link addresses, and
this kernel **applies no load-time relocations to static data** — the ELF has no `.rela*`
section and is not `-pie` (MEASURED: `llvm-readelf -S/-l`). Code is fine under the slide
because clang reaches symbols with `adrp`/`add` (PC-relative); static *data* is not. That
hazard is already written down twice in this tree:

- `src/kernel/blk_cache_press_test.c:124` — "the kernel applies no `.data.rel.ro`
  relocations, so a `const char *` member in a static initializer reads back NULL";
- `src/kernel/unix_test.c:38-42` — a static aggregate "would carry the link address",
  hence `RT_ADDR()`.

`nv_rm_mock` violated it, and the ARM tier reached it for the first time this round
because blocker 1 had always stopped the image from being built. (x86_64 is innocent by
accident: the image loads at `KIMG_BASE` = its link address, so a folded table is right
there — which is why 578/0 never showed this.)

### Fix

The table is built **once, at runtime**, from PC-relative addresses:

- `nv_rm_mock.c`: `nv_rm_mock` (a `const` aggregate) is replaced by `g_rm_if` in `.bss` +
  `mock_if_fill()`, which assigns each member individually (an assignment cannot be folded
  into a `.rodata` template — `struct t x = { fn, … };` would be), exposed as
  `const struct nv_rm_if *nv_rm_mock_if(void)`, filled under a spinlock behind a one-word
  ready flag so an ioctl pays nothing after the first. MEASURED: `llvm-nm` on the new ELF
  shows `g_rm_if`/`g_rm_if_lock`/`g_rm_if_ready` in `.bss` and **no** `R nv_rm_mock`.
- `nv_rm.h` keeps the contract, now `nv_rm_mock_if()` with the reason written next to it;
  `nv_shim.c`'s `rm()` returns it (the only call site; the tests use the
  `nv_rm_mock_*` seams, unchanged).
- The same defect in the same branch, fixed with it: `nv_shim.c`'s `g_drm_refusals[]`
  held `const char *name`. It is now `char name[28]` per row — the project's own pattern
  for this (`blk_cache_press_test.c`), so the drm face names a refusal from the table
  instead of from the link address of a string.

Evidence that this is the same class and that the fix moves the right thing (MEASURED, the
HEAD `nv_shim.c` compiled to `/tmp/nv_shim_old.o` with the same flags, vs the working
tree):

```
BEFORE  g_drm_refusals @0x1c0 size=336 entries=21 (stride 16)
          row 0: nr=0x0a name_ptr=0x0        ← a relocation; ld.lld fills in the LINK address
AFTER   g_drm_refusals @0x1ba size=609 entries=21 (stride 29)
          row 0: nr=0x0a name[28]='gem_flink'
```

(x86_64 is why no test caught it: the refusal tests assert `-ENOSYS` by request, not by
name — see "what is not verified".)

## Gates (run on c4, exact lines)

**ARM build — PASS** (`disk.img` + `hobbyos.elf`, no stub)

```
$ nvswarm build wp-arm2 ARCH=arm MODE=unit_tests LO_ON_DISK= hobbyos.elf disk.img
… ld.lld -T linker.ld -o hobbyos.elf obj/arm/… (138 objects)   → exit 0
disk.img 1,073,741,824 B · hobbyos.elf 6,390,384 B
```

**ARM unit tier — PASS (the point of this lane)**

```
$ nvswarm sh wp-arm2 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests.sh'
Building unit test image (ARCH=arm)...  exit 0
Running unit tests...
Tests run:    463
Tests failed: 0
UNIT TESTS PASSED                                   ← exit 0; 265 “Running test_…” rows, 0 “FAILED”
```

Before the fix, same command: build failed at `nvsmi.c:68`; with round 2's harness stub,
the EL1 dump above after 180 rows and no summary.

**Intel build + unit tier — PASS (unchanged)**

```
$ nvswarm build wp-arm2                                            # ARCH=intel, MODE=unit_tests
$ nvswarm sh wp-arm2 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'
Tests run:    578
Tests failed: 0
UNIT TESTS PASSED                                   ← exit 0; 371 rows
$ mdir -i disk.img ::/ | grep -c NVSMI → 1           # the intel image still ships the tool
```

**ARM link geometry — still green** (MEASURED, `llvm-readelf`/`llvm-nm` on the new ELF)

```
.text   0x40080000, 1,484,152 B
.rodata 0x401ea578,  260,168 B
.bss    0x40400000, 20,636,116 B (19.68 MiB)   # the 112-byte RM table moved .rodata -> .bss
__bss_end 0x417AE1D4 → 65,347,116 B (62.32 MiB) under linker.ld's 0x45600000
```

## What changed

| file | change |
|---|---|
| `Makefile` | NVSMI rules + `::/NVSMI.BIN` gated on `ARCH=intel`, with the ABI reason (lines 1871-1897, 3114-3121) |
| `src/kernel/nv_rm_mock.c` | `const struct nv_rm_if nv_rm_mock` → runtime-filled `g_rm_if` + `nv_rm_mock_if()`; block comment names the hazard and the measurement |
| `src/include/nv_rm.h` | `extern const struct nv_rm_if nv_rm_mock` → `const struct nv_rm_if *nv_rm_mock_if(void)` with the reason |
| `src/kernel/nv_shim.c` | `rm()` calls `nv_rm_mock_if()`; `g_drm_refusals[].name` is `char[28]`, not `const char *` |

Nothing else in the tree was touched: no `ada/`, no `lane-*`, no other `wp-*`; nothing
pushed or merged.

## What is NOT verified

- **The drm refusal *names* are still not asserted by any test.** `test_nvshim_drm_refusals`
  checks `-ENOSYS` per request, and in this run the `[NVSHIM]` row budget (`SHIM_ROW_BUDGET`
  = 8 per stage) had already been spent by the 'N' face, so no `drm refuse` row appeared in
  the log either way. That fix is proved at object level (name inline in the table, above),
  not by an observed print. A test that asserts `nv_shim_drm_refusal_name()` returns
  `"gem_flink"` etc. would close it; I did not add one because it changes another lane's
  test file for a one-line assertion — recommend it upstream.
- **ARM was run twice, intel once.** Both ARM runs (same command, fresh `disk.img` each
  time via the script's `fresh_disk` step) printed `463 / 0 / UNIT TESTS PASSED`; the intel
  run was not repeated. The lazy fill was exercised single-threaded; two CPUs racing
  `nv_rm_mock_if()` for the first time is *designed* safe (spinlock, ready-last) but was
  not observed under contention.
- **ARM runtime (non-unit) boot** — desktop, `t3_tier.sh arm`, networking — was not run;
  only the kernel unit tier. The ARM `disk.img` is one file smaller than before (no
  `/NVSMI.BIN`); no other ARM image content changed.
- **The card lane is untouched**: nothing here was tested against the RTX 4090, and
  `nv.c`/`nv_rpc.c` (x86-only TUs) still contain static `const char *` tables
  (`nv_chips`, `reason_names`, `arg_names`, `link_names`) of the same class. They are
  harmless *today* only because those TUs are empty on ARM and x86 is not slid; converting
  them is the systemic fix's job, not this lane's.
- `__bss_end` headroom is MEASURED for this link only; nothing here re-measured the Limine
  slide independently — `0x3FA400000` is derived from `&uart_puts` vs its link address in
  the fault dump, which is a direct observation of the running image.

## Next step

1. **Round-3 integration**: carry `86ba64d8` + `8d126e69` into `nv/integ-swarm` (they are
   independent of each other and of the `net`/`phys_to_virt` commits). After them the ARM
   unit tier is a real gate for every lane instead of a "link-only" check.
2. **The systemic bug behind blocker 2 is still open and will bite again**: kernel static
   data is not relocated under the ARM slide. `linker.ld` already collects `.data.rel.ro`
   "so const tables of pointers get their relocations at load time", but nothing applies
   any. Either (a) link the ARM kernel `-pie`/`--emit-relocs` and apply `R_AARCH64_RELATIVE`
   in `boot.s` before `.bss` clear, or (b) make "no pointers in static initializers" a
   checked rule — e.g. a post-link scan that fails when a `.rodata`/`.data` word lies in
   `[0x40080000, __bss_end)` but is not itself a relocated slot. (a) is the real fix; (b)
   is the cheap guard that would have caught this at link time.
3. Land the assertion for (2)'s string cases upstream: `_Static_assert` is not available
   for the shape, but a unit test that reads one such table entry per subsystem through
   `RT_ADDR`-style runtime addresses would fail on ARM immediately.
