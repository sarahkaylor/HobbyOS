# O1b audit — find-vs-insert vs the vm_region API (run-802 race class)

Bug (proven by P3c, host + run-802 serial): kernel `sys_brk()` ran its
"covered?" check through the LOCK-FREE `vm_region_find()` while a
concurrent `vm_region_insert()` memmoved/realloc'd the same `as->regions[]`
array under `vm_lock` -> torn read -> covered=0 -> overlap-rejected reserve
-> spurious -ENOMEM -> sbrk -1 -> malloc NULL -> fastMalloc BCRASH
(VA=0xbbadbeef).

The array `regions[]` is only ever mutated under `vm_lock`
(`region_grow` reallocs, `region_insert_at` memmoves, removal compacts).
The race existed because several READERS walked the array **without** the
lock.  This file catalogs every caller of the region ops, per fix.

Legend: `RACE` = walk outside vm_lock that could interleave with an insert/
remove.  The fault window is find-vs-insert, so the rule applied everywhere:
**the check/decision and the subsequent mutation must share ONE vm_lock
critical section.**

## Fixes (commits 7b8b513, 071bef7 on browser/perf-os)

| Caller (file:fn) | Pre-fix | Post-fix |
|---|---|---|
| `process.c:sys_brk` | `vm_region_find` **lock-free** + `vm_region_insert` (separately locked) — **THE BUG** | `vm_region_cover()`: find + reserve in ONE critical section |
| `vm.c:vm_mmap` (non-fixed) | `vm_hole_find` **lock-free** + `vm_region_insert` (separately locked) | `vm_hole_find_locked` + `vm_region_insert_locked` in ONE CS (obj created first: it takes vm_lock itself) |
| `vm.c:vm_mmap` (MAP_FIXED) | `vm_region_remove` + `vm_region_insert` (two separate locks) | `vm_region_remove_locked` + `vm_region_insert_locked` in ONE CS |
| `vm.c:vm_munmap_range` | `vm_region_find` **lock-free** (existence check) + locked remove | `vm_region_find_locked` + zap + remove inside the existing single acquisition |
| `vm.c:vm_madvise` | `vm_region_find` **lock-free** + locked zap | same one-CS pattern |
| `vm.c:vm_map_fb` | `vm_region_find` **lock-free** (idempotence) + `vm_region_insert` | find_locked + insert_locked in ONE CS |

## Callers that already held vm_lock (converted to *_locked for the explicit
contract; behavior identical, no lock added)

| Caller (file:fn) | Note |
|---|---|
| `vm.c:vm_touch` | per-page find under vm_lock |
| `vm.c:v2_image_materialize` | find under vm_lock after I/O |
| `vm.c:vm_handle_fault` | fault path find under vm_lock |
| `vm.c:vm_image_prefetch_run` | prefetch map-if-absent under vm_lock |
| `vm.c:vm_prot_set_span` | mprotect rewrite loop (caller holds vm_lock) |
| `vm.c:vm_mprotect` | range walk under vm_lock |

## Provably-safe lock-free / serialized readers (left unchanged, documented)

| Caller | Why safe |
|---|---|
| `vm_test.c` (host/unit kernel suite) | single-threaded test harness, no concurrency; calls the public (now self-locking) wrappers anyway |
| `program_loader.c` inserts (heap/img/stack) | pre-scheduler single-writer: runs during process_create before any thread of the new AS is scheduled |
| Public `vm_region_find`/`vm_hole_find` | now take vm_lock themselves — safe for boolean peeks by-default; pointer only valid until next mutation (documented in vm.h) |

## API after the fix
- `vm_region_find_locked()` / `vm_hole_find_locked()` — static inner ops,
  caller holds vm_lock; the only way to walk the array in production.
- `vm_region_find()` / `vm_hole_find()` — self-locking public wrappers
  (single-threaded tests / boolean peeks).
- `vm_region_cover()` — the sys_brk grow primitive: covered-check + reserve
  atomic under ONE vm_lock acquisition. Never sleeps / touches userspace
  while locked.

## Host A/B (probe/brk_race.c — model of the kernel array + locking)
- BEFORE (old pattern): **29 spurious -ENOMEM** over 8 runs / 2,500,008 finds
  (every run >0; reproduced the run-802 shape).
- AFTER (one critical section): **0 spurious** over 6 runs / 2,000,006 finds.

Builds: `make ARCH=intel|arm MODE=unit_tests hobbyos.elf` exit 0 (kernel +
vm_test.o linked, both arches).
