# FAT16: a lock that may sleep

Status: **partially landed**. `fat_lock` is now a `ylock_t` rather than an
`irqsave` spinlock, so the holder no longer sleeps with interrupts off; the
remaining FAT-sector lost update (`write_fat`'s unprotected read->modify->write)
is fixed by `fat_io_lock`. See `docs/lock-scope.md` §6a for what shipped and the
measurements that guard it. Kept for the `[LOCKFOREVER]` evidence below, which is
still the clearest record of why the old lock shape could not work.

## Where things stand

RimWorld's Unity/Mono payload runs on HobbyOS x86_64 under TCG on an ARM64 Mac,
reading its assets from the FAT16 image. It reaches Mono's assembly loading
(85 distinct managed assemblies at last boot) and then wedges. The wedge is not
the game: it is `fat_lock`, an `irqsave` spinlock held across blocking device
I/O, so the holder sleeps with interrupts off and never releases.

Measured with the `[LOCKFOREVER]` instrument added on `macos-host-intel`
(prints the holder CPU, its heartbeat age, and the holder's held-lock stack):

```
[LOCKFOREVER] lock=fat_lock owner=7 owner_hb_age=1834 held_n=1
              h=fat_lock                       <- asleep, not running
[LOCKFOREVER] lock=fat_lock owner=2 owner_hb_age=1671 held_n=2
              h=wr_locks[slot]@fat16_write+0x9b
              h=fat_lock                       <- writer holds both
```

A stale heartbeat on the owner means the CPU is asleep, which is also why
`STALLBREAK-DIAG` reported `owner_pid=-1` and no `[STALLBREAK]` could fire.

## Already fixed (commits on macos-host-intel)

Every site where the lock could simply be dropped around the I/O:

| Path | Fix |
|---|---|
| `read_fat` chain walk in `fat16_read_direct_locked` | walk outside the lock, cursor snapshotted |
| `alloc_cluster` at 7 of 8 call sites | lock-free, atomic reservation bitmap |
| `fat16_write` chain step (`read_fat`, `write_fat`) | release/re-acquire |
| `unlink`/`rmdir`/`truncate`/`truncate_to` free chains | release per link |
| `find_entry_in_subdir` | useless acquire/release pair removed |
| `fat16_stats` | lock removed entirely (advisory scan) |
| `fat16_read` cluster crossing | release/re-acquire |

## What cannot be fixed that way

Four functions are **read-modify-write transactions**: the lock protects the
verification between the read and the write.

```c
/* fat16_sync_entry, all under fat_lock */
read dir sector
  -> reject if on->name[0] is 0x00/0xE5   (entry deleted under the handle)
  -> reject if !fat16_entry_eq(on, &f->fat16.disk_seen)
  -> *on = entry
  -> write dir sector
```

Dropping the lock around the I/O lets another writer change the sector between
the read and the write, and the verification becomes meaningless. The same shape
appears in `update_dotdot`, `fat16_mkdir` and `fat16_rename`.

| Function | Sites under the lock |
|---|---|
| `fat16_sync_entry` | 1 (read+write pair) |
| `update_dotdot` | 1 |
| `fat16_mkdir` | 4 |
| `fat16_rename` | 6 |

## Options

### A. Yield-lock for `fat_lock` (recommended)

`fat_lock` keeps its mutual-exclusion meaning but stops being a spinlock. The
acquire retries with `arch_yield()` instead of spinning, so interrupts stay on
and the holder can be scheduled on the same CPU.

```c
typedef struct { volatile int held; } ylock_t;

static void ylock_acquire(ylock_t *l) {
  for (;;) {
    int e = 0;
    if (__atomic_compare_exchange_n(&l->held, &e, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
    arch_yield();          /* IRQs stay on: this is the whole point */
  }
}
static void ylock_release(ylock_t *l) {
  __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
}
```

Why it is sound here:
- `fat_lock` is never taken from interrupt context (FAT calls are syscall-side),
  so yielding inside the critical section cannot deadlock against an IRQ.
- `arch_yield()` is already used by `process_sleep`, so it is proven in kernel
  context.
- No lost-wakeup problem: there is no wake bookkeeping to lose.

Cost: 142 references to convert (`spinlock_acquire_irqsave(&fat_lock)` →
`ylock_acquire`, and the matching release). Mechanical but wide; the `flags`
variables become unused in most functions.

### B. True blocking mutex

Nicer under load, but `process_sleep`/`process_wakeup` have a lost-wakeup window:
release can fire between the waiter's final check and its sleep, and
`process_wakeup` only wakes a process already `BLOCKED`. Closing it needs a
register-then-recheck under `proc_lock`, i.e. a new primitive. More code, more
ways to be subtly wrong, and this VM has 8 vCPUs and one disk — yielding is
cheap.

## Verification protocol

The instrument already exists; this is a measurement, not a judgement call.

1. `make host_tests_portable` — must stay 64/64.
2. Boot `disk.img`, click RimWorld on the toolbar, run 10 minutes.
3. Success criteria, in order:
   - `[LOCKFOREVER]` count **0** (currently 9),
   - `Initialize engine version: 2022.3.35f1` present,
   - `Forcing GfxDevice: OpenGL Core` present,
   - a frame presented (`[GLXT] glXSwapBuffers` / non-black screenshot).
4. Regression check: `fat16_test` write-verify paths and the tail-checksum test
   (a wrong cluster from a botched conversion shows up there first).
5. Directory-integrity check after the run: mount `disk.img` on the host
   (`mdir`/`mtype`) and confirm `/GAMES/RIMWORLD` still lists intact and
   `Player.log` is not NUL-padded.

## Risk and rollback

- Corruption risk is concentrated in the RMW paths: a conversion bug there
  produces a wrong directory entry, not a crash. Step 5 is the guard.
- Rollback is a single revert: the conversion is one commit touching `fat_lock`
  call sites only.

## Open question for the owner

A or B. A is recommended and is what I would implement.
