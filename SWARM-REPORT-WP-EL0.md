# SWARM-REPORT — WP-EL0: the `/dev/nvshim` DRM ABI, proved from EL0

Branch `nv/wp-el0-shim`, cut from `5cc95b9d` (`nv/integ-swarm`, round 3 merged,
intel **583 / 0**, ARM 465 / 0). Rung **N2**, the "caller's side" half that
`SWARM-REPORT-WP-F3.md` §6 named as unproven: *"nothing userspace has ever mapped
this."*

**Status: gate met, MEASURED — 33/33 rows PASS from a real EL0 process**, plus
**one ABI defect found, fixed, and shown red before green**, plus a mutation run
that turns the same test red.

Claims are tagged MEASURED (seen in a log on this branch), SOURCE (read from a
cited contract) or INFERRED. Evidence:
`evidence/nv/el0-green-33of33.log` (the transcript, trimmed to console rows),
`evidence/nv/el0-red-closed-fd.log`, `evidence/nv/el0-red-map-handle-mutation.log`,
`evidence/nv/el0-red-movaps-fault.log`.

---

## 1. What changed

| File | What |
|---|---|
| `src/user/nvabi_probe.c` | **new.** The EL0 client: a freestanding static-PIE x86_64 ELF, 836 lines, no libc (own `syscall` stubs, own number formatting, own FNV-1a). Opens `/dev/nvshim`, runs VERSION → GET_CAP → GETPARAM → VM_INIT → GEM_NEW → mmap → write → VM_BIND → CHANNEL_ALLOC → SYNCOBJ_CREATE → EXEC → SYNCOBJ_WAIT, then 19 hostile/refusal rows. One `write(1)` per row, one row per stage. Client-side `_Static_assert`s on every struct size and request number it uses, so a contract drift breaks the client's build instead of confusing it. |
| `src/kernel/linux_compat_fd.c` | **the fix** (§5): `l_fd_ioctl()` answers `-EBADF` for a closed fd, before any request dispatch. 14 lines including the comment. |
| `Makefile` | `nvabi_elf` → `$(OBJ_DIR)/nvabi_probe.elf` (`clang -static-pie -nostdlib`, ET_DYN asserted, x86_64-only). **Not** a `disk.img` prerequisite, so the unit tier's build is untouched. |
| `tools/nv/el0_wave.sh` | **new.** Build → stage `::/NVABI.ELF` + `::/WAVE.CFG` through the Makefile's `RUN_STAGE` hook → boot MODE=test under a deadline → extract rows → PASS/FAIL on `[EL0] RESULT`. |
| `evidence/nv/el0-*.log` | the four transcripts quoted below. |

No driver code was copied. Struct layouts and request numbers come from
`src/include/nv_drm_uapi.h`, which mirrors `include/uapi/drm/nouveau_drm.h` and
`include/uapi/drm/drm.h` (SOURCE), and are re-asserted independently in the
client.

---

## 2. Why the vehicle is a Linux-ABI ELF, and not a native `.BIN`

Measured/SOURCE, and worth recording because it is the first thing a native
client hits:

- **A native HobbyOS program cannot issue a single DRM request.** There is no
  `SYS_IOCTL` row — `src/include/syscall.h`'s table is frozen at `SYS_MAX 106`
  and `src/libc/src/ioctl.c` says so in its header comment; the per-device rows
  that exist (`SYS_FB_IOCTL` 92, `SYS_INPUT_IOCTL` 93) are each one device's
  requests. `/dev/nvshim`'s surface is served only through the compat
  `ioctl(16)` route (`linux_compat_fd.c:919-926`).
- **So the honest caller is what nvk actually is: a Linux ELF.** The probe is
  built as a static-PIE with `clang -nostdlib` (no musl on the build host, and
  musl's start-up would be a second thing under test) and enters through
  `elf_loader` → `PROC_FLAG_LINUX_ABI`. The loader row proves it:
  `v2 loader: PID=2 entry=0x100000BB70 sp=0x1800000000 ... abi=0x1 native=0`.

Adding a native `SYS_IOCTL` row would have been the other way in. It is not this
lane's call to spend a frozen syscall number, and it would not have exercised the
route nvk will use.

---

## 3. The transcript (MEASURED, verbatim)

`tools/nv/el0_wave.sh` on `sarah@192.168.0.250`, MODE=test, `/WAVE.CFG = NVABI.ELF`.
Client rows (`[EL0]`) and the kernel's own (`[NVSHIM]`) are interleaved exactly as
they were emitted; the full file is `evidence/nv/el0-green-33of33.log`.

```
[EL0] 01 open path=/dev/nvshim fd=3 PASS
[EL0] 02 version name=nouveau v=1.3.1 name_len=8 PASS
[EL0] 03 get_cap capability=SYNCOBJ value=1 PASS
[NVSHIM] drm device dev=1 chipset=0x192 mock=1
[NVSHIM] drm getparam param=11 value=0x192
[NVSHIM] drm getparam param=4 value=0x2684
[NVSHIM] drm getparam param=8 value=0x100000000
[EL0] 04 getparam chipset=0x192 pci=0x2684 fb_size=0x100000000 PASS
[NVSHIM] drm refuse size nr=80 want=16 got=8
[EL0] 05 arg_size vm_init_enc_size=8 ret_errno=22 PASS
[NVSHIM] drm vm vm=1 umd_end=0x8000000000 total=0x10000000000
[EL0] 06 vm_init umd_end=0x8000000000 total=0x10000000000 errno=0 PASS
[NVSHIM] drm gem_new handle=1 size=0x2000 offset=0x204c4a000 domain=0x9 map=0x1000
[EL0] 07 gem_new handle=1 size=0x2000 bus=0x204c4a000 map_handle=0x1000 PASS
[NVSHIM] drm gem_info handle=1 offset=0x204c4a000 size=0x2000 map=0x1000
[EL0] 08 gem_info map_handle=0x1000 size=0x2000 foreign_errno=22 PASS
[NVSHIM] drm mmap refused off=0x0 reason=no_such_object
[EL0] 09 mmap_off0 errno=22 PASS
[NVSHIM] drm mmap refused handle=778 reason=not_this_clients_object
[EL0] 10 mmap_foreign offset=0x30a000 errno=22 PASS
[NVSHIM] drm mmap handle=1 va=0x1080000000 len=0x2000 phys=0x204c4a000 domain=0x9
[EL0] 11 mmap offset=0x1000 len=0x2000 va=0x1080000000 errno=0 PASS
[EL0] 12 bo_write bytes=64 digest=0x80bce9ae last_byte=0x5a PASS
[NVSHIM] drm bind vm=1 ops=1 first_addr=0x10000 first_range=0x2000
[EL0] 13 vm_bind ops=1 addr=0x10000 range=0x2000 handle=1 PASS
[NVSHIM] drm channel ch=1 pushbuf=0x2050a1000 size=0x10000
[EL0] 14 channel ch=1 domains=0x2 nr_subchan=0 PASS
[NVSHIM] drm syncobj create handle=1 signaled=0
[EL0] 15 syncobj_create handle=1 PASS
[NVSHIM] drm exec ch=1 pushes=1 sigs=1 bytes=64 stream=0x80bce9ae fence=0x1
[EL0] 16 exec ch=1 push_va=0x10000 push_len=64 digest=0x80bce9ae errno=0 PASS
[NVSHIM] drm syncobj wait n=1 retired=1
[EL0] 17 syncobj_wait handles=1 first_signaled=0 errno=0 PASS
[NVSHIM] drm syncobj wait n=1 retired=0 timeout=poll
[EL0] 18 syncobj_reset_wait reset_ret=0 wait_errno=62 PASS
[NVSHIM] drm syncobj wait n=1 retired=1
[EL0] 19 syncobj_signal_wait signal_ret=0 wait_errno=0 PASS
[NVSHIM] drm refuse nvif errno=ENOSYS
[EL0] 20 refuse_nvif reason=nvif errno=38 PASS
[EL0] 21 unknown_nr nr=0x7e errno=25 PASS
[NVSHIM] drm vm refused reason=objects_exist
[EL0] 22 vm_reinit reason=objects_exist errno=38 PASS
[EL0] 23 ptr_arg arg=0xdead0000 errno=14 PASS
[EL0] 24 ptr_inner op_ptr=0xdead1000 errno=14 PASS
[EL0] 25 ptr_exec_push push_ptr=0xdead2000 errno=14 PASS
[NVSHIM] drm exec refused push_index=0 va=0x80000 rm=-22 reason=stream_not_in_memory
[EL0] 26 exec_unbound_va push_va=0x80000 errno=22 PASS
[NVSHIM] drm mmap refused handle=1 len=0x3000 size=0x2000 reason=beyond_object
[NVSHIM] drm mmap refused flags=0x2 reason=needs_shared
[NVSHIM] drm mmap refused prot=0x5 reason=protection
[EL0] 27 mmap_geometry len_past_object=22 private=22 exec=22 PASS
[NVSHIM] drm mmap handle=1 va=0x1080002000 len=0x1000 phys=0x204c4a000 domain=0x9
[EL0] 28 mmap_alias va2=0x1080002000 errno=0 PASS
[NVSHIM] drm device dev=1 chipset=0x192 mock=1
[NVSHIM] drm gem_new handle=3 size=0x1000 offset=0x205ca1000 domain=0x9 map=0x3000
[EL0] 29 two_clients fd2=4 old_handle_errno=22 new_handle=3 PASS
[NVSHIM] drm gem_close handle=1
[EL0] 30 gem_close_unmaps close_ret=0 child_status=11 PASS
[NVSHIM] drm device dev=1 chipset=0x192 mock=1
[NVSHIM] drm getparam param=11 value=0x192
[EL0] 31 fork_inherit_fd inherited_fd=3 child_exit=0 PASS
[EL0] 32 close_ioctl closed_fd=3 errno=9 PASS
[EL0] 33 fd_lifetime new_fd=3 old_handle=1 errno=22 PASS
[EL0] RESULT PASS 33/33
```

**The row that is the point of the lane: the two `exec` rows.** The kernel's EXEC
row says `bytes=64 stream=0x80bce9ae`, and the client computed `digest=0x80bce9ae`
over the bytes it wrote *through its own mmap*. The kernel read, out of the RM's
pool frames, exactly the 64 bytes an EL0 process wrote through page tables — no
copy, no flush request, and the digest comes from two independent
implementations (the kernel's `shim_sum_update` and the client's `fnv1a`).

### 3a. What each named suspect actually got

| Suspect | EL0 rows | Result (MEASURED) |
|---|---|---|
| **argument size validation** | 05 | a real number (`0x50`) carrying `_IOC` size 8 → `-EINVAL`, and the **next** VM_INIT still succeeds, so the refused call created nothing (`drm refuse size nr=80 want=16 got=8`) |
| **pointer translation** | 23, 24, 25, 26 | unmapped `arg` → `-EFAULT`; unmapped `op_ptr` inside a batch → `-EFAULT`; unmapped `push_ptr` → `-EFAULT`; a valid push array whose **GPU VA** is unbound → `-EINVAL` with `reason=stream_not_in_memory`. The kernel survived all four (no kernel-mode fault row, no panic) |
| **fd lifetime** | 29, 30, 31, 32, 33 | two fds = two clients (old handle foreign, new handle `3` vs `1`); `GEM_CLOSE` takes the mapping down (a forked child that touches it dies with `status=11`); a forked child is a fresh client with the parent's handle foreign; closed fd → `EBADF` (**this one was broken — §4**); a re-opened fd is a fresh client |
| **mmap offset** | 07, 09, 10, 11, 27, 28 | `map_handle == handle << 12` on the client's side of the wire; offset 0 and a foreign handle refused by name; the mmap lands at `0x1080000000` over frames at `0x204c4a000`; length past the object, `MAP_PRIVATE` and `PROT_EXEC` each refused with their own reason; a second mapping of the same object aliases the first |

---

## 4. The defect EL0 found (red → green)

**`ioctl(closed_fd, …)` answered `ENOTTY` (25) where Linux answers `EBADF` (9).**

The compat ioctl route validated the fd *range*, then fell through a chain of
`file_is_dev*()` type checks — which answer 0 for a slot that holds nothing — to a
final `-ENOTTY`. So a client that used an fd after `close()` was told *"this node
does not understand that request"* instead of *"you closed it"*, and a
use-after-close became indistinguishable from a wrong request number. That is the
worst shape of this project's recurring failure: a wrong answer that looks like a
different, plausible one.

Red, before the fix (`evidence/nv/el0-red-closed-fd.log`, whole run 32/33):

```
[EL0] 31 fork_inherit_fd inherited_fd=3 child_exit=0 PASS
[EL0] 32 close_ioctl closed_fd=3 errno=25 FAIL
[EL0] 33 fd_lifetime new_fd=3 old_handle=1 errno=22 PASS
[EL0] RESULT FAIL 32/33 failed=1
```

Green, after the fix (`evidence/nv/el0-green-33of33.log`):

```
[EL0] 32 close_ioctl closed_fd=3 errno=9 PASS
[EL0] RESULT PASS 33/33
```

The fix is in `src/kernel/linux_compat_fd.c::l_fd_ioctl`, 14 lines with the
comment: after the range check, resolve the fd through the group table and answer
`-EBADF` if the slot is empty (the same check `l_fd_fadvise64()` already made).
It is one level up from `nv_shim.c` rather than inside it, and that is the honest
description: `nv_shim.c` never sees a closed fd, so no change inside it could have
fixed this. It applies to the whole table, so every device node the compat surface
serves now inherits Linux's answer.

**Why no kernel test could have found it:** `nv_shim_drm_test.c` calls
`nv_shim_drm_ioctl(p, fd, …)` directly with an fd it obtained from `file_open()`
and never closes mid-test. The defect lives in the *route*, between the syscall
and the handler — the exact seam this lane exists to test.

---

## 5. Two more things only EL0 could show

**(a) The first EL0 run died on `movaps` — a client defect, recorded because it
is what a naive EL0 client hits.** `evidence/nv/el0-red-movaps-fault.log`, after
11 clean rows:

```
[KERNEL] User process fault! Vector: 13 RIP: 0x10000048ac
[KERNEL] fault-pc region=0x0000001000001000+0x0000000000008000 prot=5 kind=1 ... err=0x0 rsp=0x00000017FFFFFAE8
```

`objdump` at `main+0x38ac` is `0f 29 84 24 d0 02 00 00  movaps %xmm0,0x2d0(%rsp)`
— an aligned 16-byte store to `0x17FFFFFDB8`, which is 8 mod 16 → `#GP(0)`,
vector 13, err 0. Cause: `-O2` vectorised the 64-byte BO copy, and the probe's
`_start` had a frame pointer of its own, so `main` was entered 8 bytes off SysV
alignment. The kernel's entry sets `sp=0x1800000000` (16-aligned, Linux's rule);
a freestanding client owns its own alignment. Fixed in `_start` with
`andq $-16, %rsp`. A musl crt would have masked this; a freestanding one cannot.

**(b) A forked child that inherits the fd is a *different* client.** Row 31,
`child_exit=0`: the child's `GETPARAM` works (it silently gets a fresh client —
see the second `drm device dev=1` row at line 58/63 of the transcript) and the
parent's handle is foreign to it (`-EINVAL`). Linux shares the `drm_file` across
`fork`; this node keys a client on *(process-group pid, fd)*, which is documented
in `nv_shim.h` but is a real divergence a nvk worker would meet. Measured, not
fixed: fixing it means keying the client on the open *file* object and adding
object refcounts to the mock, which is a design change, not a repair.

---

## 6. The mutation run (a test that never failed is not evidence)

Mutated one behaviour the EL0 program depends on, in `nv_shim.c`:

```c
- uint64_t handle = offset >> 12;   /* drm_vma_node_offset_addr's encoding */
+ uint64_t handle = offset >> 13;   /* MUTANT */
```

Chosen deliberately on the **decode** side: the client's `map_handle` is then
correct and the kernel misnames the object, so no client-side arithmetic can hide
it. Red (`evidence/nv/el0-red-map-handle-mutation.log`):

```
[EL0] 07 gem_new handle=1 size=0x2000 bus=0x204c4a000 map_handle=0x1000 PASS
[EL0] 09 mmap_off0 errno=22 PASS
[EL0] 10 mmap_foreign offset=0x30a000 errno=22 PASS
[EL0] 11 mmap offset=0x1000 len=0x2000 va=(none) errno=22 FAIL
[EL0] RESULT FAIL (no CPU mapping: the rest of the ABI needs it)
```

The program then stops with a named failure rather than continuing into steps it
cannot support. Mutant reverted; `git diff` after the revert contains only the
Makefile and `linux_compat_fd.c` changes.

---

## 7. Gates (MEASURED)

| Gate | Command | Line |
|---|---|---|
| intel build | `nvswarm build wp-el0` | `rc=0` |
| intel unit tier | `run_unit_tests_intel.sh` | `Tests run: 583` / `Tests failed: 0` / `UNIT TESTS PASSED` |
| EL0 wave, end to end | `tools/nv/el0_wave.sh` on c4 | `[el0] PASS`, `[EL0] RESULT PASS 33/33` |
| per-TU ARM, kernel file touched | `make ARCH=arm obj/arm/linux_compat_fd.o` | 0 errors (2 pre-existing warnings in `vm.h`) |
| ARM guard, client file | `clang --target=aarch64-linux-gnu -ffreestanding -c src/user/nvabi_probe.c` | rc=0, empty TU (the file is x86_64-only by construction, and says so) |

---

## 8. What a stock nvk still needs — and what this does NOT prove

Not in this lane's gift, but now measurable from the same harness:

1. **NVIF** (`0x47`), **GEM_PUSHBUF** (`0x81`), **the three PRIME rows**,
   **SVM** — all still named `-ENOSYS`. Row 20 shows the refusal is delivered,
   attributed, and named from EL0; that is the whole of what this lane adds about
   them. **A stock nvk stops at NVIF** — the winsys creates its device object
   through it (`nvif`'s `NV_IF_...` object model), so N5 (`vulkaninfo`) cannot be
   reached from this node yet. INFERRED from the refusal set + the uAPI, not
   measured against a real nvk.
2. **Timeline syncobjs** — refused by name; only the binary half is served.
3. **`fork` semantics** — §5(b).
4. **A real engine.** Every fence here retires because the mock retires at submit
   (`drm exec … fence=0x1`, then `syncobj wait n=1 retired=1`). The *state
   machine* is proven; the *hardware* is not, and row 18 (`wait_errno=62` after a
   reset) is the control that keeps row 17 from being true by construction.

What this run does NOT prove: that `nvk` itself attaches (it is not on the image
and NVIF alone stops it); that any of this survives a real GSP-RM (the RM is the
mock); that the `-ENOSYS` rows would be served correctly if implemented; or that
a 2 GiB+ BO subtree works (`SHIM_MAX_ALLOC_BYTES` is 512 MiB per client and this
probe asks for 8 KiB).

---

## 9. Next step

The cheapest next rung is **NVIF**: it is the single row between this ABI and a
winsys that can enumerate, and it is `-ENOSYS` by one table entry. The same
harness (`tools/nv/el0_wave.sh`) will report whether a real winsys got further,
because the probe already speaks the exact sequence Mesa's nouveau winsys runs and
prints a named refusal at the first thing that is not there.
