# RimWorld M6 launch log — /proc plumbing, launcher, and the ordered failure ladder

Date: **2026-11-02 21:30 CST** (host: 32-core workstation, KVM).
Tree: `rw-l10-launch` seeded from `rimworld` tip **7c07abe3**.
Lane: L10 (launch plumbing). Reproduce with `tools/rw/run-rimworld.sh {probe|launchprobe|game|stub}`.

This file records what the real binary does, in order. Every quoted line is
raw serial output or raw host output; anything derived from reading code is
labelled as such. No line here is a prediction.

## 0. Result

- `/proc/self/exe`, `/proc/self/fd/N` (readlink **and** re-open by path),
  `/proc/self/maps` and `/proc/self` are implemented and **proven in-guest by
  a real glibc 2.31 program** (`tools/rw/run-rimworld.sh probe`, 0 failures).
- The launcher (`RWLAUNCH.BIN`) mounts the NFS export, creates a writable
  `HOME`, sets `LC_ALL=C` / `LD_LIBRARY_PATH=/LIB64`, and spawns the game with
  `-force-glcore -screen-width 1280 -screen-height 720 -screen-fullscreen 0`.
  It is validated end-to-end by spawning the same glibc probe from `/NFS`
  (`launchprobe` mode, 0 failures, §3.1).
- **The real `RimWorldLinux` never reaches userspace.** It is an **ET_EXEC
  linked at 0x200000**, and HobbyOS's compat loader refuses it:

  ```
  elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)
  ```

  That is the first blocking condition. It is architectural, not a missing
  syscall: the v2 address space cannot map anything below 64 GiB (§4.2).
- A PIE stand-in carrying the game's `NEEDED` list measures the **next** wall
  behind that one: `ld.so` cannot map `UnityPlayer.so` from NFS
  (`failed to map segment from shared object`, §5). **No run has reached
  `pthread_create` or GL yet**, so the first blocking *syscall* in the game is
  still unmeasured — this ladder stops earlier.

## 1. What the host says the game actually calls (design input)

Measured with `tools/rw/proc-usage.sh 30` (strace over the real binary,
Xvfb + llvmpipe, `-force-glcore`). Counts over one 30 s run:

| call | count | shape |
|---|---|---|
| `openat("/proc/self/fd/N", O_RDONLY\|O_NOCTTY\|O_CLOEXEC)` | **639** | re-opens a `/sys/.../uevent` file after it was read to EOF (a udev-style device enumeration: the targets are sysfs `uevent` nodes) |
| `openat("/proc/self/maps", O_RDONLY\|O_CLOEXEC)` | **3** | then `fstat` → `S_IFREG\|0444, st_size=0`, then 1024-byte `read()`s to EOF |
| `readlink("/proc/self/exe", …, 99)` | **5** | startup + render threads |
| `readlinkat(AT_FDCWD, "/proc/self/exe", …, 4096)` | **1** | |
| `readlink("/proc/self", …, 1023)` | **2** | kernel answers the pid as a decimal string |
| `access("/proc/self/maps", F_OK)` | **1** | existence probe before the open |

Raw (abridged):

```
1437338 readlinkat(AT_FDCWD, "/proc/self/exe", "/mnt/fast/games/steamapps/common"..., 4096) = 55
1437338 readlink("/proc/self", "1437338", 1023) = 7
1437338 openat(AT_FDCWD, "/proc/self/fd/13", O_RDONLY|O_NOCTTY|O_CLOEXEC) = 10</sys/devices/.../sound/card1/pcmC1D0c/uevent>
1437338 read(10</sys/devices/.../uevent>, "MAJOR=116\nMINOR=9\nDEVNAME=snd/pc"..., 4104) = 51
1437338 access("/proc/self/maps", F_OK) = 0
1437338 openat(AT_FDCWD, "/proc/self/maps", O_RDONLY|O_CLOEXEC) = 11</proc/1437338/maps>
1437338 fstat(11</proc/1437338/maps>, {st_mode=S_IFREG|0444, st_size=0, ...}) = 0
1437338 read(11</proc/1437338/maps>, "00200000-00201000 r--p 00000000 "..., 1024) = 1024
```

Two consequences the implementation had to honour:

1. re-opening `/proc/self/fd/N` must yield a **fresh file description at
   offset 0** (the caller has already read the file to EOF). `dup()` semantics
   would return 0 bytes and break the caller.
2. `maps` must be a **stream** with `st_size == 0`, not a sized object.

### 1.1 The failure this plumbing removes (reproduced on the host)

Running the real binary *through ld.so* (`/lib64/ld-linux-x86-64.so.2
./RimWorldLinux …`) makes `/proc/self/exe` name the interpreter. Unity's
`Player.log`, line 1:

```
There is no data folder
```

That is why `/proc/self/exe` answers a dedicated `exe_path` field and not
`p->name` (32-byte display name, truncated) and not the interpreter base.

## 2. Implementation

| File | Change |
|---|---|
| `src/include/process.h` | new `char exe_path[160]` on the PCB: the absolute path the loader was asked to execute |
| `src/kernel/program_loader.c` | `set_exe_path()` on spawn (after cwd is known) and on both exec tails (v2 + legacy); relative names resolve against cwd |
| `src/kernel/process.c` | fork and thread creation inherit `exe_path` |
| `src/kernel/linux_compat_proc.c` | `l_readlink_target()` now answers `/proc/self` (pid), `/proc/self/exe` (`exe_path`), `/proc/self/fd/N` (the fd's path), cwd/root. Sentinel changed from `1` to `0` (a one-digit pid string collided with it — measured: `/proc/self` returned ENOENT on the first probe boot) |
| `src/kernel/linux_compat_fd.c` | `openat("/proc/self/fd/N")` re-opens the fd's path through `file_open()` with a fresh description; `-ENOENT` for a closed fd, `-ENXIO` for a nameless one |
| `src/kernel/fs.c` | `struct file` records the VFS-resolved open path; new `FILE_TYPE_PROCMAPS` streaming reader (one line per **real** `vm_region`); `file_stat_fd`/`file_stat_path` report 0444 size 0; `file_path_of()` |
| `src/include/fs.h` | `path[VFS_PATH_MAX]`, `maps_cursor`, `FILE_TYPE_PROCMAPS`, `file_path_of()` |
| `src/user/rw_procprobe.c` | the glibc 2.31 probe (built like the M1 gate), staged under a path longer than 32 bytes |
| `src/user/rwlaunch.c` | native launcher: DHCP wait, `mount("10.0.2.2:/srv/nfs/rimworld","/NFS")`, `HOME=/RW_HOME`, `LC_ALL=C`, `LD_LIBRARY_PATH=/LIB64`, `-force-glcore` args, `spawn_ex`, `waitpid`, then dumps `Player.log` to the console |
| `src/user/rwstub.c` | labelled PIE stand-in (`NEEDED UnityPlayer.so`, `RUNPATH $ORIGIN`) used only to measure the wall after ET_EXEC |
| `tools/rw/proc-usage.sh` | host strace of `/proc/self/*` usage |
| `tools/rw/run-rimworld.sh` | builds/stages/boots `probe`, `launchprobe`, `game` and `stub` modes and prints the ladder |

`maps` renders `start-end perms offset dev inode [path]` from
`as->regions`: perms from `prot`, `s`/`p` from `VM_MAP_SHARED`, offset from
`obj_off` (interpreter-mapped DSOs) or `base - USER_IMG_BASE` (the kernel
image), dev `00:00`, inode `0`, and the executable path only on `VMK_IMG`
regions. Anonymous and DSO regions print an empty pathname — the kernel does
not record DSO names, and inventing them would be fabrication.

## 3. In-guest proof of the /proc work

`tools/rw/run-rimworld.sh probe` — probe staged at
`/RIMWORLD_PROBE_LONG_NAME/PROC.ELF` (34 characters, i.e. longer than the
32-byte display name, so the old answer would have been truncated). Raw serial:

```
Loading program for scheduler (v2 AS): RIMWORLD_PROBE_LONG_NAME/PROC.ELF
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 exe_entry=0x10000019C0 start=0x1008001100
[PROC] proc probe start pid=2
[PROC] readlink(/proc/self/exe) = "/RIMWORLD_PROBE_LONG_NAME/PROC.ELF" (len=34)
[PROC] PASS exe link is absolute
[PROC] PASS exe link is the executable, not the interpreter
[PROC] AT_EXECFN = "RIMWORLD_PROBE_LONG_NAME/PROC.ELF"
[PROC] readlink(/proc/self) = "2"
[PROC] PASS readlink /proc/self == pid
[PROC] readlink(/proc/self/cwd) = "/"
[PROC] open(/PROBE.TXT) = 3
[PROC] first read=30 second read=0
[PROC] PASS fixture read to EOF
[PROC] readlink(/proc/self/fd/3) = "/PROBE.TXT"
[PROC] PASS fd link is the opened path
[PROC] open(/proc/self/fd/3) = 4
[PROC] reopened read=30 "PROC-FIXTURE:0123456789abcdef
[PROC] PASS re-open starts at offset 0 (not dup semantics)
[PROC] open(/proc/self/maps) = 3
[PROC] fstat(maps) mode=0100444 size=0
[PROC] PASS maps is a regular file of size 0
[PROC] maps read=932 lines=18
[PROC] maps: 1000000000-1000001000 r--p 00000000 00:00 0 /RIMWORLD_PROBE_LONG_NAME/PROC.ELF
[PROC] maps: 1000001000-1000002000 r-xp 00001000 00:00 0 /RIMWORLD_PROBE_LONG_NAME/PROC.ELF
[PROC] maps: 1000002000-1000004000 r--p 00002000 00:00 0 /RIMWORLD_PROBE_LONG_NAME/PROC.ELF
[PROC] maps: 1000004000-1000005000 rw-p 00004000 00:00 0 /RIMWORLD_PROBE_LONG_NAME/PROC.ELF
[PROC] maps: 1008000000-1008001000 r--p 00000000 00:00 0
[PROC] maps: 1008001000-1008024000 r-xp 00000000 00:00 0
...
[PROC] done failures=0
```

The same boot shows `interp_base=0x1008000000 start=0x1008001100`: the entry
point really is `ld.so`, while `/proc/self/exe` names the executable. Those two
lines together are the proof that the answer is the loaded program, not the
interpreter.

Regression check: `tools/gate/t3_linux_probe.sh proc` still passes
(`[LP] readlink exe=/PROBE.ELF len=10`, `[LP] done rc=0`) — the stat/readlink
contract used by the existing compat probes is unchanged.

### 3.1 The same probe through the launcher, executed from NFS

`tools/rw/run-rimworld.sh launchprobe` stages the probe at
`/NFS/RWPROBE.ELF` (the export, i.e. the game's own filesystem) and builds the
launcher with that path as its target. This is the exact code path the game
would take: launcher → `spawn_ex` → kernel maps from NFS → `ld.so` →
compat process. Raw serial:

```
302  [RW] rwlaunch: exec /NFS/RWPROBE.ELF HOME=/RW_HOME LC_ALL=C LD_LIBRARY_PATH=/LIB64
311  Loading program for scheduler (v2 AS): /NFS/RWPROBE.ELF
319  elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 exe_entry=0x1000001A60 start=0x1008001100
321  [RW] rwlaunch: spawn_ex -> 3
     [PROC] proc probe start pid=3
     [PROC] HOME=/RW_HOME LC_ALL=C LD_LIBRARY_PATH=/LIB64
     [PROC] readlink(/proc/self/exe) = "/NFS/RWPROBE.ELF" (len=16)
     [PROC] PASS exe link is absolute
     [PROC] PASS exe link is the executable, not the interpreter
     [PROC] AT_EXECFN = "/NFS/RWPROBE.ELF"
     [PROC] readlink(/proc/self) = "3"
     [PROC] maps: 1000000000-1000001000 r--p 00000000 00:00 0 /NFS/RWPROBE.ELF
     ...
     [PROC] done failures=0
374  [RW] rwlaunch: waitpid=3 status=0
```

This proves the launcher's environment and argv reach the child, the
child's `/proc/self/exe` names the NFS executable, and the whole launch path
works for a PIE. The game is the one image that cannot use it — ET_EXEC.

## 4. The game run — ordered failure ladder

`tools/rw/run-rimworld.sh game --timeout 200`. Raw serial, consecutive lines
(serial line numbers on the left):

```
290  [WAVE-MANIFEST] RWLAUNCH.BIN
294  Loading program for scheduler (v2 AS): RWLAUNCH.BIN
299  [RW] rwlaunch: start
300  [RW] rwlaunch: net ip=251789322
302  [NFS] mounted 10.0.2.2:/srv/nfs/rimworld at /NFS
303  [RW] rwlaunch: mount 10.0.2.2:/srv/nfs/rimworld -> /NFS rc=0
304  [RW] rwlaunch: exec /NFS/RimWorldLinux HOME=/RW_HOME LC_ALL=C LD_LIBRARY_PATH=/LIB64
311  Loading program for scheduler (v2 AS): /NFS/RimWorldLinux
319  elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)
320  [RW] rwlaunch: spawn_ex -> -1
321  [RW] rwlaunch: spawn_ex failed (see the kernel's loader line above)
324  [EXITPC] pid=2 last_user_pc=0x1000000BCF code=0x3
327  System halt from CPU 3.
```

**The run ends at line 319.** No syscall from the game is ever issued; there
is no `[LCOMPAT] ENOSYS` line to report, no DSO lookup, no `/proc` access.
This is the honest ladder for the current OS.

### 4.1 Why: the binary is ET_EXEC, and the plan never recorded its type

Host `readelf -h /srv/nfs/rimworld/RimWorldLinux`:

```
Type:                              EXEC (Executable file)
Entry point address:               0x201000
```

```
LOAD 0x000000 0x0000000000200000 ... R
LOAD 0x001000 0x0000000000201000 ... R E
LOAD 0x002000 0x0000000000202000 ... RW
LOAD 0x003000 0x0000000000203000 ... RW
```

`docs/rimworld-plan.md` §3.1 and `docs/rimworld-evidence.md` describe the file
as "ELF64, dynamically linked" with its `PT_INTERP`, `RUNPATH` and `NEEDED`
list — they never print `e_type`. Every host run works because Linux maps an
ET_EXEC at its fixed address. HobbyOS cannot.

### 4.2 Why this is architectural, not a missing flag

- `src/kernel/elf_image.c:245` flags `needs_low_window` for any ET_EXEC whose
  lowest PT_LOAD is below `ELF_USER_WINDOW_BASE` (64 GiB); `elf_loader.c:367`
  refuses it.
- The v2 page-table walker refuses any address outside the 32 GiB window:
  `src/kernel/arch/x64/mmu.c:433` — `if (i3 < 64 || i3 > 95) return 0;`.
- The low PDPT entries are the **kernel's own** identity mappings:
  `mmu.c:386-392` — "the AS PDPT's kernel entries (0..7) point at the shared
  kernel PDs" (0–8 GiB). `0x200000` is inside that range.

So an ET_EXEC at 0x200000 cannot be mapped without moving the kernel's low
mappings or adding a second user window — not a launcher or syscall fix. This
is the **single next blocker** for M6 and it is a plan-level decision (the
plan's M6 assumes the binary loads).

## 5. The wall behind it (measured with a stand-in, not the game)

To see past ET_EXEC without pretending the game runs, `stub` mode builds
`RWTEST.ELF`: an ET_DYN PIE whose only unusual property is the game's load
dependency (`DT_NEEDED UnityPlayer.so`, `RUNPATH $ORIGIN`), placed at
`/NFS/RWTEST.ELF`. The real launcher spawns it through the same code path.
Raw serial:

```
309  Loading program for scheduler (v2 AS): /NFS/RWTEST.ELF
318  elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 exe_entry=0x1000001100 start=0x1008001100
320  [RW] rwlaunch: spawn_ex -> 3
325  /NFS/RWTEST.ELF: error while loading shared libraries: UnityPlayer.so: failed to map segment from shared object
327  [EXITPC] pid=3 last_user_pc=0x100801F2BE code=0x7F
329  [RW] rwlaunch: waitpid=3 status=32512
```

So the next wall is `mmap` of a **file on NFS**: `ld.so` opens
`/NFS/UnityPlayer.so` successfully (2.0 MB of libc are already mapped from
FAT16 in the same run: `/LIB64/libc.so.6`, 2029592 bytes), then its `mmap`
fails. Code path (static reading, not a measured errno): `vm_mmap()` calls
`file_fat16_ident()` (`fs.c`), which answers `-ENOTSUP` for an NFS fd, and
`ld.so` renders that as "failed to map segment". This is
the known **M5b** gap ("a VM object with an NFS page-in callback"). It is the
second blocker, not the first; fixing it without ET_EXEC changes nothing for
the real binary.

## 6. Single next blocker, and what is still unmeasured

**Next blocker: `RimWorldLinux` is ET_EXEC at 0x200000; the compat loader
refuses ET_EXEC below the 64 GiB user window, and the v2 page tables cannot
represent an address that low.** Until that is decided, the game issues no
syscall at all.

After it, in order:

1. `mmap` of `UnityPlayer.so` from NFS → `-ENOTSUP` (measured with the
   stand-in, §5; M5b).
2. Threads/GL: **not measured**. The ladder never reaches `pthread_create` or
   a GL call, so no claim is made here about which one blocks first. The
   host's 1.09 M `futex` calls (plan §3.4) make `futex`/`clone` the expected next
   class, but that is inference from host traces, not an in-guest measurement.

## 7. Repro

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l10-launch
bash tools/rw/proc-usage.sh 30        # host: what the game calls on /proc
bash tools/rw/run-rimworld.sh probe   # in-guest /proc proof (expect failures=0)
bash tools/rw/run-rimworld.sh launchprobe  # same probe spawned from NFS by the launcher
bash tools/rw/run-rimworld.sh game    # the real binary: ladder ends at the loader
bash tools/rw/run-rimworld.sh stub    # PIE stand-in: the NFS-mmap wall
```

Logs land in `obj/rw-launch/serial-{probe,launchprobe,game,stub}.log`
(git-ignored).
The game tree is never copied into the repo: it is served read-only from
`/srv/nfs/rimworld`; `bash tools/rw/check-no-game-files.sh` stays green.

## 8. Limits of this work (stated so they are not mistaken for gaps)

- `/proc/self/maps` names only the executable image; DSOs and anonymous
  regions carry an empty pathname because the kernel records no name for them.
  There is no vdso line (HobbyOS has none).
- `/proc/self/fd/N` re-opens **by path**. A file unlinked after open cannot be
  reopened through its fd (Linux can). Not exercised by the measured game.
- The launcher dumps at most 24 KiB of `Player.log`; the engine never wrote
  one in these runs.
- `RWTEST.ELF`/`rwstub.c` are a measurement fixture. They are not the game and
  no result here claims the game ran.

---

## 9. M6 in-guest — the AD-9 launcher reaches Mono (2026-11-03)

Tree: `rw-l14-launch`, seeded from `rimworld` tip **6e7474cc**.
Repro: `export HOME=/home/sarah; bash tools/rw/run-rimworld.sh ad9 --timeout 150`
(raw serial: `obj/rw-launch/serial-ad9.log`).

This section is the first run of the **real** game in the guest — not the
stub, not a stand-in. The executable is HobbyOS-built; the engine, Mono and
the game's data are the shipped files.

### 9.1 What was installed

| Piece | Where | Size |
|---|---|---|
| `RimWorldLinux` (AD-9 launcher, `src/user/rwmain.c`) | `/NFS/RimWorldLinux` (game dir) | 15,808 B |
| original ET_EXEC stub | `/NFS/RimWorldLinux.orig` (NFS copy only) | 14,720 B |
| `UnityPlayer.so` | `/RWDSO/UnityPlayer.so` | 33,010,672 B |
| `libmonobdwgc-2.0.so` + `libmono-native.so` + `libMonoPosixHelper.so` | `/RWDSO/` | 3,872,688 + 2,303,416 + 264,160 B |
| `lib_burst_generated.so` + `libsteam_api.so` | `/RWDSO/` | 247,528 + 416,413 B |
| `libz.so.1` (Mono NEEDED; focal-era zlib) | `/LIB64/`, `/lib/x86_64-linux-gnu/` | 108,936 B |

The launcher is a glibc-2.31 PIE built against the pin's own headers and crt
(`AD-11`), linked `-rdynamic` with `libdl-2.31.so`. It reads
`/proc/self/exe`, derives `<dir>`, `dlopen`s `RW_UNITYPLAYER`
(`/RWDSO/UnityPlayer.so`, set by `rwlaunch`) with `RTLD_NOW|RTLD_GLOBAL`,
`dlsym`s `_Z10PlayerMainiPPc`, and calls it with the launcher's own
`argc/argv`. Nothing about the game's code changes.

AD-10's local staging is reached through the launcher's exported `dlopen`:
Unity asks for the Mono/plugin DSOs by their data-dir path, the wrapper maps
the **basename** into `/RWDSO` and calls the real `dlopen` via
`dlsym(RTLD_NEXT, "dlopen")`:

```
[RWMAIN] dlopen remap /NFS/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so -> /RWDSO/libmonobdwgc-2.0.so
```

That is the line that proves the 3.8 MiB Mono DSO was opened from the FAT
image, not from NFS (`mmap` of an NFS file is `-ENOTSUP`, §5).

### 9.2 Ordered failure ladder (each row: first symptom, then the fix)

The rows below are in the order the real binary produced them. Each quote is
raw serial output from the run that hit it.

**1. ET_EXEC at 0x200000 — removed by AD-9 (previous run, §4).**
The shipped stub never loads. Replaced by the PIE launcher above; the loader
now accepts it:

```
Loading program for scheduler (v2 AS): /NFS/RimWorldLinux
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 exe_entry=0x10000017C0 start=0x1008001100
[LCOMPAT] argv argc=10 a0=/NFS/RimWorldLinux a1=-force-glcore a2=-screen-width execfn=/NFS/RimWorldLinux
```

**2. `realpath("/proc/self/exe")` → ENOENT.** Unity does not `readlink` the
link directly on this path; it calls `realpath`. glibc's `realpath` `lstat`s
every component, and `file_stat_path()` only knew FAT16, so `/proc`, `/proc/self`
and `/NFS` were all `ENOENT`:

```
[LCOMPAT] clone flags=0x3D0F00 stack=0x1082C13FB0 tls=0x1082C14700 -> 1
Error 2 encountered while resolving executable path: No such file or directory
[RWMAIN] PlayerMain returned 1
```

Fixed in `src/kernel/fs.c` (`file_stat_path`): `/proc` and `/proc/self` answer
as directories, `/proc/self/{exe,cwd,root}` as symlinks whose size is the
link target, and **NFS paths are answered from `nfs_lookup`'s fattr3** instead
of falling through to FAT16. (No `[LCOMPAT] ENOSYS` was involved; the syscall
existed and answered ENOENT.)

**3. Page-cache invalidation killed a live DSO mapping.** After the engine
opened its own `Player.log` (a FAT16 write), the first *new* UnityPlayer text
page could not be filled and the process took SIGSEGV:

```
[KERNEL] pid=3 (/NFS/RimWorldLinux) memory fault VA=0x0000001080F50DF0 PC=0x0000001080F50DF0 rw=rx in=PROT region=0x000000108092F000+0x00000000017A4000 prot=5 kind=7 ... -> killed
[LCOMPAT] fault sig=11 pid=3 tgid=3 tls=0x10801F3640 live_fs=0x0x10801F3640
Unable to load mono library from /NFS/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so
Failed to load mono
```

Cause: every FAT16 write calls `file_obj_invalidate_all()`. The comment there
says a live mapping "stays alive on its own references", but `file_obj_page()`
refused to fill any page once the object had left the slot table, so the
33 MB UnityPlayer text mapping died at its first page-in after any write.
Fixed by keeping the source identity (`file_start_cluster`) on the
`vm_object` itself and letting a live object keep refilling
(`src/include/vm.h`, `src/kernel/file_obj.c`).

**4. `libmonobdwgc` would not load: `libz.so.1` missing.** With page-in
fixed, mono initialised far enough to be dlopen'ed and its NEEDED list checked;
`libmonobdwgc` needs `libz.so.1`, which `/LIB64` did not carry. The raw
evidence from that run (the wrapper's failure text was added afterwards; the
engine's own lines are the proof):

```
/RWDSO/libmonobdwgc-2.0.so
Unable to load mono library from /NFS/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so
Failed to load mono
```

`tools/rw/run-rimworld.sh ad9` now stages the Mono tree's `libz.so.1`
(108,936 B) into `/LIB64` and `/lib/x86_64-linux-gnu`; the next run printed
`Mono path[0]` instead of `Failed to load mono`.

**5. The next blocker: Mono `mmap`s `mscorlib.dll` from NFS → `-ENOTSUP`.**
Mono starts, prints its path and config path, then cannot load the core
assembly. The exact refusal (a new `[LCOMPAT] mmap fail` row on the compat
mmap error path):

```
[LCOMPAT] mmap fail fd=4 len=4622848 off=0 -> -95 path=/NFS/RimWorldLinux_Data/Managed/mscorlib.dll
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
Mono config path = '/NFS/RimWorldLinux_Data/MonoBleedingEdge/etc'
GC Warning: Couldn't read /proc/stat
The assembly mscorlib.dll was not found or could not be loaded.
It should have been installed in the `/NFS/RimWorldLinux_Data/Managed/mono/4.5/mscorlib.dll' directory.
```

`-95` is `-ENOTSUP`: `vm_mmap` only knows FAT16 file mappings
(`file_fat16_ident`), and an NFS fd is refused before any page is mapped.
`mscorlib.dll` exists at that path (4,622,848 B) and `open`/`fstat` succeed;
the failure is the mapping, not the file.

### 9.3 Where the ladder ends, and what that means for AD-10

The engine itself runs: it prints its memory configuration, creates
`PlayerPrefs` folders, spawns two threads (`clone flags=0x3D0F00`), opens
`Player.log`, loads `UnityPlayer.so` and `libmonobdwgc-2.0.so` from the local
staging, and reaches Mono initialisation. **No `[LCOMPAT] ENOSYS` occurred in
this run (`grep -c ENOSYS serial-ad9.log` = 0), no loader refusal of the
launcher, and no missing DSO symbol** — the first refusal is `mmap` of an NFS
file.

That is the **M5b** gap the plan already tracks, now measured on the game's
own managed data: AD-10 stages the *native* DSOs, but Mono `mmap`s every
managed assembly, and `RimWorldLinux_Data/Managed/` (40 MiB) is on NFS. So
either AD-10 must also stage `Managed/` locally and route the assembly opens,
or M5b (a VM object with an NFS page-in callback) must land. That decision is
above this lane; it is the single next blocker, and this run does not claim
the game reaches a window, GL, or the menu.

### 9.4 Raw run (final, consecutive rows)

```
[WAVE-MANIFEST] RWLAUNCH.BIN
[RW] rwlaunch: start
[RW] rwlaunch: net ip=251789322
[RW] rwlaunch: mount 10.0.2.2:/srv/nfs/rimworld -> /NFS rc=0
[RW] rwlaunch: exec /NFS/RimWorldLinux HOME=/RW_HOME LC_ALL=C LD_LIBRARY_PATH=/LIB64 RW_DSO_DIR=/RWDSO
Loading program for scheduler (v2 AS): /NFS/RimWorldLinux
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 exe_entry=0x10000017C0 start=0x1008001100
[LCOMPAT] argv argc=10 a0=/NFS/RimWorldLinux a1=-force-glcore a2=-screen-width execfn=/NFS/RimWorldLinux
[RW] rwlaunch: spawn_ex -> 3
[RWMAIN] AD-9 PlayerMain launcher start argc=10
[RWMAIN] /proc/self/exe = /NFS/RimWorldLinux
[RWMAIN] data dir = /NFS/RimWorldLinux_Data
[RWMAIN] RW_DSO_DIR = /RWDSO
[RWMAIN] dlopen(/RWDSO/UnityPlayer.so, RTLD_NOW|RTLD_GLOBAL)
[RWMAIN] UnityPlayer.so handle=0x10400022d0
[RWMAIN] PlayerMain=0x108105f8b0 argc=10 — calling
[LCOMPAT] clone flags=0x3D0F00 stack=0x1082C13FB0 tls=0x1082C14700 -> 1
[UnityMemory] Configuration Parameters - Can be set up in boot.config
PlayerPrefs - Creating folder: /RW_HOME/.config/unity3d/unknown
PlayerPrefs - Creating folder: /RW_HOME/.config/unity3d/unknown/unknown
Unable to load player prefs
[LCOMPAT] clone flags=0x3D0F00 stack=0x1083414FB0 tls=0x1083415700 -> 4
[LCOMPAT] mmap fail fd=4 len=4622848 off=0 -> -95 path=/NFS/RimWorldLinux_Data/Managed/mscorlib.dll
[KERNEL] Process 3: /NFS/RimWorldLinux exited unexpectedly or gracefully.
[EXITPC] pid=3 last_user_pc=0x10800E3146 code=0x1
[RW] rwlaunch: waitpid=3 status=256
[RWMAIN] dlopen remap /NFS/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so -> /RWDSO/libmonobdwgc-2.0.so
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
Mono config path = '/NFS/RimWorldLinux_Data/MonoBleedingEdge/etc'
GC Warning: Couldn't read /proc/stat
The assembly mscorlib.dll was not found or could not be loaded.
It should have been installed in the `/NFS/RimWorldLinux_Data/Managed/mono/4.5/mscorlib.dll' directory.
[RW] rwlaunch: done
[KERNEL] Process 2: RWLAUNCH.BIN exited unexpectedly or gracefully.
[EXITPC] pid=2 last_user_pc=0x1000000CCF code=0x0
System halt from CPU 3.
```

`Player.log` (the engine's own file, 440 B; dumped by the launcher):

```
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
Mono config path = '/NFS/RimWorldLinux_Data/MonoBleedingEdge/etc'
GC Warning: Couldn't read /proc/stat
The assembly mscorlib.dll was not found or could not be loaded.
It should have been installed in the `/NFS/RimWorldLinux_Data/Managed/mono/4.5/mscorlib.dll' directory.
```

### 9.5 Limits of this section

- The run stops inside Mono startup. It says nothing about GL, X11, audio or
  the 125-thread footprint: the engine never got that far. No screenshot and
  no menu claim is made.
- The `dlopen` redirect is a *launcher* mechanism for native DSOs only. It
  does not touch `open`/`mmap` of ordinary data or managed assemblies, which
  is exactly where the ladder ends.
- `/proc/self` is presented to `stat` as a directory, not as a pid symlink;
  `stat("/proc/self/exe")` reports a symlink rather than following it.
- `GC Warning: Couldn't read /proc/stat` is recorded, not fixed: Mono's GC
  probes it once; it is not what stops `mscorlib`.
