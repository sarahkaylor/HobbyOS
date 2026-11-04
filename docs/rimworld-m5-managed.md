# M5c — Mono's assembly mmap: a VFS bind to a local `Managed/` copy

Status: **landed and proven in-guest**. The `mscorlib.dll` mmap wall is gone;
the game now runs managed code, spins up its full thread pool, and stops at the
*display* (no X server in the launch), which is the next blocker — see
`docs/rimworld-launch-log.md` §10.

Lane: L15 (`rw-l15-managed`), branch seeded from `93db7676`.

## 1. The wall this removes

The AD-9 launcher got the real game to Mono init and stopped there:

```
[LCOMPAT] mmap fail fd=4 len=4622848 off=0 -> -95 path=/NFS/RimWorldLinux_Data/Managed/mscorlib.dll
The assembly mscorlib.dll was not found or could not be loaded.
```

`-95` is `-ENOTSUP`. On this kernel the only file-backed VM object is FAT16's:
`vm_mmap` resolves an fd through `file_fat16_ident()` (`src/kernel/fs.c`), which
answers `-ENOTSUP` for every other fd type — including `FILE_TYPE_NFS`. So a
path under `/NFS` can be `open`ed and `read`, but never `mmap`ed. Mono `mmap`s
**every** managed assembly, so the 40 MiB `RimWorldLinux_Data/Managed/` tree is
a hard blocker, and AD-10's native-DSO staging does not cover it (AD-10 staged
`UnityPlayer.so` + the Mono DSOs + plugins, not the assemblies).

## 2. Decision: M5c (bind + local copy), not M5b (NFS page-in VM object)

| | M5c — VFS bind + local `Managed/` | M5b — NFS-backed `vm_object` |
|---|---|---|
| What it needs | a prefix translation in path resolution (~140 lines), a syscall row, a staging step in the harness | a new VM object kind whose page-in callback issues NFS `READ3` from the fault path, with retry/timeout, plus `mprotect`/COW interactions and a cache keyed on an NFS file handle |
| Blast radius | path resolution only; no new I/O path, no new failure mode under memory pressure | the page-fault path, i.e. every process, including the desktop and the shell |
| Unblocks | Mono's assemblies (the thing on the critical path) | assemblies **and** the 730 MiB of `Data/` (assets) |
| Disk cost | 40,994,816 B (~39.8 MiB) on `disk.img` | none |
| Risk | low: a wrong rewrite names a file that does not exist → ENOENT, not corruption | high: a page-in that cannot complete inside a fault has to fail somewhere, and `SIGBUS` at an arbitrary asset read is a worse debugging story than ENOENT |

Chosen: **M5c**, because the ladder is stopped by an *assembly*, not by an
asset, and because the bind is a resolution change that cannot corrupt memory.
M5b stays scheduled (plan §M5) for the case where `Data/` (730 MiB) also has to
be mapped, or where staging the assemblies locally is undesirable.

The one thing M5c must not do is make the bind a lie about the namespace: the
translation is applied at `vfs_abs_path()`, the single funnel every path-based
operation goes through, so `open`, `stat`, `readdir`, `chdir` and therefore
`mmap` all agree on what a bound path names.

## 3. What was built

| Piece | File | Lines |
|---|---|---|
| bind table + translation | `src/kernel/vfs_bind.c`, `src/include/vfs_bind.h` | 143 + 59 |
| hook: every canonical path | `src/kernel/vfs.c` (`vfs_abs_path` → `vfs_bind_apply`) | +22 |
| hook: native `open()` | `src/kernel/fs.c` (`file_open`) | +14 |
| syscall row 104 | `src/include/syscall.h`, `arch/{x64,arm}/trap.c`, `src/user/libc.c`, `src/user_include/libc.h` | +63 |
| launcher registers it before the spawn | `src/user/rwlaunch.c` | +49 |
| staging + verification | `tools/rw/run-rimworld.sh` (`ad9` mode) | +27 |
| host unit test (59 checks) | `src/host/vfs_bind_test.c` | 207 |

Semantics, deliberately minimal (full contract in `src/include/vfs_bind.h`):

- `vfs_bind("/NFS/RimWorldLinux_Data/Managed", "/RWMANAGED")` maps a **prefix**
  on component boundaries: `/NFS/.../Managed/x` → `/RWMANAGED/x`;
  `/NFS/.../ManagedX` is untouched.
- Longest matching `from` wins; the table holds 4 entries, no allocation, no
  locking (written once by the launcher, read-only afterwards).
- **Idempotence is enforced at registration.** Every path crosses the VFS more
  than once (compat `openat` canonicalises, then `file_open` canonicalises
  again), so translating an already-translated path must be the identity. A
  bind whose `to` lands inside any `from` (its own or another) is refused with
  `-EINVAL`, which is exactly the condition that would let one translation feed
  another. A too-long result is refused rather than truncated: a shortened path
  names a different file.
- The 4-entry table and the chain refusal are the whole v1 generality. Nested
  `from`s are allowed (longest wins); nested `to`s are allowed.

`file_open` needs its own hook because a *native* absolute `open()` (the shell,
`ls`, a wave) does not pass through the compat `openat` handler that
canonicalises first. It substitutes the name only when a bind actually matched.

## 4. Cost, measured

- **Disk:** `Managed/` = 102 files, 40,994,816 B. `disk.img` free space went
  from 866,107,392 B to 824,393,728 B: **41,713,664 B (39.8 MiB)** including
  the directory and its slack. The image keeps 783 MB free; `Data/` (730 MiB)
  is untouched and still on NFS.
- **Staging time:** 0.6 s for all 102 files in one `mcopy` (host side).
- **Boot/run time:** no measurable cost — the bind is a bounded string compare
  in `vfs_abs_path` (4 entries × one `inside()` each), and the guest reads the
  assemblies from FAT instead of NFS, which is faster.
- **Kernel BSS:** 4 × (2 × 160) + 4 ≈ 1.3 KiB.
- **Code:** 184 added lines outside the test.

## 5. Evidence that it works

Guest serial, the run of 2026-11-04 (`obj/rw-launch/serial-ad9-m5c-run1.log`):

```
[RW] rwlaunch: bind /NFS/RimWorldLinux_Data/Managed -> /RWMANAGED rc=0
[RW] rwlaunch: exec /NFS/RimWorldLinux ...
/RWDSO/libmonobdwgc-2.0.so
/LIB64/libz.so.1
/RWMANAGED/mscorlib.dll          <- the open Mono does, served from FAT
/RWMANAGED/mscorlib.dll
[LCOMPAT] clone flags=0x3D0F00 stack=0x1083414FB0 tls=0x1083415700 -> 4
```

and then, from the engine's own `Player.log`:

```
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
Mono config path = '/NFS/RimWorldLinux_Data/MonoBleedingEdge/etc'
GC Warning: Couldn't read /proc/stat
mono_thread_internal_set_priority: pthread_setschedparam failed, error: "Operation not permitted" (1)
RuntimeInitializeOnLoadManagerInitializer: Failed reading 'ScriptingAssemblies.json'
Error getting num native displays: Video subsystem has not been initialized
```

Three things to read out of that:

1. `Mono path[0]` still prints the **original** path — correct: the bind changes
   resolution, not the string Unity keeps. The proof that resolution changed is
   the `/RWMANAGED/mscorlib.dll` open above, and the *absence* of any
   `[LCOMPAT] mmap fail` (count 0 in the run; it was 1 before).
2. Mono is fully initialised: `mscorlib` loaded, the runtime started, its thread
   pool created (23 live PCB slots — `Finalizer`, `Job.Worker 0`, 17 ×
   `Background Job.`, `AssetGarbageCol`, `BatchDeleteObje`, `Loading.AsyncRe`,
   plus the main game thread; a 24th, `SDLTimer`, is `THREAD_DONE`), and
   `RuntimeInitializeOnLoadManager` ran — i.e. **managed code executed in the
   guest**.
3. `ScriptingAssemblies.json` (2915 B, on `/NFS`, *not* in `Managed/`) could not
   be read. No mmap was attempted for it (0 `mmap fail` rows), so this is not a
   repeat of the old wall. The launcher now `chdir`s to the game directory
   (`/NFS`, as a shell launch would) and the `Player.log` came back
   **byte-identical**, so the cwd/relative-path hypothesis is ruled out — the
   line is recorded as an open observation, not explained
   (`docs/rimworld-launch-log.md` §10.5).

## 6. Verification

| Check | Result |
|---|---|
| `obj/vfs_bind_test_host` (real kernel TU, 59 assertions) | PASS |
| in-OS `test_vfs_bind` (kernel suite, through `vfs_abs_path`) | PASS |
| `bash tools/rw/check-no-game-files.sh` | green (nothing from the game is in the tree; staging reads `/srv/nfs/rimworld`) |
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **398/0** — the 397 baseline plus the new `test_vfs_bind`, 0 failures |
| `bash tools/gate/m2_mono.sh` | PASS |
| in-guest: no `mmap fail`, no `ENOSYS` | confirmed |

## 7. Limits

- `Data/` is still on NFS: any asset the engine `mmap`s (rather than `read`s)
  will hit the same `-ENOTSUP` and needs M5b. Nothing in this run got far enough
  to try; the engine died at video init before loading a level.
- The bind is **global**, like `mount`: a second process sees it too. Per-process
  namespaces do not exist in this kernel and are not needed for M6.
- The bind table is not persisted or listed by `sysinfo(8)`; it is a launch-time
  facility, not a user-visible mount. `vfs_bind_count()`/`vfs_bind_info()` exist
  for a future `mount`-style listing.
- `vfs_abs_path()` now fails (`-1`) when a matched translation does not fit the
  caller's buffer. Every in-tree caller passes `VFS_PATH_MAX`, and the bound
  target (`/RWMANAGED`) is *shorter* than the source, so this cannot trigger for
  the game's paths.

## 8. Plan amendments for the integrator (L0 owns `docs/rimworld-plan.md`)

- **§M5, the "mmap a DSO from NFS — not supported" bullet:** M5c has landed; a
  path under an NFS mount can now be *served from FAT* through a bind. The bullet
  stays open for the general case (M5b) because `Data/` is still NFS-backed.
- **§M6 status:** replace the L14 paragraph's "Current wall" with: the bind +
  local `Managed/` retired the `mscorlib` mmap wall; the game now initialises
  Mono, runs managed code (24 threads), and stops at Unity's video subsystem
  because the launch has no X display. See `docs/rimworld-launch-log.md` §10.
- **§M5 tasks:** add "bind + local `Managed/` (M5c)" as done, and keep "M5b NFS
  page-in VM object" scheduled for `Data/`.
