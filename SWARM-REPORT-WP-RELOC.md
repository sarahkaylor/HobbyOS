# SWARM-REPORT — WP-RELOC (kill the `.rodata` relocation class on AArch64)

Lane: `wp-reloc` · branch `nv/wp-reloc-arm` (cut from `5cc95b9d`, the round-3
integration tip) · box `sarah@192.168.0.250` (`/srv/nv/wp-reloc`) · worktree
`/Users/sarah/Documents/GitHub/hobbyos-nv/wp-reloc` · 2026-11-23.

> **Follow-up (landed after this report).** The audit list this lane produced
> by hand in §5, and the "it cannot grow silently" claim in §4, are now a
> checked-in, per-`MODE` inventory with a link-time guard:
> **`docs/nv-arm-reloc.md`** (list in §4, gate in §3, refresh via
> `make reloc_baseline MODE=<mode>`). Read that doc for the current numbers;
> this report stays as the round-4 measurement record.

Tags: **MEASURED** = I ran it on c4 and read the number · **SOURCE** = read from
the tree · **INFERRED** = reasoning, not observed. All arithmetic below was
computed in code, not by hand.

## Outcome

The kernel now applies its own load-time relocations to static data before the
first C consumer reads them. `const` tables of function and string pointers —
the shape that faulted at `0x401485C0` in round 3 with ESR `0x2000000` — work
under Limine's slide instead of being worked around one call site at a time.

| | |
|---|---|
| mechanism chosen | **`-pie` link + a boot-time `R_AARCH64_RELATIVE` pass** (shape *a*), plus shape *b* as the build guard |
| commits | `f022bf97` `test(arm): a folded const pointer table must hold RUNTIME addresses` · `ae19c0a9` `fix(link): apply R_AARCH64_RELATIVE at boot -- static data obeys the slide` |
| pointer words in the unit image | **417**, every one covered (411 before the new test's own tables) |
| `[RELOC]` row, boot | `entries=0x1A1 applied=0x1A1 slide=0x3FA400000 outside=0 status=0 ok` |
| ARM unit tier | **468 / 0 / UNIT TESTS PASSED** (465 before + 3 new assertions) |
| intel unit tier | **583 / 0**, unchanged |
| red run (base tree + the new test) | **467 run / 2 failed / UNIT TESTS FAILED**, exit 1 |
| gate rejection (link stopped being `-pie`) | **make exit 2**, `FAIL -- 417 kernel pointer(s) with no applied relocation` |

The slide the pass computes, `0x3FA400000`, is the same number round 3 derived
independently from `&uart_puts` minus its link address — two unrelated
measurements of the running image agreeing (MEASURED).

## 1. What was measured before anything was written

Round 3 fixed three call sites and named two more TUs. The first job was to
size the class, because that is what decides between "convert the tables" and
"apply the relocations". `tools/kernel_reloc_audit.py` (new, stdlib only) finds
every 8-byte word in an allocated non-executable section whose value lands in
the kernel's own link range `[0x40080000, 0x417AF90C)`:

```
$ python3 tools/kernel_reloc_audit.py --gate hobbyos.elf      # the tree at 5cc95b9d
# pointer words baked into static data (need relocating): 411
#   applied at boot (R_AARCH64_RELATIVE, addend in image): 0
#   NOTHING WILL APPLY THEM (left at their link address):     411
hobbyos.elf: FAIL -- 411 kernel pointer(s) with no applied relocation    (exit 1)
```

**411 words, not three tables.** 21 tables (section 5). The largest single
object is the Linux-ABI syscall dispatch: seven tables, 308 words, including a
~120-row `l_tab_proc`/`l_tab_fd` set written as `{nr, fn, "name"}` and a
function-local `const struct l_sysent *const tables[]` at
`linux_compat.c:593` that indexes them (SOURCE).

That measurement is what decides the shape, and it also produced a live bug
nobody had noticed:

> `nv_shim_refusal_name(NVSHIM_IOC_NVIF)` returns a `const char *`. clang folds
> its six-branch if-chain into a table of six string pointers (`ldr x0, [x9,
> x8, lsl #3]`, MEASURED in `llvm-objdump`), so on the slid image it returned
> the **link address of `"nvif"`**. `test_nvshim_layout` asserts
> `nv_shim_refusal_name(...) != 0` — and a link address is non-NULL, so the
> assertion passed *for the wrong reason* for three rounds. This is the
> project's rule 5 (an observation that could not have been negative) in its
> natural habitat.

Same shape, silently wrong on ARM today, from the audit: `fat16_getdents64`'s
`selves[2] = {".", ".."}` (`fat16.c:3503`) copies its two names *from* the
table into the dirent buffer, and `el_compat_83[]` (`elf_loader.c:149`) holds
`{const char *name; const char *staged;}`. Neither is reached by the ARM unit
tier (INFERRED from the green tier, SOURCE for the code), which is exactly why
they survived.

## 2. The mechanism: apply `R_AARCH64_RELATIVE` at boot

### 2.1 Why not shape (b) — "make the build fail on folded tables"

Shape (b) *alone* is not shippable: a hard failure today rejects the tree at
411 words / 21 tables, so it can only be landed together with 21 rewrites —
including a 308-word syscall dispatch whose rows are `const` precisely so they
stay in `.rodata`, and whose "runtime built by assignment" form would be ~300
assignments executed at every boot. That is the same per-call-site conversion
round 3 already did three times, with more surface. It also makes
`const char *` members unusable in kernel data forever, which is a permanent
tax on every table written from here on.

Shape (b) *is* the right guard, so it is in the tree as the second half of this
change (§4): the gate fails when a word holds a kernel address that no applied
relocation covers. Fix and guard, not fix or guard.

### 2.2 Why `-pie` and not `--emit-relocs`

`--emit-relocs` on the existing non-PIE link does emit `.rela.rodata` (409
entries, MEASURED) — but the section is flagged `I` only, i.e. **no
`SHF_ALLOC`**, so it is in no `PT_LOAD` segment and Limine never loads it into
memory. A boot pass cannot read a table that is not in the image. (MEASURED
flags; INFERRED the "not loaded" consequence, which follows from `SHF_ALLOC`.)

`-pie` is what makes `ld.lld` synthesise `.rela.dyn` as an **allocated**
section holding exactly one `R_AARCH64_RELATIVE` per pointer word: 411 entries,
type histogram `411 × R_AARCH64_RELATIVE`, nothing else (MEASURED,
`llvm-readelf -rW`/`-dW`: `RELA 0x401eedf0 RELASZ 9864 RELACOUNT 411`). The
count matches the audit's 411 independently.

`-pie` needs one more flag: without `-z notext` the link refuses the tree
(MEASURED):

```
ld.lld: error: relocation R_AARCH64_ABS64 cannot be used against local symbol;
recompile with -fPIC          (× many, e.g. obj/arm/linux_compat_shm.o:(l_tab_shm))
```

`-z notext` permits dynamic relocations in a read-only segment, which is what a
pointer inside `.rodata` is. The pass runs before `mmu_init()`, so the slots are
writable exactly when they are patched and read-only afterwards.

**Does Limine load an `ET_DYN` kernel?** Measured before writing the pass: the
`-pie` image boots and runs the tier (465 tests, the tree at the time), one
test failing for the reason in §1. Same entry point, same `p_vaddr`s, slide
unchanged.

### 2.3 Why x86_64 is left alone

The x86_64 kernel loads at `KIMG_BASE`, its link address, so a folded table is
already correct there — which is why 583 intel tests never showed this class
(SOURCE for the load base; MEASURED that the intel tier is green with the
change present and the gate not run for `ARCH=intel`). Its link is untouched:
no `-pie`, no `.rela.dyn`, nothing to apply, and the audit gate is
`ARCH=arm`-only in the Makefile.

### 2.4 How it is wired

| file | change |
|---|---|
| `Makefile` | `ARCH=arm` `LDFLAGS = -T linker.ld -pie -z notext --no-dynamic-linker`, with the reason; after the link, `$(RELOC_AUDIT) --gate $@` |
| `linker.ld` | `__image_start` at the first byte of `.text`; an explicit `.rela.dyn` output section between `__rela_start`/`__rela_end`; `ASSERT(__image_start == 0x40080000)` keeps the link base in step with the C constant |
| `src/kernel/arch/arm/boot.s` | `bl arch_relocate_self` in `run_main`, i.e. after the `.bss` clear and before `main()` |
| `src/kernel/arch/arm/reloc.c` | the pass, the counters, the `[RELOC]` row and the halt-on-failure |
| `src/include/reloc.h` | the contract (`arch_reloc_*`), AArch64-only |
| `src/kernel/main.c` | `arch_reloc_report()` right after `uart_init()` |
| `src/kernel/reloc_test.c` | the gate test (§3) |
| `src/kernel/unit_test.{c,h}` | registration, AArch64-only |
| `tools/kernel_reloc_audit.py` | the audit and the post-link gate |

Three details that are not obvious and are each a way to get it wrong:

1. **Order.** `clear_bss` must run *before* the pass, because the pass's
   counters are `.bss` and the clear would erase its verdicts; and the pass must
   run *before* `main()`, the first C consumer of static data. Nothing between
   the two reads static data. (The round-3 suggestion — "before the `.bss`
   clear" — would have silently discarded the status.)
2. **The slide is `&__image_start − 0x40080000`.** Any symbol reference in code
   is `adrp`/`add`, i.e. slid at runtime, so the *link* base has to be a
   literal. `linker.ld`'s `ASSERT` is what keeps the two in step, and
   `test_reloc_pass_stats` asserts the result is page-aligned.
3. **Every entry is accounted for.** A type other than `R_AARCH64_RELATIVE`, a
   misaligned `r_offset` (an unaligned store is a hardware fault with the MMU
   off), or an empty/ragged table sets a status bit; the report row prints it
   and `halt()`s. Nothing is skipped silently, and `outside=` counts applied
   values that did not land inside the image (0 here), which is what would catch
   a wrong link base.

## 3. The red/green gate

`src/kernel/reloc_test.c` builds the hazard on purpose — a
`const struct {const char *name; int (*fn)(int); int (*fn2)(int);}` and a
`const char *const[]` — plus the real table behind `nv_shim_refusal_name()`.

Two measured details make it a test instead of decoration:

- `__attribute__((used))`, because a `static const` table nobody proves is
  needed at run time may be deleted;
- every read goes through `reloc_opaque_*()`, which launders the table address
  through an `asm` barrier. **The first version of this file lost its first two
  assertions**: with a visible initializer clang assumed the index in bounds,
  merged the identical `"reloc_probe"` literal, and folded the comparison to
  constant-true — MEASURED by disassembling the emitted `reloc_test_suite`
  (`ldr` of `g_reloc_names[1]` present, the `row->name` comparison absent).

### Red — `f022bf97` (the test) on the *base* tree (no pass, no `-pie`)

```
$ nvswarm sh wp-reloc 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests.sh'
  Running test_reloc_folded_rodata...
EXPECT_EQ FAILED: reloc_streq(row->name, "reloc_probe") == 1 at src/kernel/reloc_test.c:97
  Left:  0
  Right: 1
  Running test_reloc_real_table...
EXPECT_EQ FAILED: reloc_streq(nv_shim_refusal_name(NVSHIM_IOC_NVIF), "nvif") == 1 at src/kernel/reloc_test.c:124
  Left:  0
  Right: 1
Tests run:    467
Tests failed: 2
UNIT TESTS FAILED                                   script exit 1
```

465 + the 2 new tests, both failing on the string reached through a folded
pointer — a *clean* failure, not the exception: `EXPECT_EQ` returns, and the
assertions that would call through the pointer sit behind it, so a red run
reports instead of dying in the handler and hiding the rest of the tier. (The
crash itself was measured in round 3; this test's job is to fail loudly on
every future tree that reintroduces the shape.)

### Green — `ae19c0a9`

```
$ nvswarm sh wp-reloc 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests.sh'
[RELOC] entries=0x00000000000001A1 applied=0x00000000000001A1 slide=0x00000003FA400000 outside=0x0000000000000000 status=0x0000000000000000 ok
  Running test_reloc_folded_rodata...
  Running test_reloc_real_table...
  Running test_reloc_pass_stats...
Tests run:    468
Tests failed: 0
UNIT TESTS PASSED                                   script exit 0
```

465 → 468 is exactly the three new tests; nothing else changed count.

```
$ nvswarm build wp-reloc && nvswarm sh wp-reloc 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'
Tests run:    583
Tests failed: 0
UNIT TESTS PASSED                                   script exit 0
```

## 4. The guard, and that it actually rejects

Every AArch64 link now ends with the audit:

```
python3 tools/kernel_reloc_audit.py --gate hobbyos.elf
# kernel link range [0x40080000, 0x417AF90C)   .rela.dyn=417 entries (present)
# pointer words baked into static data (need relocating): 417
#   applied at boot (R_AARCH64_RELATIVE, addend in image): 417
#   NOTHING WILL APPLY THEM (left at their link address):     0
hobbyos.elf: reloc gate OK -- 417 kernel pointer word(s), every one applied at boot
```

The rejection was measured, not assumed — same tree, link flags reverted:

```
$ nvswarm sh wp-reloc 'make ARCH=arm MODE=unit_tests LDFLAGS="-T linker.ld" hobbyos.elf'
hobbyos.elf: FAIL -- 417 kernel pointer(s) with no applied relocation, 0 unhandled
relocation type(s). ...                              make exit 2
```

So the class has two doors closed: a tree that drops the mechanism fails the
build at the link, and a tree that keeps the mechanism but adds an entry type
the pass does not apply fails it too (`bad_types`, MEASURED logic path, not
exercised — see §6).

## 5. Audit of the folded tables still in the tree (AArch64 unit image)

`tools/kernel_reloc_audit.py --by-symbol` on the green image. "words" = pointer
words needing relocation; all 417 are now applied. Source attribution is
SOURCE (symbol → file); reachability on ARM is INFERRED.

| words | table | where | reached on ARM today? |
|---:|---|---|---|
| 108 | `l_tab_fd` | `linux_compat_fd.c:1750` | no (Linux-ABI syscalls are x86-only) |
| 94 | `l_tab_proc` | `linux_compat_proc.c` | no |
| 36 | `l_tab_tmr` | `linux_compat_timers.c` | no |
| 26 | `l_tab_core` | `linux_compat.c:458` | no |
| 18 | `tables[]` (function-local) | `linux_compat.c:593` | no |
| 18 | `l_tab_mem` | `linux_compat_mem.c` | no |
| 18 | `reason_names` | `nv_rpc.c:94` | **yes** — `nv_rpc_reason_name()` on every error path |
| 13 | `cons_dtags` | `kconsole.c:302` | yes when `CP_DIAG=` is set |
| 12 | `do_hw_params.nm` | `linux_snd.c` | no (`/dev/snd` is x86-only today) |
| 10 | `arg_names` | `nv_rpc.c:108` | yes, GSP-RPC argument validation |
| 9 | `cons_dnames` | `kconsole.c:314` | yes when `CP_DIAG=` is set |
| 9 | `vm_pw_name` | `vm.c:1749` | yes — page-walk `[MR]` rows |
| 8 | `el_compat_83` | `elf_loader.c:149` | yes — 8.3 compat lookups |
| 8 | `l_tab_shm` | `linux_compat_shm.c` | no |
| 8 | `link_names` | `nv_rpc.c:121` | yes, GSP-RPC link validation |
| 7 | `kStoreNames` | `nv_flcn.c:51` | yes — falcon store variant names |
| 5 | `.got` (5 slots) | linux-compat signal/epoll hooks | yes |
| 2 | `fat16_getdents64.selves` | `fat16.c:3503` | no (Linux-ABI `getdents64`) |
| 2 | `elem_name` | `linux_snd.c:737` | no |
| 3 + 3 | `g_reloc_probe`, `g_reloc_names` | `reloc_test.c` (new) | yes, by the new test |

**These are no longer hazards** — they are the population the pass serves, and
the reason shape (b) was rejected as the primary mechanism. What the audit
buys is that the list is now produced by a tool from the linked image instead
of by grep, and that it cannot grow silently.

Two corrections to round 3's notes (MEASURED from the audit of the ARM image):
`nv_rpc.c` is **not** empty on AArch64 — `reason_names`, `arg_names` and
`link_names` are in the ARM image (36 words) and were returning link
addresses; and the class was never limited to `nv.c`/`nv_rpc.c`.

## 6. What is NOT verified

- **Only the unit tier was booted.** `MODE=desktop` ARM was link-checked
  (gate OK, 411 words, `ET_DYN`, MEASURED) but not booted; the desktop, GUI,
  `t3_tier.sh arm`, networking and the card lane were not run. The pass is
  mode-independent, but that is reasoning, not a run.
- **The `-z notext` writable-`rodata` window is unobserved from the far side.**
  The slots are patched before `mmu_init()`; nobody measured whether `mmu.c`
  later maps that range read-only in a way that would trip a *write* — there
  are no writes after boot, so this is untested rather than wrong.
- **`bad_types` and `RELOC_ST_ALIGN` were never exercised.** Both are reachable
  only by a link that emits another relocation type or an unaligned slot; the
  tree cannot produce either today. They fail closed by construction, not by
  test.
- **The `outside=` counter is 0 for this link only.** It would catch a wrong
  `KIMAGE_LINK_BASE` (every pointer one constant off), but only if the wrong
  base still left the *other* checks passing; with a wrong base the slots land
  outside the image and the counter fires — that path was not run.
- **One slide observed.** `0x3FA400000`, twice, on this host/QEMU/Limine
  combination. Nothing forces Limine to slide by a multiple of 2 MiB; the pass
  only assumes 8-byte alignment of the slots and page alignment of the base
  (asserted in the test), which is all `R_AARCH64_RELATIVE` needs.
- **x86_64 is untouched, including its latent tables.** `nv.c`'s `nv_chips`
  and friends are still folded; on x86_64 that is correct by construction
  (base == link), and the gate does not run there. If the x64 load base ever
  moves, the class returns there and the audit would need an intel gate.
- **`test_nvshim_layout`'s `!= 0` assertion was not changed.** The real
  assertion now lives in `reloc_test.c` (which asserts the string, not the
  trueness); strengthening the other lane's test is left to its owner.

## 7. Next step

1. **Land this before any further ARM work.** It is the invariant the other
   lanes' tables were relying on without knowing it; every round-3-era table is
   now correct rather than accidentally harmless.
2. **Run the ARM desktop tier** (`t3_tier.sh arm`) on a tree with this change,
   to cover the non-unit half of §6.
3. **Consider the same gate for x86_64** if `linker_x64.ld` ever moves off
   `KIMG_BASE`, and give `nv.c`'s tables the same treatment only if that
   happens — not before, since on x64 they are already right.
