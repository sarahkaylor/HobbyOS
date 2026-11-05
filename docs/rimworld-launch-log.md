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

---

## 10. 2026-11-04 — M5c: `mscorlib.dll` mmaps, Mono runs, the wall moves to the display

Lane L15 (`rw-l15-managed`), tree seeded from `93db7676`. Two changes: the VFS
prefix bind (M5c) and a local 40 MiB `RimWorldLinux_Data/Managed/`, so Mono's
assembly opens resolve to FAT16 and its `mmap`s are servable. Decision, cost and
the host unit test are in `docs/rimworld-m5-managed.md`.

### 10.1 Raw run, in order (serial rows verbatim)

```
[RW] rwlaunch: mount 10.0.2.2:/srv/nfs/rimworld -> /NFS rc=0
[RW] rwlaunch: bind /NFS/RimWorldLinux_Data/Managed -> /RWMANAGED rc=0
[RW] rwlaunch: chdir /NFS rc=0
[RW] rwlaunch: exec /NFS/RimWorldLinux HOME=/RW_HOME LC_ALL=C LD_LIBRARY_PATH=/LIB64 RW_DSO_DIR=/RWDSO
Loading program for scheduler (v2 AS): /NFS/RimWorldLinux
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 exe_entry=0x10000017C0 start=0x1008001100
[LCOMPAT] argv argc=10 a0=/NFS/RimWorldLinux a1=-force-glcore a2=-screen-width execfn=/NFS/RimWorldLinux
[RW] rwlaunch: spawn_ex -> 3
[RWMAIN] AD-9 PlayerMain launcher start argc=10
[RWMAIN] /proc/self/exe = /NFS/RimWorldLinux
[RWMAIN] data dir = /NFS/RimWorldLinux_Data
[RWMAIN] dlopen(/RWDSO/UnityPlayer.so, RTLD_NOW|RTLD_GLOBAL)
/RWDSO/UnityPlayer.so
/LIB64/libm.so.6
/LIB64/librt.so.1
/LIB64/libgcc_s.so.1
/LIB64/libpthread.so.0
[RWMAIN] UnityPlayer.so handle=0x10400004d0
[RWMAIN] PlayerMain=0x108105f8b0 argc=10 — calling
[LCOMPAT] clone flags=0x3D0F00 stack=0x1082C13FB0 tls=0x1082C14700 -> 1
[UnityMemory] Configuration Parameters - Can be set up in boot.config
PlayerPrefs - Creating folder: /RW_HOME/.config/unity3d/unknown
PlayerPrefs - Creating folder: /RW_HOME/.config/unity3d/unknown/unknown
Unable to load player prefs
[LCOMPAT] clone flags=0x3D0F00 stack=0x1083414FB0 tls=0x1083415700 -> 4
/RW_HOME/Player.log
/RWDSO/libmonobdwgc-2.0.so
/LIB64/libz.so.1
/RWMANAGED/mscorlib.dll
/RWMANAGED/mscorlib.dll
[LCOMPAT] clone flags=0x3D0F00 stack=0x1083414FB0 tls=0x1083415700 -> 5
[LCOMPAT] clone flags=0x3D0F00 stack=0x10849F4FB0 tls=0x10849F5700 -> 6
[LCOMPAT] clone flags=0x3D0F00 stack=0x10851F5FB0 tls=0x10851F6700 -> 7
```

**No `[LCOMPAT] mmap fail` row exists in this run** (`grep -c` = 0; it was 1 at
`mscorlib.dll` before). **No `[LCOMPAT] ENOSYS`** either (count 0). The
`/RWMANAGED/mscorlib.dll` lines are `fat16_open_ex`'s own path print: the bind
turned the engine's `/NFS/.../Managed/mscorlib.dll` open into a FAT16 open, and
the mapping that follows succeeds.

### 10.2 The engine's own log (`Player.log`, 534 B, dumped by the launcher)

```
[RWMAIN] dlopen remap /NFS/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so -> /RWDSO/libmonobdwgc-2.0.so
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
Mono config path = '/NFS/RimWorldLinux_Data/MonoBleedingEdge/etc'
GC Warning: Couldn't read /proc/stat
mono_thread_internal_set_priority: pthread_setschedparam failed, error: "Operation not permitted" (1)
RuntimeInitializeOnLoadManagerInitializer: Failed reading 'ScriptingAssemblies.json'Error getting num native displays: Video subsystem has not been initialized
```

There is **no** `Initialize engine version` and **no** `GfxDevice` line: the
engine never reached its own banner. `Mono path[0]` still names the NFS path on
purpose — the bind changes resolution, not the string Unity keeps.

### 10.3 What the run proves

- **Mono is fully up.** `mscorlib` loaded (from FAT), the runtime initialised,
  its thread pool created, and `RuntimeInitializeOnLoadManager` ran — managed
  code executed in the guest. The last live-slot dump lists **23 slots**:
  `AssetGarbageCol`, the main game thread, `Finalizer`, `Job.Worker 0`,
  **17 × `Background Job.`**, `BatchDeleteObje`, `Loading.AsyncRe` (slot 4,
  `SDLTimer`, is `THREAD_DONE`). At the previous ladder stop there were 3
  threads; the game's Mono/Unity start-up now runs to its pool of 24.
- **The run ends in a *park*, not a refusal.** Every one of those threads is
  `st=8` (`PROC_STATE_FUTEX`) with no wake pending; the kernel says
  `[ALIVE-SET] no READY work, yet the scheduler counts live work` and
  `[IDLESTUCK] cpu=N rounds=2000x ... slot=3 pid=3 st=8 ... /NFS/RimWorldLinux`.
  The main thread is waiting for a subsystem that failed and never posts a
  completion.
- The launcher's 180 s deadline then fires:
  `[RW] rwlaunch: waitpid=0 status=-1` → dumps `Player.log` →
  `[RW] rwlaunch: done`. There is no `System halt`: the game's threads are still
  alive when the launcher exits, so the wave never quiesces.

### 10.4 The single next blocker: **there is no display for the launched game**

The last line the engine writes is

```
Error getting num native displays: Video subsystem has not been initialized
```

That is SDL's video subsystem failing *before* any GLX/GL work, and it is a
launch-plumbing gap, not a kernel one:

1. `run-rimworld.sh ad9` boots `MODE=test`, which starts **no X server** — the
   desktop/X session only exists in `MODE=desktop` (that is how
   `tools/gate/m3_x11_client.py` gives its Xlib client a display).
2. The launcher's envp carries `HOME`, `LC_ALL`, `LANG`, `LD_LIBRARY_PATH`,
   `TMPDIR`, `RW_UNITYPLAYER`, `RW_DSO_DIR` — **no `DISPLAY`**.
3. `/LIB64` holds the glibc pin and `libz.so.1` only: **no `libX11.so.6`, no
   `libGL.so.1`** (they are not `DT_NEEDED` of `UnityPlayer.so`; Unity/SDL load
   them at runtime), so even with a display SDL's x11 driver could not open one.

So the ladder is stopped earlier than GLX: **Unity's video/display init**. Until
the game is launched inside the guest's X session with the `xcompat` X11/GL
DSOs staged, no GLX context is attempted at all — `glXChooseFBConfig`,
`GfxDevice`, input, audio (`libpulse`) and Steam are all still unmeasured.

### 10.5 Two engine rows that are recorded, not explained

- `mono_thread_internal_set_priority: pthread_setschedparam failed, error:
  "Operation not permitted" (1)` — Mono sets a thread priority; the compat
  `sched_setscheduler`/`setpriority` answers `EPERM`. Non-fatal (Mono continues
  into managed code), but it is a real compat answer to fix or document.
- `RuntimeInitializeOnLoadManagerInitializer: Failed reading
  'ScriptingAssemblies.json'` — the file exists (2,915 B) at
  `/NFS/RimWorldLinux_Data/ScriptingAssemblies.json`, and **no mmap was
  attempted for it** (0 `mmap fail` rows), so this is not the old NFS-mmap wall.
  A/B tested: the launcher was changed to `chdir` to the game directory (`/NFS`,
  as a shell launch would — `[RW] rwlaunch: chdir /NFS rc=0`) and the 534-byte
  `Player.log` came back **byte-identical**, so a relative-path/cwd explanation
  is ruled out. Remaining candidates (unmeasured): Unity reading it through a
  path that includes `Managed/`, or the read itself failing on NFS inside
  Mono/Unity's file layer. It does not stop the run — the video failure does.

### 10.6 Limits of this section

- No window, no GL, no menu, no input, no audio: the engine died before its
  banner. No screenshot and no `Initialize engine version` claim is made.
- The run was made three times (before/after the launcher's `chdir`, and again
  after the VFS call-site refactor that hands FAT16 the canonical path); all
  three produced the same wall and a **byte-identical** 534-byte `Player.log`.
  The ladder above is from the second run (`obj/rw-launch/serial-ad9-m5c-run2.log`);
  the others are `...-run1.log` and `...-run3.log`.
- `Data/` is still on NFS. Nothing in this run proved that an *asset* mmap is
  needed; M5b stays scheduled for that case.

## 11. 2026-11-04 — M6 in a real desktop session: X11 comes up, the wall is Unity's SDL lifecycle

Lane L17 (`rw-l17-desktop`), tree seeded from `20a0e26f`. The run of §10 booted
`MODE=test`: no desktop, no `DISPLAY`, no `libX11.so.6`/`libGL.so.1` in `/LIB64`.
This wave boots the native desktop instead, starts the launcher **from the
desktop's Apps menu** and stages the X11/GL compat DSOs, so the game finally
reaches the display path — and the wall moves past X11 into Unity's own SDL
lifecycle.

### 11.1 The desktop-session harness

`tools/rw/run-rimworld-desktop.sh` (+ `tools/rw/rw_desktop_drive.py`, the QMP
half of `tools/gate/m3_x11_client.py`) does, in one boot:

1. builds `MODE=desktop DESKTOP=hobby` (the same session the M3/M4 gates use),
   rebuilds `xcompat_dsos`, links the AD-9 launcher and the native `rwlaunch`;
2. stages `/LIB64` = glibc 2.31 pin + `libz` + `libX11.so.6`/`libasound.so.2`
   (`src/user/xcompat/stage.sh`) + `libGL.so.1`/`libGLX.so.0`
   (`tools/mesa/stage.sh`), `/RWDSO` = the game's DSOs (AD-10), `/RWMANAGED` =
   the 102 assemblies (M5c), installs the AD-9 launcher as
   `/srv/nfs/rimworld/RimWorldLinux` and `RWLAUNCH.BIN` in the image root;
3. boots with QMP, waits for `[MENU] count=189`, and launches `RWLAUNCH.BIN`
   with real key events (F1, End, Up×1, Enter) — the desktop spawns it with the
   window-protocol pipe pair, which is the only way a compat client gets a
   window: `src/user/xcompat/libx11.c` writes `ESC ] X <w>;<h> ~` on fd 1 and
   reads `ESC ] G …` from fd 0, and the desktop owns the other ends.

The launcher had to learn that it is a desktop child: its own rows now go
through `print_console()` (fd 1 is the window protocol there, not the serial
console), and the child keeps fd 0/1 (the pipes) while fd 2 is mapped to
`/dev/console` with `spawn_ex`'s fd map, so `[RWMAIN]` and Unity stderr rows
reach the serial log. `rwmain.c` writes its diagnostics to stderr for the same
reason. The child environment is `HOME=/RW_HOME LC_ALL=C DISPLAY=:0
LD_LIBRARY_PATH=/LIB64:/RWDSO:/NFS SDL_VIDEODRIVER=x11 RW_TRACE_DLOPEN=1`.

### 11.2 Two compat-DSO gaps retired (both in `src/user/xcompat/libx11.c`)

**a. `libX11.so.6` did not export SDL's BASE-Xlib symbol set.** SDL's dynamic
loader does not resolve symbols one by one: every `SDL_X11_SYM` in the
`BASEXLIB` module of `SDL_x11sym.h` must be non-NULL or
`SDL_X11_LoadSymbols()` clears `SDL_X11_HAVE_BASEXLIB`, unloads the driver and
reports "No available video device". Measured with the launcher's `dlopen`/
`dlsym` trace (`RW_TRACE_DLOPEN=1`): `dlopen ok libX11.so.6` followed by

```
[RWMAIN] dlsym FAILED XMissingExtension: /LIB64/libX11.so.6: undefined symbol: XMissingExtension
[RWMAIN] dlsym FAILED _XEatData / _XFlush / _XFlushGCCache / _XRead / _XReadPad
[RWMAIN] dlsym FAILED _XReply / _XSend / _XSetLastRequestRead
[RWMAIN] dlsym FAILED XmbTextListToTextProperty
```

and **no `XOpenDisplay` call anywhere in the run** — the driver had already been
discarded. The ten private `XlibInt.h` I/O entry points are now implemented
against this display's protocol (flush = the shadow + `ESC ] F` present path;
read/eat = read fd 0, the desktop's message pipe; reply/send = no X wire).

**b. `XMatchVisualInfo` matched the wrong class.** It required
`c_class == InputOutput` (1, a *window* class); SDL's `get_visualinfo()` calls
`XMatchVisualInfo(dpy, screen, DefaultDepth(), TrueColor, &v)` with the *visual*
class (4) and treats a 0 return as "this screen has no visual". Every screen was
skipped. It now matches the display's actual visual class and depth.

### 11.3 Final ladder (raw serial rows, in order)

```
[MENU] count=189
[LAUNCH] RWLAUNCH.BIN
[RW] rwlaunch: start
[RW] rwlaunch: mount 10.0.2.2:/srv/nfs/rimworld -> /NFS rc=0
[RW] rwlaunch: bind /NFS/RimWorldLinux_Data/Managed -> /RWMANAGED rc=0
[RW] rwlaunch: console fd for child stderr=3
[RW] rwlaunch: exec /NFS/RimWorldLinux HOME=/RW_HOME LC_ALL=C DISPLAY=:0 LD_LIBRARY_PATH=/LIB64:/RWDSO:/NFS RW_DSO_DIR=/RWDSO
[RW] rwlaunch: spawn_ex -> 4
[RWMAIN] AD-9 PlayerMain launcher start argc=10
[RWMAIN] dlopen ok /RWDSO/UnityPlayer.so
[RWMAIN] PlayerMain=0x108105f8b0 argc=10 — calling
[RWMAIN] dlopen ok libX11.so.6 -> 0x1040032610
[RWMAIN] dlopen FAILED libXext.so.6 / libXcursor.so.1 / libXinerama.so.1
[RWMAIN] dlopen FAILED libXi.so.6 / libXrandr.so.2 / libXss.so.1 / libXxf86vm.so.1
[RWMAIN] dlopen FAILED libudev.so.1 / libudev.so.0
```

With the traced DSO (`-DXC_TRACE`) the same run shows the X11 driver's own
progress, in order:

```
[XC] XOpenDisplay enter / ok          (twice — SDL's display + request_display)
[XC] XSync ×2                         (window-manager probe)
[XC] XMatchVisualInfo class=5         (DirectColor: miss)
[XC] XMatchVisualInfo class=4         (TrueColor: hit)
[XC] XGetKeyboardControl
[XC] XAutoRepeatOn
[XC] XDisplayKeycodes
[XC] XQueryPointer
[XC] XCloseDisplay                    (×2 — SDL_VideoQuit → X11_DeleteDevice)
```

`libXext`/`libXcursor`/`libXinerama`/`libXi`/`libXrandr`/`libXss`/`libXxf86vm`
are SDL's optional extension modules; their absence disables XShape/XRandR/
XInput2, not the driver. `libGL.so.1`/`libGLX.so.0` are staged but **never
loaded**: no `glX*` call is made in this run.

The engine's own log (`Player.log`, 1214 B, dumped by the launcher):

```
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
GC Warning: Couldn't read /proc/stat
mono_thread_internal_set_priority: pthread_setschedparam failed, error: "Operation not permitted" (1)
RuntimeInitializeOnLoadManagerInitializer: Failed reading 'ScriptingAssemblies.json'
Error getting num native displays: Video subsystem has not been initialized
```

No `Display 0 'screen'`, no `Initialize engine version`, no `GfxDevice` — the
engine never got past its display manager. The screenshot shows the desktop
with the launcher's window still in *terminal* mode (Unity's stdout text,
"Unable to load player prefs"), i.e. no `ESC ] X` was ever sent: no
`XCreateWindow`/`XMapWindow` happened.

### 11.4 The single next blocker

**`Error getting num native displays: Video subsystem has not been initialized`**
— SDL's own error (`SDL_video.c:441`, returned when `_this == NULL`) — because
`SDL_Init` **rolled back the video subsystem it had just initialised when the
joystick subsystem failed on the missing `libudev.so.1`**. In order:

- the driver is up before that point: `X11_InitKeyboard`'s calls
  (`XGetKeyboardControl`, `XAutoRepeatOn`, `XDisplayKeycodes`) only run after
  `X11_InitModes()` returned success, and `X11_InitModes` refuses to return
  success with zero displays;
- immediately after them the joystick stage runs and fails:
  `[RWMAIN] dlopen FAILED libudev.so.1 / libudev.so.1 / libudev.so.0`;
- two `XCloseDisplay` calls follow in the same breath (`SDL_VideoQuit` →
  `X11_DeleteDevice` closes `data->display` and `data->request_display`) — this
  is `SDL_Init`'s rollback of the subsystems it had already started;
- **no second `XOpenDisplay` ever happens** (and no second `dlopen` of
  `libX11.so.6`): the player never re-initialises video, so its later display
  query returns the error above and the engine parks (24 threads, all `st=8`,
  launcher deadline at 180 s) before `Initialize engine version`.

**Host A/B (the discriminator).** The same launcher, game and DSOs on the host
(Xvfb `:99`) print the *same* line when `libudev` is hidden from `dlopen`
(`tools/rw/udev-absent-probe.c` builds the preload):

```
$ gcc -shared -fPIC -o /tmp/noudev.so tools/rw/udev-absent-probe.c -ldl
$ LD_PRELOAD=/tmp/noudev.so ./RimWorldLinux -force-glcore ...   # hides libudev*
Preloaded 'lib_burst_generated.so'
Preloaded 'libsteam_api.so'
Error getting num native displays: Video subsystem has not been initialized
Error getting num native displays: Video subsystem has not been initialized
Error getting num native displays: Video subsystem has not been initialized
...
Initialize engine version: 2022.3.35f1 (011206c7a712)
GfxDevice: creating device client; threaded=1; jobified=0
```

So that line is not an X11-compat failure: it is what Unity prints when SDL's
joystick subsystem cannot start. The difference is that the host engine then
recovers (it re-initialises video and reaches `GfxDevice`), while the guest
engine does not. Without the preload the host run is clean —
`Display 0 'screen': 1920x1080`, `Initialize engine version`, `GfxDevice`,
`Renderer: llvmpipe`, `OPENGL LOG: Creating OpenGL 4.5 graphics device`.

**Fix direction (next lane):** ship a compat `libudev.so.1` the same way
`libasound.so.2` ships a null ALSA: `udev_new`/`udev_monitor_new_from_netlink`
return objects, the enumerate path returns **zero** input/sound devices (there
are none on HobbyOS), and the monitor fd is a pipe read end that never signals.
Then SDL's joystick init succeeds, `SDL_Init` does not roll video back, and the
player's display query returns 1 display — the ladder then reaches window
creation/GLX, which are still unmeasured.

### 11.5 Limits

- No frame, no GLX, no menu: `glXChooseFBConfig`/`glXCreateContext` have not
  been reached, so M4's in-guest GL stack is still unmeasured under the player.
- Audio and Steam are untouched (their DSOs are absent; SDL's audio init never
  ran).
- The traced DSO used for the X-level rows is the same source with
  `-DXC_TRACE`; the untraced build produces the identical `Player.log`
  (1214 B) and the identical stall.
- Host reference runs are labelled as such; the guest evidence is the serial
  log `obj/rw-desktop/serial-desktop.log` and `obj/rw-desktop/shots/`.

## 12. 2026-11-04 — M6: `libudev.so.1` keeps SDL's video subsystem; the wall moves to Unity's window creation

Lane L19 (`rw-l19-udev`), tree seeded from `b9cd2bf1`. §11 ended at
`Error getting num native displays: Video subsystem has not been initialized`
because `SDL_Init` rolled the video subsystem back when its joystick stage
could not `dlopen` `libudev.so.1`. This wave ships that DSO
(`src/user/xcompat/udev_null.c`), and the line is **gone**: the player now
reports a real display. It still does not create a window.

### 12.1 The shim: 26 symbols, zero devices

Measured from the binary, not from a header:

```
$ strings -a UnityPlayer.so | grep -E '^udev_' | sort -u
26 symbols (the full list is in EXPORTS.md §2.6)
```

Three further `^udev_` strings are the engine's *error text*, not symbols
(`udev_new() failed`, `udev_enumerate_new() failed`,
`udev_monitor_new_from_netlink() failed`), so the generator
(`tools/gen_exports.py`) takes the intersection of the `udev_*` shape with the
host libudev's 92 dynamic exports — the same rule the libX11 set uses. All 26
required names are exported by the shim (50 total; the extras are the rest of
the public surface, all NULL-safe). `required-udev.txt` / `required-udev.h`
are generated and checked in, like the libX11/ALSA/pulse sets.

Contract (opaque handles only — libudev publishes no structs, so there is no
layout to get wrong):

| call | answer |
|---|---|
| `udev_new` / `udev_unref` | non-NULL sentinel handle / no-op, NULL-safe |
| `udev_enumerate_new` | non-NULL sentinel, even for a NULL udev handle |
| `add_match_*`, `scan_devices` | `0` (success) |
| `get_list_entry`, `list_entry_*` | NULL — the empty list |
| `device_new_from_*`, all device getters | NULL / 0 / 1 (`is_initialized`) |
| `monitor_new_from_netlink` | non-NULL sentinel monitor + a pipe created once |
| `monitor_get_fd` | the pipe **read end** (write end held open: readable never, EOF never) |
| `enable_receiving`, `filter_*` | `0` |
| `receive_device`, `get_action` | NULL ("nothing pending") |

Built like every compat DSO: `-shared -fPIC -nostdlib`, **0 `DT_NEEDED`**,
**0 undefined symbols**, SONAME `libudev.so.1`; `make xcompat_udev_check`
asserts all three. Staged by `src/user/xcompat/stage.sh` into `/LIB64`, which
is where the run below found it (`/LIB64/libudev.so.1` appears in the
launcher's dlopen trace).

### 12.2 Host A/B before the guest (and the fd choice)

Same game, Xvfb `:77`, 800×600, 55 s per run, one shim build on
`LD_LIBRARY_PATH` at a time:

| variant | `Display 0` | `Initialize engine version` | `GfxDevice` | `Video subsystem … not initialized` |
|---|---|---|---|---|
| pipe fd (shipping) | yes | yes | yes (`Renderer: llvmpipe`) | absent |
| `-1` fd (measurement build) | yes | yes | yes | absent |
| control: `udev_new()` → NULL | **no** | yes | yes | **present, twice** |

Both fd answers are tolerated by this SDL build; the pipe is shipped because
it keeps "hotplug is enabled and being watched" true and cannot be read as a
failed watch (`fd < 0`). The control is the important row: it proves the
game is loading *this* DSO, because with a shim that refuses a handle the
host prints the guest's §11 line verbatim. Details: `EXPORTS.md` §7.3.

### 12.3 The guest ladder (raw rows, in order)

`bash tools/rw/run-rimworld-desktop.sh`, `MODE=desktop DESKTOP=hobby`,
launched from the Apps menu. The X rows come from the `-DXC_TRACE` build, the
`[UD]` rows from a `-DUDEV_TRACE` build of the shim (both measurement-only;
the shipping DSOs contain neither string and produce the identical
`Player.log`, 1181 B).

```
[XC] XGetKeyboardControl
[XC] XAutoRepeatOn
[XC] XDisplayKeycodes
[XC] XQueryPointer
[RWMAIN] dlsym FAILED udev_device_get_action: /NFS/RimWorldLinux: undefined symbol: udev_device_get_action
/LIB64/libudev.so.1
[RWMAIN] dlopen ok libudev.so.1 -> 0x104005c1d0
[UD] udev_new
[UD] monitor_new
[UD] monitor_filter
[UD] monitor_filter
[UD] monitor_enable
[UD] enumerate_new
[UD] scan_devices
[UD] get_list_entry
[UD] enumerate_new
[UD] scan_devices
[UD] get_list_entry
[UD] enumerate_new
[UD] scan_devices
[UD] get_list_entry
```

Read in order:

- SDL's X11 video driver finishes: `XMatchVisualInfo` → keyboard →
  `XQueryPointer` is the last Xlib call of `X11_VideoInit`. **No
  `XCloseDisplay`** (the §11 rollback) — the video subsystem stayed up.
- `libudev.so.1` is dlopened from `/LIB64` and the joystick stage runs to
  completion: one `udev_new`, one monitor (`new`/`filter`×2/`enable`), and
  three enumerate/scan/list cycles (SDL's joystick detect). Every call
  returns; `receive_device` is never called (nothing in this path polls the
  monitor).
- Then **nothing**. No further Xlib call, no `XCreateWindow`, no `XMapWindow`,
  no `libGL.so.1` dlopen, no `glX*` call, no `[LCOMPAT] ENOSYS`
  (`grep -c 'LCOMPAT.*ENOSYS'` = **0**). `libGL.so.1`/`libGLX.so.0` are
  staged in `/LIB64` and are never asked for.

The engine log is now 1181 B (was 1214 B) and its last line is the first
display report this track has ever produced:

```
Mono path[0] = '/NFS/RimWorldLinux_Data/Managed'
Mono config path = '/NFS/RimWorldLinux_Data/MonoBleedingEdge/etc'
GC Warning: Couldn't read /proc/stat
mono_thread_internal_set_priority: pthread_setschedparam failed, error: "Operation not permitted" (1)
RuntimeInitializeOnLoadManagerInitializer: Failed reading 'ScriptingAssemblies.json'Display 0 '0': 1920x1080 (primary device).
```

**`Error getting num native displays: Video subsystem has not been
initialized` does not appear at all**, and `Display 0 '0': 1920x1080` is new.
The name `'0'` is SDL's X11 display name when RandR monitor info is
unavailable, not a shim artifact: the host, with `libXrandr` hidden, prints
`Display 0 '0': 1280x800` too (§12.4).

The engine is quiescent, not looping: `[IDLESTUCK] cpu=6 rounds=80000` and
the main thread (`slot=4 /NFS/RimWorldLinux`) is `st=8 wake=0
rip=0x1082404170` — parked, not spinning; the player plus its 22 threads
(16 × `Background Job.`, `Job.Worker 0`, `Finalizer`, `SDLTimer`,
`AssetGarbageCol`, `BatchDeleteObje`, `Loading.AsyncRe`) are all waiting.

### 12.4 The single next blocker

**Unity never starts window creation.** The last engine line is

```
Display 0 '0': 1920x1080 (primary device).
```

and the step that should follow it — `XCreateWindow` → `XMapWindow` →
`ESC ] X w;h ~` → `glXChooseFBConfig`/`glXCreateContextAttribsARB`, i.e. the
first `libGL.so.1` dlopen — **does not happen**. So there is no `GfxDevice`,
no `Initialize engine version`, and (the question §11 left open) **no GLX
request at all**: Unity has not asked for 4.5 core, so the 4.5-vs-3.3
question is still unmeasured in the guest.

What it is *not*: not a missing symbol (every `dlsym` on the X11 surface
succeeds), not a missing DSO (`libGL` is never requested), not a syscall
(`0` `[LCOMPAT] ENOSYS` lines), and not the udev stage (all fourteen `[UD]`
calls returned).

**The empty display-mode list was tested and is not sufficient.** With
`libXrandr` hidden from the host client (`tools/rw/xext-absent-probe.c`, Xvfb
1280×800, `SDL_VIDEODRIVER=x11`) SDL reports `Display 0 '0': 1280x800` and the
engine still reaches `Initialize engine version`, `GfxDevice` and
`OPENGL LOG: Creating OpenGL 4.5 graphics device`. Hiding `libXext`/`libXi`
*as well* is a different failure: SDL_Init itself rolls back again
(`Error getting num native displays` ×2, 497-byte log), which is why the
probe hides only RandR — that confound is worth remembering before blaming a
missing extension DSO.

Next diagnostics, cheapest first:

1. **Find the parked syscall.** The main thread is `st=8 wake=0` with no
   pending Xlib/udev/GL call; the IDLESTUCK row has only a rip. A compat
   syscall trace for pid 4 (or an `rwmain` wrapper that prints the last
   `SDL_*` entry) answers whether it waits in `read(0)` (the window-protocol
   pipe), `write(1)` (same pipe, if the desktop stops draining) or a futex.
2. **Wrap `SDL_*`.** The launcher already interposes `dlopen`; interposing
   `SDL_CreateWindow`/`SDL_GL_LoadLibrary` the same way turns "no window" into
   "the last SDL call attempted".
3. **`SDL_JOYSTICK_DISABLE_UDEV=1` A/B** in the child env, to make the udev
   exoneration an A/B rather than an inference.

### 12.5 Screenshot

`docs/rimworld-m6-udev-desktop.png` (from
`obj/rw-desktop/shots/06_running.png`, taken while the game was running): the
desktop with the launcher's window **still in terminal mode** — the Unity
memory banner, `PlayerPrefs - Creating folder`, `Unable to load player prefs`
— i.e. **no `ESC ] X` was ever written**, consistent with `XMapWindow` never
being called (the `ESC ] X` is emitted by `XMapWindow` alone). The taskbar
still shows the process under its launcher name.

### 12.6 Limits

- Still no window, no GLX, no frame. The frame evidence for this track is
  still only the M4 cube (`docs/rimworld-m4-window.png`), not the game.
- The guest run uses the untraced DSO for the `Player.log` and the traced
  build only for the ordered rows; both produce the same 1181-byte log and
  the same stall (verified by running both).
- `pthread_setschedparam → EPERM`, `GC Warning: Couldn't read /proc/stat` and
  `Failed reading 'ScriptingAssemblies.json'` are unchanged from §10/§11 and
  are not on this wave's critical path.

## 13. 2026-11-04 — M6: the post-display park is a pthread mutex that is never released

Lane L20 (`rw-l20-park`), tree seeded from `4b6ad9c7`. §12 ended with the
player's main thread parked (`st=8`) after `Display 0 '0': 1920x1080` and no
`XCreateWindow`. The open question was *what* it waits on. This wave answers it
with a futex flight recorder and three probes, and the answer is not a lost
wakeup in the futex queue: the thread is blocked acquiring a `pthread_mutex_t`
whose holder never unlocks.

### 13.1 Instrumentation (diagnostic, no behaviour change)

`src/include/futex_trace.h` + `src/kernel/linux_compat_thr.c`:

| piece | what it prints | why |
|---|---|---|
| `[FUTEX] #n OP pid= as= key= rip= …` | the first 48 ops of a boot | boot-order handshakes |
| 64-entry ring, dumped by `futex_trace_dump_locked()` | `[FUTEX] r#n …`, the last ops before the hang | 1.09 M ops cannot go to the console |
| `WAIT` vs `WAITBS` | the actual futex op | `FUTEX_WAIT` = mutex, `WAIT_BITSET` = condvar/sem |
| `rip=` | the syscall-site return address | names the libc/libpthread/Unity wrapper that issued it |
| `[FUTEX] q slot= pid= st= as= key= bitset= waits= wakes= sigp= name` | every slot still queued, plus pending signals | names the waiter, not just the rip |
| `[FUTEX] st pid= sp= <6 stack words> word=` | saved user RSP and what is on it | backtrace without frames |
| `[FUTEX] mutex pid= … 5 words` | the `pthread_mutex_t` body for lock-shaped words | `__lock/__count/__owner/__nusers/__kind` |
| `[FUTEX] rgn pid= base= len= kind= prot=` | the mapping the key lives in | heap vs DSO global vs stack |
| `[FUTEX] code pid= rip= <16 bytes>` | the instruction stream at the saved rip | identifies the wrapper offline |
| `[FUTEX] near-miss …` | a wake with `woke=0` while a thread is parked on the same key with a different address space or a disjoint bitset | the lost-wakeup shape, printed only when true |
| `[FUTEX] exit pid=` | every thread exit | a dead owner is a deadlock |
| `[SIG] send pid= sig= st= futex=` | every compat signal | Mono stop-the-world vs lock holder |

`[IDLESTUCK]` rows gained `futex=<key>`, so the per-thread dump the hang needs
(pid, name, rip, wait key, state) is now one line per slot. The app-side
counterpart is in the AD-9 launcher (`src/user/rwmain.c`): with
`RW_TRACE_MUTEX=1` it records every `pthread_mutex_lock/trylock/timedlock/
unlock`, `sem_wait/post` and `pthread_cond_wait/timedwait` into
`/RW_HOME/mtx.log` (61,414 rows in the run below). It is **off by default** —
the launcher does not set the variable, so the shipping run is unchanged.

### 13.2 The ladder (raw rows)

```
[FUTEX] r#64 WAIT pid=4 ... key=0x17FFFFB6D8 rip=0x108011895D val=0x0 to=4294967295 res=0
[FUTEX] r#65 WAKE pid=24 ... key=0x17FFFFB6D8 rip=0x108011895D n=1 woke=1 of=4
[FUTEX] r#66 WAIT pid=24 ... key=0x1110001EA0 rip=0x108011895D val=0x0 to=4294967295 res=0
[FUTEX] r#67 WAIT pid=4 ... key=0x17FFFFB6B8 rip=0x108011895D val=0x0 to=4294967295 res=0
[FUTEX] r#68 WAKE pid=25 ... key=0x17FFFFB6B8 rip=0x108011895D n=1 woke=1 of=4
[FUTEX] r#69 WAIT pid=25 ... key=0x10F0000334 rip=0x108011895D val=0x0 to=4294967295 res=0
[FUTEX] r#70 WAIT pid=4  ... key=0x10C00964A0 rip=0x1082404170 val=0x2 bitset=0xFFFFFFFF to=-1 res=0
```

`r#70` is the **last futex operation of the boot** (`queue: … ops=68…71`), and
no `WAKE` on `0x10C00964A0` appears anywhere in the ring. Read in order:

- Every other futex in the process is issued through libc's `syscall()`
  wrapper (`rip=0x108011895D` = libc+0x11895D). `r#70` is the only one issued
  from libpthread's own asm: `0x1082404170` = libpthread+0x13170, the
  instruction after the `syscall` in `__lll_lock_wait` — a plain
  **`pthread_mutex_lock` slow path**.
- The wait value is **2** (glibc's "locked and a waiter set the WAITERS bit"),
  the timeout is **infinite** (`to=-1`, i.e. `utime == NULL`), and the bitset
  is `MATCH_ANY`, so this is not a bitset or timeout shape at all.
- The key is in a 4 MiB anonymous mapping:

```
[FUTEX] rgn pid=4 key=0x10C00964A0 base=0x10C0000000 len=0x400000 kind=3 prot=3
[FUTEX] mutex pid=4 key=0x10C00964A0 0x2 0x10 0x0 0x0 0x0
[FUTEX] st pid=4 sp=0x17FFFFB4D8 0x10823FC0A3 0x0 0x10C00964A0 0x10C00964A0 0x1000002479 0x30 word=0x2
```

  `__lock=2`, and `__count=0x10`, `__owner=0`, `__nusers=0`, `__kind=0`
  (NORMAL, so glibc keeps no owner tid — the one thing that would have named
  the holder). `__count=0x10` is therefore the neighbouring field of the
  enclosing 4 MiB object, not mutex state.
- The stack is a real call chain: `[sp]` returns into libpthread's
  `__pthread_mutex_clocklock_common` (+0xC0A3), `[sp+32]` returns into the
  launcher's own `pthread_mutex_lock` wrapper (`0x1000002479`), and the
  wrapper's caller is UnityPlayer+0x1D43DA5. So the call really is
  `pthread_mutex_lock(H)` from Unity, entered through the normal PLT.

**It is not `read(0)`, not `poll`, not `waitpid`.** The thread is in the
kernel's futex wait, on a userspace mutex.

### 13.3 Three probes: the wait is legitimate, and the word stays locked

A lost wake would show up as a waiter that could have proceeded. Three
independent probes say it could not:

1. **Per-word prod.** Giving *only* `0x10C00964A0` a 1 s deadline makes the
   main thread wake 14 times (`waits` 20 → 34) and re-park on every one: the
   word is still 2 on each return. glibc's `__lll_lock_wait` re-executes
   `xchg(lock,2)` after every return, so a wake only completes when the word
   has become 0. It never does.
2. **Mass spurious wake.** One-shot, at the first hang dump, the kernel set
   every queued waiter READY:

   ```
   [FUTEX] prod: spuriously woke 23 parked threads
   [FUTEX] r#71 WAIT pid=4  ... key=0x10C00964A0 ... val=0x2
   [FUTEX] r#75 WAIT pid=15 ... key=0x10D07265C0 ... val=0xFF800002
   [FUTEX] r#76 WAIT pid=14 ... key=0x10D071E480 ... val=0xFF800002
   …
   ```

   Every worker *did* run and re-park — their semaphore word moved from
   `0xFF800001` to `0xFF800002` — so the scheduler and the wake path work.
   Main re-parked on `H` with the word still 2, and the game did not advance.
3. **`near-miss` detector.** Across the whole boot it reported nothing: no
   wake ever found a thread parked on the same key with a different address
   space or a disjoint bitset.

The app-side recorder agrees: of 61,414 recorded sync operations, **exactly
one** touches the 4 MiB arena, and it is main's own lock; there is **no
`unlock` of `0x10C00964A0` anywhere**, and no other thread ever locks it.

**Verdict.** The wait is legitimate. The expected waker is the holder's
`pthread_mutex_unlock(0x10C00964A0)`, which never runs: the holder is itself
parked in one of the 23 quiescent waits. This is not the compare-and-park
race (the compare and the park are under `proc_lock`), not a bitset mismatch
(all masks `MATCH_ANY`), not a wrong address space (all rows `as=0x75304090`),
and not a wake addressed to the group (the queue is per-thread and the woken
pid is recorded). Those four candidates are ruled out by rows, not by
argument.

### 13.4 What is still unknown, and the next instrument

Nothing in the kernel or in the userspace recorder sees the holder *take* the
lock, which means the owner entered the critical section through a path that
does not call the interposed `pthread_mutex_lock` — in practice either
glibc's own condvar re-acquisition (`__pthread_mutex_cond_lock`, called
inside `pthread_cond_wait`) or Unity's own raw-futex lock on the same word.
Naming it needs a *write* observation on the word, not another read: an x86
debug-register or read-only-page watchpoint on `0x10C00964A0`, logging
(pid, rip) on every write, is the instrument that will name the owner. That
is the next lane's first step; it is deliberately not guessed at here.

The one structural fact worth carrying forward: the boot performs only ~70
futex operations in total. A working Linux boot of this player has thousands
by the time it prints `Display 0`. The process is quiescent from very early —
the thread pool is created, each worker does one startup handshake and parks,
and main then blocks on a lock that was already taken. So the bug is not in
the *last* wait but in whatever made a worker park while holding that lock.

### 13.5 Regressions

Serialized (never with the desktop harness):

```
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 398   Tests failed: 0
bash tools/gate/m2_threads.sh            m2: PASS — 8 pthreads, mutex+condvar+TLS+join
bash tools/gate/m12_thread_ceiling.sh    m12: PASS — 150 threads, TLS, shared futex, cancel
```

### 13.6 Limits

- No window, no GLX, no frame; still the same `Display 0` line as §12.
- The userspace recorder is a measurement build: it adds a `write()` per
  recorded sync call and is off in the shipping launcher.
- `pthread_setschedparam → EPERM`, `GC Warning: Couldn't read /proc/stat` and
  `Failed reading 'ScriptingAssemblies.json'` are unchanged and still not on
  the critical path.

## 14. 2026-11-04 — M6: the post-display park is an UNINITIALIZED mutex, not a held one

Lane L22 (`rw-l22-mutex`), tree seeded from `4d3e55d7` (the L20 merge). §13 ended
with "the wait is legitimate and the holder is parked elsewhere", inferred from
the absence of a `WAKE` and from three probes. This wave tested the two cheap
alternatives first (launcher symbol shadowing, then a hardware write
watchpoint) and **refutes that conclusion**: nobody holds the mutex, and the
word was never a locked mutex in the first place. It is the first qword of a
0x30-byte node in one of Unity's own block chains.

### 14.1 A/B: the launcher's pthread shadowing is not the cause

The AD-9 launcher is linked `-rdynamic`, so every `pthread_*`/`sem_*` wrapper it
defines is exported from the executable and wins symbol resolution for
UnityPlayer.so (the executable is first in the global scope). If the wrapper
were non-reentrant, a NORMAL-mutex self-deadlock would look exactly like the
measured park.

The discriminator is one define, not a rewrite: `-DRW_NO_SHADOW_PTHREAD` omits
every pthread/sem definition (and the mtx recorder) while keeping `dlopen`/
`dlsym` exported, so AD-10's redirect still works and the only difference is the
interposition itself.

```
launcher dynsym:            base      noshadow
  pthread_mutex_lock          2          0        (readelf -sW --dyn-syms | grep -c)
  dlopen                    yes        yes
```

Serial, same kernel, same disk, same run script, `--run-timeout 100`:

| run | launcher | last futex ring row | park |
|---|---|---|---|
| A | shipping (`-rdynamic`, wrappers) | `r#67 WAIT pid=4 key=0x10C00964A0 rip=0x1082404170 val=0x2 to=-1` | `[IDLESTUCK] slot=4 … futex=0x10c00964a0` |
| B | `RW_NO_SHADOW=1` | `r#66 WAIT pid=4 key=0x10C00964A0 rip=0x1082404170 val=0x2 to=-1` | `[IDLESTUCK] slot=4 … futex=0x10c00964a0` |

**The park does not move.** Same key, same libpthread slow path, same infinite
wait, same `__lock=2`. The interposition — and therefore the recorder and every
`dlvsym(RTLD_NEXT,…)` path in the launcher — is ruled out by rows, not argument.
(Keep the knob: `RW_NO_SHADOW=1 tools/rw/run-rimworld-desktop.sh` is now the
standing A/B for any future "our wrapper did it" theory.)

### 14.2 The instrument: DR0 data-write watchpoint (`src/kernel/rw_watch.c`)

A read cannot name a writer, so the word gets a hardware breakpoint:
`DR0 = 0x10C00964A0`, `DR7 = 0x000D0001` (L0=1, RW0=01 data-write, LEN0=11
4 bytes), `#DB` (vector 1) consumed at the top of the trap handler, `DR6`
cleared per hit, and one `[WATCH] wr` row per store with pid, cpu, rip, new
value and the general registers. Built only with `make RW_WATCH_ADDR=0x…`; the
default kernel compiles the module to nothing.

Two instrument bugs were found and fixed before the rows meant anything, and
both are worth keeping:

1. **Arm on every trap, not on the timer range.** The first build armed only on
   vectors 32..47; under `-smp 8` it printed one `[WATCH] armed cpu=0` and zero
   write rows — the other seven cores take IPIs and syscalls, not the PIT.
   Arming on any trap covers every core within a millisecond of boot.
2. **Self-test before believing "no rows".** A silent mechanism and a
   never-written word look identical. The module now points DR0 at a kernel
   scratch word, writes it, and reports `[WATCH] selftest #DB=FIRED|MISSED`
   (`FIRED` in every run below).

```
[WATCH] armed cpu=0 addr=0x10C00964A0 len=4 dr7=0xD0001
[WATCH] selftest #DB=FIRED
[WATCH] armed cpu=1..7 …            (8/8 cores)
```

### 14.3 The four writes to `0x10C00964A0` in the whole boot

Rip → module+offset uses the run's own bases: UnityPlayer `0x10801FC000`
(`[RWMAIN] PlayerMain=0x108105f8b0` − `PlayerMain`'s `0xe638b0`), libpthread
`0x10823F1000` (`__lll_lock_wait`'s syscall return is `+0x13170`).

| n | pid/cpu | rip | site | store | value at word |
|---|---|---|---|---|---|
| 1 | 4/0 | `0x1080B67DD9` | UnityPlayer+0x96BDD9 | after `movq $0,(%rcx)` @+0x96BDD2 | `0x0` |
| 2 | 4/0 | `0x1080B67DEB` | UnityPlayer+0x96BDEB | after `mov %rcx,(%rsi)` @+0x96BDE8 | `0xC00964D0` |
| 3 | 4/1 | `0x10823FBFF3` | libpthread+0xAFF3 | after `lock cmpxchg %edx,(%rdi)` in `__pthread_mutex_lock` — **the store does not happen**, the locked instruction still traps | `0xC00964D0` (unchanged) |
| 4 | 4/1 | `0x1082404155` | libpthread+0x13155 | after `xchg` of 2 in `__lll_lock_wait` (sets the WAITERS bit) | `0x2` |

`n=3` is the decisive one: `__pthread_mutex_lock`'s fast path
(`atomic_compare_and_exchange_val_acq(lock, 1, 0)`) **failed** against a word
holding `0xC00964D0`, returned that value in `rax`, and fell into
`__lll_lock_wait`, which set the waiter bit and parked. Nothing was ever
holding a lock.

`n=1`/`n=2` are UnityPlayer's small-block pool: `movq $0,(%rcx)` then
`xchg %rsi,0x80(%rbx)` (publish) and `mov %rcx,(%rsi)` (link the previous
head), with `%rdx = %rdi = 0x30` — the block size. The block at
`0x10C00964A0` is being **linked into a 0x30-byte chain**, and the link lands
in the 40-byte `pthread_mutex_t`'s `__lock` field.

### 14.4 The memory picture at the park (one-shot dump on the store of 2)

```
[WATCH] parkmem addr=0x10C00964A0 -0x30..+0x50:
  0x10C00964A0 0x0 0x0 0x0 0x0 0x0  0x1000000002 0x0 0x0 0x0 0x0 0x0  0x10C0096500 0x0 0x0 0x0
```

Read it as qwords at `addr-0x30 … addr+0x50`: `[addr-0x30] = 0x10C00964A0`,
`[addr] = 0x1000000002` (after the waiter-bit store; `__lock=2`, `__count=0x10`
= the high half of the previous pointer `0x00000010C00964D0`), and
`[addr+0x30] = 0x10C0096500`. **Every 0x30 bytes a qword points at the next
block**: `0x…6470 → 0x…64A0 → 0x…64D0 → 0x…6500`. That is a chain, not a
mutex: the word the kernel parks on is a node's `next` pointer.

`__count=0x10` in §13's `[FUTEX] mutex` row is the same fact from the other
side — it is not mutex state at all, it is the pointer's high half.

### 14.5 Counterfactual: the mutex is the blocker, and the chain is systemic

If a *free* mutex is all that stands between the game and window creation, then
forging one acquisition answers it. `-DRW_WATCH_FIXUP=1` makes the `#DB`
handler, at the failed `lock cmpxchg` only (`f0 0f b1 17` immediately before
the rip, so the allocator's own stores are never touched), zero the word and set
ZF in the saved `rflags`, so `__pthread_mutex_lock` continues as if it had
acquired the mutex. Diagnostic only; never a shipping behaviour.

```
[WATCH] FIXUP n=1 was=0xC00964D0 rip=0x10823FBFF3 -> word=0, ZF forced (counterfactual, not a fix)
[WATCH] wr n=4 … rip=0x10823FBFF3 val=0x0            (the acquire the fast path believes it made)
[WATCH] wr n=5 … rip=0x10823FD7DD val=0x0 rdi=0x10C00964A0   (the caller's unlock — so the forgery was contract-consistent)
[IDLESTUCK] slot=4 pid=4 st=8 futex=0x10c0096470 rip=0x1082404170 /NFS/RimWorldLinux
```

The lock at `0x10C00964A0` is gone. The game then locks **the next block down
the chain**, `0x10C0096470`, from a *different* Unity call site:

```
[FUTEX] st pid=4 sp=0x17FFFFB408 0x10823FC0A3 0x10B0020010 0x10C0096470 0x10C0096470 0x10000024B9 0x108047DC9D word=0x2
[FUTEX] mutex pid=4 key=0x10C0096470 0x2 0x10 0x0 0x0 0x0
```

`0x108047DC9D` = UnityPlayer+0x2801C9D — a *second* Unity call site (the first
park's caller was UnityPlayer+0x1D43DA5). The word above the mutex in the frame
is `0x10000024B9`, the return address inside the launcher's own
`pthread_mutex_lock` (symbol start `0x1000002470`), i.e. the same interposed
wrapper. Same shape: `__lock` holds a chain pointer, `__count` holds its high
half, `__lll_lock_wait`, infinite.

The engine itself moved one stage, which is what makes this a counterfactual and
not a curiosity. `Player.log` gains a row that no previous run produced:

```
RuntimeInitializeOnLoadManagerInitializer: Failed reading 'ScriptingAssemblies.json'Display 0 '0': 1920x1080 (primary device).
[PhysX] Initialized MultithreadedTaskDispatcher with 2 workers.
```

So the queue push that parked at `0x10C00964A0` really was the blocker in front
of the *next* engine stage, and the chain node at `0x10C0096470` is the one
behind it.

### 14.6 Verdict

- **Nobody holds the mutex.** There is no owner to name and no `WAKE` to wait
  for: `pthread_mutex_lock` was called on a word that was already
  `0x10C00964D0`, i.e. on memory that is a Unity pool node's link field. §13's
  "the holder is itself parked in one of the 23 quiescent waits" is refuted by
  the write rows.
- **The wait is still "legitimate" in the kernel's terms** — glibc correctly
  refuses to enter a critical section whose word is not 0 — so no futex,
  scheduler or wake-path change can fix it. The bug is the *caller* locking an
  uninitialized/relinked `pthread_mutex_t`.
- **It is systemic, not one object.** Freeing that word moves the park to the
  neighbouring node at `0x…6470`, from a second call site. The whole 0x30-byte
  chain in `0x10C0000000+0x400000` is being used as mutexes whose `__lock` is a
  chain link.

### 14.7 The next instrument (not guessed — the address is computed)

The remaining question is *who publishes the pointer* into the queue object and
when the block was freed. Both are one watchpoint away:

```
queue   = *(UnityPlayer_base + 0x1F5C1F0)     ; the loader prints UnityPlayer_base
                                                (measured 0x10801FC000 → slot 0x108211E1F0)
mutex   = *(queue + 8)                         ; measured 0x10C00964A0
```

`this` comes from UnityPlayer+0x1BDB0D8 (`mov (%rax),%rdi` where
`rax = *(0x1F5C1F0)`, then `call 0x1D43D90 { mov 0x8(%rdi),%rdi; call
pthread_mutex_lock@plt }`). Re-point `RW_WATCH_ADDR` at `queue+8` and the same
module names the store that installs `0x10C00964A0`, and `RW_WATCH_ADDR` at the
node's owner (`pool+0x80`) names the free-list publisher. A second run with
`RW_WATCH_ADDR` at `0x10C00964D0` shows whether the chain is a *free* list
(pop leaves the link) or a live intrusive list.

### 14.8 Regressions

Serialized (never with the desktop harness):

```
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 398   Tests failed: 0
bash tools/gate/m2_threads.sh            m2: PASS
bash tools/gate/m12_thread_ceiling.sh    m12: PASS
```

### 14.9 Limits

- No window, no GLX, no frame; the ladder now ends on a *different* mutex
  (`0x10C0096470`), not on `XCreateWindow`.
- The fixup is a counterfactual probe, deliberately not a fix: it forges one
  acquisition to prove the blocker.
- The watchpoint is compile-time (`RW_WATCH_ADDR`) and must be re-pointed per
  address; DR0 has one slot.
- Why Unity's pool relinks a block that a live object points at is **not**
  answered here. Two candidates remain: Unity frees and re-carves the page
  (Unity-side), or an HobbyOS `mmap`/`madvise` semantic makes Unity believe a
  page was released when it was not. `vm_region_insert_locked` rejects overlaps
  and only merges exactly-adjacent identical regions, so a region-level double
  map is ruled out.

## 15. 2026-11-04 — M6: the memory-syscall stream of a real boot, and the one wrong semantic the audit found

Lane L24 (`rw-l24-mem`), tree seeded from `4b9599ac`. §14 left two candidates
for the free-list/live-object overlap: Unity re-carves the page, or an
`mmap`/`madvise` semantic of ours makes Unity believe memory was released when
it was not. This wave answers it with a **call stream** instead of a theory, and
then diffs every call shape the stream contains against Linux.

### 15.1 The instrument: `[MM]`, one row per memory syscall

`make RW_TRACE_MEM=1` compiles `l_mem_trace()` into
`src/kernel/linux_compat_mem.c` **and calls it from the dispatcher**
(`linux_compat.c`), not from each handler: every handler has several exits and a
second exit path drifts. The dispatcher snapshots the six argument registers
before the call and reports what the handler left in `rax`, so one line carries
`op`, pid, the raw six arguments and the return value:

```
[MM] #41 mmap pid=4 a=0x0 b=0x1000 c=0x3 d=0x22 e=-1 f=0x0 -> 0x1080000000
```

Empty build ⇒ not a byte changes in the shipping kernel.

### 15.2 What the game actually calls (231 rows, whole boot to the park)

| op | rows | notes |
|---|---|---|
| `mmap` | 94 | 4 file `MAP_FIXED` (ld.so segments), 2 `/dev/fb0` `MAP_SHARED`, the rest anon |
| `mprotect` | 72 | every one a *commit* of a PROT_NONE reservation, or a stack guard |
| `madvise` | 31 | **`MADV_NORMAL` only** (advice 0) |
| `brk` | 20 | glibc malloc, 0x21000 a step |
| `munmap` | 14 | every one a *trim* of a fresh reservation |
| `mremap`, `msync`, `mincore`, `memfd` | **0** | not used at all |

The strategy is unmistakable, and it is **not** the one the audit list guessed:
the game never uses `mremap`, never uses `MADV_DONTNEED`/`MADV_FREE`, and never
`mmap(MAP_FIXED)`s into its own arena. It **reserves, trims and commits**:

```
#45 mmap  (a=0, len=0x1FFFF000, prot=0, PRIVATE|ANON|NORESERVE) -> 0x1082414000
#46 munmap(0x1082414000, 0xDBEC000)      trim the unaligned head  -> keeps 0x1090000000
#47 munmap(0x10A0000000, 0x2413000)      trim the unaligned tail
#48 madvise(0x1090000000, 0x100000, MADV_NORMAL)
#49 mprotect(0x1090000000, 0x100000, RW)  commit the first window
```

and per job thread:

```
#57 mmap  (0, 0x801000, PROT_NONE, PRIVATE|ANON|MAP_STACK) -> 0x1082414000
#58 mprotect(0x1082414000 + 0x1000, 0x800000, RW)          guard page below the stack
```

The arena that holds the park word is the *first 4 MiB* of a later reservation: `#63 mmap(0, 0x1FFFF000, PROT_NONE) -> 0x10C0000000`,
`#64 munmap(0x10D0000000, 0xFFFF000)`, `#65 madvise(0x10C0000000, 0x400000,
NORMAL)`, `#66 mprotect(0x10C0000000, 0x400000, RW)`. Nothing else in the whole
boot maps, unmaps, mprotects or madvises that address again (checked by
replaying every row).

`tools/rw/mm_replay.py` replays the rows as ownership (mmap adds, munmap
removes, MAP_FIXED overlays) and confirms what the region list already
guaranteed: **no non-`MAP_FIXED` `mmap` ever returned a range overlapping a live
one**. The only duplicate-range event in the boot is `/dev/fb0` mapped twice
(both answers are the fixed `USER_FB_BASE_V2` slot, by design).

### 15.3 The audit, call by call, against a real Linux

`src/user/linux_probe_vmcommit.c` is an ordinary musl program that performs the
shapes above — the reservation dance with an aligned 256 MiB window and two
sibling 4 MiB commits, a 1 MiB commit in the middle of a reservation, the
8 MiB + guard stack shape, a hint inside a live mapping, an interior `munmap`,
`mremap` grow/shrink/move, `MADV_DONTNEED`, and `brk` — and prints one row per
semantic. The same source builds on the workstation (reference) and in the guest
(`bash tools/gate/t3_linux_probe.sh vmcommit`).

| row | host | guest (before) |
|---|---|---|
| reserve trim (munmap head+tail) | ok | ok |
| commit first 4MiB | ok | ok |
| commit next 4MiB keeps sibling | ok | ok |
| commit 1MiB in reservation middle | ok | ok |
| stack commit (base+page, 8MiB) | ok | ok |
| reserved guard page reads zero | ok | ok |
| prot_none preserves contents | ok | ok (frame reuse, not a guarantee) |
| hint inside live map is ignored | ok | ok |
| munmap interior split | ok | ok |
| mremap grow(maymove) / shrink / in place | ok | ok |
| madvise DONTNEED zeroes anon | ok | ok |
| **brk grow across a split heap** | **ok** | **FAIL (-ENOMEM)** |

So `mremap`, `mmap` with a hint, `MAP_FIXED`, partial `munmap`,
`madvise(DONTNEED)` and `mmap`-overlap are all Linux-conformant on this kernel —
including `mremap`'s content copy, `MREMAP_MAYMOVE` placement and in-place
shrink. The one wrong semantic was in `brk`.

### 15.4 The bug: `brk` could not grow a heap that had been split

`sys_brk` decided "is the grow already backed?" with a **single-region** test —
one `vm_region` had to cover the whole `[base,end)` span with `VMK_HEAP` +
`VM_PROT_WRITE` — and otherwise inserted the **whole span**. After any split (a
partial `mprotect`, a partial `munmap`, anything that divides the heap) that
insert overlaps the surviving fragment, so `vm_region_insert` answers `-ENOMEM`
and **every later `brk` grow fails for the life of the process**. Linux's `brk`
extends the data segment whatever its VMA count and only refuses a *foreign* VMA
in the way.

Fixed in `src/kernel/process.c`: walk the span fragment by fragment, accept any
`VMK_HEAP` region whatever its protection (it is still the data segment),
insert only the uncovered tail, and refuse only a non-HEAP region in the way.

Evidence, same probe, same tree, only `process.c` swapped:

```
before (HEAD process.c):  [LP] brk grow across split heap   FAIL detail=115
                          grow=0x1040010000 (the OLD break: refused)
after  (span walk):       [LP] brk grow across split heap   ok   detail=127
                          grow=0x1040040000
host reference:           [LP] brk grow across split heap   ok   detail=127
```

This is a real defect (one `mprotect` of one heap page was enough), but it is
**not** the park's cause: the game's heap is a single 1 GiB `VMK_HEAP` region
(`[BRK] grow base=0x1040021000 r=0x1040000000+0x40000000k2p3`, measured), so
every grow it issues takes the already-covered path. The park does not move
(§15.6).

### 15.5 What the park word actually is — measured, not inferred

The corrupting stores are Unity's own, and the trace proves the kernel cannot be
their author: the 4 MiB window that contains `0x10C00964A0` is mapped once
(`#63`), committed once (`#66`) and never touched by any later memory syscall;
its VMA is a single region; and no `mmap` in the boot overlaps it.

`§14`'s DR0 rows, re-read with the extra registers now printed, are a **carve
loop inside UnityPlayer**:

```
UnityPlayer+0x96BDD2  movq $0,(%rcx)          ; block->next = 0
UnityPlayer+0x96BDDC  xchg %rsi,0x80(%rbx)    ; old = bucket->head; head = block
UnityPlayer+0x96BDE8  mov  %rcx,(%rsi)        ; old->next = block
UnityPlayer+0x96BDCA  add  %rdx,%rcx ; cmp %rax,%rcx ; jbe …
```

with `%rdx = %rdi = 0x30` (the bucket's block size), `%rbx = 0x108218D2B0` (the
bucket), `%rax = 0x10C0097FD0` (the end of the 16 KiB unit
`0x10C0094000..0x10C0098000`). It walks the unit and relinks **every 0x30 block
in it**.

The word's owner is a *live* Unity `Mutex`, not a stray pointer. The launcher's
mutex tracer (`RW_TRACE_MUTEX=1`, staged by `/MTXON`, now also logging
`pthread_mutex_init`/`destroy` for every address) records:

```
[MTR] init m=0x10c00962f0 … 0x10c0096710  23 rows, stride 0x30, same caller
[MTR] init m=0x10c00964a0 pid=4 from=0x1081d9ee16
[MTR] lock m=0x10c00964a0 pid=4 from=0x1081f3fda5     <- the park, and the only lock
```

23 consecutive `pthread_mutex_init`s from **one** call site. Disassembling that
site (`/srv/nfs/rimworld/UnityPlayer.so`, vaddr == file offset) shows the
initialiser:

```
1ba2de0  push %rbp ; mov %rdi,%rbp
1ba2dec  call pthread_mutexattr_init
1ba2df9  call pthread_mutexattr_settype(…, 1)      ; PTHREAD_MUTEX_ERRORCHECK
1ba2e06  call pthread_mutexattr_setprotocol(…, 1)  ; PTHREAD_PRIO_INHERIT
1ba2e11  call pthread_mutex_init(m, &attr)
1ba2e30  jmp  pthread_mutex_destroy@plt            ; the matching fini
1ba2e60  mov $0x30,%eax ; ret                      ; sizeof(Mutex) == 0x30
```

and the park site is a job-queue dequeue (`this+8` is the `pthread_mutex_t *`,
`this+0xf8`/`+0xfc` are a ring's head/tail).

So: 23 `Mutex`es allocated from this bucket, still live, **never
`pthread_mutex_destroy`ed in the whole boot**, and the bucket's carve relinks
their blocks as it walks the unit. One 16 KiB unit therefore has two owners
inside Unity's allocator. No kernel memory operation can produce that on a
mapping that was never split, moved or unmapped.

### 15.6 Ladder after the fix

```
[IDLESTUCK] slot=4 pid=4 st=8 wake=0 futex=0x10c00964a0 rip=0x1082404170 /NFS/RimWorldLinux
Player.log: … RuntimeInitializeOnLoadManagerInitializer: Failed reading
            'ScriptingAssemblies.json'Display 0 '0': 1920x1080 (primary device).
```

Unchanged: no `[XC]` rows, no `XCreateWindow`/`XMapWindow`, no engine line past
`Display 0`. The next blocker is still the same word.

### 15.7 Where the next attempt should look

The remaining question is not a syscall semantic — it is *which* of Unity's two
allocator views is stale. Two instruments, both already built:

- `RW_WATCH_ADDR=0x108218D2F0` (`bucket+0x40`, the "last block" the carve
  publishes) and `RW_WATCH_ADDR=0x108218D330` (`bucket+0x80`, the head): every
  push/pop then names the call site that hands the unit out, and the two
  `rw` rows of the double-carve sit next to their callers (the watch row now
  prints `rbp` and twelve user-stack qwords).
- The `/MTXON` knob (`RW_TRACE_MUTEX=1` on the host) now logs `init`/`destroy`
  for **every** address plus every arena op, to `/RW_HOME/mtx.log`; the harness
  extracts it next to `Player.log`.

### 15.8 Regressions

Serialized (never with the desktop harness):

```
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 398   Tests failed: 0
bash tools/gate/m2_threads.sh            m2: PASS
bash tools/gate/m12_thread_ceiling.sh    m12: PASS
python3 tools/gate/m3_window_probe.py    m3: PASS
bash tools/gate/t3_linux_probe.sh mem    [LP] done rc=0
bash tools/gate/t3_linux_probe.sh vmcommit  [LP] done rc=0
```

### 15.9 Limits

- The carve is named, the two-owner condition is proven, but *why* Unity's
  allocator re-carves that unit is not answered here. It is a userspace
  allocator invariant, not a syscall return value: every memory syscall in the
  boot is Linux-conformant for the shapes the game uses.
- `mprotect(PROT_NONE)` still drops the pages (vm.c 4.4). The probe shows the
  contents usually survive because the frame allocator hands the same frame
  back, but that is luck, not a contract; the game only guards never-touched
  pages, so it is not exercised.
- `MAP_FIXED_NOREPLACE` still answers `-EINVAL` (the pre-4.17 Linux answer).
  Nothing in the boot uses it.

## 16. 2026-11-04 — M6: the park was glibc REFUSING the mutex init (no PI futexes), not a held lock

Lane L25 (`rw-l25-pifutex`), tree seeded from `6f42d9de`. §15 ended with a
proven two-owner condition inside Unity's allocator. The hypothesis this wave
tested was threading-side: `src/kernel/linux_compat_thr.c` answered
`FUTEX_LOCK_PI`/`UNLOCK_PI`/`TRYLOCK_PI` with `-ENOSYS`, and a `PRIO_INHERIT`
mutex whose lock silently failed would let two threads into a critical
section. **That hypothesis is refuted as stated, and a different, worse
consequence of the same `-ENOSYS` is confirmed.**

### 16.1 What glibc 2.31 actually does with a PRIO_INHERIT mutex

Disassembling the pinned `libpthread-2.31.so` (`__pthread_mutex_init`) and
reproducing it on the host with a seccomp filter that answers `ENOSYS` to the
PI futex commands gives the whole mechanism:

1. The **first** `pthread_mutex_init()` of a `PRIO_INHERIT` mutex probes the
   kernel: `futex(&local_zero, FUTEX_UNLOCK_PI|PRIVATE, 0)` at `+0xA7FF`.
   A tolerated errno (EPERM, EAGAIN, …) sets `tpi_supported = +1`;
   **`-ENOSYS` alone sets it to `-1`** (`+0xA84E`).
2. With `tpi_supported == -1`, every `PRIO_INHERIT` init takes `+0xA7B8`
   (`mov $0x5f,%eax` = **EOPNOTSUPP 95**) and jumps straight to the epilogue:
   **the mutex is never written**. `__lock` keeps whatever the allocator left
   in the block.
3. `pthread_mutex_lock()` returning an error is *not* the failure mode: a
   contended PI lock that gets `ENOSYS` calls `__libc_fatal` ("The futex
   facility returned an unexpected error code") and aborts. Measured on the
   host against the same DSO.

So Unity's `Mutex` pool is not "locked by nobody" — it is a set of objects
whose initialiser **returned 95 and did nothing**. Unity ignores the return
(measured: no abort, the game continues), then locks the object. Its `__lock`
is the 0x30-byte bucket's free-list `next` link, which is why §14 saw
`0x10C00964D0` in the word and a chain of `next` pointers every 0x30 bytes.

Measured in-guest with the new probe (`src/user/rw_pthread_pi.c`, gate
`tools/gate/m2_pi.sh`), with the word pre-filled with exactly the measured
link:

```
before (HEAD kernel):  [PTPI] init rv=95 kind=0 word=0xc00964d0
                       [PTPI] FAIL pi-mutex-init (... the first lock would park
                              on the stale word)
after  (this lane):    [PTPI] init rv=0 kind=33 word=0
```

`kind 33` = `PTHREAD_MUTEX_RECURSIVE(1) | PRIO_INHERIT(32)` — also correcting
§15's "ERRORCHECK" reading: `UnityPlayer+0x1BA2DF4` loads `1`, which is
RECURSIVE.

In the real game the same transition is visible in the launcher's recorder
(`RW_TRACE_MUTEX=1`):

```
before:  [MTXR] init-odd m=0x10c00964a0 kind=0  rv=0   (kind 0 = never written)
after:   [MTXR] init-odd m=0x10c00964a0 kind=33 rv=0
```

### 16.2 The implementation (`src/kernel/linux_compat_thr.c`)

`FUTEX_LOCK_PI`, `FUTEX_UNLOCK_PI` and `FUTEX_TRYLOCK_PI` are real now.
Priority inheritance itself is **not** implemented (this OS has no priority
scheduling to inherit into); mutual exclusion is. The one non-obvious rule:

**A woken `LOCK_PI` waiter must already own the word when it resumes.**
`pthread_mutex_lock.c:419-441` checks only `ESRCH`/`EDEADLK` from
`futex_lock_pi()` and otherwise proceeds to `__count = 1`; a plain wake with
`rax = 0` would therefore be read as "the kernel gave me the mutex". So:

- `UNLOCK_PI` **hands the lock over**: under `proc_lock` it CASes the word to
  the first PI waiter's tid (`| FUTEX_WAITERS` if another waiter remains) and
  only then makes that waiter READY with `context[0] = 0`.
- `LOCK_PI` parks only after re-reading the word under `proc_lock` (so a
  handoff cannot slip in), and `OWNER_DIED` is preserved when recovering so
  glibc still turns it into `EOWNERDEAD`.
- A plain `FUTEX_WAKE` **never** wakes a PI waiter, and
  `linux_compat_signal_interrupt()` skips them: for the same glibc reason an
  `-EINTR` resume would be read as ownership. The signal stays pending and is
  delivered when the lock arrives.
- `FUTEX_WAIT_REQUEUE_PI` / `CMP_REQUEUE_PI` stay `-ENOSYS` with a counted row:
  no boot has issued one (a PI mutex used with `pthread_cond_wait` would).
- The robust-list walk had a real bug this lane found and fixed: glibc marks a
  PI entry with the **low bit** of the list pointer (`entry 0x1000004081` for a
  mutex at `0x1000004060`), exactly as Linux's `exit_robust_list` masks it.
  Without the mask the "futex word" landed at `mutex+1` and `OWNER_DIED` was
  written into the wrong bytes, so a dying PI owner never marked its mutex.

### 16.3 Tests

- `src/kernel/linux_compat_pi_test.c` drives the **real** word transitions
  (the handler exports them under `KERNEL_MODE_UNIT_TEST`): uncontended,
  self-lock/EDEADLK, contended + WAITERS, trylock EAGAIN, owner-died recovery
  (with and without waiters), release, foreign release EPERM, and a failed-CAS
  must not store. 9 new tests: `Tests run: 407  Tests failed: 0`.
- `src/user/rw_pthread_pi.c` + `tools/gate/m2_pi.sh`: the pre-filled-mutex
  repro above, 8 threads × 20 000 critical sections (`total=160000 expect=160000`),
  recursive re-lock, trylock → EBUSY, and a ROBUST|PI owner death →
  `owner-died=EOWNERDEAD (130)`. `[PTPI] PASS`.

### 16.4 The ladder after the fix

```text
[PhysX] Initialized MultithreadedTaskDispatcher with 2 workers.
[MTXR] init-odd m=0x10c00964a0 kind=33 rv=0
Desktop is 1920 x 1080 @ 0 Hz
[RWMAIN] dlopen FAILED libGL.so.1: /LIB64/libGL.so.1: undefined symbol: _XLockMutex_fn
Unable to find a supported OpenGL core profile
Failed to create valid graphics context: please ensure you meet the minimum requirements
Forced renderer 17 is not supported, exiting
```

- **No park.** `IDLESTUCK` count is 0 (was 1, on `0x10C00964A0`), and the mutex
  summary is `lock=29653 bad=0 try=2531 bad=1 unlock=32183 bad=0` — the single
  trylock failure is Mono's `EBUSY` (`kind=1`), not a PI mutex.
- The game is **past** the allocator park and past PhysX; it reaches the
  display, then dies loading `libGL.so.1`.
- The next blocker is a **loader dependency**, not a syscall: our Mesa
  frontend `obj/mesa/lib/libGL.so.1` has undefined `_XLockMutex_fn`,
  `_XUnlockMutex_fn`, `_Xglobal_lock` and **no `DT_NEEDED` at all** (checked
  with `readelf -dW`), so it can only resolve Xlib internals from the global
  scope. The compat `libX11.so.6` does export them (`nm -D` shows all three),
  but by the time the game dlopens libGL they are not in the scope it searches.
  The M4 gate passes because its client links libX11 into the executable.
  Next step: give the frontend `DT_NEEDED libX11.so.6` (link `-lX11` in the
  Mesa build, `patches/`), or promote the already-loaded libX11 with
  `RTLD_GLOBAL` in the AD-10 `dlopen` wrapper before returning libGL.

### 16.5 Incidental finding: `alarm(2)` never fires — syscall 37 is ENOSYS

The launcher's SIGALRM ring dump (§14) never ran because `alarm()` is
`setitimer`-less here: `[LCOMPAT] ENOSYS nr=37` appears in every run. The
recorder now prints its summary periodically from `pthread_mutex_init`
instead. Not a blocker, but any diagnostic that relies on `alarm()` in this
guest is dead code.

### 16.6 Regressions

Serialized (never with the desktop harness):

```
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 407   Tests failed: 0
bash tools/gate/m2_threads.sh            m2: PASS
bash tools/gate/m12_thread_ceiling.sh    m12: PASS
bash tools/gate/t3_linux_probe.sh vmcommit  [LP] done rc=0  (PASS)
bash tools/gate/m2_pi.sh                 m2pi: PASS
```

### 16.7 Limits

- No priority inheritance. A low-priority holder is not boosted; the handoff
  gives mutual exclusion only. Stated, not implied.
- PI waiters are not interruptible by signals while parked (deliberate: an
  `-EINTR` resume would be read as ownership by glibc). A pending signal is
  delivered after the lock is granted.
- `FUTEX_WAIT_REQUEUE_PI`/`CMP_REQUEUE_PI` remain `-ENOSYS`; no measured boot
  issues one yet.
- No window, no GLX, no frame: the ladder now ends in the GL loader, one layer
  above where it started.

## 17. 2026-11-04 — M6: the renderer predicate is Unity's own device-create, and the GLX answers were wrong in five places

Lane L27 (`rw-l27-renderer`), tree seeded after wave 10 + the launcher preload.
§16 ended at `Forced renderer 17 is not supported, exiting`. The hypothesis
this wave started from — "Unity refuses the renderer from a GLX capability
probe" — is **half right**: the refusal is Unity's `IsGfxRendererSupported`,
but what fed it was not a version query. It was the **GL device create**, and
that failed because `glXChooseFBConfig` answered a **TrueColor** visual to a
`GLX_DIRECT_COLOR` request. Five shim bugs sat between the message and a
mapped window.

### 17.1 The predicate, with addresses

All offsets are file-relative in `UnityPlayer.so` (base in Player.log:
`[RWMAIN] dlopen ok libGL.so.1 ... base=` is libGL's, not this one;
`[RWMAIN] dlopen ok /RWDSO/UnityPlayer.so ... base=0x1080b04000` is).

```text
e64b20  caller: if (!IsGfxRendererSupported(*renderer, mode, ctx) && *renderer != -1)
e64b2f      printf("Forced renderer %d is not supported, exiting")   # .rodata 1b7635
e65210  IsGfxRendererSupported(): switch (renderer)
e6522a    cmp $0x11 (17, kGfxRendererOpenGLCore) -> the GL path
e7b1c0      GfxDevice::Create(renderer 13, ctx)      [0xd = the clamp target]
e7b240        e76e50(device)  -> the version-mapping loop
               192cfec(desc)                <- THE REAL DEVICE CREATE
               if NULL: 192cc31(0x11,&maj) / (0x12,&min)   (props 17/18)
                        ef44d0(maj,min,0)  -> renderer id or 0
                        if (id == 6) { printf("Unable to find a supported
                                        OpenGL core profile")  # .rodata 23ba38
                                        return NULL; }
                        else  ef4340(id) -> {maj,min} table, retry 192cfec
eeb2f0    return g_gfxDevice != NULL       (0x200f900)
e65241    if (g_gfxDevice == NULL) printf("Failed to create valid graphics
          context: please ensure you meet the minimum requirements")  # 29a1fa
```

`ef44d0(major, minor, 0)` linear-searches renderers **6..12** only
(`ef4340()+16*i`, table at `0x200fb40`, entries `{maj,min,feature-mask}`:
6={3,2} … 12={4,4}); renderer **13 = {4,5}** is the `-force-glcore` clamp
target and is *not* in that scan, so a 4.5 request maps to 0 and the loop
walks down. The message is therefore a **secondary** failure: it is printed
when the downgrade loop reaches the 3.2-core floor, which only happens because
`192cfec` kept failing.

`192cfec` fails before any `glXCreateContextAttribsARB`: the GLX device create
chooses an FBConfig, creates the window, and stops. That is where the real
divergence was.

### 17.2 Host vs guest, same game, same `-force-glcore`

Host: Xvfb `:77`, Mesa 26.0.8 llvmpipe, `apitrace trace`. Guest: the same
`UnityPlayer.so`, with the Mesa frontend's new `HOBBYOS_GLX_TRACE=1` trace
(`tools/mesa/patches/0005`, staged by `/GLXON`).

| step | host | guest (before) |
|---|---|---|
| `glXChooseFBConfig {RENDER_TYPE=RGBA, R/G/B/A=8, DOUBLEBUFFER=1, DEPTH=24, STENCIL=8, X_VISUAL_TYPE=GLX_DIRECT_COLOR}` | 6 configs; `config[0]` → `visualid=0x22, class=5 (DirectColor)` | **1 config; `visualid=0x21, class=4 (TrueColor)`** |
| `glXGetFBConfigAttrib` | per attribute | never called |
| `glXCreateContextAttribsARB {4.5 core, RELEASE_BEHAVIOR=FLUSH, NO_ERROR=False}` | context | **never called** |
| `XCreateWindow` (device window) | (SDL's window already mapped; this path not taken) | `-> 0 (window already exists)` |

Root cause of the first row: the compat `libX11.so.6` published **one** visual
(`0x21 TrueColor`) and `XGetVisualInfo()` **ignored its mask** — it returned
that visual whatever `VisualClassMask` asked for. Mesa's `choose_visual()` →
`get_visual()` searches X visuals by class, so the DirectColor request was
answered with TrueColor and Unity's GLX device gave up. A real 24-bit X screen
publishes `0x21 TrueColor` *and* `0x22 DirectColor` (`xdpyinfo` on the host:
exactly that pair, then more TrueColor ids).

### 17.3 The five shim fixes (each measured, not guessed)

1. **Visuals** — `XOpenDisplay` publishes `0x22 DirectColor` as well;
   `XGetVisualInfo()` honours `VisualIDMask | VisualScreenMask |
   VisualDepthMask | VisualClassMask` (and the colour-mask ones) over the two
   visuals; `XMatchVisualInfo()` matches either class.
2. **Window lifetime** — `XDestroyWindow()` releases the slot (`win.id = 0`,
   every field re-armed by `win_clear()`). SDL creates a 32×32 window, probes
   a legacy context in it, destroys it; the old rule left `win.id` set, so
   Unity's device window could never be created.
3. **The protocol fd** — Unity's `-logFile` `dup2()`s the log file over **fd 1
   and fd 2**, so the shim's outbound `ESC ] X`/`] F` went into `Player.log`
   and the desktop answered nothing: measured `[XC] wait_event: blocking read
   fd0` forever after `XMapWindow`. `rwlaunch` now keeps the pipe on **fd 9**
   (`dup2(1,9)` before spawn) and the shim selects it with an fstat probe —
   *fd 1 regular file and fd 9 FIFO* — falling back to fd 1 for a directly
   launched client (the m3/m4 probes), which is why those gates stay green.
4. **PropertyNotify** — a real server notifies every client that selected
   `PropertyChangeMask`, including the setter. Unity selects exactly that mask
   on the GL window and waits in `XIfEvent`; `XChangeProperty` and the
   `XStoreName`/`XSetWMHints`/`XSetWMNormalHints`/`XSetClassHint`/
   `XSetWMProtocols` paths now queue it.
5. **MapNotify belongs to XMapWindow**, not to the desktop's later `ESC ] G`
   geometry: Unity replaces its event mask with `PropertyChangeMask` before
   the geometry arrives, so a MapNotify queued from `set_geometry` under
   "current mask has StructureNotifyMask" was lost. The shim remembers that
   StructureNotify/Exposure were selected *at least once* on the window
   (`structure_selected`/`exposure_selected`) and delivers MapNotify from
   `XMapWindow` (ConfigureNotify/Expose still from the geometry).
   Unity's predicate is `UnityPlayer+0x194c19b`: `event->type == 19
   (MapNotify) && event->xmap.window == *(XPointer)arg`.

Two further bugs found by the same trace, both real Xlib semantics:

- **`XIfEvent` spun forever** on any pending event the predicate rejected: the
  old loop peeked at the head and left it there, so `wait_event` returned
  immediately (event pending) and the loop re-peeked. Measured rip inside
  `XIfEvent`, **127 945 216** `pump_nonblock` calls, one CPU at 100 %. It now
  dequeues non-matching events into a held array (BSS, ring-sized) and
  re-queues them when the match is found — Xlib's documented behaviour.
- **`XCreateWindow` returned 0 for a second window.** Unity creates three
  (32×32 probe, 1920×1080 device window, 1920×1080 window) and still maps
  earlier ids. Each create now returns a fresh XID (`0x200001+`) and replaces
  the single desktop surface; `win_id_live()` lets any live XID be mapped and
  the MapNotify carries that id.

### 17.4 Before / after

Before (the shipped ladder of §16, unchanged by wave 10's preload):

```text
[RWMAIN] dlopen ok libGL.so.1 -> 0x1040211eb0 base=0x1133006000
Unable to find a supported OpenGL core profile
Failed to create valid graphics context: please ensure you meet the minimum requirements
E.g. OpenGL core profile 3.2 or later for OpenGL Core renderer
Forced renderer 17 is not supported, exiting
```

After (`RW_TRACE_GLX=1 RW_TRACE_X11=1`, visualid now DirectColor and a window
that the desktop maps):

```text
[GLXT] glXChooseFBConfig screen=0
[GLXT]   attribs {0x8011=1, 0x8=8, 0x9=8, 0xa=8, 0xb=8, 0x5=1, 0xc=24, 0xd=8, 0x22=32771}
[GLXT]   -> 1 config, visualid=0x22 class=5 depth=24
[XC] XCreateWindow id=2097154
[XC] XSelectInput mask=0x0x62c07f
[XC] XSelectInput mask=0x0x400000
[XC] PropertyNotify atom=107
[XC] XMapWindow
[XC] MapNotify queued
[XC] XIfEvent caller=0x1082450466
[XC] set_geometry 2
;34
;1916
;1018
[XC] XIfEvent matched type=19
```

The desktop puts the window into pixel mode at 1916×1018 (screenshot
`obj/rw-desktop/shots/99_final.png`: full-screen app surface). `ESC ] X
1920;1080~` no longer appears in `Player.log` — it goes to the desktop on
fd 9.

### 17.5 The next blocker (exact rows)

After the third window's MapNotify, Unity waits in `XIfEvent` again — same
predicate (`UnityPlayer+0x194c19b`, MapNotify) — for a window it never maps
through the shim, and the desktop has no event left to give:

```text
[XC] XCreateWindow id=2097155
[XC] XMapWindow
[XC] XIfEvent matched type=19
[XC] XInternAtom -> 0
[XC] XIfEvent caller=0x1082451e6e
[XC] wait_event: blocking read fd0
```

`[IDLESTUCK] slot=4 ... st=3 ... rip=<libX11>` shows the main thread runnable
inside the shim, not parked: the remaining work is Unity's multi-window
expectation against the desktop's one-surface protocol (a second/third window
is aliased, but Unity still waits for a map event for one of them), or the
first non-X call after the window handshake. It is **not** the renderer
predicate any more: `IsGfxRendererSupported` never runs because
`glXCreateContextAttribsARB` is still not reached.

### 17.6 Instruments added (kept)

- `tools/mesa/patches/0005-glx-trace.patch`: opt-in `HOBBYOS_GLX_TRACE` trace
  of `glXChooseVisual`/`glXChooseFBConfig`/`glXGetFBConfigAttrib`/
  `glXGetVisualFromFBConfig`/`glXCreateContext{,AttribsARB}`/`glXCreateWindow`/
  `glXMakeCurrent`, with attribute lists. Costs one `getenv` when off.
- `tools/rw/run-rimworld-desktop.sh`: `RW_TRACE_GLX=1` stages `/GLXON`
  (rwlaunch exports `HOBBYOS_GLX_TRACE=1`); `RW_TRACE_X11=1` builds and stages
  the `-DXC_TRACE` libX11.
- `libx11.c` trace additions: `XCreateWindow id`, `XSelectInput mask`,
  `MapNotify queued`, `PropertyNotify atom`, `XIfEvent caller/matched`,
  `set_geometry`, `wait_event: blocking read`.

### 17.7 Regressions (all serialized, never with the desktop harness)

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 407   Tests failed: 0
make -f src/user/xcompat/Makefile.inc ARCH=intel xcompat_test_run
                                          201 checks, 0 failed (204 ABI rows)
python3 tools/gate/m3_window_probe.py     m3w: PASS
python3 tools/gate/m4_gl_probe.py         m4: PASS
```

The m3 probe caught the fd change: the first version keyed on "fd 9 is open",
which is also true for a directly launched client, and the probe's `ESC ] X`
went to an unrelated descriptor (no geometry, no green/blue blocks). The
fstat pair (`fd 1 regular` **and** `fd 9 FIFO`) is what makes the game and the
probes both work.

### 17.8 Limits

- No `glXCreateContextAttribsARB` call is reached yet, so the 4.5→3.3 clamp
  (§M4) is still unexercised by the game.
- The shim aliases all X windows onto one desktop surface; a client that needs
  two simultaneously visible windows would render both into the same pixels.
- `XIfEvent`'s held array is static (one display per process by contract), so
  two threads calling it concurrently would share it.

---

## 18. 2026-11-04 — M6: the MapNotify Unity waits for belongs to a *reparented* window, and Xlib remaps it

Lane L28 (`rw-l28-mapnotify`), tree seeded after wave 11 + §17. §17 ended with
Unity blocked in `XIfEvent` for a MapNotify "for a window it never maps through
the shim". That description was **wrong in one important way**: Unity *had*
mapped the window. What it waits for is the MapNotify a real X server generates
when a **mapped** window is **reparented** into a mapped parent — the shim's
`XReparentWindow` was a `return 0` stub, so the event never existed.

### 18.1 The awaited window, named by the instrument

`XIfEvent` now prints the `XPointer` argument (the predicate dereferences it:
`UnityPlayer+0x194c19b` is `event->type == 19 && xmap.window == *(Window *)arg`),
and the whole queued `(type,window)` list. Before the fix, at the block:

```text
[XC] XReparentWindow win=2097154 parent=2097155
[XC] XIfEvent caller=0x1082451e6e
 arg=69794042712 want=2097154 q=22:2097155,22:2097154,12:2097154,28:2097154,
[XC] wait_event: blocking read fd0
```

`want=2097154` — the **child** window, not the container (2097155) and not a new
one. The queue held PropertyNotify(2097155), PropertyNotify(2097154),
Expose(2097154) and MappingNotify; no MapNotify for 2097154 was ever generated
because `XReparentWindow` did nothing. The next call site
(`UnityPlayer+0x194c19e` → `0x194c187`) is the *peer* predicate for
**UnmapNotify**, i.e. Unity sequences on both events.

### 18.2 Host reference: what a real server does (Xvfb, same binary)

Two instruments, both kept (`tools/rw/x11-watch.c`,
`tools/rw/x11-reparent-probe.c`):

| step | host (Xvfb `:95`, llvmpipe) |
|---|---|
| 1 | `Create 0x400003 32x32` → `Destroy` (SDL's GL probe) |
| 2 | `Create 0x400007 1920x1080 override=0`, `Map` → **MapNotify(event=window)**, client mask `0x60c07f` |
| 3 | `Create 0x400008 1920x1080 override=1` (client selects **0x20000 = StructureNotify only**), `Map` → MapNotify(container) |
| 4 | `Unmap 0x400007`, `Reparent 0x400007 → 0x400008`, **`Map 0x400007`** → MapNotify(child) |
| 5 | later: reparent back to root, resize to 1280x720, context created, `GfxDevice` |

Step 4 is generated **by the server**: a native probe that maps a window,
maps an override-redirect container, reparents the window into it and then
simply drains events (no `XMapWindow` call) prints

```text
  event UnmapNotify      win=0x200001
  event ReparentNotify   win=0x200001
  event MapNotify        win=0x200001
  event Expose           win=0x200001
RESULT: MapNotify(w1) after reparent, no explicit map: YES (server remaps)
```

So the guest must deliver `UnmapNotify → ReparentNotify → MapNotify` (plus
Expose) from `XReparentWindow`, for the reparented XID.

### 18.3 The fix: event state per XID, not per display

`libx11.c` had one `d->win` record for "the window". Unity creates three, and
the *container's* create wiped the GL window's `structure_selected`, so the
child's MapNotify could never be produced. The shim now keeps a
`struct xwin wins[XW_MAX]` table (root included) with per-XID `parent`,
`mapped`, `event_mask`, "ever selected" flags and geometry, and:

- `XCreateWindow`/`XCreateSimpleWindow` add an entry and emit **CreateNotify**
  to the parent's `SubstructureNotifyMask`;
- `XMapWindow` marks *that* XID mapped, sends `ESC ] X` for the surface, and
  emits **MapNotify** to the window (StructureNotify) and to its parent
  (SubstructureNotify);
- `XUnmapWindow` emits **UnmapNotify** (window + parent) and stops input for
  the surface;
- `XReparentWindow` implements the real semantics above: `UnmapNotify`,
  `ReparentNotify`, then **MapNotify + Expose** for the remapped child, and the
  child becomes the surface XID;
- `XDestroyWindow` emits **DestroyNotify** (window + parent) and frees the slot;
- the desktop's `ESC ] G` now addresses **ConfigureNotify/Expose to the XID
  that asked for the surface** (`d->surface`), not to whichever window was
  created last — the container used to receive them;
- `XSelectInput` stores the mask per XID (root included), so the container's
  `StructureNotifyMask` no longer replaces the GL window's mask;
- key/button/motion events go to the surface XID with *its* mask (the
  container selects no input); `XGetWindowAttributes`, `XQueryTree` and
  `XGetGeometry` answer per XID;
- `XCheckIfEvent` is traced, and `XPeekIfEvent` is exported (it was missing
  from the DSO entirely).

Two new defensive exports matter for the same reason: Unity's own X11
dispatch table (135 names, extracted from the loader at
`UnityPlayer+0x1940706`) and the extension families it resolves from the same
handles (`Xkb*`, `XFixes*`, `XRR*`, `XF86VidMode*`, `XScreenSaver*`,
`XShmCreatePixmap`) are called without a NULL check on this build. All are
exported now; the extension ones answer "not present" coherently.

### 18.4 Before / after

Before (§17.5):

```text
[XC] XCreateWindow id=2097155
[XC] XMapWindow
[XC] XIfEvent matched type=19
[XC] XInternAtom -> 0
[XC] XIfEvent caller=0x1082451e6e
[XC] wait_event: blocking read fd0        <- forever
```

After:

```text
[XC] XReparentWindow win=2097154 parent=2097155
[XC] XIfEvent caller=0x1082456e6e
 arg=… want=2097154 q=22:2097155,22:2097154,12:2097154,28:2097154,18:2097154,21:2097154,19:2097154,12:2097154,
[XC] XIfEvent matched type=19
[XC] XIfEvent matched win=2097154          <- the reparent remap
[XC] XCheckIfEvent caller=0x1082456e87     <- UnmapNotify probe: false, as on the host
```

The window is on the desktop while it runs (screenshot `01_running.png`:
maximized `RMLAUNCH` surface, 1916x1018 content rect) and the game moves past
the handshake into its window-activation path.

### 18.5 The next blocker (exact rows)

Immediately after the UnmapNotify probe returns, the process jumps to address
**0** — an indirect call through a NULL function pointer, with our `Display*`
still in `rdi`:

```text
[XC] XCheckIfEvent caller=0x1082456e87
[KERNEL] pid=4 (/NFS/RimWorldLinux) memory fault VA=0x0 PC=0x0 rw=rx
         in=HOLE region=NONE … rdi=0x108020E000 rbp=0x0 ret=0x0 -> killed
[LCOMPAT] fault sig=11 pid=4 tgid=4 …
```

`0x108020E000` is `&g_d` (libX11 base `0x10801fc000` + `g_d`'s `0x12000`), and
`ret=0x0` says the jump pushed no return address — a tail jump through a NULL
slot. What is ruled out by measurement:

- every one of the 135 names in **Unity's own X11 table** is exported by the
  shim (the four that were missing — `XFixesQueryVersion`,
  `XFixesCreatePointerBarrier`, `XFixesDestroyPointerBarrier`,
  `XIBarrierReleasePointer` — are now stubs and none is entered);
- `XkbGetMap` is exported and not entered;
- no `glXGetProcAddress` call is made at all (the new Mesa trace prints every
  request), and `glXCreateContextAttribsARB` — the **only** hidden glX symbol
  in the guest libGL — is now exported (patch 0006); the host's glvnd libGL
  exports it, so Unity's GLX loader can find it there;
- no shim entry point of any kind is reached between the probe and the fault.

So the NULL slot is in a table the shim cannot see. The next instrument is a
`SIGSEGV` ucontext dump inside the compat DSO (print `RIP`/`RSP` and walk the
stack) or a QEMU gdb stub attached at the fault; both name the caller of the
NULL jump, which is what the next lane needs.

### 18.6 Instruments added (kept)

- `libx11.c` trace: `xtr_u`/`xtr_s`/`xtr_awaited`/`xtr_queue` (single-row
  records; `want=` is the predicate's window), all `XTNOINL` — an *inlined*
  trace row previously buried a fault inside `XCreateWindow+0x10f8`;
  `XCreateWindow` prints id/parent/geometry/vmask/evmask, `XSelectInput` its
  XID, `XMapWindow`/`XUnmapWindow`/`XReparentWindow`/`XDestroyWindow` their
  ids, `XCheckIfEvent` its caller.
- `tools/rw/x11-watch.c`: host observer (SubstructureNotify on the root, a
  line per create/map/unmap/reparent/configure/destroy plus the client's mask).
- `tools/rw/x11-reparent-probe.c`: the native probe that establishes the
  server-remaps-on-reparent rule.
- `tools/mesa/patches/0006-glx-export-create-context-attribs.patch`:
  `glXCreateContextAttribsARB` was `GLOBAL HIDDEN` in the guest object (the
  only hidden glX entry point) and therefore absent from `libGL.so.1`'s
  `.dynsym`; exported explicitly.
- `tools/mesa/patches/0007-glx-trace-getproc.patch`: `HOBBYOS_GLX_TRACE` now
  logs every `glXGetProcAddress` name and answer — a NULL answer there is a
  jump to 0, and the last line names the missing entry.

### 18.7 Regressions (serialized, never with the desktop harness)

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 407   Tests failed: 0
python3 tools/gate/m3_window_probe.py     m3w: PASS
python3 tools/gate/m4_gl_probe.py         m4: PASS
```

### 18.8 Limits

- The shim still aliases every XID onto one desktop surface: the two windows
  are not independently visible, they are one set of pixels with per-XID
  *event* state. `XReparentWindow` therefore only changes the event topology,
  never the pixels.
- `XPeekIfEvent`'s held array is static, like `XIfEvent`'s (one display per
  process by contract).
- The Xkb/XFixes/RandR/VidMode/ScreenSaver/Shm exports are presence stubs:
  they remove NULL slots and answer "extension not present", they do not
  implement those extensions.

## 19. 2026-11-04 — M6: the NULL jump is `XIGrabTouchBegin`, and the compat signal frame was 8 bytes off the ABI

Lane L29 (`rw-l29-nulljump`), tree seeded after wave 12. §18 ended with a jump
to address 0 (`PC=0`, `rdi=&g_d`) right after the post-reparent
`XCheckIfEvent(UnmapNotify)` probe returned false, and the one instrument that
could name the caller — a `SIGSEGV` ucontext dump — did not exist yet. It does
now, and this section is what it said.

### 19.1 The instrument, and the two bugs that made it lie

`src/user/rwmain.c` (the AD-9 launcher) installs `SA_SIGINFO` handlers for
`SIGSEGV`/`SIGBUS`. The handler is `write(2)` over a hand-rolled hex/dec
formatter (no stdio, no allocation) and prints, in this order:

- the fault (`si_code`, `si_addr`);
- `ucontext` `rip`/`rsp`/`rax`/`rdi`/`rsi`/`rdx`, each tagged with
  `<module>+0x<off>` when it points into a mapped object;
- every other GP register, same tagging (`rbx rcx rbp r8..r15`);
- the first four qwords *behind* each pointer-ish register (`mem`), so a
  dispatch table, a vtable and the object that held the NULL are all visible
  without a second boot;
- 24 qwords at the faulting `rsp`, then a scan of the first 2 KiB of stack for
  words that land inside a module (the caller chain, with offsets).

Every stack read is probed with `mincore()` first: an unmapped page would fault
inside the handler and the kernel would kill the process before the row was
flushed.

Two things were measured by *not* getting a dump, and both are real:

1. **The install never reached the kernel.** `sigaction()` in this file is
   interposed (see 19.1b), so `rw_sig_install()`'s own call hit the wrapper,
   took its "keep ours" branch and returned success without a syscall. The
   launcher printed `sigdump armed` and then died with the default action.
   `rw_sig_install()` now goes through the real `sigaction` it resolved with
   `dlvsym(RTLD_NEXT, ...)`.
2. **Someone else owns SIGSEGV.** Mono installs a handler for null-check
   translation and Unity installs a crash handler *before* it
   (`UnityPlayer+0xde6800`, `SA_SIGINFO`), and both arrive after the launcher's
   install. Measured: with our handler replaced, the first fault at `PC=0` was
   followed by a *second* fault inside UnityPlayer with every register reading
   `0xB` — somebody else's handler, crashing. So `sigaction`/`signal` are
   interposed too: every non-default `SIGSEGV`/`SIGBUS`/`SIGILL` install is
   logged with the installer's return address and the launcher's dump stays
   installed (`RW_SIGKEEP=0` restores the old behaviour for an A/B).

### 19.1b The compat signal frame was 8 bytes off the x86-64 ABI

With the handler finally installed it was entered — and faulted immediately:

```text
[KERNEL] User process fault! Vector: 13 RIP: 0x1000003971
```

`0x1000003971` is `movaps %xmm0,0x30(%rsp)` in the handler's prologue: a #GP
from a misaligned SSE spill. Linux enters a signal handler with
`%rsp % 16 == 8` (the psABI's function-entry invariant, because a `call` leaves
`rsp ≡ 8`); `l_sf_plant()` built the frame at a 16-byte boundary and entered the
handler with `%rsp % 16 == 0`, so every normally-compiled handler that spills
with `movaps` dies. Verified against the host with a naked handler:
`host Linux: handler entry rsp % 16 == 8`.

The fix (`src/kernel/linux_compat_thr.c`) makes the frame start 8 bytes *below*
the boundary (`base = ((sp - L_SF_SIZE) & ~15) - 8`) — which also puts the
ucontext on `frame+8` (16-aligned, exactly as Linux has it) — and moves the
FXSAVE image from `frame+560` to `frame+568` so it stays 16-aligned for
`FXSAVE`/`FXRSTOR`. `rt_sigreturn` already re-derives `frame = rsp - 8` because
the handler's `ret` pops the pretcode, so nothing else changed. This is the
*second* handler-side bug this frame had; `l_sf_plant` is now the one place a
signal-frame ABI claim lives.

### 19.2 The dump, and the callee it names

```text
[XC] XCheckIfEvent caller=0x1082456e87
[SIGDUMP] SIGSEGV code=1 addr=0x0
[SIGDUMP] uctx rip=0x0 rsp=0x17ffffb2d8 rax=0x10400c9370 rdi=0x1080210000 rsi=0x0 rdx=0x200002
[SIGDUMP] greg rbx=0x10400c9370 rcx=0x1 rbp=0x0 r8=0x17ffffb2f8 r9=0x1 r10=0x1082b30358 r11=0x0
          r12=0x10400cf050 r13=0x200003 r14=0x1080210000 r15=0x10400335c0
[SIGDUMP] mem rax=[10400cf050,200002,200003,10802172b8] rdi=[0,0,5,b] r15=[1080d2ce6f,108244c034,...]
[SIGDUMP] stk [0]=0x1082458083 [8]=0x17ffffb2f0 ... [30]=0x108245765d ...
[SIGDUMP] chain [+0x680]->ld-linux-x86-64.so.2+0x12bdc ...
```

`stk[0]` is the return address of the faulting call, and `r10 = 0x1082b30358`
is the slot it read. With `UnityPlayer.so` loaded at `0x1080b0a000`:

- `stk[0] - base = 0x194e083` → **`0x194e080: call *(%r10)`**
  (`mov 0x60c8d5(%rip),%r10 # 0x1f5a940` at `0x194e064`; `r10 = base+0x2026358`,
  so the call is `call *(base+0x2026358)`);
- the slot `UnityPlayer+0x2026358` is filled at `UnityPlayer+0x194162a`
  (`mov %rax,0x6e4d22(%rip)`) from the resolver call
  `0x19419cc(name=*0x294a76, ctx)` — and the string at `0x294a76` is
  **`XIGrabTouchBegin`**;
- the neighbouring slots name the whole family in order: `XIQueryDevice`,
  `XIFreeDeviceInfo`, `XISelectEvents`, `XIGrabTouchBegin`,
  `XIUngrabTouchBegin`, `XIQueryVersion`, `XIGetSelectedEvents` — the eight
  names `tools/rw/x11-symbols.txt` attributes to `libXi.so.6`.

The call signature matches the dump exactly
(`XIGrabTouchBegin(dpy, deviceid=0, win=0x200002, owner_events=1, mask, 1, mods)`),
and the reason the slot is NULL is one line earlier in the serial:

```text
[RWMAIN] dlopen FAILED libXi.so.6: libXi.so.6: cannot open shared object file: No such file or directory
```

Unity resolves the XI entry points out of that handle, stores NULL for every
one of them, and calls `XIGrabTouchBegin` **without a NULL check** when the
window is shown. A missing *extension* is survivable in this build —
`XQueryExtension` already answers False for everything and Unity still calls the
entry point — so "report the extension absent" was already the state and it
crashed anyway. The soname has to exist.

### 19.3 The fix: `libXi.so.6` with "no XInput2 device here" semantics

`src/user/xcompat/xi_null.c` (staged by `src/user/xcompat/stage.sh` like every
other `libX*.so.*`, built by `Makefile.inc`, gated by
`src/user/xcompat/required-xi.txt` + `make xcompat_xi_check`):

| entry point | answer |
|---|---|
| `XIQueryVersion` | `BadRequest`, `*major=2 *minor=0` — no XInput extension to negotiate with |
| `XIQueryDevice` | `NULL`, `*ndevices_return = 0` |
| `XISelectEvents` | `Success`, does nothing |
| `XIGrabTouchBegin` | `Failure` (no touch device exists, so a touch grab cannot be granted) |
| `XIUngrabTouchBegin` | `Success` (nothing was grabbed) |
| `XIGetSelectedEvents` | `NULL`, `*num_masks_return = 0` |
| `XIFreeDeviceInfo`, `XIBarrierReleasePointer` | no-ops |

No libc, no allocation, nothing blocking, no `DT_NEEDED` — the same rules as
`udev_null.c`/`alsa_null.c`. The grab row is traced to fd 2, so the ladder
carries it: `[XI] XIGrabTouchBegin dev=0 win=0x200002 owner=1 -> Failure (no touch device)`.

### 19.4 Also: the Mesa DSO the harness staged was stale

`patches/0006` (export `glXCreateContextAttribsARB`) and `0007` (trace
`glXGetProcAddress`) were committed in wave 12 but `obj/mesa/lib/libGL.so.1`
was still the pre-patch object: `readelf --dyn-syms` had no
`glXCreateContextAttribsARB` and `strings | grep HOBBYOS_GLX_TRACE` was empty
(skill rule 29 again). That is why §18.5 concluded "no `glXGetProcAddress` call
happens at all" — the trace it was looking for was not in the binary. After
`bash tools/mesa/build-hobbyos.sh` (65 s, object cache) the trace fires on the
first GLX call and the game's `glXGetProcAddress(glXCreateContextAttribsARB)`
answers `0x1133abe250`. **A green source tree is not a staged DSO.**

### 19.5 After: the ladder reaches GfxDevice and an OpenGL 3.3 device

```text
[XI] XIGrabTouchBegin dev=0 win=0x200002 owner=1 -> Failure (no touch device)
[GLX] context request 4.5 core -> 3.3 core
Initialize engine version: 2022.3.35f1 (011206c7a712)
Forcing GfxDevice: OpenGL Core
GfxDevice: creating device client; threaded=1; jobified=0
Renderer: softpipe
OPENGL LOG: Creating OpenGL 3.3 graphics device ; Context level  <OpenGL 3.3> ; Context handle 1076667072
```

The window is on the desktop and it **paints**: `01_running.png` shows the
maximized `RLAUNCH` frame (1916x1018 content rect) whose white content area
carries Unity's own on-screen log text (`UnityMemoryConfigurationParameters`,
the PlayerPrefs path warning) — i.e. softpipe rendered glyphs, `XPutImage`
presented them and the desktop composited them. The process then dies in audio
init, below, so this is a presented frame, not a game scene.

### 19.6 The next blocker (exact row): a relocation store into a dlopen'ed DSO's data page

FMOD's audio probe loads the piece DSOs and then dies on a **write** to a page
whose VMA says read-write but whose PTE does not:

```text
[RWMAIN] dlopen ok libpulse-simple.so.0 -> 0x104c644ac0 base=0x1087d12000
[RWMAIN] dlopen ok libasound.so.2       -> 0x104c644ac0 base=0x1087d12000   <- same handle AND base
[RWMAIN] dlopen ok libasound.so.2       -> 0x104c644b30 base=0x1087d12000   <- new handle, same base
FMOD failed to initialize the output device.: "Error initializing output device. " (60)
[SIGDUMP] SIGSEGV code=1 addr=0x1087d15f18
[SIGDUMP] uctx rip=0x10080077ce(ld-linux-x86-64.so.2+0x77ce) rsp=... rax=0x1087d15f10 r10=0x1087d12000
[KERNEL] pid=4 memory fault VA=0x1087D15F18 PC=0x10080077CE rw=w in=PROT
         region=0x1087D15000+0x2000 prot=3 kind=7 -> killed
```

`ld.so+0x77ce` is applying a relocation (`rax` is the store address,
`base+0x3f10`), i.e. the first write to a `MAP_PRIVATE` file page of a DSO the
loader has just mapped: `vm_handle_fault` answered `PROT` instead of copying the
page. Two facts to check first, both visible above: glibc's `dlopen` returned
**the same handle** for two different sonames (a `st_dev`/`st_ino` collision
would do that), and the third `dlopen` mapped a *different* object at **the same
base** as the first. Both point at the loader/mmap side, not at audio.

### 19.7 Regressions (serialized, never with the desktop harness)

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 407   Tests failed: 0
python3 tools/gate/m3_window_probe.py     m3w: PASS
python3 tools/gate/m4_gl_probe.py         m4: PASS
```

### 19.8 Instruments added (kept)

- `src/user/rwmain.c`: `[SIGDUMP]` ucontext dump (registers, `mem`, stack,
  chain), `sigaction`/`signal` install watch with `RW_SIGKEEP`, `rw_sig_install`
  through the real entry point.
- `src/kernel/linux_compat_thr.c`: Linux ABI signal-frame alignment (frame ≡ 8
  mod 16, fpstate at `frame+568`).
- `src/user/xcompat/xi_null.c` + `required-xi.txt` + `xcompat_xi_check`.
- `tools/rw/build-ad9-launcher.sh`: rebuild only the AD-9 launcher (pair with
  `RW_SKIP_BUILD=1 tools/rw/run-rimworld-desktop.sh`).

### 19.9 Limits

- `RW_SIGKEEP` keeps the launcher's dump installed, which also disables Mono's
  null-check translation while it is on. It is a diagnostic default; a lane that
  wants Mono's handler back should set `RW_SIGKEEP=0`.
- The dump's module table is refreshed after every `dlopen` (the handler must
  not walk the module list itself: `dl_iterate_phdr` takes the loader lock, and
  the faults worth dumping happen inside `ld.so`). A register that points into a
  DSO loaded *after* the last successful `dlopen` still prints untagged;
  `mods=4` in the `sigdump armed` row is the startup value.
- `RW_SKIP_BUILD=1` after a gate boots that gate's `disk.img` (`PROBE.ELF`,
  `System halt`), not the desktop. Re-run the full harness after m3/m4.
- `libXi.so.6` implements no XInput2 protocol: it answers "no devices, no
  grab". A client that needs real XI2 (touch, tablets, raw events) gets the
  no-hardware answer, which is what this desktop is.

## 20. 2026-11-04 — M6: the audio kill is the COW *refault budget*, and the loader was innocent

Lane L30 (`rw-l30-loader`) started from §19.6's two open claims: "the COW path
refuses a MAP_PRIVATE file write" and "dlopen returned the same handle / the
allocator re-used a base". Instrumenting both (`[COW]` and `[MPROT]` rows in
`vm.c`, `dev=`/`ino=`/`name=` on the launcher's `dlopen ok` row) retired the
second claim and replaced the first with a sharper one.

### 20.1 What the rows said: the copy worked, the *fifth* copy was killed

The first instrumented run reproduced §19.6 exactly — and every COW row was an
identity **hit** (the leaf really was the page-cache frame):

```text
/LIB64/libpulse-simple.so.0
[COW] va=0x1087D13000 r.base=0x1087D13000+0x2000 prot=3 flags=18 kind=7 ooff=0x2000 leaf=0x2F54F000 obj=0x2F54F000 src=pagecache
[MPROT] a=0x1087D13000 len=0x1000 prot=1 rc=0 r=0x1087D13000+0x1000 rprot=1
/LIB64/libasound.so.2
[COW] va=0x1087D13000 r.base=0x1087D13000+0x1000 prot=3 flags=18 kind=7 ooff=0x2000 leaf=0x2F54E000 obj=0x2F54E000 src=pagecache
[MPROT] a=0x1087D13000 len=0x1000 prot=1 rc=0 r=0x1087D13000+0x1000 rprot=1
... four more loads, four more COW+RELRO pairs ...
[KERNEL] pid=4 memory fault VA=0x1087D13F18 PC=0x10080077CE rw=w in=PROT
         region=0x1087D13000+0x2000 prot=3 kind=7 -> killed
```

Each COW succeeded (a different leaf each time, i.e. a fresh private copy per
load). FMOD probes its audio backends by **dlopen/dlclose**: every probe maps a
new DSO into the hole the previous one freed, so the *same VA* is
copy-on-write relocated once per probe. `vm_handle_fault` charged every one of
those correct copies to the "this page keeps refaulting" budget
(`as->retry_va/retry_count`, `FAULT_RETRY_MAX` = 4) and killed the **fifth**
legitimate copy. The budget is only meaningful for the case that makes no
progress at all — a present, *writable* leaf, i.e. a stale TLB entry — so a COW
now always runs and resets the counter (`retry_count = 0`) instead of
incrementing it.

Two more things the same rows proved:

- `vm_mprotect` re-encoded the resident leaves *before* rewriting the region
  records, so a span rewrite that failed left the VMA read-write with read-only
  PTEs. Nothing failed in this boot (`[MPROT] ... rc=0`), but the order is now
  records-then-leaves so that state cannot be produced.
- The COW test itself no longer requires the leaf to BE the page-cache frame: a
  private page a protection change left read-only, or a leaf that belongs to an
  earlier object for the same file (`file_obj_invalidate_all()` drops the cache
  slot on every FAT16 write), are both legitimate copy sources. Linux keys the
  copy on "the VMA allows the write and the PTE does not", nothing else.

**Also found by the new unit test:** `vm_as_clone_into` re-mapped an object
frame into a forked child with the region's `prot` (read-write). The child's
first store would have gone straight into the page-cache frame every other
mapping shares — no COW, silent text/data corruption across processes. A
private child now gets the same read-only object frame the fill path installs.

### 20.2 The loader identity claim was a dlclose artifact

The `dlopen ok` row now prints `l_name` and the `(dev, ino)` pair glibc keys
"already loaded" on:

```text
[RWMAIN] dlopen ok libpulse-simple.so.0 -> 0x104c647d20 base=0x1087d10000 dev=257 ino=6270489 name=/LIB64/libpulse-simple.so.0
[RWMAIN] dlopen ok libasound.so.2       -> 0x104c647d20 base=0x1087d10000 dev=257 ino=6270486 name=/LIB64/libasound.so.2
[RWMAIN] dlopen ok libasound.so.2       -> 0x104c647d90 base=0x1087d10000 dev=257 ino=6270486 name=/LIB64/libasound.so.2
```

The handles and bases repeat because the *same link-map address and the same
first-fit hole* are reused after `dlclose`, not because two objects collided:
each row's `name`/`ino` is the object that was actually loaded. A third object
at an already-used base was likewise a freed-then-reused base. The allocator is
correct — and now has a test that says so (see 20.4).

What was genuinely wrong was the *identity pair itself*:

- `k_stat_fill` reported `st_dev = 0` for every file type, so the pair glibc
  compares was the synthetic inode alone, and a FAT16 file and an NFS file with
  equal inodes would have collided. Each volume now has its own `st_dev`
  (`0x101` FAT16, `0x202` NFS, pseudo-volumes for pipes/sockets/procfs/devfs)
  and `l_stat_fill` passes it through instead of zeroing it.
- `file_stat_path` answered **`st_ino = 0` for every NFS path**. That is the
  whole `/NFS` tree — UnityPlayer and the Mono DSOs — reporting one identity.
  NFS path stats now fold the server file handle, the same value `fstat()`
  reports, so `stat(path)` and `fstat(fd)` agree.

```text
  [FS-ID] 0x0000000000000101:0x00000000000022F2 0x0000000000000101:0x00000000000022F3
```

(two byte-identical files, distinct inodes, same volume — the loader identity
test in `vm_test.c`.)

### 20.3 After: the audio probe is survivable, the wall is Mono

```text
[XI] XIGrabTouchBegin dev=0 win=0x200002 owner=1 -> Failure (no touch device)
Initialize engine version: 2022.3.35f1 (011206c7a712)
Forcing GfxDevice: OpenGL Core
GfxDevice: creating device client; threaded=1; jobified=0
Renderer: softpipe
OPENGL LOG: Creating OpenGL 3.3 graphics device ; Context level  <OpenGL 3.3> ; Context handle 1077918832
[RWMAIN] dlopen ok libpulse-simple.so.0 -> 0x104c647d20 base=0x1087d10000
... six more probe loads, each COW+RELRO, none killed ...
FMOD failed to initialize the output device.: "Error initializing output device. " (60)
Forced to initialize FMOD to to the device driver's system output rate 48000, ...
FMOD failed to initialize the output device.: "An error occured that wasn't supposed to.  Contact support. " (33)
Unable to initalize any audio device (even FMOD nosound device), please check your audio drivers and/or hardware for malfunction
```

Audio is now **non-fatal**: FMOD falls back twice, reports no device, and the
engine keeps going into Mono. The next blocker is in the managed layer:

```text
Unable to find method ProcessFrame in [UnityEngine.CoreModule.dll]UnityEngine.Playables.IPlayableBehaviour
Unable to find type [UnityEngine.CoreModule.dll]UnityEngine.Playables.Playable
...
[KERNEL] pid=4 (/NFS/RimWorldLinux) memory fault VA=0x0000000000000038
         PC=0x0000001083EA8B92 rw=r in=HOLE region=NONE
         rbx=0x10F0009488 rdi=0x0 rsi=0x10819B6500 -> killed
[EXITPC] pid=4 last_user_pc=0x1080043099 code=0x4000000B
```

`0x1083EA8B92` is inside `libmonobdwgc-2.0.so` (its base is ~0x1083D6D000; the
`sigaction` install rows put Mono at `0x1083d6d279`/`0x1083e3e933`), and the
fault is a NULL+0x38 read, not a mapping failure. The `Unable to find type
[...CoreModule.dll]` rows are the engine failing to resolve its own managed
types just before it dies, so the next lane's question is *which assembled
`UnityEngine.CoreModule.dll` Mono actually opened*, not the VM.

The post-audio death site also varies run to run, which says the managed layer —
not the fault path — is the flaky frontier. Four instrumented runs produced:
`VA=0x6 UnityPlayer+0x198b548` (a `%rdi=0` table read) once, `VA=0x38
libmonobdwgc` twice, and once a wild `sys=202` (futex) write to
`VA=0x2882A10B78` while Unity was still resolving the GL entry points. None is
a `region=NONE` hole the VM should have filled: they are NULL/garbage pointers
used by code above the kernel. `01_running.png` still shows the process's stdout
window (Unity's `memorysetup-*` lines), **not** a game frame, and no `XPutImage`
content-flush row appears in any of them. This section does not claim a frame.

### 20.4 Tests

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh    Tests run: 410   Tests failed: 0
bash tools/gate/m2_mono.sh                PASS
python3 tools/gate/m3_window_probe.py     m3w: PASS
python3 tools/gate/m4_gl_probe.py         m4: PASS
```

New kernel tests (`src/kernel/vm_test.c`):

| test | asserts |
|---|---|
| `test_vm_file_cow_private_leaf` | a write fault copies a present read-only private leaf even when it is not the object's frame; the copy is writable and carries the bytes; the page-cache frame is untouched; **eight consecutive COWs on one VA all succeed** (the measured fifth-load kill). |
| `test_vm_dso_bases_distinct` | six DSOs of colliding size/hash get distinct, non-overlapping bases inside the mmap arena, one page-cache object, a hint naming an occupied base is skipped, and each mapping's private copy is independent. |
| `test_fs_dso_identity_pair` | two byte-identical files report distinct `(st_dev, st_ino)`, `stat(path)` agrees with `fstat(fd)`, and `/dev/console` lives on its own volume. |

### 20.5 Instruments kept

- `[COW]` (`vm.c`): one bounded row per copy-on-write fault — region
  base/len/prot/flags/kind/obj_off, the resident leaf, the object's page, and
  `src=pagecache|private`.
- `[MPROT]` (`vm.c`): every `mprotect` that touches an object-backed private
  region, with `rc` and the resulting region — how the RELRO split-after-copy
  pattern above is visible at all.
- `dlopen ok ... dev=<n> ino=<n> name=<path>` (`rwmain.c`): the loader identity
  table. Read it before believing a "same handle" row.

## 21. 2026-11-04 — M6: the managed layer was never corrupted; it was a 1024-byte NFS short read

Lane L31 (`rw-l31-managed`) started from §20's "the death site varies, so the
managed layer is corrupt" and ended with three concrete, reproducible bugs and
no memory corruption in the acceptance path. The variance was real but it was
the *shape of a deterministic failure's aftermath*, not a corrupted heap.

### 21.1 First: the assemblies are byte-exact through the bind

Before any kernel instrument, an in-guest probe (`src/user/rwsha.c`, run as an
app in the desktop session) read all 102 files under `/RWMANAGED` — the
VFS-bound FAT16 copy the engine's `mmap`s are served from — and printed a
SHA-256 per file:

```text
[SHA] Assembly-CSharp.dll 082db1dd4f7f1d0b72960d7e1beead8fbfe6957200e8627f65bda0dbbe1dd8f8
[SHA] UnityEngine.CoreModule.dll 69d3dbc5867bbc3992f26a21d98a897ca3907771df1a1c5609435be95b357942
... 100 more ...
[SHA] done ok=102/102
```

Every digest equals the host's `sha256sum` of
`/srv/nfs/rimworld/RimWorldLinux_Data/Managed/<name>` (102 files, 40,994,816
bytes). The probe also hashed the *mapping* — `mmap(PROT_READ, MAP_PRIVATE)`
length = the file size, the path Mono itself uses — and got the same digests
(`[MMAP] UnityEngine.CoreModule.dll 69d3dbc…`), and re-read the largest
assembly in 64 KiB chunks with an `lseek` before each chunk (`[SEEK]`), same
digest again. A `MAP_PRIVATE` write test (`[COW]`) diverged in the mapping and
left the file unchanged.

**Data integrity is not the bug.** A wrong byte in CoreModule would have
explained "Unable to find method … in [UnityEngine.CoreModule.dll]" outright;
the bytes are right.

### 21.2 The death is `mono_class_get_image(NULL)` called from UnityPlayer

The fault PC resolves exactly:

```text
[SIGDUMP] uctx rip=0x1083ea8b92(libmonobdwgc-2.0.so+0x17eb92) rdi=0x0 rsi=0x10819b6500(UnityPlayer.so+0xeae500)
[KERNEL]  memory fault VA=0x0000000000000038 PC=0x0000001083EA8B92 rw=r in=HOLE -> killed
```

```text
000000000017eb92 <mono_class_get_image>:
  17eb92: 48 8b 47 38    mov 0x38(%rdi),%rax
  17eb93: c3             ret
```

and the return address on the stack is `UnityPlayer.so+0xde7e41`:

```text
de7e35: mov 0x568(%rax),%rdi     ; a MonoClass* field of a Unity object
de7e3c: call df7960              ; import thunk -> mono_class_get_image
de7e41: mov %rax,(%rbx)
```

So Unity held a `MonoClass*` that was **NULL**: its type lookup had failed. That
is not a corrupted heap; it is a failed lookup whose result is used unguarded.

### 21.3 Why every CoreModule lookup returned NULL: a 1024-byte short read

Unity reads `<dataPath>/ScriptingAssemblies.json` (2,915 bytes) with **one**
`read(2)` of the whole file, and treats a short count as a failed load:

```text
RuntimeInitializeOnLoadManagerInitializer: Failed reading 'ScriptingAssemblies.json'
```

`[XOPEN]`/`[XREAD]` rows in the compat openat and the NFS branch of
`file_read()` named the cause on the first instrumented boot:

```text
[XSTAT] size=2915 type=1
[XOPEN] /NFS/RimWorldLinux_Data/ScriptingAssemblies.json flags=0 -> 11
[XREAD] want=2915 got=1024 cur=0 size=2915        <-- before
```

`nfs_read_file()` issued one NFS READ RPC per call and capped it at the
mount's `rsize` (`NFS_READ_CHUNK` = 1024). A regular-file `read()` must return
the whole count unless it hits EOF or an error; a 2915-byte request that
returns 1024 is a contract violation, and Unity's reader does not loop.

With no assembly list, Unity never loaded the engine assemblies, so **every**
`mono_class_from_name(image, …)` on CoreModule returned NULL — the 352
"Unable to find type/method [UnityEngine.CoreModule.dll]…" rows that §20 read
as damage — and the first unguarded use of one of those NULLs killed the
process. Which NULL was hit first depended on what ran next, which is exactly
why the death site moved between runs.

Fix (`src/kernel/nfs.c`): `nfs_read_file()` now loops over `rsize`-sized READ
RPCs until `len` bytes, EOF or a zero count. One boot later:

```text
[XREAD] want=2915 got=2915 cur=0 size=2915        <-- after
```

### 21.4 The second bug: Mono's `System.Native` shim was never loadable

`docs/rw-threads-design.md` §8 recorded "Mono requires /proc". This lane found
the sibling: Mono's `etc/mono/config` maps

```xml
<dllmap dll="System.Native" target="$mono_libdir/libmono-native.so" os="!windows" />
```

and this runtime expands `$mono_libdir` to `/usr/lib/../lib`, i.e.
`/usr/lib/libmono-native.so`. The image had no `/usr/lib` at all, so Mono's
corefx `Interop.Sys` type initializer threw and **every managed `File.*` call
in the process failed**. A five-line C# probe (`tools/rw/mono-probe/probe.cs`,
compiled with the dotnet SDK's csc against Mono's own mscorlib and run under
the guest's `MONO.EXE`) printed the inner exception:

```text
PROBE read UnityEngine.CoreModule.dll EX System.TypeInitializationException:
      The type initializer for 'Sys' threw an exception.
PROBE   inner[0] System.DllNotFoundException: /usr/lib/../lib/libmono-native.so
```

After staging the player's own `libmono-native.so` / `libMonoPosixHelper.so` at
`/usr/lib` (the path the dllmap names; `vfs_clean()` folds the `..`):

```text
PROBE text /WAVE.CFG len=20
PROBE read UnityEngine.CoreModule.dll bytes=1376768 mz=4d5a
PROBE asm UnityEngine.CoreModule, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null
```

Both harnesses stage it now (`run-rimworld-desktop.sh`, `run-rimworld.sh`).
Note the AD-10 `dlopen` redirect cannot fix this class of bug: Mono asks for an
*absolute path* that does not exist, and no `dlopen` happens at all — there is
no row to remap.

### 21.5 The third bug: the signal frame overwrote the 128-byte red zone

`l_sf_plant()` (rule 42) placed the frame at `sp - 1088`, i.e. `[sp-1088,
sp-16)`. The x86-64 psABI lets a leaf function keep live data at `[rsp-128,
rsp)`; Linux's `get_sigframe()` subtracts 128 for exactly this reason when the
frame goes on the normal stack. 112 bytes of the interrupted function's red
zone were overwritten on **every** signal delivery.

Measured (the run that died writing to `VA=0x2882A106F8`):

```text
91b10e: mov -0x50(%rsp),%rdi      ; a local in the red zone
91b113: add %esi,%edi             ; esi = 0xFFFFFFF3
91b115: lea (%rdi,%rdi,2),%rdi
91b119: movdqu %xmm0,(%rax,%rdi,8)   <-- VA 0x2882A106F8, in=HOLE
```

`-0x50(%rsp)` is inside the red zone and came back as `0x2FFFF1A4E`. Fix
(`src/kernel/linux_compat_thr.c`): `sp -= 128` before placing a frame unless
`SA_ONSTACK` (an altstack has no red zone). This is the mechanism behind §20's
"the death site varies": every signal the runtime took could corrupt the
interrupted function's locals, and Mono/Unity take signals during
stop-the-world.

### 21.6 After: the managed layer survives; the next wall is Mono's suspend

```text
[XREAD] want=2915 got=2915
0 rows of "Unable to find type/method [UnityEngine.CoreModule.dll]…"
[RWMAIN] dlopen FAILED …/UnityEngine.CoreModule.dll.so   (AOT probe, expected)
… one such pair per assembly in the list …
pthread_kill failed at suspend
[KERNEL] Process 4: /NFS/RimWorldLinux exited (signal 6).
```

Unity now parses its assembly list and walks **all 102 assemblies** (the
`*.dll.so` AOT probes are the proof the list was read). The player then aborts
inside Mono's stop-the-world: `pthread_kill failed at suspend` (3 of 5 runs) or
a wild fault whose address is the address-space pointer plus 8
(`VA=0x75304098`; `as=0x75304090` in the futex rows) in the same suspend path
(2 of 5). That is a threading/signal-delivery frontier, not data corruption.

### 21.7 Determinism table (5 runs, this build)

| run | last Player.log line | fault | alive PCBs |
|---|---|---|---|
| 1 | `pthread_kill failed at suspend` | none — SIGABRT | 12 |
| 2 | `pthread_kill failed at suspend` | none — SIGABRT | 42 |
| 3 | `[RWMAIN] signal() sig=11 handler=(nil)` | VA=0x118, PC=UnityPlayer+0x1988f52 (`movzbl 0x1(%rax)`, a NULL table from `-0x8(%rsp)`) | 45 |
| 4 | `pthread_kill failed at suspend` | none — SIGABRT | 39 |
| 5 | `[RWMAIN] signal() sig=11 handler=(nil)` | VA=0x75304098 (`as`+8), PC=UnityPlayer+0x19862ef (`xor 0x8(%r15)`), every register garbage | 16 |

The pre-fix ladder, for comparison (§20 plus this lane's first run):

| source | last Player.log line | fault |
|---|---|---|
| §20, four instrumented runs | always a `Unable to find … [UnityEngine.CoreModule.dll]…` row (352 of them, ending at the `VideoCapture` block) | the four runs produced **two** VA=0x38 `libmonobdwgc+0x17eb92` (`mono_class_get_image`), **one** VA=0x6 `UnityPlayer+0x198b548`, **one** wild futex write VA=0x2882A10B78 |
| this lane, first run | `Unable to find method InvokeOnVideoModeStoppedDelegate … VideoCapture` | VA=0x38, `libmonobdwgc+0x17eb92` (`mono_class_get_image`) |

A stable last line with a moving fault is normally the signature of corrupted
state. Here it was a deterministic missing input (the assembly list) whose NULL
consequences surfaced at different call sites — the table alone could not tell
those apart; the `[XREAD]` count did.

No frame is claimed: `XPutImage` is resolved
(`[RWMAIN] dlsym ok XPutImage`) but no content-flush row appears, and
`99_final.png` is the maximized empty window on the desktop, not a game frame.

### 21.8 Instruments and tests added

- `[XSTAT]`/`[XOPEN]`/`[XREAD]` (`fs.c`, `linux_compat_fd.c`): one row each for
  the engine's `ScriptingAssemblies.json` stat/open/read. Bounded to that
  filename; it is what turned "the managed layer is corrupt" into "got=1024".
- `src/user/rwsha.c` + `tools/rw/run-guest-sha.sh`: per-file SHA-256 of a
  directory read through the guest VFS, the mmap hash, a seek pass, a COW
  write test and a `/proc/self/maps` dump.
- `tools/rw/mono-probe/probe.cs` + `tools/rw/run-mono-probe.sh`: a C# probe
  under the guest's own Mono that prints the *inner* exception of a managed
  `File.*` call and then loads an assembly and enumerates its types.
- `tools/rw/rw-runs-table.sh`: N archived desktop runs + the table above.
- `tools/rw/rebuild-launcher.sh`: launcher-only rebuild for `RW_SKIP_BUILD=1`.
