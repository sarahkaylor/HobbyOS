# Lane W5-NOTHROW — clang in-OS `_Znwm` override wall: evidence (2026-10-14)

Fixes the w4-clang blocker b2 (the T2.4b wall) at the **build-config/closure
level** (AD-3: zero LLVM/libc++ source patches) in the warm l1 worktree on
`selfhost/w5-nothrow` (base tip 92815fa). Raw serial excerpts in
`evidence-w5-nothrow/*`. Driver: `tools/w5_nothrow_probe.py` (adaptation of
`tools/w4_clang_probe.py`, imports the harness unchanged).

## Verdict table

| smoke | in-OS command | result |
|---|---|---|
| T2.3b | `CLANG3.BIN --version` (fixed closure, host-flat) | **PASS** — full 21.1.8 banner, `phase=w5-version-done rc=0`, `ACCEPTANCE-COMPLETE` (t23b-version.serial.txt) |
| T2.4 | `CLANG3.BIN -c HELLO.C -o HELLO.O` (fixed closure, acceptance flags) | **PASS** — `phase=w5-compile-done rc=0`, `ACCEPTANCE-COMPLETE`, no EC=0x3C, no kernel fault; in-OS HELLO.O (2272 B, ELF64 REL AArch64, `_Z4mainiPPc` + `print`/`print_dec` undefs + `.rodata.str1.1`) pulled off the image and verified host-side (t24-compile.serial.txt) |

## Root cause #1 (the w4 b2 wall): cxxrt.o's strong operator new

`HobbyOS libc.a` embeds `src/libc/src/cxxrt.o` (deliberate design: strong
new/delete = "the HobbyOS allocator remains the single backing"). A toolchain
binary's link pulls that member for `__cxa_atexit`/`__dso_handle` (libc++abi's
cxa_atexit.cpp is not built), so its strong `_Znwm` (plain `.text`, no
`__lcxx_override` section attribute) beats libc++ new.o's WEAK
section-attributed `_Znwm`. libc++ built `-fno-exceptions` asserts at runtime
that the throwing `operator new` lies inside `[__start,__stop)`
`__lcxx_override` (new.cpp nothrow path); with `_Znwm` 100 KB *before* the
section, the assert trips → `brk #1` (EC=0x3C) in-OS.

Evidence (pre-fix clang-21): `_Znwm` **T** 0x100484e410 in `.text`; section
`__lcxx_override` [0x1004866cc0,0x1004866d84) contained only the aligned
new/delete (weak, in-section). cxxrt.o `_Znwm` body (`cmp x0,#1; csinc;
add x0,#0x10; bl malloc; and ...; stur raw,[-8]; cxxrt_oom`) is byte-identical
to the final binary's `_Znwm` → provenance proven. All four toolchain ELFs
(clang-21, lld, llvm-ar, llvm-objcopy) share the defect.

**Fix:** `src/user/toolchain/sysroot-headers/cxxrt_shim.cpp` + closure edit in
`toolchain-llvm-hobbyos.cmake`. The shim heads the toolchain closure (before
libcxx.a/libc.a): strong `operator new/new[]` with
`__attribute__((section("__lcxx_override")))` so the resolved `_Znwm`/`_Znam`
land INSIDE the override section (override check passes), plus
`__cxa_atexit`/`__cxa_finalize`/`__dso_handle` so lld never extracts cxxrt.o
(its strong plain-`.text` new/delete never enter the link). Fail-safe: if
cxxrt.o were ever pulled again, strong-vs-strong duplicate → loud link error.

Verified post-fix: clang-21 `_Znwm` == `__start___lcxx_override` ==
0x1004866dd0 (inside [0x1004866dd0,0x1004866f04)); single strong `_Znwm` /
`__cxa_atexit` / `cxxrt_oom` / `__dso_handle` (no cxxrt.o). Same for lld
(0x100287b1b0), llvm-ar (0x10003e2760), llvm-objcopy (0x1000397fb0).

## Root cause #2 (second in-OS wall, surfaced once #1 was fixed): delete of libc++ nothrow-new memory

With #1 fixed, clang compiled further and hit a NEW data abort:
`free(0x48424F424C4B4D47)` — the malloc BLOCK_MAGIC passed as a pointer
(t24-wall2-freemagic.serial.txt). gdb-multiarch over QEMU's gdbstub latched
the caller chain (gdb-free-caller.txt):

```
operator delete(void*) [shim] 0x10047d86b0
  -> tc_free_aligned(p)        0x10047d86e0
       -> free([p-8])          -> x0 = BLOCK_MAGIC -> data abort
```

clang allocates some objects via libc++'s `operator new(std::nothrow)`, whose
`-fno-exceptions` path returns a **raw `std::malloc` payload with NO stash** —
so `[p-8]` holds the block's own BLOCK_MAGIC, and the shim's
`free(((void**)p)[-1])` fed the magic to `free()`.

**Fix (commit 7b2607a):** HobbyOS `free()` natively distinguishes stashed
(`[p-8]`=raw pointer) from raw (`[p-8]`=BLOCK_MAGIC) payloads; the shim's
delete now simply calls `free(ptr)` exactly like libc++'s delete.cpp —
correct for BOTH classes.

## OS-wide follow-up (out of this lane's scope, recommend to the OS owner)

`src/libc/src/cxxrt.cpp`'s operator new/delete currently lack the
`__lcxx_override` section attribute AND cxxrt's `delete` uses the stash-read
form: any OS userland program that links cxxrt.o + libc++ AND calls
`new(nothrow)` (raw malloc payload) then `delete` (cxxrt) has the SAME
latent free(BLOCK_MAGIC) bug — only masked because few OS C++ programs pull
both and use nothrow new. Both fixes above are portable: give cxxrt's
new/delete the section attribute and make cxxrt's delete delegate to
`free(ptr)`.

## Probe notes

- Probe image = copy of the w3-tools staged disk; CLANG3.BIN staged as a NEW
  `/TOOLS` name (frozen entries untouched); the `SELFHOST.BIN` driver runs a
  staged `/SRC/SELFHOST.RUN` script per w4's method (no QMP keystroke typing).
- Flat conversion: host `llvm-objcopy-21 -O binary` of
  `obj/selfhost/dist-staging/usr/local/bin/clang-21` (94,192,328 B);
  entry invariants verified: `flat[0] == ELF[0x10000]` byte-identical and
  first bytes `fd 7b bb a9` (real AArch64 `stp x29,x30` prologue).
- The known w4-kernel b4 housekeeping crash (`[IDLESTUCK]` pid=5 EL1 fault)
  can fire after a *successful* run and swallow ACCEPTANCE-COMPLETE (seen
  once on w5v); both verdicts below were captured with the terminal marker
  present on a clean run.
- Spawn args remain capped at 255 chars (driver `args[256]`) — the compile
  line is trimmed to fit, as in w4 t24b.
