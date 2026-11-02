# LDHOBBY.SO — HobbyOS's native ELF interpreter (lane D, v1)

Implementation of `docs/shared-libs.md` §4 lane D. The kernel maps the
executable and this interpreter, writes `struct hb_start` at `USER_START_BASE`
and enters `_start` here; everything after that is userspace.

```
src/include/ldso.h     the interpreter's interface: ELF shapes, module/scope,
                       result, platform hooks, entry points
src/user/ld/ldso.c     policy: hb_start checks, DSO mapping, TLS layout,
                       scope order, relocation application, init arrays, entry
src/user/ld/elf_dyn.c  ELF readers: header/phdr/dynamic parsing, symbol
                       tables (GNU hash, SysV hash, linear), DT_NEEDED
src/user/ld/strtab.c   strings, the two ELF hash functions, error buffer
src/user/ld/mmap_io.c  every syscall (raw on-device, POSIX under HOST_TEST)
src/user/ld/host_test.c  the harness's assertions
tools/ldso_host_test.sh  builds fixtures + harness and runs them
```

## Load model

1. Validate `hb_start` (`magic`, `abi`, `phent`, `phnum`, entry/phdr inside
   the image) and refuse with one serial line otherwise.
2. Relocate ourselves (`R_*_RELATIVE` only). The linker output currently has
   **zero** dynamic relocations — the code uses no GOT/PLT — so this pass is a
   safety net, not a requirement. Anything else in the interpreter's own image
   is a build error and is reported as such.
3. Walk the executable's `PT_DYNAMIC`, then each loaded DSO's `DT_NEEDED`
   breadth-first in table order (deduplicated by 8.3 name). Load order *is*
   scope order; the list is never re-sorted.
4. **Reserve** each module's whole span as anonymous RW in the next free
   16 MiB `USER_LIB` slot, then map each `PT_LOAD` over the reservation with
   `MAP_FIXED`. Reserving first is not cosmetic: mapping at an address next to
   an unrelated `mmap(NULL)` result replaces whatever VMA was there (this cost
   an afternoon), and a reserved span also gives every `.bss` tail zero pages
   without a second mapping.
5. Per segment, the file offset is `p_offset - (p_vaddr & 0xfff)`; the p_align
   congruence rule makes it page-aligned. `page_down(p_offset)` is *wrong*
   whenever lld's `-z separate-code` puts a segment's vaddr on a different page
   from its offset (it does), and silently maps the wrong bytes.
6. `.bss` is zeroed explicitly after relaxation. The file mapping covers whole
   pages, so the page holding the end of the file data can also contain section
   headers; relying on "the kernel zero-fills past EOF" is not enough.
7. Text is `PROT_READ` from the first mapping. Only `PF_W` segments are
   relaxed to RW for relocation, and `protect_module` then sets every segment
   to exactly its `p_flags` and makes `PT_GNU_RELRO` read-only (unless the page
   rounded RELRO range would overlap an executable segment).

## Symbols and relocations

Scope: executable first, then DSOs in load order. Lookup uses `DT_GNU_HASH`
when present, else `DT_HASH`, else a linear `.dynsym` scan; all three paths are
tested against real tables (a module built with only one hash table must not
claim the other). `.dynsym` size comes from `DT_HASH`'s `nchain`, else from a
walk of the GNU hash buckets (exact), else from the `.dynstr - .dynsym`
distance.

Supported: `R_*_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS64`, `COPY`,
`R_X86_64_TPOFF64` / `R_AARCH64_TLS_TPREL64`, and `R_*_NONE`.
Everything else aborts the load with the module name and the numeric type;
`STT_GNU_IFUNC` symbols are refused the same way. `DT_TEXTREL` is refused at
parse time (W^X).

`COPY` resolves with the relocation's own module skipped: the executable's
copy symbol is the relocation's target, so resolving through the full scope
would be a self-copy. All modules then bind to the executable's copy, which is
what makes it authoritative (the host test proves a DSO sees a write made
through the executable's copy).

Relocations run **dependencies first** (reverse load order, executable last) so
a `COPY` in the executable reads already-relocated data from its source DSO.

## Static TLS (initial-exec only)

One block per process, laid out from what lld bakes into the executable's own
TLS accesses — measured, not chosen:

* x86_64 (variant II): `mov %fs:0, %rax; lea -0x10(%rax), ...` for a 16-byte
  TLS image. The executable's own offsets are fixed relative to the *end* of
  its block, so the executable's block must end exactly at TP; DSOs stack
  below it. `FS:[0]` must hold TP (the self pointer clang reads).
* aarch64 (variant I): `TPREL = 0x10 + offset`, so TP points at a 16-byte TCB
  head and the executable's block starts at `TP+16`; DSOs follow.

Per-module relocation bias is therefore `-(TP - block_start)` on x86_64 and
`+(block_start - TP)` on aarch64. A module whose TLS does not fit the block
(1 MiB cap) fails the load loudly. **No DTV, no `__tls_get_addr`**: a module
loaded after process start could not be given TLS, which is exactly why
`dlopen` is a v1 non-goal (docs §6).

## Build

```
clang -c -O2 -Wall -Wextra -Isrc/include -Isrc/user/ld \
  --target=<x86_64|aarch64>-none-elf -ffreestanding -nostdinc \
  -isystem $(clang -print-resource-dir)/include \
  -fPIC -fno-stack-protector -fno-builtin -fvisibility=hidden \
  src/user/ld/{ldso,elf_dyn,strtab,mmap_io}.c
ld.lld -shared -soname LDHOBBY.SO -e _start -z max-page-size=4096 \
  --no-undefined -o LDHOBBY.SO ldso.o elf_dyn.o strtab.o mmap_io.o
```

`-fvisibility=hidden` is load-bearing: it keeps the interpreter free of PLT
entries, so it can run before any relocation. `-fPIC` (not `-mcmodel=large`)
is required — the image lives above 64 GiB and large-model absolute addressing
would need text relocations. `tools/ldso_host_test.sh --cross` builds and
checks both targets.

## Host gate

`tools/ldso_host_test.sh [--cross] [--keep]` builds a five-module fixture chain
with clang + ld.lld (`LIBA.SO` sysv-hash-only, `LIBB.SO` gnu-hash-only,
`LIBD.SO` depends on `LIBE.SO`, `APP` ET_DYN + `PT_INTERP`) plus an ET_EXEC
`COPY` fixture, a missing-`DT_NEEDED` fixture and a patched-relocation fixture,
then runs 33 assertions. It owns no build-order dependency on lane C.

## Kernel contract: what is assumed, and the gaps found

Assumed (docs §2): `SYS_MMAP` maps a regular file for `fd >= 0` with
`MAP_PRIVATE`; `MAP_FIXED` (or at least an honoured address) inside
`USER_LIB_BASE..USER_LIB_LIMIT`; `SYS_MPROTECT` on the resulting regions;
`SYS_SET_TLS`; `hb_start.interp_base` filled.

Gaps reported to the coordinator:

1. `docs/shared-libs.md` §2 names `USER_LIB_BASE` / `USER_START_BASE`, but
   `src/include/vm.h` defines only the `_OFF`/`_SIZE` forms. Derived in
   `src/include/ldso.h` (`HB_LDSO_USER_LIB_BASE`, `HB_LDSO_USER_START_BASE`)
   rather than editing a coordinator-owned header.
2. The current `vm_mmap` refuses `fd >= 0` unless `MAP_SHARED` and only knows
   memfd; lane A's file-backed object is the dependency. The interpreter
   reserves with `MAP_FIXED|MAP_ANONYMOUS` and maps segments with
   `MAP_FIXED|MAP_PRIVATE`, so the file path must accept both.
3. The reserved span is `VMK_ANON` until a segment is mapped over it; the final
   kind of the DSO text region is whatever lane A assigns to file mappings
   (`VMK_LIB` per the freeze). The interpreter cannot set a region kind.
4. `hb_start.interp_base == 0` is refused with a message: without it the
   interpreter cannot find its own ELF header (the kernel reports the base, so
   this only fires on a kernel bug).
