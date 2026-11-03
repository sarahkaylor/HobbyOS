# Compat PT_INTERP: the loader contract for a real glibc ld.so
Status: **v1 — implemented, gated** (M1 loader half, lane L2). How the
Linux-ABI track maps an executable + `PT_INTERP` pair and enters the
interpreter. Companion: `docs/rimworld-plan.md` §6 M1, §7 F1/F3; the native
`LDHOBBY.SO` contract (`docs/shared-libs.md` §1-§4) is unchanged. Code:
`src/kernel/elf_loader.c` (`ELF_LOAD_COMPAT_INTERP`) + `compat_stack_block`
in `src/kernel/program_loader.c`. Gate: `tools/gate/m1_glibc_hello.sh`.
## 1. What the kernel does — and does not do
Does: parse both ELFs, map the exe's `PT_LOAD`s at the image base, map the
interpreter's `PT_LOAD`s in a `USER_LIB` slot with its own `p_flags`, write the
SysV stack block, jump to the interpreter's `e_entry`.
Does **not** (binding, plan AD-3): parse symbols/hashes/`DT_NEEDED`/versions;
relocate **anything** here — the exe's `DT_RELA` stays intact for ld.so and
libc is mapped+relocated+`mprotect`ed by ld.so; relocate the **interpreter**
(a real ld.so self-relocates from `_start`; contrast `LDHOBBY.SO`, whose
contract *is* "kernel applies R_RELATIVE, zeroes DT_RELASZ"); implement a
search path, `/etc/ld.so.cache`, `LD_LIBRARY_PATH`, `dlopen`, TLS, `/proc`, a
vDSO or RELRO; write `struct hb_start` (native ABI).
## 2. Stack layout (`%rsp` at entry)
```
        high
        | strings: argv/envp, "x86_64", exec path |  AT_RANDOM (16 B)
        | auxv pairs, AT_NULL last   (18 pairs, §3)
        | NULL  <- envp terminator   | envp[envc-1]..envp[0]
        | NULL  <- argv terminator   | argv[argc-1]..argv[0]
%rsp -> | argc                        16-byte aligned
        low
```
`argv[0]` sits at `%rsp+8` **by construction**: the array base is computed
first and `argc` written at `base-8`, with `argv_addr & 0xF == 8` enforced.
Re-aligning `%rsp` after laying the arrays down leaves an 8-byte hole and the
guest reads `argv[0]==NULL` (measured on Doom).
## 3. Auxv, exact values (`compat_stack_block`)
| key | value | why |
|---|---|---|
| `AT_PHDR` 3 | exe base + (seg0.vaddr-vaddr_base) + (e_phoff-seg0.offset) | ld.so reads the exe's `PT_DYNAMIC` from the mapping, never re-opens it |
| `AT_PHENT` 4 / `AT_PHNUM` 5 / `AT_PAGESZ` 6 / `AT_FLAGS` 8 | 56 / exe `e_phnum` / 4096 / 0 | |
| `AT_BASE` 7 | interpreter load base (0 if static) | where ld.so itself is |
| `AT_ENTRY` 9 | **exe** entry (`base+e_entry`) | not the interpreter's; ld.so jumps here after relocation |
| `AT_UID`/`EUID`/`GID`/`EGID` 11-14 / `AT_SECURE` 23 | 0 | single-user model, no setuid |
| `AT_PLATFORM` 15 | -> `"x86_64"` | glibc's hwcaps subdirectory name |
| `AT_HWCAP` 16 / `AT_HWCAP2` 26 | 0 | baseline x86_64 (SSE2); never invent optional ISA bits |
| `AT_CLKTCK` 17 | 100 | `sysconf(_SC_CLK_TCK)` |
| `AT_RANDOM` 25 | -> 16 bytes from `net_get_random_bytes` | stack-guard seed |
| `AT_EXECFN` 31 / `AT_NULL` 0 | -> the path passed to the loader / 0 | Linux puts the execve argument here, not `argv[0]`; terminator |
`AT_BASE`/`AT_PHDR`/`AT_ENTRY` are the three that make the pair work; a wrong
`AT_ENTRY` is silent (ld.so relocates, then jumps back into itself).
## 4. Base-address selection
- **Exe**: `USER_IMG_BASE` (0x1_0000_0000) + `vaddr_base & 2 MiB-1`, so
  `base-vaddr_base` keeps every `p_align`. A `PT_INTERP` image may not reach
  `USER_LIB_BASE` (0x1_0800_0000) — the interpreter and every ld.so-mapped DSO
  live there.
- **Interpreter**: first free 16 MiB `USER_LIB` slot from `USER_LIB_BASE`
  (`USER_LIB_MAX` 55). ld.so 2.31 spans ~0x2F000 → slot 0 in practice.
- **libc and friends**: chosen by *ld.so* via `mmap(NULL,...)`; the kernel puts
  them in the MMAP arena (0x1_8000_0000+). The kernel never maps a DSO.
## 5. Entry state
`%rsp` = the `argc` word (16-byte aligned); `%rdx` = 0 (the `rtld_fini` slot;
glibc's `_start` does not read it — Linux leaves it unspecified, HobbyOS makes
it deterministic); all other GPRs 0 (a fresh PCB clears `context[0..35]`).
Nothing else is passed: the stack is the interpreter's only input.
## 6. Path resolution and the DSO search order (F3)
`PT_INTERP` is resolved with `fat16_resolve_path`, which matches VFAT **long
names**, not just `readdir` display. Measured: `/lib64/ld-linux-x86-64.so.2`
(26-char component) and `/lib/x86_64-linux-gnu/libc.so.6` both resolve from a
1 GiB FAT16 image staged with `mcopy` — so the F3 8.3 **alias map is dormant**:
no `/LIBMAP.TXT`, real sonames on disk.
The kernel implements none of the search; ld.so does. Order that must hold
(launcher + staging contract): (1) `LD_LIBRARY_PATH`, the only list that
applies globally, including to a DSO's own `DT_NEEDED`; (2) the **requesting
object's** `DT_RUNPATH` (per-object, NOT inherited: the game exe's `$ORIGIN`
does not resolve `UnityPlayer.so`'s own `libdl.so.2`); (3) fixed `/LIB64`
fallback; (4) alias map (dormant). So `/LIB64` carries **every** runtime DSO
explicitly — `ld-linux-x86-64.so.2`, `libc.so.6`, `libm.so.6`,
`libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libgcc_s.so.1`; never rely on
`libc.so.6` to imply the others, each is its own `DT_NEEDED`.
## 7. Failure modes (all refuse before the process is runnable)
| condition | rc | serial line |
|---|---|---|
| `PT_INTERP` not on disk | `-ENOENT` | `interpreter '<p>' not found on the FAT16 root` |
| interp unreadable / not ELF / wrong arch / nested `PT_INTERP` | `-ENOEXEC` | `elf loader: interpreter: <reason>` |
| interp fixed-address `ET_EXEC`; exe below the window | `-ENOEXEC` | `... below the user window` / `a -static-pie build is required` |
| interp > one slot, no free slot, exe reaches the DSO band | `-ENOSPC` | `does not fit` / `no free USER_LIB slot` / `overlaps the DSO band` |
| out of frames | `-ENOMEM` | propagated |
| compat caller without `ELF_LOAD_COMPAT_INTERP` | `-ENOEXEC` | `ELF_ERR_INTERP ...` (GB-013 contract kept) |
Every failure frees the staged buffers; mapped pages die with `vm_as_teardown`
and the caller frees the child slot. A partial pair is never runnable.
## 8. Measured evidence (`obj/m1_glibc_hello.log`, real serial)
```
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000
             exe_entry=0x10000011B0 start=0x1008001100
v2 loader: PID=2 entry=0x1008001100 sp=0x1800000000 abi=0x1 native=0 interp=0x1008000000
[M1] hello from glibc 2.31 (pid 2) / argc=1 / argv[0]=PROBE.ELF / done rc=0
```
Bring-up blockers, in order: `[LCOMPAT] ENOSYS nr=257` (openat — glibc's `open()`
*is* `openat(AT_FDCWD,...)`; compare only the low 32 bits of `dirfd`), then
`libc.so.6: failed to map segment from shared object` (the kernel refused a
file `mmap` longer than the file; glibc's ET_DYN whole-object map includes
`.bss` past EOF), then the run above. `HOME=(unset)` is the wave boot's empty
environment, not an auxv defect.
