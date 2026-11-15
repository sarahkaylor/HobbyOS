# cp/wine2 — toward Gate P2 (lane C2-wine2, wave 3)

Plan: `docs/cyberpunk.md` §6 P2 and §7.2c (C2-wine2 row).  Wine citations are
`file:line` in the pinned tree (`strix:~/cp/wine-build/src/wine-11.0`).  Every
number below was measured on **strix** (Ryzen AI MAX+ 395, Zen 5, AVX-512, KVM).

## 1. `MAP_PRIVATE` copy-on-write of file-backed objects

**Before:** `vm_mmap` answered `-ENOTSUP` for `MAP_PRIVATE` of a memfd or a
tmpfs file (`[LCOMPAT] mmap fail ... -> -95 path=/tmp/.wine-0/server-…/tmpmap-…`),
so `server/mapping.c:340-381` `check_current_dir_for_exec` moved every section
file (KUSER_SHARED_DATA among them) onto the FAT config dir, and every
read-only section view the client maps (`dlls/ntdll/unix/virtual.c:2339-2398`
`map_file_into_view`: read-only views are `MAP_PRIVATE` on Linux *because*
file changes stay visible through them) would have hit the same `-ENOTSUP`,
which Wine turns into `STATUS_NO_MEMORY` (only `EINVAL`/`ENOEXEC`/`ENODEV`/
`EACCES`/`EPERM` fall back to `read()`).

**Now (Linux semantics):**

| access to a page of a private object mapping | result |
|---|---|
| read / exec, page absent | the **object's own frame**, mapped without write (`VMK_SHARED` leaf: never freed by unmap/teardown).  Later `write()`s and `MAP_SHARED` stores by any process are visible through it |
| write, page absent | a **private copy** of the object's page (zeros if the object never materialized it — the object is not grown) |
| write, object frame resident read-only | the existing COW fault (`vm_cow_page_locked`): copy, remap writable |
| kernel-side write (`read()` into it, any `vm_kwrite`) | `vm_touch` breaks any read-only leaf in a private object region first |
| `mprotect` | unchanged rule: never grants write on an object frame in a private region; private copies keep the region's protection |
| `MADV_DONTNEED` | drops private copies: the page reads the object again (Linux) |
| fork | object-frame leaves stay the object's frame and read-only in the child (now for every object kind: memfd and NFS were missing); a parent COW copy is copied for the child, also when the object has no frame at that offset (that case used to try to "refill" the object and failed the fork for FAT/ext2) |
| `F_SEAL_WRITE` | refuses `MAP_SHARED`+`PROT_WRITE` only (Linux); a private writable view is allowed |

Owned code: `src/kernel/vm.c` (`vm_obj_private_fill_locked`,
`vm_touch_leaf_ro_locked`, the memfd branch of `vm_mmap`, the fault/touch
hooks, `vm_as_clone_into`), `src/kernel/linux_compat_mem.c` (`l_mem_mmap` no
longer rewrites `-1` to `-ENOMEM`: with an address space a `-1` *is*
`-EPERM`, the seal refusal).  A private region over an object is `VMK_LIB`
(as a private file mapping), never `VMK_SHARED`: its COW copies are mapped with
the region's kind and a SHARED-marked leaf is never freed.

FAT (`VM_OBJ_FILE`), NFS (`VM_OBJ_NFS`) and ext2 (`VM_OBJ_EXT2`) already had
private COW through `vm_file_obj_materialize`; they do not share the new fill
path.  ext2 is re-verified here (probe rows C14).

**Two original known limits, fixed after the first merge (625add68):**
- **`ftruncate` shrink under live mappings** used to free object frames that
  `MAP_SHARED` views and read-through private pages still mapped — a
  use-after-free once a frame was reused.  `vm_object_truncate` now shrinks
  in two phases: under `vm_lock` it drops every leaf, in every v2 address
  space (`vm_as_pool`), that maps a cut-off object frame — plus, in private
  regions, the COW copies of those pages, as Linux's `truncate_pagecache`
  does (`even_cows`) — and shrinks the object, leaving the frames *parked*
  past `nr_frames`; then it shoots down the touched address spaces and frees
  what is still parked.  A regrow over a parked frame zeroes it; object free
  covers parked frames.  A touch past the new end faults (HobbyOS kills with
  SIGSEGV; Linux sends SIGBUS).  Private COW copies freed in phase 1 follow
  the munmap rule (local flush, then the shootdown).
- **`mremap(MREMAP_MAYMOVE)` of a private object mapping** carries its COW
  copies (`vm_mremap_carry_private`); pages still reading through refault
  from the object at the new address.  Also: `MREMAP_FIXED` always moves (it
  returned the old address when the size did not grow).

**Known limits still open:**
- FAT private mappings do not see later `write()`s (every FAT write
  invalidates the page-cache object); tmpfs/memfd/ext2 do.
- `munmap` (pre-existing) flushes only the local TLB; other CPUs running
  threads of the same group are not shot down.

**Evidence:** `src/user/linux_probe_cow.c` (C1–C16) passes on the Linux host
(strix, reference) and in HobbyOS; base fails 11 rows (all the `-95`s plus the
`EPERM`→`ENOMEM` rewrite).  Unit test `test_vm_memfd_private_cow`
(`vm_test.c`) checks frames exactly (object frame identity, writability, fork,
and `frame_free_count()` back to baseline).
