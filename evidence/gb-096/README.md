# GB-096 — 0x7 lock pointer: measured state at `d739ce9` (DEEP Verifier, 2026-10-24)

DoD **not met**; two distinct defects measured, one of them pinned with an A/B probe. Card moved to In Review.

| # | file | what |
|---|---|---|
| 01 | `01-tier-intel-main.log` | first intel T3 at main (killed while wedged) |
| 02 | `02-tier-intel-clamped.log` | intel T3 with the cpu-id clamp probe |
| 03 | `03-serial-clamped.log` | the clamped serial: 171 `[LOCKFOREVER]`, `halted=0` |
| 04 | `04-clamp-probe.py` | the probe (diagnostic, not a fix) |
| 05 | `05-build-clamped.log` | build of the probe |
| 06 | `06-serial-main-wedged.log` | second main run: wedged in an `[IDLESTUCK]` shape, no lock record |
| 07 | `07-tier-intel-main-rerun.log` | its tier log |

## 1. The `0x7` record does not appear at main
```
grep -ac 'lock=0x0000000000000007' obj/t3_intel.log   → 0     (both runs)
```
Whatever produced it, this tree does not.

## 2. What main does instead: an out-of-range CPU id, then a zero lock pointer
First run at `d739ce9`:
```
[LOCKFOREVER] cpu=23 lock=0x0000000000000000 owner=1634624882 owner_hb_age=4294967295 pc=0x0 pc2=0x0
```
139 such records, all `lock=0x0`, all `owner=1634624882` (`0x6174732B` — ASCII `+sta`), and **`cpu=23`**.

`MAX_CPUS` is **8** (`src/include/process.h:9`). `get_cpuid()` (`arch/x64/cpu.c:10`) returns
`cpuid.1.EBX[31:24]` — the **hardware APIC id** — whenever the GS base reads 0, and every caller uses the
result to index an 8-entry per-CPU array (`cpu_locals`, `lock_wait_addr`, `held_lock_depth`,
`cpu_heartbeat_ms`). 23 is out of range, so those writes land past the end of the arrays.

**Probe that pins it** (`04-clamp-probe.py`: `get_cpuid()` returns 0 when the id ≥ 8, diagnostic only):
```
before (main)     : cpu=23, lock=0x0 owner=1634624882 ×139
after  (clamped)  : cpu ids 0..7 only;  lock=0x0 records: 0;  ASCII owners: 0
```
The invalid-pointer population disappears with the index clamped. That is not a fix (a clamped id still
guesses), but it names the class: **unchecked hardware-APIC-id indexing of per-CPU arrays**, which is how a
`spinlock_t *` in neighbouring `.bss` becomes 0 or ASCII text.

## 3. What survives the clamp: an unstealable lock
```
[LOCKFOREVER] cpu=7 lock=0x0000000070200248 owner=0 owner_hb_age=4294967295 pc=0x70000603 pc2=0x700046ce
```
`0x70200248` resolves inside `bucket_locks` (`nm -n`: `70200020 b bucket_locks`). `owner=0` with
`locked=1` is **unrecoverable by construction**: `lock_steal_ok()` returns 0 for `owner_slot == 0`
(`locks.c:50`) and `lock_stallbreak_ok()` returns 0 for the same reason (`locks.c:78`). Both escape hatches
refuse the one state that needs them.

The winning fix is structural rather than another branch: stamp the owner *with* the acquisition (e.g. the
locked word carries `owner+1` instead of a bit, or write the owner before the exchange and clear it on the
failure path), so "locked but unowned" cannot exist. Until then a lock taken by a CPU that dies between the
exchange and the owner store strands the machine with a perfectly valid `spinlock_t *` and no diagnostic
beyond `owner=0`.

## 4. DoD status

| clause | status |
|---|---|
| lock pointer is a valid `spinlock_t` on **3 consecutive intel runs that reach `halted=1`** | **NOT MET** — no run reaches `halted=1`; every intel wave wedges (lock strand, then `[IDLESTUCK]`) |
| the corrupting write is **named** | **class named** (unchecked APIC-id indexing, demonstrated by the clamp A/B); the individual write site is not yet pinned to a line |
| a run with the fix shows **no `lock=0x7`** | **holds trivially** — the record does not occur at main either |

**Evidence caveat, stated because it matters:** the *pre-clamp* serial (the one carrying `cpu=23` and the
`lock=0x0`/ASCII records) was overwritten by the next run before I copied it off. The quoted pre-clamp
numbers come from the live session; the on-disk serial `03` is the clamped run, which is the one that shows
the invalid-pointer population gone.
