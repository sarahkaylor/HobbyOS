# ELF loader design for the HobbyOS compat track (GB-013, DEEP)

Status: **v1 — the mapping decision is SETTLED; the mapper implementation is the next unit of work.**
Owner: Coordinator. Contract: `docs/linux-abi.md` §1 (startup) and §2 (syscall entry/return).
Parser (done, tested): `src/include/elf_image.h` + `src/kernel/elf_image.c` + `src/host/elf_image_test.c`.

---

## 1. The decision: the compat corpus is **static-PIE (`ET_DYN`), not fixed-address `ET_EXEC`**

A musl `ET_EXEC` binary is linked at **0x400000**. HobbyOS **cannot map a user page there**, and that
is not a bug — it is the address-space model:

* `vm_region_insert_locked()` refuses any region outside the user window:
  `if (base < USER_VA_BASE || base + len > USER_VA_TOP) return -1;` (`src/kernel/vm.c:437-440`).
* The window is at **64 GiB** (`USER_VA_BASE`, `src/include/vm.h`) *because* low VAs are taken:
  every address space's PDPT entries 0..7 are the shared kernel **identity map**
  ("the AS PDPT's kernel entries (0..7) point at the shared kernel PDs", `src/kernel/arch/x64/mmu.c:372-375`),
  covering x64 **0-8 GiB** and ARM `0x40000000-18 GiB` (`src/include/vm.h`, window rationale comment).
* A single shared address space means the kernel keeps running with the user's CR3: the identity map
  cannot be dropped per-process without a kernel-entry table switch. That is an architecture change, not
  a loader feature.

Therefore a fixed-address Linux binary has nowhere to live. **A position-independent static binary does
not need a fixed address.** `ET_DYN` **without `PT_INTERP`** (musl's `-static-pie`) is exactly that: the
image is relocatable, the program applies its own `R_X86_64_RELATIVE` relocations at startup, and the
loader only has to place the segments at a base that preserves the ELF's own alignment rule
(`p_vaddr ≡ p_offset (mod p_align)`).

Consequences, recorded in `gaming.md`:

1. **The W0/W1… corpus for the compat track is built `-static-pie`** (`x86_64-linux-musl-gcc -static-pie`,
   or `cc -static-pie` for host smoke binaries). The weight-class ladder (§1.2) is unchanged, the build
   flags are not.
2. `ET_EXEC` still **parses** (`elf_parse` accepts it) but is flagged `needs_low_window`, and the loader
   must **refuse it with a clear serial message** rather than mapping it somewhere the program will not
   recognise. A mis-mapped binary is a mysterious guest crash; a refusal is a five-second diagnosis.
3. `PT_INTERP` (dynamic, either flavour) stays refused — that is the Program-2 glibc/ld.so epic.

**Evidence that the decision is real, not theoretical** (all produced while making it):

```
$ cc -static-pie -O1 -o /tmp/w0-spie /tmp/w0.c          # ET_DYN, no PT_INTERP
$ file /tmp/w0-spie   -> ELF 64-bit LSB pie executable ... statically linked
$ ELF_REAL_BINARY=/tmp/w0-spie ./elf_image_test_host
  PASS real binary /tmp/w0-spie -> ok (entry 0x78a0, 4 segs)      # relative entry, as PIE requires
$ ELF_REAL_BINARY=/tmp/w0-static ./elf_image_test_host           # glibc ET_EXEC for contrast
  PASS real binary /tmp/w0-static -> ok (entry 0x401720, 4 segs)  # needs_low_window = 1
```
18 synthetic fixtures (`tools/linux-abi/gen_test_elf.py`) cover the accept class (ET_EXEC, static-PIE,
bss, 3-segment) and one vector per rejection class; 58 checks, 0 failed.

## 2. Loader plan (the mapper half of GB-013)

Files: a new `src/kernel/elf_loader.c` (kernel-only, uses `vm.h`/`fs.h`), dispatched from
`load_v2_internal()` in `src/kernel/program_loader.c` after the file is opened, on the first 4 bytes
(`\x7fELF` → ELF path; anything else → the existing flat loader, unchanged).

1. **Read the file** through the existing FAT16 path (`fat16_open` / `fat16_read_direct_pos`) into a
   kernel scratch buffer, capped at `USER_IMG_SIZE` (128 MiB) — the same gate the flat loader uses.
2. **`elf_parse()`** on that buffer. Refusals: `ELF_ERR_INTERP` / `needs_low_window` / anything else →
   log `elf_strerror(rc)` on the serial console and fail the load (`-ENOEXEC` to the caller).
3. **Pick a base.** `USER_IMG_BASE` (64 GiB, already page- and 2 MiB-aligned) for an image whose
   segments start at 0 (the normal static-PIE layout); in general
   `base = elf_page_down(USER_IMG_BASE + (img.vaddr_base & (2 MiB - 1)))`-style alignment so that
   `base + p_vaddr` keeps the ELF's internal alignment. The whole span must clear `USER_HEAP_BASE_V2`
   (the flat loader's `USER_IMG_BASE + img_len > USER_HEAP_BASE_V2` check is the same test).
4. **One region per PT_LOAD segment**, with the *real* protections from `p_flags`
   (`VM_PROT_READ`/`WRITE`/`EXEC`, `VMK_IMG`, `VM_MAP_PRIVATE`) — never one big RWX region. Two
   adjacent segments that share a page must be merged into the weaker-protection union (a page's
   permission is per-page, not per-segment): compute the union first, then insert.
5. **Copy each segment** with `elf_copy_into()` into a zeroed destination (the contract: the caller
   supplies a pre-zeroed region, which the v2 IMAGE slot is). The destination is the *frame-backed*
   image region, not the raw VA — on x64 the loader needs the physical frame mapping the flat loader
   already has (`vm_image_load_first_page` / demand-fill path); start with eager copy for W0 sizes
   (≤ a few hundred KiB) and reuse the lazy `img_active` machinery only if the measurement demands it.
6. **Entry:** `process_set_entry(pid, base + img.entry, USER_MAIN_STK_TOP_V2 - 8)` — i.e. the ELF's
   relative entry plus the chosen base, never the raw `e_entry` (0x78a0 is not a mapped address).
7. **Mark the process** with `PROC_FLAG_LINUX_ABI` in the PCB so the trap path routes its `syscall`
   traps to the compat table (GB-014) instead of the native chain. Nothing else about the PCB changes.
8. **Startup contract** per `docs/linux-abi.md` §1: `%rdi`=argc, `%rsi`=argv, `%rdx`=envp, aux vector
   (`AT_ENTRY` = the *runtime* entry, `AT_PHDR` = base + phoff, `AT_PHNUM`, `AT_PAGESZ`,
   `AT_RANDOM`, `AT_EXECFN`) appended after `envp`, stack 16-byte aligned. This is the piece the
   *parser* tests cannot cover — it needs the T3/T5 kernel-boot path.

**Do not:** map `ET_EXEC` at its own VA; ignore `p_flags`; return the native `-2` "would block" marker to
a compat process; touch the ARM ABI path (the compat track is x86_64-only by design, §2.1).

## 3. Verification plan (what "done" means for GB-013)

| tier | what it proves |
|---|---|
| T1 (done) | parser accept/reject matrix, segment plan, bss/hole semantics — `elf_image_test_host`, 58 checks |
| T2 | `elf_parse` + base/alignment selection reachable in-EL1 (a kernel-mode unit test with a fixture image in the FAT16 test image) |
| T3 | a static-PIE `hello` actually **runs** on `ARCH=intel`: `write(1,"W0\n",3)` then `exit_group(0)`, via the compat syscall path |
| T4 | the same binary on the Linux build VM vs HobbyOS-intel: stdout bytes + exit status identical (GB-007 harness, GB-015 assertions) |
| T5 | (later, GB-041) a static-PIE SDL game plays under the same loader |

Two-arch rule still holds: the ELF path is guarded so ARM builds and stays green; ARM never loads ELF
(§2.1) and must not regress.

## 4. Open questions (deliberately not decided here)

1. **Eager vs demand-paged** segment fill for large images (SDL/DOOM at W3/W4). Start eager; measure.
2. **`.text` write-then-execute**: our loader copies bytes into an executable region (kernel writes to
   it before the process runs) — fine with a single address space, but it means the region is RW at
   copy time. Decide whether to add a W^X tightening pass before first user entry.
3. **`execve` for compat processes** (GB-014 shim): re-uses this loader on the `nas` path
   (`program_loader.c:914` region), including argv/envp replacement.
4. **Huge static-PIE segments straddling the 1 GiB heap slot** if the toolchain ever emits a large
   `p_align`-aligned layout; the executor's `-static-pie` output today is ~800 KiB.
