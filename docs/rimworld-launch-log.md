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

### 21.9 Tests

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh    Tests run: 410   Tests failed: 0
bash tools/gate/m2_mono.sh                PASS
python3 tools/gate/m3_window_probe.py     PASS (M3_EXIT=0)
python3 tools/gate/m4_gl_probe.py         PASS (M4_EXIT=0, guest GLDESK.PPM 365013 bytes)
```

New regression row in the M5 NFS gate (`tools/rw/run-rwnfs-probe.sh`,
`oneshot_nfs()` in `src/user/rwnfs_probe.c`):

```text
[RW] ONESHOT want=2915 got=2915
[RW] PASS one read() of a 2915-byte NFS file returns 2915 bytes
```

Before the `nfs_read_file()` fix the same request returned 1024 bytes (the
engine's own `[XREAD] want=2915 got=1024` is the measured before). The gate's
only remaining failure is the pre-existing, by-design `mmap` of an NFS file
(`-ENOTSUP`, errno 95) — that is exactly why `/RWMANAGED` exists.

## 22. 2026-11-04 — M6/L32: Mono's suspend was `-EINVAL`, not `-ESRCH`; and a compat thread never took its own signals

§21 handed this lane a discriminator: Mono's stop-the-world dies with
`pthread_kill failed at suspend`, so is the target **gone** (Mono's list is
stale, our `-ESRCH` is right) or **alive** (our `-ESRCH` is the bug)? Neither.
It was `-EINVAL`, for a *live parked thread*, because the compat kill path
applied the **native** signal set before it looked at the target's ABI.

### 22.1 What the compat path actually answered (measured, not inferred)

The first instrumented boot answered the question in one row:

```text
[TGTK] op=tgkill tgid=4 tid=6 sig=30 st=8 th=1 ptgid=4 gen=1 fut=0x10842E14B8 rc=-22 pc=0x1082D0DF68
```

- `tid=6` is a real member of group 4 (`th=1`, `ptgid=4`), state 8
  (`PROC_STATE_FUTEX`) parked on a futex, `gen=1` — the PCB had never been
  recycled, so "Mono's list is stale" and "the tid was reused" are both out.
- `sig=30` is **SIGPWR**.
- `rc=-22` is **EINVAL** — glibc's `pthread_kill` returns it as errno 22.

Then the abort path, verbatim:

```text
[SIGIN] pid=4 sig=6 st=3 h=0x1083E3ECBF abi=1 kn=1 thr=0     <- glibc raise(SIGABRT)
[TGTK] op=tgkill tgid=4 tid=4 sig=6 st=3 th=0 ptgid=4 gen=0 fut=0x0 rc=0
[SIGIN] pid=4 sig=6 st=3 h=0x0 abi=1 kn=1 thr=0              <- SIG_DFL: group kill
[KERNEL] Process 4: /NFS/RimWorldLinux exited (signal 6).
```

and Player.log's last line was `pthread_kill failed at suspend`.

### 22.2 Why SIGPWR, and why EINVAL is fatal (static proof from the DSO)

`libmonobdwgc-2.0.so` names its own numbers. In `GC_stop_init`:

```text
27cf5c: movl $0x1e,(%r15)     ; GC_sig_suspend    = 30 = SIGPWR   (GC_sig_suspend was -1)
27cf77: movl $0x18,(%r14)     ; GC_sig_thr_restart = 24 = SIGXCPU
27cfca/27cfde: sigaction(GC_sig_suspend, {GC_suspend_handler, SA_SIGINFO|SA_RESTART})
```

and `GC_suspend_all`'s loop is a three-way branch:

```text
27cc97: call   pthread_kill@plt
27cc9c: cmp    $0x3,%eax      ; ESRCH
27cc9f: je     27ccc2         ;   -> "thread is gone", skip it
27cca1: test   %eax,%eax
27cca3: jne    27cd05         ;   -> ANY other errno is fatal
27cd11: ...    "pthread_kill failed at suspend: errcode= %d"
27cd2f: call   *GC_on_abort
27cd31: call   abort@plt
```

So Boehm tolerates **only** 0 and ESRCH. Our `-EINVAL` was fatal by
construction. And `-EINVAL` came from `process_signal()`'s first gate:

```c
int rt = (sig >= 32 && sig < HO_SIG_MAX);
if (!sig_known(sig) && !rt) return -EINVAL;   /* BEFORE the target is resolved */
```

`sig_known()` is the **native** frozen set (1, 2, 4, 6-13, 15, 17). SIGPWR(30)
and SIGXCPU(24) are not in it, and no ABI check had happened yet, so a
Linux-ABI target got EINVAL for signals a real Linux process receives every
day — SIGQUIT(3), SIGTRAP(5), SIGALRM(14), SIGXCPU(24), SIGPWR(30),
SIGSYS(31). This is the same class of bug as §16's PI-futex probe: a *native*
capability check answering for the compat class.

### 22.3 The second bug: an accepted signal was never delivered to a thread

Fixing the range alone changed `rc=-22` to `rc=0` — and the game then survived
the suspend *without ever running the handler*: zero `[SIGD]` rows. The
recorded bit sat on the thread PCB because `process_signal_trap_exit()` was

```c
if (!cur || cur->is_kernel_process || cur->is_thread) return;   /* leader only */
```

`is_thread` is correct for the native engine (its frame is built on the
leader's stack) and wrong for Linux, where `tgkill(tid)` runs the handler on
**that** thread. Mono suspends threads one at a time; the signal must land on
the one named. The hook now passes the group anchor (handlers/restorer live
there) and lets a Linux-ABI thread take the frame on its own stack:

```c
struct process *grp = process_group(cur);
if (cur->is_thread && !(grp->abi_flags & PROC_FLAG_LINUX_ABI)) return;
if (!cur->sig_pending && (cur == grp || !grp->sig_pending)) return;
process_signal_deliver(grp, tf);      /* frame on cur, disposition on grp */
```

### 22.4 After: the stop-the-world completes, and the domain resets

Same target, same park, now the full handshake — suspend, ack, resume, return:

```text
[SIGIN] pid=6 sig=30 st=8 h=0x1083FA66E7 abi=1 kn=0 thr=1      <- SIGPWR, thread 6 parked
[TGTK] op=tgkill tgid=4 tid=6 sig=30 st=3 th=1 ptgid=4 gen=1 fut=0x0 rc=0
[SIGP]  pid=6 sig=30 fsp=0x1084D04BD8 base=0x1084D04788 top=0x1084D04BC8 alt=0
[SIGD]  pid=6 sig=30 h=0x1083FA66E7 ipc=0x108010271B           <- handler on THAT thread
[SIGIN] pid=6 sig=24 st=3 h=0x1083FA6955 abi=1 kn=0 thr=1      <- SIGXCPU = resume
[TGTK] op=tgkill tgid=4 tid=6 sig=24 st=3 th=1 ptgid=4 gen=1 fut=0x0 rc=0
[SIGD]  pid=6 sig=24 h=0x1083FA6955 ipc=0x108010271B
[RT]    pid=6 rip=0x108010271B rsp=0x1084D04C58                <- handler returned
```

`kn=0` is the row that matters: SIGPWR is *not* in the native set and is
accepted anyway because the target is `abi=1`. Seven SIGPWR suspends and six
SIGXCPU resumes later the managed layer finishes:

```text
- Loaded All Assemblies, in  0.000 seconds
- Finished resetting the current domain, in  0.000 seconds
[RWMAIN] dlopen remap …/Plugins/lib_burst_generated.so -> /RWDSO/lib_burst_generated.so
```

**No frame yet**, but the process no longer dies in the managed layer: in 2 of
3 runs it is still alive at the harness's 180 s deadline, parked in a futex
with every thread idle (§22.7). The one abort run of §21's table is gone.

### 22.5 Linux semantics for the numbers involved

| call | Linux | before | after |
|---|---|---|---|
| `tgkill(tid, SIGPWR)` to a live parked thread | 0 | **-EINVAL** | 0 |
| `tgkill(tid, SIGXCPU)` (Boehm resume) | 0 | -EINVAL | 0 |
| `tgkill(dead tid)` | -ESRCH | -ESRCH | -ESRCH (unchanged: Boehm skips it) |
| `kill(native, SIGQUIT)` | 0 | -EINVAL | -EINVAL (native frozen set kept) |
| `kill(pid, 0)` | existence probe | probe | probe |
| `kill(pid, 64)` (SIGRTMAX) | 0 | -EINVAL | -EINVAL (`HO_SIG_MAX` is 64; stated gap) |

The fix is *not* "accept everything": out-of-range numbers still answer
-EINVAL for every target, and native targets keep `sig_known()` exactly as D6
froze it (the unit test asserts both halves).

### 22.6 Four runs at this tip

| run | suspend/resume plants | managed layer | outcome |
|---|---|---|---|
| 1 | 7 SIGPWR + 6 SIGXCPU, 12 `[RT]` | `Loaded All Assemblies` | alive at the 180 s deadline, idle (§22.7) |
| 2 | 7 SIGPWR + 6 SIGXCPU, 12 `[RT]` | `Loaded All Assemblies` | alive at the 180 s deadline, idle |
| 3 | **none** — no `[TGTK]` row at all | 2 dll probes | fault `UnityPlayer+0x91b119`, `VA=0x2882A106F8` |
| 4 (post-gate) | **none** — no `[TGTK]` row at all | 2 dll probes | fault `UnityPlayer+0x91b119`, `VA=0x2882A106F8` |

**Every run that reaches the stop-the-world passes it (3 of 3); the two that
fail never send a signal at all.** Runs 3 and 4 are the *other* pre-existing
class §21 measured in 2/5 runs (a wild write in UnityPlayer's static
initializer with a garbage red-zone local). This lane did not fix it and does
not claim it: no signal was delivered before the fault, and the only frame in
those runs is the fault's own —
`[SIGP] pid=4 sig=11 fsp=0x17FFFFB2F8 base=0x17FFFFAEA8 top=0x17FFFFB2E8`,
whose top is **0x40 below** the clobbered slot at `0x17FFFFB328`. Signal-frame
overflow is excluded by measurement, not by argument.

### 22.7 Next blocker: the *resume* half of the handshake

The last rows of runs 1 and 2 are the same shape:

```text
[SIGD] pid=6 sig=30 h=0x1083FA66E7 ipc=0x108010271B     <- 8th suspend, no [RT] after it
[IDLESTUCK] cpu=1 rounds=20005 t=132124
[IDLESTUCK] slot=4 pid=4 st=8 wake=100000000 futex=0x10842e8728 rip=0x1082d0f678 /NFS/RimWorldLinux
[IDLESTUCK] slot=6 pid=6 st=3 wake=0 futex=0x0 rip=0x108004348a Finalizer
```

Thirteen plants, twelve returns: the last suspended worker is sitting inside
Boehm's suspend handler waiting for the SIGXCPU that never comes, while the
collector (pid 4, the main thread) parks in `futex(0x10842E8728)` with a
100,000,000 ms timeout. So the wall is no longer "the signal is refused" or
"the signal is not delivered" — it is the **ack/resume handshake** after the
world stops. That is the next lane's subject, and the rows above are its
starting point.

Two side observations, recorded so they are not mistaken for the blocker:

- `submit_io_cmd: STILL SPINNING! io_cq[0].cid=65535 io_cq[1].cid=65535
  io_cq_head=1 CSTS=0x1` (`src/kernel/virtio_blk.c:176`) appears 7 times per
  run — but the game keeps running through every one (a `[SIGIN]` row lands
  after them, and the run reaches the 180 s deadline), so this is a
  **completion-latency** signal on the NVMe poll, not the wedge.
- `mmap` of `lib_burst_generated.so` is not attempted: it is `dlopen`ed from
  `/RWDSO`, the last managed-side action before the idle.

### 22.8 Instruments and tests

Rate-limited kernel rows (all in the compat signal path, all off in a normal
boot because they only fire on compat tgkill/delivery):

- `[TGTK]` (`linux_compat_proc.c`): op, tgid, tid, sig, target state,
  `th`/`ptgid`, **PCB generation** (`process_slot_seq`), futex word, rc, and
  the caller's PC. This is the row that answered the lane's question.
- `[SIGIN]` (`process.c`): every resolved signal with the target's state,
  stored handler, ABI and `sig_known()` verdict; `[SIGIN] miss` prints the raw
  slot state for an ESRCH so "gone" and "recycled" are distinguishable.
- `[SIGD]`/`[SIGP]`/`[SIGB]`/`[RT]` (`linux_compat_thr.c`): frame planted
  (handler + interrupted PC), frame extents vs the red zone, "pending but
  masked", and the return half (`rt_sigreturn` rip/rsp).

Tests:

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh    Tests run: 411   Tests failed: 0
```

`test_p5_compat_signal_range` (new): SIGPWR/SIGXCPU/SIGQUIT are -EINVAL for a
native target and accepted for a Linux-ABI one, rt numbers stay accepted, and
`0`/`HO_SIG_MAX` stay -EINVAL.

Gate set on this tip: `m2_mono` PASS, `m2_threads` PASS, `m12` PASS,
`m3_window_probe` PASS, `m4_gl_probe` PASS (see §22.9 for the exact lines).

Evidence: `obj/rw-desktop/evidence-l32/{before-abort,after-1,after-2,after-3}.serial.log`
with the matching `Player.log`s.

### 22.9 Gates, and the post-gate boot

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 411   Tests failed: 0
bash tools/gate/m2_mono.sh               m2m: PASS — Mono 6.12 ran a threaded C# program under compat
bash tools/gate/m2_threads.sh            m2: PASS — 8 pthreads, mutex+condvar+TLS+join+dlopen TLS
bash tools/gate/m12_thread_ceiling.sh    m12: PASS — 150 threads + per-thread TLS + SIGCANCEL(32)
python3 tools/gate/m3_window_probe.py    m3w: PASS — full SDL XCreateWindow sequence + WM_DELETE_WINDOW
python3 tools/gate/m4_gl_probe.py        m4: PASS — GL 3.3 core triangle rendered and PRESENTED
```

Run 4 is the post-gate boot required by the "never trust a gate's disk.img"
rule: kernel and disk were rebuilt **in order** (kernel first, then
`disk.img`, then the stripped kernel copied to `::/boot/hobbyos.elf` and
verified to contain the new `[SIGIN]` row) before the harness ran with
`RW_SKIP_BUILD=1`. It hit the §22.6 wild-write class, not a suspend failure,
which is the honest distribution at this tip: the suspend wall is gone, and
what remains is one unexplained pre-GC corruption class plus the resume
handshake behind it.

Evidence: `obj/rw-desktop/evidence-l32/{before-abort,after-1,after-2,after-3,after-4-postgates}.serial.log`
with the matching `Player.log`s.

## 23. 2026-11-04 — M6/L33: a compat thread's park now takes its signal, and the game runs its own main

§22 ended with the resume half of Boehm's handshake open: the 8th `SIGPWR` was
planted with no `[RT]` after it, and the collector parked in
`GC_suspend_ack_sem` for ever. The hypothesis handed to this lane — "a compat
thread blocked in `futex_wait` does not take a pending signal" — was
**partly right and partly backwards**, and the two pre-existing bugs found
underneath it mattered more than the hypothesis.

### 23.1 What the rows said (measured, three runs)

Before any fix, the shape was:

```text
[SIGIN] pid=6 sig=30 st=8 …            <- suspend aimed at a PARKED thread
[TGTK]  … tid=6 sig=30 st=2 … rc=0     <- our interrupt hook DID wake it
[SIGD]  pid=6 sig=30 …                 <- …and the handler was planted…
… [RT] pid=6 …                         <- …but only after the handler that
                                          was ALREADY running returned
```

and the two decisive rows were not about delivery at all:

```text
[IDLESTUCK] slot=6 st=3 rip=0x108004348a Finalizer
[SIGD] pid=6 sig=30 → [LCOMPAT] ENOSYS nr=130 name=Finalizer rip=0x108004348A
```

`nr=130` is **rt_sigsuspend**. Boehm's suspend handler is statically

```text
27c7b1: sem_post(&GC_suspend_ack_sem)      ; the ack
27c845: sigsuspend(&{restart signal only}) ; park until SIGXCPU, loop on
                                             GC_world_is_stopped / stop_count
```

(disassembled in `libmonobdwgc-2.0.so`, `GC_suspend_handler_inner`), so the
"stopped" worker was spinning in an **ENOSYS loop** in user space. The world
was never really stopped, every suspend acked at a timing-dependent point, and
one late `SIGPWR` (queued while the previous handler was still spinning) was
delivered only after that handler returned — which is exactly the `[SIGD]` with
no `[RT]`.

Two more rows explain why a woken thread did not run its handler before the
syscall's return became visible:

- `process_signal_resume_deliver()` skipped **every** `is_thread` PCB, so a
  worker woken from `futex_wait` with `-EINTR` returned to glibc *without* the
  frame, and glibc re-entered `futex_wait` before any trap exit could deliver.
- `l_sf_plant()` set `tf->lr` but never `arch_set_user_sp()`, and the resume
  path (`enter_user_space`) takes the user RSP from `gs:[24]`, not from
  `tf->lr`. A frame planted at a resume boundary therefore entered the handler
  on the interrupted function's stack; its `ret` popped a local instead of
  pretcode and the thread died at `PC=0x8` (measured by the new probe, case A).

### 23.2 The probe: host reference vs guest, one table

`src/user/linux_probe_futex_sig.c` (musl static-PIE, ordinary Linux program;
`tools/gate/t3_linux_probe.sh futex_sig`). It proves the ordering Linux
guarantees — handler first, *then* the syscall's `-EINTR` is visible — and the
discriminating SA_RESTART pair.

| case | what it proves | host (reference) | guest before | guest after |
|---|---|---|---|---|
| A | `FUTEX_WAIT` (infinite), handler **without** SA_RESTART | `-1/EINTR`, handler ran first | slept forever, handler never ran | **same as host** |
| B | same wait with **SA_RESTART** | handler runs once, wait RESTARTS; explicit wake returns 0 | returned EINTR (no restart) | **same as host** |
| C | `rt_sigsuspend` | `-EINTR`, handler first, pre-suspend mask restored | `-ENOSYS` (loop) | **same as host** |
| D | `clock_nanosleep` | `EINTR`, handler first | slept / no handler | **same as host** |
| E | futex wait with a **masked** pending signal | `ETIMEDOUT`, no handler during the wait | mask not honoured in the other direction | **same as host** |
| F | `poll` on an empty pipe | `-1/EINTR`, handler first | no interrupt | **same as host** |
| G | `read` on an empty pipe | `-1/EINTR`, handler first | re-parked for ever | **same as host** |

Guest row, final: `[LP] done rc=0` (7/7). Host row: identical text, `rc=0`.

### 23.3 The fixes (all Linux semantics, none a workaround)

1. **`rt_sigsuspend`(130) exists.** Atomically install the argument mask, park
   (not spin), restore the pre-suspend mask once a signal has been dequeued
   (before the handler runs, so the handler's `uc_sigmask` is the original),
   and return `-EINTR`. `rt_sigpending`(127) also lands, because the probe's
   mask case needs it and the guest previously answered `-ENOSYS`.
2. **A compat thread takes its signal at its resume boundary.**
   `process_signal_resume_deliver()` now accepts a Linux-ABI thread (the group
   ANCHOR supplies handlers/restorer, the frame lands on the resumed thread)
   and checks both pending masks. Rewound parks (`l_park`: poll/select/read)
   still defer — their own entry check answers `-EINTR` after the handler.
3. **Block time is checked, like `signal_pending()`.** `futex_wait`,
   `nanosleep`/`clock_nanosleep`, `l_park`, `rt_sigsuspend` and the native
   pipe read/write block all return `-EINTR` when a deliverable signal is
   already pending, instead of sleeping through it.
4. **SA_RESTART restarts.** When the interrupting handler carries
   `L_SA_RESTART` (Mono's suspend and restart handlers both do), the saved PC
   is rewound over the 2-byte `syscall` and `rax` becomes the number again;
   the frame is planted with the `syscall` as the interrupted PC, so
   `rt_sigreturn` re-executes it. This is what makes the probe's A/B pair
   differ, and it is why every game row now says
   `why=futex-sa-restart`.
5. **The resume planted a frame on the wrong stack** — `l_sf_plant()` now
   calls `arch_set_user_sp(base)` (as the native engine does) and materializes
   `[base-320, base)` for the resume copy.
6. **`rt_sigprocmask` returned the POST-change mask as `oldset`.** musl's
   `__block_all_sigs(&set)` therefore captured "everything blocked" as a
   thread's saved mask, and every musl thread restored that mask in `start()`:
   `[SIGB] pid=… pend=0x200 mask=0xFFFFFFFEFFFBFEFF` on every probe thread. The
   same shape affects glibc's `pthread_sigmask`. Fixed to return the mask as
   it was before the change.
7. **`clock_gettime(CLOCK_MONOTONIC_COARSE)` was `-EINVAL`.** Mono asks for
   clock id 6 at domain reset and **aborts** on failure; ids 4/6/7 now answer
   the monotonic clock and 5 answers realtime.

### 23.4 Instrumentation added

- `[FB]` — a `futex_wait` that refused its park because a deliverable signal
  was pending (`tid key sig pend mask`). One row in run 2:
  `[FB] tid=7 key=0x10D009F100 sig=30 pend=0x20000000 mask=0x0 -> EINTR before park`.
- `[FWK]` — why a compat park ended: `why=futex`, `futex-sa-restart`,
  `sigsuspend`, `sleep`, `sleep-sa-restart` or `restart`, with `pend`/`mask`.
- `[SIGR]` — a frame planted at a resume boundary (the row that was missing in
  §22's 8th suspend).
- `[FUTEX] WAIT … pend=` — the waiter's pending mask at the instant it parked.

### 23.5 Three desktop runs at this tip

| run | SIGPWR | SIGXCPU | `[RT]` | 8th suspend | Player.log last real line | end |
|---|---|---|---|---|---|---|
| 1 | 11 | 11 | 23 | **resumed** (pid 7) | `[PhysX] Initialized MultithreadedTaskDispatcher with 2 workers.` + `UnloadTime: 2825.000000 ms` | SIGSEGV `UnityPlayer.so+0x1947cef`, `addr=0xe8`, `rdi=0` during window teardown |
| 2 | 23 | 20 | 41 | **resumed** (pid 7) | `RimWorld 1.6.4871 rev600` | alive at the 170 s deadline; main thread looping on an instruction-fetch `#PF` at `rip=cr2=0x10823B1C00` (`vec=14 err=0x14`) |
| 3 | 11 | 11 | 22 | **resumed** (pid 7) | `[PhysX] Initialized …` + `UnloadTime: 2417.000000 ms` | clean exit (no SIGDUMP, no fault) |

Every suspend/resume pair in runs 1 and 3 is matched per pid (6: 8/8, 7: 2/2,
27: 1/1). Run 2's three unresumed suspends are the last round, cut off by the
main-thread fault described below, not by the handshake. The decisive rows for
the old wall, verbatim from run 1:

```text
[FWK] tid=7 sig=30 why=futex-sa-restart key=0x10D009F100 pend=0x20000000 mask=0x0
[TGTK] op=tgkill tgid=4 tid=7 sig=30 st=2 th=1 ptgid=4 gen=1 fut=0x0 rc=0
[SIGP] pid=7 sig=30 fsp=0x1085509CF8 base=0x10855098A8 top=0x1085509CE8 alt=0
[SIGD] pid=7 sig=30 h=0x0x1083FA66E7 ipc=0x108011895B
[SIGR] pid=7 grp=4 ipc=0x1083FA66E7          <- planted at the resume boundary
[RT]   pid=7 rip=0x108011895B rsp=0x1085509D78   <- handler returned
```

**`[LCOMPAT] ENOSYS nr=130` is absent from all three logs.** The managed layer
now does real work: `- Loaded All Assemblies, in 32.584 seconds` (it was 0.000
and hollow before), `- Finished resetting the current domain`, a GL 4.5→3.3
context made current on window `0x200002`, `requesting resize 1280 x 720`,
`[PhysX] Initialized`, and in run 2 the game's own banner
`RimWorld 1.6.4871 rev600`.

**No frame yet.** The window still shows no `XPutImage` content flush.

### 23.6 Next blocker, named with its line

1. **Runs 1 and 3: the player shuts down right after the first scene is set
   up, and run 1's teardown faults in the X11 event path.** Exact rows:

   ```text
   [PhysX] Initialized MultithreadedTaskDispatcher with 2 workers.
   UnloadTime: 2825.000000 ms
   [XI] XIUngrabTouchBegin -> Success (nothing was grabbed)
   [SIGDUMP] SIGSEGV code=1 addr=0xe8
   [SIGDUMP] uctx rip=0x108244fcef(UnityPlayer.so+0x1947cef) rdi=0x0
             rsp=0x17ffffb4e0 rax=0x1082b2e0c0(UnityPlayer.so+0x20260c0)
   ```

   `0x1947cef` is inside Unity's window/X-event code (the stack chain runs
   through `libX11.so.6+0xe9dd`, `+0x9e68` and `UnityPlayer.so+0x1948b04`),
   `rdi=0` and the fault is a read at `NULL+0xe8`. `UnloadTime` precedes the
   fault, so this is a **teardown** defect, not the cause of the quit; the
   quit itself is the first thing to explain (run 3 exits cleanly from the
   same point, so it is timing-dependent).
2. **Run 2: the main thread spins on an instruction-fetch page fault.**
   `[WATCHDOG]` shows `CPU2 … vec=14 err=0x14 cr2=0x10823B1C00 pid=4` with
   `rip=0x10823B1C00` (I/D=1, P=0: executing from a non-present user page)
   while every other thread is parked in a futex. Either that page is a
   legitimate lazy mapping the fault path refuses to materialize, or the PC
   was corrupted — the fault's `err` says instruction fetch, and the RIP is a
   user address on a CPU in kernel mode.

Both are post-`domain reset` failures and neither is the suspend handshake;
this lane does not claim them.

### 23.7 Gates and regressions at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 411   Tests failed: 0
bash tools/gate/m2_mono.sh               m2m: PASS — Mono 6.12 ran a threaded C# program
bash tools/gate/m2_threads.sh            m2: PASS — 8 pthreads, mutex+condvar+TLS+join+dlopen TLS
bash tools/gate/m12_thread_ceiling.sh    m12: PASS — 150 threads + per-thread TLS + SIGCANCEL(32)
python3 tools/gate/m3_window_probe.py    m3w: PASS — full SDL XCreateWindow sequence
python3 tools/gate/m4_gl_probe.py        m4: PASS — GL 3.3 core triangle rendered and PRESENTED
bash tools/gate/t3_linux_probe.sh futex_sig   PASS — 7/7 cases, host rows identical
```

Evidence: `obj/rw-desktop/evidence-l33/run{1,2,3}.serial.log` and
`run{1,2,3}.Player.log`; `tools/rw/l33-sig-summary.py` reproduces the suspend/
resume table from any serial log.

### 23.8 What this lane did not reproduce, and does not claim

The §22.6 pre-existing class — a wild write in `UnityPlayer`'s static
initializer, `UnityPlayer+0x91b119`, `VA=0x2882A106F8`, with
`[SIGP] top=0x17FFFFB2E8` — **did not occur in these three runs** (0/3; no
fault at that PC and no `VA=0x2882A106F8` anywhere in the logs). It is still
unexplained and unchanged by this lane: the only frame in such a run would be
the fault's own, 0x40 below the clobbered slot, which excludes signal-frame
overflow by arithmetic.

What replaced it is the post-`domain reset` pair in §23.6: the first-scene
teardown `SIGSEGV` at `UnityPlayer.so+0x1947cef` (runs 1 and 3, with run 3
exiting cleanly from the same point) and run 2's instruction-fetch `#PF` loop
at `rip=cr2=0x10823B1C00`. Neither is the suspend handshake, and neither is
fixed here.

## 24. 2026-11-04 — M6/L34: every queued X event carried a NULL `Display*`, and that was the first-scene crash

§23 left two deaths. Both are now named, one is fixed with a regression that
fails on the old bytes, and the launch reaches the game's own banner without a
fault. The short-read suspicion was wrong — but the layer under it is a
throughput wall, now measured.

### 24.1 Short-read audit: no cap below the caller's request

Every read entry point was read, not grepped for a constant:

| path | where | cap | verdict |
|---|---|---|---|
| compat `read(2)` | `trap.c` `sys_read` | none (`int size` passed through) | OK |
| FAT16 file read | `fs.c`→`fat16_read` | `min(size, remaining)`, sector loop to EOF | OK |
| NFS file read | `fs.c`→`nfs_read_file` | per-RPC `read_size`=1024, **loops** to `len`/EOF/0 | OK (L31 fix intact) |
| NFS exec image | `fs_read_image` | 1024 per call, loops | OK |
| mmap page-in | `file_obj_page` | exactly `FRAME_SIZE`, tail zero-filled | OK |
| `readv` | `linux_compat_misc.c` | 0x10000000 per segment, short read legal at segment end | OK |
| `pread64` | `linux_compat_misc.c` | passes `n` | OK |
| pipe | `pipe.c` | returns available bytes (POSIX-legal short read) | OK |
| console / `/dev/null` | `fs.c` | EOF (0) | OK |
| `/proc/self/maps` | `fs.c` | 1024-byte streaming (legal) | OK |
| VFS prefix bind | `vfs_bind.c` | path translation only, no data path | OK |

End-to-end proof, from the guest, with one `read(fd, buf, size)` per file
(`src/user/rwread1.c`, `tools/rw/run-guest-read1.sh`, `[R1]` rows compared with
the host's `sha256sum`):

```text
[R1] globalgamemanagers            want=738764 got=738764 count=OK sha=OK
     246422d54d011cb5c0a2743feedde98cc80cd651776b914dd8e5bdef3c2304b5
[R1] globalgamemanagers.assets     want=860632 got=860632 count=OK sha=OK
     73223d29af351e5abda7904ce39da7335a9e5cf6e1266a0ede0b33ad8ce20166
[R1] globalgamemanagers.assets.resS want=2796240 got=2796240 count=OK sha=OK
     39dd38fcd8dd95d41d6d55fe16a448d29fe2c31d9a162c825c24c6d99a893d7a
[R1] level0                        want=1776   got=1776   count=OK sha=OK
     1b4cedcc07d5f279cc2bfca892aee56c5f59b0e4a345b8906194ce40cc8ed740
[R1] level1                        want=3496   got=3496   count=OK sha=OK
     6162acf02de9f895472c5cf4e30e13d0eba2eb06f88d13efb9ee0fab5e295dd2
```

(`fstat` and `lseek(SEEK_END)` agreed with the host size on every row; the
digests above are the guest's, computed over the bytes the single `read()`
returned, and they equal the host's `sha256sum`.)

`resources.assets` (9,902,968 bytes) did **not** return inside the 300 s
window — not a short read, a throughput wall (§24.5). No `count=SHORT` row
exists anywhere.

### 24.2 (a): `rdi=0` is `XConfigureEvent.display`; `+0xe8` is `Display::screens`

The faulting instruction, from `objdump` at `UnityPlayer.so+0x1947cef`:

```text
1947ce8: mov 0x18(%r15),%rdi        ; XEvent* + 0x18 == xany.display
1947cec: mov (%rbx),%rsi
1947cef: mov 0xe8(%rdi),%rcx        ; Display::screens  <- fault, rdi = 0
1947cf6: movslq 0xe0(%rdi),%rdx     ; Display::default_screen
1947cfd: shl $0x7,%rdx              ;   x sizeof(Screen) = 0x80
1947d01: mov 0x10(%rcx,%rdx,1),%rdx  ; screens[default_screen]->+0x10
```

The containing function's FDE is `0x19472d3..0x1948887` and its prologue is
`mov %rsi,%r15` — its second argument is the `XEvent*` (its first compare is
`(%r15)` = `type`; the key path reads `0x54(%r15)` = `XKeyEvent.keycode` and
this path reads `0x10`=send_event, `0x18`=display, `0x28`=window, `0x30/0x34`=x/y
of `XConfigureEvent`, all pinned by `src/user/xcompat/xabi.h`).

So `rdi` is the `Display*` **inside the event**, and `+0xe8` is the `screens`
slot of Xlib's public `Display` prefix, reached through the inlined
`ScreenOfDisplay(dpy, DefaultScreen(dpy))` macros. It was NULL because the
compat `libX11` never wrote `xany.display` when it queued an event — `grep`
found no writer of that field anywhere in the shim. A real Xlib stamps the
display it read the event from; a client may use `ev.xconfigure.display`
instead of the handle it passed to `XNextEvent`, and Unity does.

Fix (`libx11.c` `queue_event`, the single choke point for input and for
re-queued held events): copy the event, stamp `e.xany.display = d` for every
event that has the field (`XErrorEvent` and `KeymapNotify` excepted, same rule
`xc_queue` already applies to `serial`), then enqueue.

Regression: `src/user/xcompat/sdl_window_probe.c` now fails the M3 gate when
`ev.xconfigure.display != d`. A/B on the same gate, same probe:

```text
before (make XCOMPAT_NO_EVENT_DISPLAY=1):
  [WP] FAIL: ConfigureNotify display=(nil) expected 0x1080014000 (xany.display not stamped)
  m3w: FAIL
after:
  m3w: PASS - the shim served SDL's whole XCreateWindow sequence and delivered WM_DELETE_WINDOW
```

### 24.3 (b): `cr2=0x10823B1C00` is UnityPlayer text + `0x18a9c00` — a demand page-in, not freed JIT

`0x10823B1C00 - 0x1080b08000` (UnityPlayer's load base) = `0x18a9c00`, inside
`.text` (`0x733040..0x1ed4f51`). The new `[PFE]` row (capped at 24, in
`vm_handle_fault`) records the shape of every exec fault. A measured row from
the same runs (a different DSO: 2 MiB, base `0x1080022000`):

```text
[PFE] rip=0x10800BDF50 page=0x10800BD000 base=0x1080022000+0x178000
      prot=5 kind=7 flags=18 ooff=0x22000 obj=0x75308D00 objsz=0x1EF818 present=0
[PIN] off=0x1EA000 size=0x1EF818
[POK] off=0x1EA000 n=full
```

`prot=5` (R+X), `kind=7` (VM_OBJ_FILE), `flags=18` (MAP_PRIVATE) and
`present=0`: this is the loader's `mmap` of a DSO file, and the page is
legitimately absent. (The 24-row cap was exhausted by smaller DSOs' text pages
before UnityPlayer's own region was reached in these runs; `0x10823B1C00` sits
`0x18a9c00` into UnityPlayer's 33,010,672-byte mapping, i.e. well inside the
region the loader maps whole at `0x1080b08000`.) It is **not** a freed JIT page
(a JIT page would be an anonymous region, `obj=0`), and it is **not** a
stale-TLB retry (that path requires a *present* leaf and would print
`present=1`; the hardware's `err` says `P=0`). In runs 2/3/5 all 24
instrumented exec faults were `present=0` and all 24 fills returned `n=full`.

What turns it into a permanent loop is not the fault path but the block layer:
L33's run 2 log at that moment is a flood of
`submit_io_cmd: STILL SPINNING! io_cq[0].cid=65535 ...` — the NVMe poll never
sees its completion, so `file_obj_page` never returns and the same fault frame
is reported by every watchdog tick.

L34 changes the failure mode: `submit_io_cmd` now fails the request after 100
spin windows (tens of seconds) with a `BLKSTALL` row and an error status, so a
wedged device becomes a named `in=PROT` kill instead of an unbounded kernel
loop. The stall itself is not reproduced in L34's runs (0 `STILL SPINNING`
rows in six runs); the wedge mechanism is still open and is *not* claimed.

### 24.4 Six desktop runs at this tip

| run | display stamp | duration | SIGSEGV | Player.log last real line | present |
|---|---|---|---|---|---|
| 1 | no (baseline) | 200 s | none | `Finished resetting the current domain` | no |
| 2 | yes | 220 s | none | `RimWorld 1.6.4871 rev600` | no |
| 3 | yes | 400 s | none | `RimWorld 1.6.4871 rev600` | no |
| 4 | yes | ~8 s | `UnityPlayer+0x91b119`, `rdi=0x2ffff1a41` | — | no |
| 5 | yes | 600 s | none | `RimWorld 1.6.4871 rev600` | no |
| 6 | yes | 700 s | none | `RimWorld 1.6.4871 rev600` | no |

Runs 2/3/5/6 all write `UnloadTime:` (2.0-2.2 s) and then the game's own
banner; `[SIGDUMP]` at `UnityPlayer+0x1947cef` does not occur again (0/4).
Run 4 is the §22.6 pre-existing wild-write class (`UnityPlayer+0x91b119`,
`rdi=0x2ffff1a41`), unchanged by this lane and still unexplained.

**No frame.** `[XE] PUT` (the always-on `XPutImage` row) has zero rows in all
six runs: the client never presented a single image, so the window stays
black. One `[XE] FLUSH 0;0;1916;1018` row exists, which is the pre-paint
`XFlush` of an unconfigured full-size window, not a content flush.

### 24.5 Next blocker, measured: the game is starved by 1 KiB NFS RPCs at ~42 KB/s

`[NFSIO]` (bytes/rpcs/uptime, printed every 30 s) during run 6:

```text
ms=43,557  bytes=432,263   rpcs=426    (mount + first reads)
ms=103,557 bytes=4,025,387 rpcs=3,936
ms=163,557 bytes=4,991,136 rpcs=4,882
ms=193,557 bytes=6,290,592 rpcs=6,151  -> ~42 KB/s steady, 23-31 ms per RPC
```

`NFS_READ_CHUNK` is 1024 bytes (one UDP datagram), and each RPC costs ~25 ms:
a socket is created and connected per RPC (`rpc_call`), and
`virtio_net_poll_rx()` throttles its idle `QUEUE_NOTIFY` to 100 Hz. 6.3 MB in
193 s means RimWorld's def/scene load (tens of MB of XML and `.assets`) cannot
finish inside any harness deadline. That is the line to attack next: reuse one
UDP socket for READ RPCs, and/or raise `read_size` toward the 1472-byte
datagram limit, before any more fault-path work.

### 24.6 Gates at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   Tests run: 411   Tests failed: 0
python3 tools/gate/m3_window_probe.py    PASS (with the new display check)
python3 tools/gate/m3_window_probe.py    FAIL on the no-stamp DSO (A/B, above)
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 triangle rendered and PRESENTED
bash tools/gate/m2_threads.sh            PASS — 8 pthreads, mutex+condvar+TLS+join
bash tools/gate/m12_thread_ceiling.sh    PASS — 150 threads + TLS + SIGCANCEL(32)
bash tools/gate/m2_mono.sh               PASS — Mono 6.12 threaded C# program
bash tools/rw/check-no-game-files.sh     OK
```

Evidence: `obj/rw-desktop/evidence-l34/run{1,2,3,5}.{serial,Player}.log`,
`obj/rw-desktop/read1.log` + `read1.out` (the `[R1]` rows),
`obj/rw-desktop/m3-nostamp.log` and `m3-stamp.log` (the A/B),
`obj/rw-desktop/m4-l34.log`, `g-m2thr.log`, `g-m12.log`, `g-m2mono.log`,
`unit-l34.log`.

## 25. 2026-11-04 — M6/L35: the load was four multipliers; NFS now runs at 38.8 MB/s

**Verdict: the NFS wall is gone.** A guest `read()` now moves 38.8 MB/s
(9,902,968 bytes in 255 ms) and 40.0 MB/s sustained over 64 MiB, against
1023.9 B/RPC at depth 1 and ~42 KB/s in §24. The game uses the new speed: it
reaches `Initialize engine version` and `RimWorld 1.6.4871 rev600` in ~60 s
of uptime, runs its own managed `Steamworks.NET` initialisation, and only
then dies — not on I/O, on Mono's own trampoline check. Still **no frame**;
the next blocker is named in §25.5.

### 25.1 What one READ RPC actually spends its time on

`[NFSRTT]` (new, `src/kernel/nfs.c`) buckets every READ RPC by the byte count
it asked the server for and splits its life into `sock` (socket create +
connect), `send`, and `wait` (`net_socket_recv_timeout`). Same bench, same
guest, the pre-L35 transport (A/B switch `nfs_perf_legacy=1`):

| want | n | sock_us | send_us | wait_us | wait_min | wait_max |
|---|---|---|---|---|---|---|
| 1024 | 1,024 | **0** | **4** | **6,335** | 23 | 21,917 |
| 1024 | 4,097 | 0 | 4 | 9,217 | 23 | 54,198 |

That table eliminates three of the five suspects and convicts a fourth:

| suspect | verdict |
|---|---|
| rsize pinned at 1024 | **confirmed** — `NFS_READ_CHUNK`, 1023.9 B/RPC measured |
| strictly serial (depth 1) | **confirmed by construction** — `rpc_call` created, connected, sent, received and closed one socket per RPC, and `nfs_read_file` looped over it |
| one packet per poll | **not the bug** — `virtio_net_handle_irq`/`poll_rx` both drain the used ring in a `while` loop |
| per-RPC setup (connect/handshake) | **eliminated** — `sock_us=0`, `send_us=4`; a UDP "connect" only records the peer |
| SunRPC retransmit backoff | **eliminated** — `wait_max` is 22-54 ms, far under the 500 ms timeout, and no retry row appears |
| **a wakeup that costs a timer tick** | **convicted** — `wait_min=23 us` proves the server+slirp round trip is *microseconds*; the mean is 6.3-9.2 ms and the distribution is the signature of "the reply arrived after the sleeper stopped looking, and the next wake is the 100 Hz PIT" |

So the arithmetic was two independent multipliers, and both were real:

```text
before: 1023.9 B/RPC x depth 1  / 6.3 ms  =  162 KB/s   (measured 161.1 KB/s, 1 MiB)
after : 1337   B/RPC x depth 16 / 0.26 ms =   82 MB/s   (ceiling; measured 38.8-40.0)

ceiling table (bytes in flight / latency), before -> after
  1 KiB  x 1  @ 6.3 ms  = 0.16 MB/s   <- where the launch was
  1 KiB  x 8  @ 6.3 ms  = 1.3  MB/s
  32 KiB x 1  @ 0.26 ms = 123  MB/s
  20 KiB x 16 @ 0.26 ms = 82   MB/s   <- implemented (1344 B x 16)
```

The residual gap between 82 and 39 MB/s is the guest+slirp *service rate*:
`[NFSPIPE]` shows 7,405 RPCs in 255 ms = ~29,000 RPC/s, and
`1,337 B x 29,000/s = 38.8 MB/s`. Depth is no longer the limit; the number of
datagrams per second is. 729 MiB therefore takes **~18 s**, not 4.9 h.

### 25.2 The four fixes, each measured

1. **rsize 1024 -> 1344.** The transport is UDP over a 1500-byte MTU and this
   stack neither emits nor reassembles IP fragments, so a reply must fit one
   datagram: 1472 (payload) - 128 (RPC header 24 + status 4 + post_op_attr 88
   + count 4 + eof 4 + opaque length 4) = **1344 bytes**. Larger is not
   "unsupported by negotiation", it is *impossible without fragment
   reassembly* — the client also adapts down to a server that answers short
   (two equal shorts in a row, never below 512).
2. **Depth 1 -> 16.** `nfs_read_pipelined()` issues up to `NFS_READ_DEPTH`
   READs on one socket before reaping, matches replies by xid (out-of-order
   safe, duplicate/stale datagrams skipped), fills each block at its own file
   offset, and falls back to serial reads from the last contiguous byte if a
   block is short, dropped or times out. `[NFSPIPE]` proves the shape:
   `len=65536 rs=1344 issued=49 replies=49 short=0 bad=0 got=65536`.
3. **The wait stopped sleeping.** `net_socket_recv_timeout()` now drains the
   virtio RX used ring itself (`net_rx_pump()` -> `virtio_net_poll_rx`) and
   spins with `cpu_relax()` instead of `sti; hlt`. Two rounds of measurement
   were needed: a 200 us poll window fixed only the fastest replies (mean
   still 6.3 ms, pipeline minimum 6.5 ms), a 2 ms window fixed the idle guest
   but not a loaded one, and only "never sleep on an outstanding RPC" removed
   the 100 Hz tick from the data path. `wait_us` for a 1 KiB RPC: **6,335 ->
   79 us**; `wait_min` 23 -> 33 us, `wait_max` 21,917 -> 3,133 us.
4. **One socket per mount, not one per RPC.** This was invisible until the
   other three were fixed and the run got long: the first full-file run did
   9.9 MB at 10.8 MB/s and then *decayed* — 40 ms/RPC after ~20k RPCs, and a
   single 64 KiB read taking 1.25 s. The cause is per-ephemeral-port state on
   the far side of slirp: every RPC burned a new guest UDP port
   (`net_socket_create` hands out 49,152..65,535). One cached socket per
   mount (`struct nfs_mount.rd_sock`, CAS-guarded `rd_busy` so a second
   thread gets a temporary socket instead of interleaving xids) removed the
   decay entirely: 10.8 -> 38.8 MB/s, and the 64 MiB span now runs at the
   same rate as the first megabyte.

UDP `recv()` also had to become datagram-framed: `handle_udp` pushes one
length record per datagram and `recv`/`recv_timeout` return at most one, or
a pipelined client reads reply k's tail spliced onto reply k+1's head and
fails to parse either.

### 25.3 `src/user/rwnfs_bench.c` — the numbers

In-guest, `read()` only, against the live export (`tools/rw/run-nfs-bench.sh`;
each row prints bytes, ms, the kernel's READ-RPC delta and B/RPC so it can be
recomputed). Host reference for the same file over loopback NFS: **3.3 GB/s**,
i.e. the server was never the constraint.

| phase | bytes | ms | rpcs | B/RPC | MB/s |
|---|---|---|---|---|---|
| open1024 (1 read) | 1,024 | 3 | 1 | 1024 | — (3 ms) |
| sweep1024 | 1,048,576 | **89** | 1,024 | 1024 | **11.7** |
| sweep4096 | 1,048,576 | 65 | 1,024 | 1024 | 16.1 |
| sweep16384 | 1,048,576 | 46 | 832 | 1260 | 22.7 |
| sweep65536 | 1,048,576 | 42 | 784 | 1337 | 24.9 |
| **file64k (resources.assets)** | **9,902,968** | **255** | 7,405 | 1337 | **38.8** |
| file1k (same file) | 9,902,968 | 650 | 9,671 | 1023 | 15.2 |
| **span64m** | **67,151,056** | **1,678** | 50,212 | 1337 | **40.0** |
| player64k (UnityPlayer.so) | 33,010,672 | 959 | 24,682 | 1337 | 34.4 |

Before, same bench, same guest, 1 MiB per phase: `seq1024` **6,507 ms**
(161.1 KB/s), `seq4096` 10,298 ms (101.8 KB/s), `seq16384` 10,395 ms,
`seq65536` 10,736 ms — and over the full 9.9 MB, 107,919 ms (91.7 KB/s).
The 9.9 MB target (< 2 s, >= 5 MB/s) lands at **0.255 s / 38.8 MB/s**.

The MB/s figures above are host-load bound: the box was carrying a load
average of 12-16 from other lanes, and a later 2 MiB re-run at a quieter
moment measured the same 64 KiB path at **55.1 MB/s** (`file64k 2,097,152 B
in 38 ms`, `[NFSRTT] wait_us=51`). Treat 38.8 MB/s as the conservative
number and ~55 MB/s as what the transport does when the host is idle. The
latency column is the honest one either way: `wait_us` 6,335 -> 51-79 us for
the same 1 KiB RPC.

### 25.4 Byte-exactness

`tools/rw/run-rwnfs-probe.sh`: the one-shot 64 KiB read of `UnityPlayer.so`
and its FNV-1a/sum32 still match the host, the 2,915-byte single-`read()`
regression (§21, rule 49) still returns 2,915 — now through the pipeline
(`[NFSPIPE] len=2915 rs=1344 issued=3 replies=3 short=0 bad=0 got=2915
us=246`) — the 102-entry `Managed` listing and the 63-character LOOKUP still
pass. The one FAIL row is the **pre-existing by-design**
`mmap(/NFS/...) -> -1 errno=95`: `vm_mmap` refuses any fd for which
`file_fat16_ident()` fails (its comment names "pipe/socket/NFS/dir"), which is
the §21/rule-14 limitation the `Managed` prefix bind exists to work around —
`src/kernel/vm.c` is untouched by this lane.

### 25.5 The ladder at this tip: NFS is no longer the wall

Three desktop runs (`tools/rw/run-rimworld-desktop.sh --timeout 420
--run-timeout 600`), host load average 12-16:

| run | reached | NFS bytes read | death | present |
|---|---|---|---|---|
| 1 | **`RimWorld 1.6.4871 rev600`** + managed `Steamworks.NET` init | 4,712,187 (3,605 RPCs) | `tramp-amd64.c:641` assertion -> SIGABRT -> `VA=0xE` in=HOLE | no |
| 2 | GLX/XI window setup, pre-banner | 0 | SIGSEGV `UnityPlayer+0x19862ef`, garbage regs (`r15=0x75304090`) | no |
| 3 | pre-banner | 0 | SIGSEGV `PC=0x1081423119 rdi=0x2FFFF1A41 rw=w` — §22.6/rule 51 class | no |

Run 1 is the one that matters for this lane: the launch now *loads*. Its
`[NFSIO]` rows:

```text
[NFSIO] bytes=0x15887   rpcs=0x49   uptime_ms=0x7530    (30,000 ms)
[NFSIO] bytes=0x47EAFB  rpcs=0xE15  uptime_ms=0xEA68    (60,008 ms)
Initialize engine version: 2022.3.35f1 (011206c7a712)
RimWorld 1.6.4871 rev600
* Assertion: should not be reached at tramp-amd64.c:641
```

4.62 MB in that 30 s window and the banner at 60 s of uptime, against §24's
193.5 s to read 6.29 MB. Player.log's last real lines before the assertion are
the game's own managed Steamworks init (`[Steamworks.NET] SteamAPI.Init()
failed ...` — non-fatal, the game logs it and continues), so managed game code
is running.

Runs 2 and 3 die *before* the banner in the pre-existing garbage-pointer class
(§22.6, rule 51: `rdi=0x2FFFF1A41`, garbage `rbx/rbp/rsi`), which §24 saw in
1 of 6 runs and this sample sees in 2 of 3 — a bad draw, not an NFS
regression: neither run issued an NFS READ at all. Treat "does it load?" as
**1 of 3**, and expect the next lane to fix the wild-write class before the
ladder is stable.

**No frame in any run.** `[XE] PUT` (XPutImage) has **0** rows in all three;
the only `[XE]` row is the pre-paint `[XE] FLUSH 0;0;1916;1018` from §24.5,
and the screenshots show the launcher's stdout window, not game content.

**Next blocker, exact line: `* Assertion: should not be reached at
tramp-amd64.c:641`** — Mono's `stack_unaligned()` trampoline guard, i.e. a
Mono trampoline was entered with `%rsp` not 16-byte aligned. The death chain,
verbatim from run 1's serial, is:

```text
[SIGD] pid=4 sig=6 h=0x0x1083E3ECBF ipc=0x108004300B        <- SIGABRT: the assertion
[KERNEL] pid=4 (/NFS/RimWorldLinux) memory fault VA=0x000000000000000E
         PC=0x00000010818E3804 rw=r in=HOLE region=NONE ... -> killed
[RW] rwlaunch: waitpid=4 status=11
```

i.e. the assertion aborts the runtime, Mono's own crash reporter runs, and
then a read of `VA=0xE` lands in an unmapped hole and the kernel kills the
process. That is a signal-frame/trampoline alignment question in the compat
layer (rules 42/58 are its siblings), **not** I/O, and it is the next lane's
first line. `[LCOMPAT] ENOSYS nr=201` (`time(2)`) also appears once, earlier,
and is worth filling in the same pass.

### 25.6 Gates at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   411/0 PASS
bash tools/gate/m2_mono.sh               PASS — Mono 6.12 threaded C# HELLO-OK
python3 tools/gate/m3_window_probe.py    PASS (incl. the L34 display stamp)
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 triangle PRESENTED
bash tools/rw/run-nfs-bench.sh           PASS — rows above
bash tools/rw/run-rwnfs-probe.sh         6 PASS, 1 pre-existing mmap-of-NFS FAIL (-95)
```

Evidence: `obj/rw-desktop/evidence-l35/` — `bench-after.serial` (the `[BENCH]`,
`[NFSRTT]` and `[NFSPIPE]` rows), `bench-before.log` (the A/B),
`run1.harness.log` (run 1's serial ladder + Player.log dump),
`run2.{serial,Player.log}`, `run3.{serial,Player.log}`, `probe.serial`,
`unit-l35.log`, `g-m2mono.log`, `g-m3.log`, `g-m4.log`, `shots/`.

## 26. 2026-11-05 — M6/L36: the frame round trip is EXACT (0/46), two real defects fall out, `time(2)` lands

**Verdict: the 8-byte skew the trampoline dies on does NOT enter through
`rt_sigreturn`, the `ERESTARTSYS` restart or the altstack frame.** In a real
launch every one of the **46** signal round trips restores `%rsp`
bit-identically (`[SFR] ... d=0`), the host-vs-guest probe agrees row for row on
all five alignment behaviours, and a new generic detector finds no syscall site
whose `%rsp` parity ever changes (`[SKW]`: 0 rows). Two REAL defects fell out on
the way and are fixed: the resume path used to write its iretq frame into the
interrupted function's **128-byte psABI red zone**, and a freshly cloned
thread's frame slot carried the **parent's** user RSP. `time(201)` is
implemented. The launch now reaches `Initialize engine version` in 3 of 3 runs
(vs 1 of 3 at L35) and the RimWorld banner in 2 of 3, and still aborts in Mono's
trampoline guard — which is now provably NOT a frame-alignment defect of this
compat layer (§26.6 names it and the one datum that pins it).

### 26.1 The probe: host vs guest, five alignment behaviours

`src/user/linux_probe_tramp_sig.c` runs unchanged on the workstation (reference
kernel, glibc 2.39) and in the guest (`tools/gate/t3_linux_probe.sh tramp_sig`).
It measures what a signal frame, an SA_RESTART syscall restart and an altstack
frame hand back to compiled code, because Mono's JIT keeps `%rsp` 16-byte
aligned inside a method body and enters every callee through a `call`
(`%rsp % 16 == 8`). Every row is produced by a naked asm entry that records
`%rsp` before any prologue can move it, so the number is the kernel's own
product:

| row | host (reference) | guest (HobbyOS) |
|---|---|---|
| T1 handler entry `%rsp%16` / ucontext%16 | **8 / 0** | **8 / 0** |
| T1 a function CALLed from the handler | **8** | **8** |
| T1 `call` immediately after the handler returned | **8** (`call_after_handler`) | **8** |
| T1 body `%rsp%16`, `movdqa`-aligned local | **0 / 1** | **0 / 1** |
| T2 futex interrupted + SA_RESTART: still parked, handler ran, `rsp_delta`, `call` after | **1 / 1 / 0 / 8** | **1 / 1 / 0 / 8** |
| T3 altstack: entry%16 / on altstack / uc%16 / after | **8 / 1 / 0 / 8** | **8 / 1 / 0 / 8** |
| T4 two signals (second delivered at the first `rt_sigreturn`) | **8 / 8** | **8 / 8** |
| T5 SIGSEGV handler rewrites `uc_mcontext.REG_RIP`, returns | **8 / 0 / 8 / `rsp_delta=0`** | **8 / 0 / 8 / `rsp_delta=0`** |
| `[LP] done rc=` | **0** | **0** |

The three paths the launcher's death could plausibly come from are therefore
identical to Linux, byte for byte: (a) a handler that calls a function and
returns, then a `call` into a 16-aligned-sensitive sequence; (b) the same across
a futex `EINTR` restart with `SA_RESTART` (T2 keeps the thread parked, so the
restart really happened); (c) the same with `sigaltstack` + `SA_ONSTACK`.
`T2`'s `rsp_delta` is measured between two `naked_rsp` calls around a raw
`syscall`, with no compiler-inserted `sub $8,%rsp; push $0` in between (the
first version of the probe measured -16 and it was GCC's own call-site
realignment, not the kernel).

### 26.2 The in-game sigreturn table: 46 round trips, 0 mismatches

Instrumentation added to the compat delivery engine (`[SFP]` at every plant,
`[SFR]` at every `rt_sigreturn`, `[SFX]` on any difference, `[SFV]` when a
resumed frame's own RSP slot disagrees with the SP the resume tail loads,
`[SFS]` at every SA_RESTART rewind, `[SFA]` ring dump on SIGABRT). One launch
(run 2 of the ladder below) produces, deterministically, the same table in
run 3:

```text
[SFP] pid=27 gen=31 sig=30 kind=2 sp=0x108FF6FD98 sp16=8 base=0x108FF6E8F8 b16=8 alt=0
[SFR] pid=27 gen=31 sig=30 kind=2 plant=0x108FF6FD98 frame=0x108FF6FD98 d=0 p16=8 f16=8
...
47 plants, 46 returns, 0 x [SFX], 0 x [SFV], 0 x [SKW], 16 x [SFS]
  pid=6  24 round trips at sp=0x1084D04CB0   d=0
  pid=7  12 round trips at sp=0x1085509D78   d=0
  pid=27 10 round trips at sp=0x108FF6FD98   d=0
```

`kind` records provenance — `0` fault, `1` trap/syscall exit, `2` resume
boundary after an SA_RESTART rewind, `3` plain resume — so the table separates
"a signal interrupted user code" from "a parked thread was woken and restarted".
All 46 rows have `d=0`, i.e. the restored RSP is the planted RSP to the byte,
which is exactly the property Mono's trampoline needs and cannot get from a
frame that moved by 8. The SIGABRT itself is the 47th plant (`gen=47`,
`kind=1`): the asserting thread had **no** signal delivered to it before the
abort, which is why §26.6 rules the frame out as its cause.

### 26.3 Fix 1 — the resume frame was written into the user's RED ZONE

`enter_user_space`'s user-mode resume copied its 296-byte register frame plus
the 5-word iretq frame to `[target_sp-320, target_sp)` in **user** memory, where
`target_sp` is the interrupted process's own RSP. That window is exactly the
x86-64 psABI's 128-byte **red zone** plus the head of the interrupted frame, so
every preemptive resume overwrote live locals of a leaf function the 100 Hz
timer happened to interrupt. The corruption is deterministic — the bytes
written are one fixed trap frame — which is why the wild-pointer deaths always
showed the same garbage register (`r15=0x75304090`, `UnityPlayer+0x19862ef`
reading `0x8(%r15)`, a CRC32 loop) in run after run, and why "the death site
varies" while the register does not.

The iretq frame does not need to live in user memory at all: `iretq` loads RSP
from the frame's own RSP slot. The scratch now goes on this CPU's kernel stack
(`mov rax,rsp; sub rax,384; and rax,-16`), where `IF=0` already makes the copy
interrupt-atomic. Effect measured on the ladder: the pre-banner wild-pointer
class that killed 2 of 3 runs at L35 (and run 1 of this lane before the fix)
disappeared — 3 of 3 runs now reach `Initialize engine version`, and 2 of 3
reach the RimWorld banner.

### 26.4 Fix 2 — a cloned/exec'd thread's frame slot held the PARENT's RSP

`context[30]` is the frame's own user-RSP slot (`restore_context` loads
`tf->lr` from it) and `context[33]` is what the resume tail actually installs.
`process_clone` copied the caller's whole context and then set only
`context[33] = child_stack`, so a signal delivered at the new thread's **first**
resume planted its frame around the *parent's* RSP — and the `rt_sigreturn`
after it would resume the child on the parent's stack. Measured before the fix
(the probe's own T2 thread):

```text
[SFV] resume pid=1 frame_lr=0x17FFFFFC58 gsp=0x1080022EF8 d=-2147339616
```

`process_clone`, `process_set_entry`, the kernel-task creator and the exec path
now all set `context[30]` with `context[33]`, and the `[SFV]` row is gone
(0 in every run since). This is a latent defect rather than the trampoline's
cause — the child's first resume normally carries no pending signal — but it is
the same class (a frame that does not describe the thread it belongs to).

### 26.5 `time(2)` (201) is implemented

`[LCOMPAT] ENOSYS nr=201` appeared once per launch and is gone.
`l_core_time` answers `time_t time(time_t *tloc)` from the kernel's
boot-anchored RTC clock (`hb_clock_realtime_ms()/1000`, `src/kernel/time.c`),
not from uptime, and stores the value through `tloc` when it is not NULL
(`-EFAULT` if that write faults). Added: `L_SYS_time 201` in
`src/include/linux_syscall.h`, the `l_tab_core` row, `X(time)` in
`src/host/linux_abi_test.c`, and `tests/fixtures/abi/linux_syscalls.golden`
regenerated (`time 201`; the errno/layout goldens are host-provenance and were
reverted per rule 9).

### 26.6 Ladder at this tip, and the next blocker's exact line

Three desktop runs at this tip (`bash tools/rw/run-rimworld-desktop.sh
--timeout 420 --run-timeout 600`), final tree:

| run | reached | death (verbatim) | `[XE] PUT` | present |
|---|---|---|---|---|
| A | `Initialize engine version` + **`RimWorld 1.6.4871 rev600`** | `* Assertion: should not be reached at tramp-amd64.c:641` | 0 | no |
| B | `Initialize engine version` | `* Assertion at ../../mono/arch/amd64/../x86/x86-codegen.h:410, condition 'offset == (gint32)offset' not met` | 0 | no |
| C | `Initialize engine version` + **`RimWorld 1.6.4871 rev600`** | `* Assertion: should not be reached at tramp-amd64.c:641` | 0 | no |

3 of 3 reach the engine, 2 of 3 reach the game's own banner (L35: 1 of 3), and
0 of 3 present a frame. Signal evidence in all three: 47/47 `[SFP]`, 46/46
`[SFR]` with `d=0`, 0 `[SFX]`, 0 `[SFV]`, 0 `[SKW]`, 16 `[SFS]` SA_RESTART
rewinds — so the frame round trip that this lane was chartered to prove is
**exact** and is not what the abort is about.

**Next blocker 1, exact line: `tramp-amd64.c:641`** (2 of 3 runs) — Mono's
generic-trampoline stack-alignment guard. The mechanism, from the sources:

```c
/* tramp-amd64.c, mono_arch_create_generic_trampoline: */
amd64_pop_reg (code, AMD64_R11);        /* pop the encoded-argument RA   */
amd64_push_reg (code, AMD64_RBP);       /* push rbp                      */
amd64_mov_reg_reg (code, AMD64_RBP, AMD64_RSP, ...);
amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, framesize);  /* 16-aligned  */
/* Check that the stack is aligned */
amd64_mov_reg_reg (code, AMD64_R11, AMD64_RSP, ...);
amd64_alu_reg_imm (code, X86_AND, AMD64_R11, 15);
amd64_alu_reg_imm (code, X86_CMP, AMD64_R11, 0);
... call stack_unaligned (tramp_type) ...
```

with `framesize = ALIGN_TO (offset, MONO_ARCH_FRAME_ALIGNMENT)` and
`MONO_ARCH_FRAME_ALIGNMENT` = **16** (`mini-amd64.h:177`), the guard requires
**entry `%rsp % 16 == 0`**. A JIT body must be `%rsp % 16 == 0` (psABI: a
method entered at 8 keeps 0 inside the body), so a *specific* trampoline is
entered at 8 and its `call` reaches the generic one at 0. The abort therefore
says a **JIT-compiled method was running with `%rsp` 8 bytes low**, and the
compat layer cannot be its cause: the asserting thread (pid 4) received no
signal before the abort (its only plant in the run is the SIGABRT itself,
`gen=47`), and 46/46 round trips are byte-exact.

The one datum that names it is `stack_unaligned`'s own
`printf("%d\n", tramp_type)`; it is lost because fd 1 is `Player.log` and
`abort()` does not flush. The launcher now sets `setvbuf(stdout,_IONBF,0)`, and
the number is still not in the log — Unity ships its **own** Mono fork, so its
`tramp-amd64.c:641` need not be mono-project 6.12.0.199's line 641, and the
asserting site may be a different `g_assert_not_reached`. The decisive next
probe is therefore a **DR0 breakpoint on the guard** (`src/kernel/rw_watch.c`
already arms DR0 for data; the same machinery with an exec breakpoint reads the
caller's return address and `%rdi` = `tramp_type`), which names both the
trampoline flavour and the call site in one boot.

**Next blocker 2, exact line: `x86-codegen.h:410`, `offset == (gint32)offset`**
(1 of 3 runs) — `mono_x86_patch()` refusing to patch a branch whose target is
more than ±2 GB from the instruction. This one has a root cause **in this
tree**: the compat `mmap` scatters Mono's JIT code chunks over a **3.0 GB**
span, `0x1084518000` … `0x1137096000` (`[JIT]` rows, identical in runs A and C),
and Mono's amd64 codegen patches `rel32` call sites expecting every target
inside ±2 GB. Linux's own mmap keeps anonymous executable mappings in one
top-down arena; ours is first-fit over the whole user range. Fix direction:
place anonymous `PROT_EXEC` mappings (and their `mprotect(RWX)` commits) in a
dedicated contiguous arena within ±2 GB, then re-run this ladder — that also
removes one plausible source of the "far" trampoline flavours that could make
blocker 1's frame 8 bytes off.

### 26.7 Gates at this tip

### 27. L37 (merged separately): the resume frame wrote into the red zone

## 26. 2026-11-05 — M6/L37: the pre-banner killer was the RESUME frame on the user stack (and §24's DSO was never staged)

**Verdict: the class that killed 2 of 3 launches is decoded and fixed — 6 of 6
desktop runs reach `RimWorld 1.6.4871 rev600`.** The value every earlier
section read as a stack-shaped pointer (`rdi=0x2FFFF1A41`) is **not an address
at all**: it is `3 × (uint32)(slot − 13)`, where the qword at `rsp-0x50` had
been overwritten with the thread's own RSP by `enter_user_space`'s resume
scratch. §21.5 / rule 51 moved the *signal* frame out of the 128-byte red zone;
the *resume* frame was still inside it, on **every** preemption (851,968 of them
in one run). Two more defects fell out of the same measurement: the staged
`libX11.so.6` was older than the §24 display stamp, so §24's fix was not in any
binary this worktree booted, and a blind `min(target_sp, tf->lr)` put the
scratch at address 0 for fresh processes.

### 26.1 Decode: `0x2FFFF1A41` is `3 x` a truncated index, not a stack VA

The staged `UnityPlayer.so` (md5 `45408fc1…`, identical to the game dir and to
`/srv/nfs/rimworld`) at `+0x91b119`, the site §21.5 named and §22.6/§24/§25 kept
re-meeting:

```text
91b10e: mov  -0x50(%rsp),%rdi     ; a 32-bit index spilled into the RED ZONE
91b113: add  %esi,%edi            ; esi = 0xFFFFFFF3 (-13); 32-bit, zero-extends
91b115: lea  (%rdi,%rdi,2),%rdi   ; x3
91b119: movdqu %xmm0,(%rax,%rdi,8)   <-- the fault
```

Measured in run `before-1` (full ucontext in `[SIGDUMP]`, one line per field):

| value | where it came from |
|---|---|
| `rip=0x1081423119` | `UnityPlayer+0x91b119` (base `0x1080b08000`) |
| `rsp=0x17FFFFB3D8` | interrupted user RSP |
| `[SLOT-50] va=0x17FFFFB388 val=0x17FFFFB3D8 ==rsp=1` | **the slot holds the thread's own RSP** |
| `esi=0xFFFFFFF3` | `-13` |
| `rdi=0x2FFFF1B61` | `3 × 0xFFFFB3CB`, and `0xFFFFB3CB = (uint32)(0xFFFFB3D8 − 13)` |
| `rax=0x1082A834F0`, `cr2=0x2882A10FF8` | `rax + 8 × 0x2FFFF1B61 = 0x2882A10FF8` — exact |

So the "address" is a **negative int index** times a 12-byte stride: the low 32
bits of a stack pointer read as `int` are negative, the 32-bit `add` keeps it
negative, and the resulting 4 GiB-scale index lands in a hole. That is why the
value looked "shaped like a stack VA": it *is* generated from a stack pointer,
but through `3 × (uint32)`.

The same arithmetic retro-decodes §22.6 without another boot:
`0x2FFFF1A41 = 3 × 0xFFFFB36B`, `0xFFFFB36B = 0xFFFFB378 − 13`, and
`0x17FFFFB378` is exactly the RSP that §22.6's own row implies
(`[SIGP] fsp=0x17FFFFB2F8` = rsp−128, "clobbered slot at `0x17FFFFB328`" =
rsp−0x50). §22.6 called that slot "a garbage red-zone local"; it was the
thread's RSP, written by the resume path.

### 26.2 The writer: `enter_user_space` builds its iretq frame on the user stack

HobbyOS has no per-thread kernel stack for x64 user traps, so the resume path
copies a 37-qword register image plus five iretq slots onto the **resumed
task's own user stack**, then iretqs from there. Pre-fix geometry
(`trap.c`, `enter_user_space`, user-mode branch):

```text
lea rdi,[r13-320] ; rep movsq (37 qwords)   -> [target_sp-320, target_sp-24)
lea rsp,[rdi-296] ; five iretq slots         -> [target_sp-40, target_sp)
```

so the whole window is `[target_sp-320, target_sp)` — the psABI red zone
`[target_sp-128, target_sp)` **and** 192 bytes below it. The register image
lands at fixed offsets, which is what makes the fault diagnosable:

| frame offset | written value | pre-fix address (rsp = target_sp) |
|---|---|---|
| dest+240 (`tf->lr`) | the interrupted user RSP | `rsp-0x50` |
| dest+248 (`tf->elr`) | the interrupted RIP | `rsp-0x48` |
| dest+256 (`tf->spsr`) | RFLAGS | `rsp-0x40` |
| dest+288/296/304/312 | CS `0x1B`, RFLAGS, RSP, SS `0x23` | `rsp-0x20 … rsp-0x08` |

`before-1`'s `[STK-80]` row IS that image, byte for byte:

```text
rsp-0x50 0x00000017FFFFB3D8   <- tf->lr  (the index the code then read)
rsp-0x48 0x0000001081422FFE   <- tf->elr (an address inside the same function)
rsp-0x40 0x0000000000200213   <- RFLAGS
rsp-0x20 0x000000000000001B   <- CS
rsp-0x10 0x00000017FFFFB3D8   <- RSP slot
rsp-0x08 0x0000000000000023   <- SS
```

No signal was involved: the game's threads are preempted by the 100 Hz timer,
so the poison is written ~10^5-10^6 times per run. Whether it *matters* depends
on the interrupted function keeping a live red-zone slot: Unity's static
initialiser loop spills its index at `-0x50(%rsp)` and reads it six
instructions later, which is why 4 of 6 runs died at this one PC with three
different `cr2` values.

### 26.3 Six runs before the fix: 6/6 pre-banner, two writers

`tools/rw/l37-runs.sh before 6 150` (each run: full `[SIGDUMP]`, 16 stack
qwords, region rows; `tools/rw/l37-table.py` regenerates this table):

| run | rip (module) | cr2 | rw | slot@rsp-0x50 | == rsp | reached `Initialize engine` | class |
|---|---|---|---|---|---|---|---|
| 1 | `UnityPlayer+0x91b119` | `0x2882A10FF8` | w | `0x17FFFFB3D8` | **yes** | yes | resume red-zone |
| 2 | `UnityPlayer+0x198b548` | `0x6` | r | `0x17FFFFABD0` | **yes** | no | resume red-zone |
| 3 | `UnityPlayer+0x1947cef` | `0xE8` | r | `0x10C02C8C70` | no | yes | NULL `Display*` (§24) |
| 4 | `UnityPlayer+0x19862ef` | `0x75304098` | r | `0x17FFFFAC98` | **yes** | no | resume red-zone |
| 5 | `UnityPlayer+0x1947cef` | `0xE8` | r | `0x1040034100` | no | yes | NULL `Display*` (§24) |
| 6 | `UnityPlayer+0x19862ef` | `0x75304098` | r | `0x17FFFFAC98` | **yes** | no | resume red-zone |

4 of 6 are the resume class at **three different PCs** (`+0x91b119`,
`+0x198b548`, `+0x19862ef`) — the "the death site varies" observation that
kept this class unexplained; the invariant is the slot, not the PC. 2 of 6 are
`rdi=0` reading `+0xe8` (`Display::screens`), §24's class, whose fix was in the
source but **not in the staged DSO** (§26.5).

The pre-fix image is quoted from the `[STK-80]` row above, not from the
`[SIGSIG]` scan: that scan looked for an adjacent `(0x1B, 0x23)` pair and could
never fire (the slots are three qwords apart — §26.9), which is exactly the
"broken instrument reads as no evidence" trap.

### 26.4 The fix, and the bug the fix itself had

`enter_user_space` now places the scratch **entirely below the red zone**:

```text
mov rax,[r12+240] ; tf->lr      \
mov rcx,r13 ; sub rcx,rax       |  use tf->lr as the base only when it is a
cmp rcx,0x100000 ; jae 8f       |  plausible planted-frame address just below
cmp rax,0x10000  ; jb 8f        |  target_sp
mov r13,rax                     /
8: lea rdi,[r13-448]            ; 448 = 128 (red zone) + 320 (window)
```

The iretq RSP slot carries the resumed RSP, so the window does **not** have to
end at `target_sp`; nothing at or above `target_sp-128` is written any more.
The `tf->lr` term is for resume-boundary signal delivery, where `l_sf_plant`
has already placed a frame at `tf->lr` ~1.2 KiB below `target_sp`: without it
the scratch would overwrite that frame's FXSAVE image.

The **first** version took `min(target_sp, tf->lr)` unconditionally, and a
fresh process is created by `process_set_entry()`, which sets
`context[31]/[33]` and leaves `context[30]` (`tf->lr`) **zero** — so the
scratch became `0-448` and the kernel took `Vector: 14 Error Code: 0x2` at
CPL0 within a second of boot (measured: `DESKTOP.BIN` died before the menu;
zero `[RESUME-RZ]` rows). The `jae/jb` plausibility pair is the fix for that.

Two assertion rows make the geometry checkable in every future run:

- `[RESUME-RZ] pid=… tsp=… win_lo=… win_hi=… redzone_hit=0 bad_total=0
  resumes=851968` — one row per user resume (first 8 and every 65536th
  printed): the scratch window vs `[tsp-128, tsp)`, plus a counter.
- `[SIGRZ] pid=… sig=… sp0=… base=… top=… rz=1 bad_total=0 plants=24` — one
  row per **signal** delivery, asserting
  `base + 1088 <= sp0 - 128` (Linux's rule); `rz=2` means SA_ONSTACK (no red
  zone). 24/24 plants clean in the after runs, 0 violations in all 12 runs.

### 26.5 The second killer: the staged `libX11.so.6` predated §24 (rule 45)

With the kernel fixed but the seeded `obj/` in place, 1 of 6 runs reached the
banner and **5 of 6** died at `UnityPlayer+0x1947cef` (`rdi=0`, `cr2=0xE8`) —
§24's class, which §24 reported 0/4 after its fix. `make xcompat_dsos`
produced a **different binary from the same source**:

| | size | md5 |
|---|---|---|
| staged in `disk.img` / seeded `obj/` | 89,792 | `d84a6905bffca5a30ae27321f28d162d` |
| rebuilt from the merged tip | 89,960 | `9af556477462cb0e568ac2442715f9bd` |

and the rebuild contains 32 `mov %reg,0x18(%reg)` stores (the `xany.display`
stamp + the `xmap/xunmap/xreparent` fields) against 14 in the seeded object.
The lane had been running with `RW_SKIP_BUILD=1`, which skips the DSO build and
stages whatever `obj/xcompat` holds — exactly the "green source tree, stale
staged DSO" failure of rule 45. With the rebuilt DSO staged, the class is gone
in 6 of 6.

### 26.6 Six runs after the fix: 6/6 reach the game's banner

`tools/rw/l37-runs.sh after2 6 150`, same kernel except the scratch move plus
the DSO rebuild:

| run | `Initialize engine version` | `RimWorld 1.6.4871 rev600` | death after the banner | `[RESUME-RZ]` bad/total | `[SIGRZ]` bad/plants |
|---|---|---|---|---|---|
| 1 | yes | **yes** | `x86-codegen.h:410` assertion → `VA=0xE` | 0 / 851,968 | 0 / 24 |
| 2 | yes | **yes** | same | 0 / 851,968 | 0 / 24 |
| 3 | yes | **yes** | same | 0 / 786,432 | 0 / 23 |
| 4 | yes | **yes** | same | 0 / 851,968 | 0 / 24 |
| 5 | yes | **yes** | same | 0 / 851,968 | 0 / 24 |
| 6 | yes | **yes** | same | 0 / 917,504 | 0 / 24 |

Before → after: pre-banner deaths **6/6 → 0/6**; `RimWorld 1.6` banner **0/6 →
6/6**. No run in the after set faults before the banner; the only fault is the
crash reporter's `VA=0xE` read after Mono aborts.

### 26.7 The deterministic test: `linux_probe_redzone`

`src/user/linux_probe_redzone.c` + `bash tools/gate/t3_linux_probe.sh redzone`.
An ordinary musl static-PIE program plants six magics at `-0x08…-0x120(%rsp)` in
one asm block, spins a 3-instruction loop (~0.2-0.6 s, so several 100 Hz ticks
must land inside it), then reads them back. A control pass with `iters=0`
(no preemption window) proves the measurement itself; only red-zone offsets are
counted (`-0x90/-0x120` are reported as `info`: they are *below* the red zone,
unallocated stack that the resume scratch legitimately uses).

| kernel | control rows | red-zone spin rows | verdict |
|---|---|---|---|
| host (Linux reference) | 6/6 KEPT | 18/18 KEPT | `rc=0` |
| pre-fix (`-320`) | 6/6 KEPT | **18/18 CLOBBERED** | `rc=18` |
| fixed (`-448`) | 6/6 KEPT | 18/18 KEPT | `rc=0` |

The pre-fix values name the writer: `-0x08 → 0x23` (SS), `-0x10 → ` the thread's
own RSP, `-0x20 → 0x1B` (CS), `-0x30 → 0x100000000`, `-0x70 → 0`, `-0x120 → 0`.
Evidence: `evidence-l37/probe-redzone-{before,after}.log`.

### 26.8 What this says about (a) resume-boundary frames and (b) masks/altstack

- **(a) nesting / SA_ONSTACK.** The signal frame was already correct: 24/24
  `[SIGRZ]` plants per run with `rz=1` (or `rz=2` on Unity's SA_ONSTACK
  handler, whose frame goes to the altstack at `0x1084B04BB8` and never touches
  the user stack). The overlap that *did* exist was the resume scratch against
  a freshly planted frame — the `min(target_sp, tf->lr)` term removes it; it is
  visible in the old geometry (`[target_sp-320, target_sp)` vs the frame's
  FXSAVE at `base+568…base+1080`), not as a separate measured crash.
- **(b) mask / alt-stack skew.** Every delivery row carries the altstack state
  (`alt=1` for Unity's handler, `alt=0` for Mono's Suspender) and `rt_sigreturn`
  returns are counted (`[RT]`), with no skew in 12 runs; the mask semantics are
  already gated by `linux_probe_futex_sig` cases C (pre-suspend mask restored)
  and E (masked pending signal does not interrupt). No new defect found here.

### 26.9 An instrument that lied, and was fixed

The first `[SIGSIG]` scan looked for an adjacent `(0x1B, 0x23)` qword pair and
therefore **could never fire**: the iretq stamping puts CS at `dest+288` and SS
at `dest+312`, three qwords apart. A broken instrument reads exactly like "no
image", which would have "confirmed" any theory. It now scans `q[i]==0x1B &&
q[i+3]==0x23` and reports the CS offset (`-0x120` pre-fix at
`target_sp == rsp`).

### 26.10 Gates at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   411/0 PASS
bash tools/gate/m2_mono.sh               PASS — Mono 6.12 threaded C# HELLO-OK
bash tools/gate/m2_threads.sh            PASS — 8 pthreads, mutex+condvar+TLS+join
bash tools/gate/m12_thread_ceiling.sh    PASS — 150 threads + TLS + SIGCANCEL cancel
python3 tools/gate/m3_window_probe.py    PASS — XCreateWindow ladder + display stamp
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 triangle PRESENTED
bash tools/gate/t3_linux_probe.sh tramp_sig  PASS — [LP] done rc=0, host==guest
bash tools/rw/run-rwnfs-probe.sh         15 PASS + the pre-existing by-design
                                         `mmap(/NFS/...) -> -95` row (rule 14)
```

Evidence: `obj/l36/evidence-l36/` — `final{A,B,C}.{serial,Player.log}` (the
ladder, the `[SFP]`/`[SFR]`/`[SFA]` tables, the trampoline aborts),
`final{A,B,C}.harness.log`, `shots{A,B,C}/`, `run{1,2,3}.*` (the first ladder,
before the launcher's `setvbuf`), `../l36/unit.log`, `m2mono.log`,
`m2threads.log`, `m12.log`, `m3.log`, `m4.log`, `nfs.log`, and the probe's host
and guest logs (`obj/linux_probe_tramp_sig.log` = guest).

<!-- L37 side -->
bash tools/gate/m12_thread_ceiling.sh    PASS — 150 threads + TLS + SIGCANCEL(32)
python3 tools/gate/m3_window_probe.py    PASS — SDL XCreateWindow ladder (incl. §24 display stamp)
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 triangle PRESENTED
bash tools/gate/t3_linux_probe.sh redzone PASS — 6/6 red-zone rows KEPT
```

### 26.11 Next blocker, exact line

Every after run reaches the banner and then aborts in Mono:

```text
RimWorld 1.6.4871 rev600
* Assertion at ../../mono/arch/amd64/../x86/x86-codegen.h:410,
  condition `offset == (gint32)offset' not met
[KERNEL] pid=4 memory fault VA=0x000000000000000E PC=0x00000010818E3804 rw=r in=HOLE -> killed
```

`UnityPlayer+0xddb804` is Mono's crash reporter reading `VA=0xE` — the same
death chain §25.5 recorded after `tramp-amd64.c:641`, so the *assertion* moved
and the cascade did not. `offset == (gint32)offset` is an x86 codegen
displacement check: a call/jump/field offset that does not fit in 32 bits. In a
96 GiB user window that is a code-layout question (a JIT stub farther than
2 GiB from its target), not a signal-frame one — the next lane's first line.

Evidence: `obj/rw-desktop/evidence-l37/` — `before-{1..6}.{serial,Player}.log`
with `.summary`, `after-{1..6}` (kernel fix only, stale DSO), `after2-{1..6}`
(the fix), `probe-redzone-{before,after}.{log,serial.log}`, `dso-hashes.txt`,
`unit-l37.log`, `gate-*.log`.

### 26.12 Post-gate boot (rule 5)

After the gate set above, kernel + `disk.img` were rebuilt in order and the
harness ran **without** `RW_SKIP_BUILD` (a probe/GL gate leaves its own image;
`RW_SKIP_BUILD=1` would boot `PROBE.ELF`). That seventh run reaches
`Initialize engine version` and `RimWorld 1.6.4871 rev600` again, hits the
`tramp-amd64.c:641` variant of the JIT assertion, and reports
`[RESUME-RZ] … redzone_hit=0 bad_total=0 resumes=917504`. So the fixed tree is
7 of 7 reaching the banner, and both assertion variants (`tramp-amd64.c:641`
and `x86-codegen.h:410`) are the same next blocker (rule 70's ±2 GB branch
patch), not a regression of this lane.

## 27. 2026-11-05 — M6/L38: a ±2 GiB JIT code arena; both JIT assertions are gone 3/3

**Verdict: the game no longer dies in Mono's JIT.** Both assertion variants of
§26.6 — `x86-codegen.h:410` (`offset == (gint32)offset`) and
`tramp-amd64.c:641` — are **absent from 3 of 3 desktop runs**, and Mono JITs its
methods instead of refusing a branch: 14-15 executable windows per run (44 in
the three runs below), **every one served from a single arena** (`src=arena`),
11 of them inside 704 KiB and the largest a 10 MiB block. The
player reaches `Loaded All Assemblies` + `Finished resetting the current
domain` + `RimWorld 1.6.4871 rev600` and then *keeps running its own frame
loop* until the launcher's 180 s deadline — it issues
`[XE] PUT 0;0;1916;1018` in 1 of 3 runs and eight `[XE] FLUSH 0;0;1916;1018`
rows in all 3, instead of aborting. **No game content is on screen yet**: the window's pixels are all
`#000000` (1,950,392 of 2,073,600), so §27.5 names the next blocker — the first
non-black frame, not the JIT.

### 27.1 The measurement: 64 KiB chunks, caller-supplied hints, first-fit that drops them

The policy that produced the 2.8-3.0 GiB spread is `vm_mmap`'s non-fixed path,
and it is one line of arithmetic: **first-fit bottom-up from
`USER_MMAP_BASE_V2` (0x1080000000, +2 GiB in the 32 GiB user window) over the
whole 18 GiB MMAP arena, with a 64 KiB `VM_HOLE_GAP` between temporally distinct
regions**. There is no ASLR anywhere in this tree (the layout is a pure function
of allocation order), and a caller hint is honoured **only** when
`vm_hole_find(as, hint, LIMIT, len) == hint`, i.e. when the span starting exactly
at the hint is free *and* the 64 KiB gap rule is satisfied for the next region.
The module itself is not in a fixed slot: `UnityPlayer.so` is the first mmap and
lands at 0x1080b08000 (11.03 MiB above the arena base) by first-fit.

Mono's code manager asks for code with `mmap(NULL, 0x10000, RWX,
MAP_PRIVATE|MAP_ANON)` — 64 KiB per chunk — and from the second chunk on it
passes a **hint of `previous_end + 0x10000`**, exactly the 64 KiB gap. The
kernel nonetheless refused that hint for chunks 2-4 and then lost the frontier
to unrelated allocations, so the chunks marched:

| chunk | base (`[JIT]` row, pre-arena ladder) | Δ from chunk 0 | hint honoured |
|---|---|---|---|
| 0 | 0x1084518000 | — | (no hint) |
| 1 | 0x1084AF1000 | +5.8 MiB | no |
| 2 | 0x108FAC6000 | +0.177 GiB | no (`hint=0x1084B01000`) |
| 3 | 0x108FEE0000 | +0.181 GiB | no (`hint=0x108FAD6000`) |
| 4 | 0x108FF75000 | +0.182 GiB | no (`hint=0x108FEF0000`) |
| 5-10 | 0x108FF85000 … 0x108FFD5000 | +0.182 GiB | **yes** (6 chunks, 64 KiB apart) |
| 11 | 0x1190000000 | **+4.18 GiB** | no |

Why the gap between chunk 1 and chunk 2 is 0.17 GiB rather than 64 KiB: the span
`[0x1084528000, 0x1090000000)` is not left to the JIT. Inside it, in the same
window, Unity and Mono place four 256 KiB RW windows, a 0x60000 reservation,
two 0x468A00 mappings, a 0x201000 reservation and Mono's 64 MiB GC heap
reservation at 0x1088000000; above it sit **eight 256 MiB `PROT_NONE` windows**
at 0x1090000000, 0x10A0000000 … 0x1100000000 (Unity's 512 MiB reservations,
each trimmed by `munmap`), and the game's own 4 MiB block chain lives in the
one at 0x10C0000000 (§16). So the free space below 0x1090000000 is consumed by
everything *except* the JIT, and the 12th chunk has nowhere left but 4 GiB up —
which is 2.85 GiB from the module text, i.e. outside every `rel32`.

That is the whole of rule 70's mechanism, now measured from the request stream
(`[MM]`, `RW_TRACE_MEM=1`: 4,617 rows) instead of inferred from a crash.

### 27.2 The arena

`src/kernel/vm.c`, new section "the JIT code arena":

* **Claim.** On the first `PROT_EXEC` anonymous, non-`MAP_FIXED` mapping of an
  address space, scan the region list for the **largest free gap inside
  ±`USER_JIT_REACH` (2 GiB) of the address first-fit would have returned**,
  clamped to the MMAP arena. Require ≥ `USER_JIT_MIN` (4 MiB), cap at
  `USER_JIT_ARENA_MAX` (1 GiB), align the base up to `USER_JIT_ALIGN` (64 KiB,
  Mono's chunk granularity) **and shrink the size by the same amount**.
* **Reservation.** The span is covered by one placeholder region (protection 0,
  flags 0, no object, kind `VMK_ANON`), so the ordinary first-fit can never
  hand out an address inside it and no other placement rule changes. The claim
  is pinned to the *first* anchor the process ever wanted (`jit_anchor0`), so a
  claim that only becomes possible after megabytes of unrelated allocation is
  refused rather than granted 6 GiB away.
* **Serve.** Bump pointer + a 16-entry LIFO free list (a `munmap` of a span
  fully inside the arena returns it; a code manager that frees a chunk gets the
  same address back). The bump skips any span a `MAP_FIXED` request carved into
  the arena instead of failing.
* **Fallback, never an error.** No gap ≥ 4 MiB, arena exhausted, or a
  `MAP_FIXED` request → the old first-fit path, with `[JITA] full` said once
  when the arena had been claimed and ran out. A claim that finds no room is
  silent on purpose (there is nothing to report but the layout); it shows up as
  `[JITV] … src=fit` rows, which is exactly how §27.3's bug was finally seen. `brk`, thread stacks, the DSO band, the FB slot and
  Unity's reserved windows are untouched: they never enter this path.
* **Locking.** The claim, the bump, the free list and the placeholder carve all
  move under `vm_lock` (the `_locked` region primitives): two threads of one
  group can enter the compat mmap path together, and an unlocked bump hands
  both the same base (the second `vm_region_insert` then fails and its caller
  reads a spurious `ENOMEM`). `vm_jit_release` likewise runs inside
  `vm_munmap_range`'s critical section.

Measured in the game: `[JITA] claim base=0x1084520000 size=0xBAE0000
anchor=0x1084518000 gap=0xBAE8000` — a **186.875 MiB** contiguous arena 58.1 MiB
above the module base, i.e. 1/11th of the ±2 GiB budget, claimed where the first
chunk would have gone anyway. All 15 windows of the run are inside it.

### 27.3 The bug the first version had, and the test that now pins it

The first arena passed its unit test (an empty address space, where every gap
ends on a 2 GiB boundary) and **failed on every desktop run**: the claim found
the 186.9 MiB gap (`gap=0xBAE8000`), aligned the base from 0x1084518000 up to
0x1084520000, and inserted `size = gap` — one 32 KiB poke past the gap's end
into the region at 0x1090000000. `vm_region_insert` refuses an overlap with
`-1`, the claim returned 0 without a word, and the JIT fell back to first-fit:
11 chunks scattered, the assertion still fired, and the only trace was a
`[JITV] … src=fit`. The fix is to compute `gap_end` first and clamp the size to
it; the regression is `test_vm_jit_arena_gap_end`, which builds a bounded
mis-aligned gap, closes the whole ±2 GiB window at its end, and asserts
`as->jit_end == ceiling` (it fails on the old code because no arena is claimed
at all). The diagnostic that found it was a temporary `[JITA] try … anchor= lo=
hi= gap= at= nr= cap=` row per attempt; it is not in the shipped tree (the
permanent rows are `[JITA] claim` and `[JITA] full`), but it is the row to add
first if a future tree ever claims nothing.

### 27.4 Ladder at this tip

`bash tools/rw/l38-runs.sh final 3 600` — three sequential desktop runs of the
committed tree:

| run | JIT windows | `src=arena` | `Initialize engine` | banner | assertion | `[XE] PUT` / `FLUSH` (Player.log) | `[XE] PUT` (serial) | window pixels |
|---|---|---|---|---|---|---|---|---|
| 1 | 15 | 15 | yes | yes | **none** | 1 / 8 (skip 1) | 0 | 1,950,392/2,073,600 `#000000` (94.1%) |
| 2 | 14 | 14 | yes | yes | **none** | 0 / 8 (skip 1) | 0 | 1,950,392/2,073,600 `#000000` (94.1%) |
| 3 | 15 | 15 | yes | yes | **none** | 0 / 8 (skip 1) | 0 | 1,950,392/2,073,600 `#000000` (94.1%) |

3 of 3 claim the same arena (`base=0x1084520000 size=0xBAE0000`), 44 of 44
windows come from it, and **neither assertion appears in any run** — where the
pre-arena ladder aborted in 6 of 6 (§26.6/§26.11). The window budget is 2.79 GiB
of scatter before, 704 KiB of contiguous chunks after: chunk *n* is at
`arena_base + n*0x10000` (`[JITV] n=1 … n=13`), and the last window of the run
is a 10 MiB `len=0xA00000` request (n=14) that also lands inside the arena.

Two things the table also says, because they bound the next step:

* the **first** `XPutImage` is skipped in every run (`[XE] PUTSKIP 0;0;0;0`,
  `content_ready == 0`, rule 23) — the presents that reach the desktop come
  from the GL swap path (`[XE] FLUSH 0;0;1916;1018`, `send_flush`);
* after the eighth flush the log turns into **40,360 / 40,736 / 43,198**
  `[XI] XIGrabTouchBegin` rows and there is no further flush until the 180 s
  deadline. The game's own window is on screen, is the right size (1916×1018
  content rect in a 1920×1080 desktop), and is **all black** — of 2,073,600
  pixels, 1,950,392 are `#000000` and the remaining 123,208 are the desktop's
  own frame and title bar.

### 27.5 Next blocker, exact line

Not the JIT any more, and not a signal frame. In order:

```text
[XE] PUTSKIP 0;0;0;0                    src/user/xcompat/libx11.c:3164  (content_ready == 0)
[XE] FLUSH 0;0;1916;1018                src/user/xcompat/libx11.c:672   (GL swap present, 8 rows)
[XI] XIGrabTouchBegin dev=0 win=0x200002 owner=1 -> Failure (no touch device)   x 40,360
```

So the next lane's subject is the **first non-black frame**, and the two rows
above separate its two halves: either the game draws and the swap presents a
black buffer, or Unity never draws again after the 8th swap and the `[XI]` loop
is what it does instead. The loop is suspicious on its own terms — rule 44's
shim answers "no touch device, no grab" and Unity retries it twice per
`XIUngrabTouchBegin`, 40k times in 180 s, which is a *polling* rate, not a frame
rate. It is also the one unbounded trace in the tree: every call writes a row to
fd 1, i.e. one console write per attempt, which throttles the very loop being
measured.

The decisive probe is cheap: bound the `[XI]` row (8 rows, like every other
`l34_row`), and add a once-a-second `[XSW]` row carrying (swap count, a checksum
of the first 4 KiB of the swapped buffer, the desktop's `content_ready`) — so
"never draws" and "draws black" are different rows instead of one black
screenshot.

### 27.6 Gates at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   413/0 PASS  (411 + test_vm_jit_arena,
                                                    test_vm_jit_arena_gap_end)
bash tools/gate/t3_linux_probe.sh redzone PASS — 6/6 red-zone rows KEPT, rc=0
bash tools/gate/m2_mono.sh               PASS — Mono 6.12 threaded C# HELLO-OK
bash tools/gate/m2_threads.sh            PASS — 8 pthreads, mutex+condvar+TLS+join
bash tools/gate/m12_thread_ceiling.sh    PASS — 150 threads + TLS + SIGCANCEL(32)
python3 tools/gate/m3_window_probe.py    PASS — SDL XCreateWindow ladder + display stamp
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 triangle PRESENTED
bash tools/rw/check-no-game-files.sh     OK   — no RimWorld artifacts in the tree
```

### 27.7 Evidence

`obj/rw-desktop/evidence-l38/`:

* `final-{1,2,3}.{serial,Player}.log` + `.summary` — the ladder above
  (`final-N.summary` is generated by `tools/rw/l38-runs.sh`, the table by
  `tools/rw/l38-table.py`);
* `arena-{1,2,3}.*` — the *pre-fix* arena: `src=fit` rows, the 12th chunk 4.0 GiB
  away, and the `[JITA] claim insert failed` diagnostic that named the
  alignment bug;
* `diag-1.*` — the run that printed `[JITA] try pid=4 anchor=0x1084518000 …
  gap=0xBAE8000 at=0x1084518000 nr=96 cap=128` and then
  `claim insert failed rc=-1 base=0x1084520000 size=0xBAE8000`;
* `unit-final.log` (413/0), `gate-*.log` + `gate-*.rc` (the seven gates above);
* `shots/` — the desktop screenshots (the game window is the black rectangle);
* `tools/rw/l38-layout.py` — replays a `[MM]`/`[JITV]` trace and prints the
  chunk table and the free-gap map at the moment of the first chunk (the
  measurement in §27.1).


## 28. 2026-11-05 — M6/L39: the present path, named layer by layer; the first frame is kept and shown

The L38 ladder ended with a black window and two candidate explanations that no
row could separate: *Unity never draws again after the eighth flush* or *it
draws and the pixels are dropped on the way to `/dev/fb0`*. This section adds
the instrument that separates them (`[XSW]`, one row per `glXSwapBuffers`),
fixes the first-present skip, bounds the `[XI]` storm, and names the layer the
black rectangle is actually in.

### 28.1 Where a present happens

`libGL.so.1` in this tree is built from the **gallium xlib frontend**, not from
`src/glx` — `obj/mesa/obj` has no `glxcmds` object at all, so a patch against
`src/glx/glxcmds.c` builds green and changes nothing (rule 29's cousin, and it
cost one build here). The real path is:

```text
Unity
 └─ glXSwapBuffers                 src/gallium/frontends/glx/xlib/glx_api.c
     └─ XMesaSwapBuffers           xm_api.c   (st_context_flush + fence wait)
         └─ xmesa_swap_st_framebuffer          xm_st.c
             └─ xmesa_st_framebuffer_display
                 └─ swrastXPutImage -> XPutImage        compat libX11.so.6
                     └─ shadow (B8G8R8A8, one static 1920x1080 buffer)
                         └─ XFlush -> shadow_blit -> /dev/fb0
                             └─ ESC ] F <x>;<y>;<w>;<h> ~   desktop presents the rect
```

Two facts about that path matter, and both were invisible from the outside:

1. **Nothing in the swap path calls `XFlush`/`XSync`.** `XMesaFlush()` is the
   only X flush in the frontend and the swap does not call it. A real Xlib
   writes the `XPutImage` request to the server socket as it builds it; this
   compat Xlib keeps the frame in its shadow until *something* flushes. So a
   swap could succeed, return 0, and leave the desktop showing the previous
   (black) frame. `XcPresentMark()` now flushes the shadow it just filled.
2. **The client can swap before the surface exists.** `XMapWindow` used to send
   `ESC ] X` and return; the desktop's `ESC ] G` arrived later, so
   `content_ready` was still 0 when Unity swapped. Mesa asks `XGetGeometry`
   for the drawable size, got the window's created size, and `XPutImage` was
   called with `content_ready == 0` — the old code printed
   `[XE] PUTSKIP 0;0;0;0` and dropped the frame.

### 28.2 The `[XSW]` row

`glXSwapBuffers` calls the shim's `XcPresentMark(dpy, swaps, glflush, drawable)`
(weak symbol, so a host build against a real libX11 resolves it to NULL and the
probe costs one branch). The shim owns the `XPutImage`/`XFlush` counters and
the checksum, so the row exists even when the swap hands `XPutImage` nothing:

```text
[XSW] n=<marks> swap=<glXSwapBuffers> put=<XPutImage> putok=<copied>
      flush=<XFlush> crc=<fnv64 of the surface> changed=<since the last row>
      w=<drawable w> h=<drawable h> ready=<content_ready> nz=<visible pixels>/<total>
      box=<x0,y0,x1,y1 of visible pixels> copy=<XCopyArea> glflush=<glFlush>
```

What each answer looks like:

| question | row |
|---|---|
| the client never presents | `swap` frozen, `put`/`putok` frozen |
| it presents but the image is empty | `putok` advances, `crc` constant, `nz` = 0 |
| it presents real pixels and they do not reach the desktop | `crc` changes, `nz`/`box` non-zero, screenshot black |
| it presents and the desktop shows it | all of the above plus a screenshot that matches `box` |

`nz` counts pixels whose **B|G|R** is non-zero — alpha is `0xFF` on every
opaque frame, so counting all four bytes says "all pixels" for a black frame
(measured: `nz=921600/921600` on an opaque black buffer before the metric was
fixed). Rows are emitted for the first 3 swaps, then every 10th, plus the first
swap that carried pixels, so a 60 fps client pays one 7.8 MB scan every ten
frames.

### 28.3 The first frame is kept, and presented

`XPutImage` no longer drops an image that arrives before the geometry: it is
copied into a second static buffer (`g_defer_store`) at *its own* size, and
`set_geometry` blits the overlap into the surface and sends the flush as soon as
the desktop names the content rect. `XMapWindow` also reads that reply itself
(bounded: 8 polls of 20 ms, `[XE] MAPGEOM ready=… polls=…`), so the surface is
usually laid out before the client's first swap.

Measured (`final4`-era run, `[XE]` rows in Player.log):

```text
[XE] PUT 0;0;1280;720                  the swap's image: 1280x720, before any geometry
[XE] PUTDEFER 0;0;1280;720             kept at its own size (was: PUTSKIP, dropped)
[XE] PUTLATE held=1 x=2 y=34 w=1916 h=1018 defer=1280x720
                                       desktop laid the surface out; overlap blitted + flushed
[XSW] n=1 swap=1 put=1 putok=1 flush=85 crc=0x9e42e911decedfdb changed=0
      w=1280 h=720 ready=1 nz=921600/1950488 box=0,0,1279,719 copy=0 glflush=15
```

`nz=921600` out of a 1916x1018 surface with `box=0,0,1279,719` says the whole
1280x720 frame is non-black and it is sitting in the top-left of the surface.
**Unity draws; the frame is not black.** That retires the "draws black" half of
the L38 question: what is missing is presents *after* the first one.

### 28.4 The `[XI]` storm was a trace storm, and `Success` is not consumable

The L38 log carried 40,360 / 40,736 / 43,198 `[XI] XIGrabTouchBegin` rows and
the shim wrote one serial row per call — the measurement was throttling the loop
it measured. The row is now bounded (first 4, then every 4096th, with the
running count in it): **17 grab rows / 30 `[XI]` rows per 180 s run** against
40k+, while the call rate itself is unchanged (~24k calls, every one a no-op).

The negotiation answer matters more than the trace. What was tried:

* `XIQueryVersion -> Success (2.x)` with `XIQueryDevice -> 0 devices`
  (the shape a real server without a touchscreen has): the grab storm stops
  (exactly **1** grab row), but Unity **never reaches the banner** — Player.log
  ends at `Finished resetting the current domain`, the event pump runs 0 times
  (`[XHB]` never appears) and the main thread sits in a condvar wait/signal
  loop. 1 of 1 run.
* `XIQueryVersion -> BadRequest` (what this DSO always answered) with the same
  bounded rows: banner 3 of 3 in the ladder of §28.6, the pump loop runs,
  `[XI]` is 9-83 rows per run instead of 40k.

So hypothesis (A) is false as stated: the storm did not block presents, and
"success + zero devices" is a state Unity cannot start from. BadRequest plus a
bounded row is the configuration that keeps the ladder alive, and the honest
finding is that **Unity re-asks for the touch grab on every input poll and there
is nothing to grant** — no touch device is faked.

### 28.5 What the black rectangle is: the window churn, not the renderer

With the present path instrumented end to end, the remaining row is unambiguous:

```text
[XHB] pumps=240000 swaps=1 puts=1 putok=1 flush=120088 heals=13332 wins=13336 maps=13335 fs=0 grant=0 ready=1
```

* **one** `glXSwapBuffers` in the whole run (the splash frame of 28.3);
* **13,336 `XCreateWindow` and 13,335 `XMapWindow` calls** in ~150 s — ~89
  windows per second;
* 13,332 desktop repair requests (`heals`), i.e. one per window, each answered
  with a full-surface blit by this shim;
* the final screenshot is **1,950,392 / 2,073,600 pixels `#000000` (94.1%)**,
  and the only non-black pixels inside the content rect are a 9x13 patch at
  (960,540) — the desktop's mouse pointer.

The X-level trace (`RW_TRACE_X11=1`) shows what one iteration is:

```text
[XC] XCreateWindow   parent=1 geom=1920x1080 depth=24 evmask=0x0
[XC] XSelectInput    id=<container> mask=0x20000 (StructureNotify)
[XC] XMapWindow      id=<container>   -> ESC ] X 1920;1080 -> ESC ] G 2;34;1916;1018
[XC] XReparentWindow win=<gl window> parent=<container>
[XC] XIfEvent        matched type=19 (MapNotify) win=<gl window>
[XC] XRaiseWindow / XSync / XFlush x several
[XC] XReparentWindow win=<gl window> parent=1        <- reverted
[XC] XUnmapWindow    id=<container>
```

That is Unity's borderless-fullscreen dance: it wants a 1920x1080 window, this
desktop can only offer the 1916x1018 content rect of its frame, so the client
creates a container, reparents into it, decides it is not fullscreen, reverts
and retries. Each retry also re-enters `set_geometry`, which re-lays the
surface whenever the reported rect changes — so the one frame that *was*
presented is overwritten within milliseconds and the steady state is a black,
freshly-initialised surface.

Fixed on the way (kept, they are independently correct): `win_clear()` no
longer destroys the live surface when an unrelated window is created or
destroyed (`surf_reset()` does it when the *surface* goes), `XResizeWindow`/
`XMoveResizeWindow` honour a client resize instead of returning 0, and
`XSendEvent` answers the EWMH `_NET_WM_STATE` fullscreen ClientMessage the way a
WM would. **They do not stop the churn**: Unity's check is on its own window's
size after the reparent, which this desktop cannot satisfy without a real WM.

So the named layer for the remaining black frame is **the window-system
emulation (fullscreen), not the renderer and not the present plumbing**: the
present path carries a real 1280x720 frame into the surface and flushes it
(`crc`, `nz`, `box` above), and the client then throws its window away ~89 times
a second before a second frame can be swapped.

### 28.6 Ladder at this tip

| run | banner | `[XSW]` (swap/put/putok/crc/changed) | surface content | `[XHB]` last row | `[XI]` rows | window |
|---|---|---|---|---|---|---|
| 1 | yes | `swap=1 put=1 putok=1 crc=0x940850f19945c95b changed=0` | `nz=921600/921600` (alpha metric, see below) | `pumps=60000 flush=30088 heals=3332 wins=3336` | 9 | 94.1% `#000000` |
| 2 | yes | `swap=1 put=1 putok=1 crc=0x940850f19945c95b changed=0` | `nz=921600/921600` (alpha metric) | `pumps=60000 flush=30088 heals=3332 wins=3336` | 9 | 94.1% `#000000` |
| 3 | yes | `swap=1 put=1 putok=1 crc=0x9e42e911decedfdb changed=0` | `nz=921600/1950488 box=0,0,1279,719` | `pumps=240000 flush=127150 heals=14075 wins=14120` | 83 | 94.1% `#000000` |

Read the table with §28.5 in hand: every run presents **once**, the presented
frame carries a full 1280x720 picture (`box` is the whole frame and every pixel
in it has a non-zero colour channel), the run then creates 3,336-14,120 windows
without swapping again, and the desktop is left with a black content rect.
`nz` in runs 1-2 is the alpha-inclusive form of the metric; it was fixed to
B|G|R after run 2 (`nz=921600/921600` on an opaque black buffer is why), so
run 3 is the row to compare against.

Three runs, one build each (run 3 of the ladder picked up the metric fix; the
instrumented re-run `f5-1` reproduces run 3 exactly — `swap=1`, `
crc=0x9e42e911decedfdb`, `nz=921600/1950488`, `box=0,0,1279,719`).

A startup stall is still in the tree and is NOT understood: 1-2 of 3 runs stop
right after `Initialize engine version` with no `[XHB]` row at all (the event
pump runs fewer than 20 000 times), while the same build reaches the banner in
the other runs. It is the next lane's first measurement; the rows to confirm it
are `[XHB]` (absent = the pump never ran) and `[XE] MAPGEOM` (`polls`/`seq`).

### 28.7 Gates at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   413/0 PASS
bash tools/gate/t3_linux_probe.sh redzone PASS — redzone reached its done marker
bash tools/gate/m2_mono.sh               PASS — Mono 6.12 threaded C# HELLO-OK
python3 tools/gate/m3_window_probe.py    PASS — SDL XCreateWindow ladder + WM_DELETE ClientMessage
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 core triangle PRESENTED in the guest
```

`m4` found a real defect in the first version of patch 0008: its declaration
block sat at the include section, which patch 0005 already owns, so on an
already-patched tree 0005's reverse-dry-run idempotency probe failed and the
build refused to apply it (`Reversed (or previously applied) patch detected`).
The hunk now sits at `glXSwapBuffers` itself and a second consecutive build
reports `patch already applied: 0005` and `0008` — that is the check to repeat
after touching this patch.

### 28.8 Evidence

`obj/rw-desktop/evidence-l39/`:

* `final-{1,2,3}.{serial,Player}.log` + `.summary` — the ladder of §28.6
  (`tools/rw/l39-runs.sh <tag> <n> [run-timeout]` runs it; every row that the
  tables quote is grepped out of these files, never typed);
* `f5-1.*` — the instrumented re-run that reproduces run 3 exactly, including
  the `nz`/`box` fields;
* `final4-*`, `final2-*`, `xibad-1.*`, `trace1-1.*`, `fs1-1.*`, `resize1-1.*`,
  `rep1-1.*` — the runs behind §28.4 and §28.5 (the `XIQueryVersion -> Success`
  stall, the X-level trace of one fullscreen iteration, the `wins=` counter);
* `tools/rw/l39-table.py` — regenerates the §28.6 table from the logs;
* `tools/rw/l39-runs.sh` — the ladder runner (sequential by construction: two
  desktop harnesses in one worktree fight over `disk.img`, rule 5).

## 29. 2026-11-05 — M6/L40: the fullscreen dance is stopped; the answers that mattered were GEOMETRY, not state

§28.5 left one sentence as the frontier: *the window-system emulation
(fullscreen), not the renderer and not the present plumbing*. This section
names the exact answer the client was missing, stops the loop (13,336
`XCreateWindow` → **12** per run), and puts a real RimWorld frame on the
desktop. It also finds that the "startup stall with no `[XHB]`" was **not the
fullscreen dance** but a stale kernel in `disk.img`.

### 29.1 The retry, as a signature

`RW_TRACE_X11=1` on the L39 tree prints one fullscreen iteration in 67 rows.
The shape, with the arguments that repeat, is:

```text
XCreateWindow     parent=1 geom=1920x1080        <- the container, override-redirect
XSelectInput      id=<container> mask=0x20000    <- StructureNotify, container only
XMapWindow        id=<container>                 -> ESC ] X 1920;1080 -> ESC ] G 2;34;1916;1018
XIfEvent          MapNotify(<container>)
XInternAtom       _MOTIF_WM_HINTS (absent)
XReparentWindow   win=<gl> parent=<container>
XIfEvent          MapNotify(<gl>)                <- the reparent's remap
XRaiseWindow <gl> ; XSync x3
XCheckIfEvent     (5x) ; then the *decision*: XCheckIfEvent for the geometry event
XReparentWindow   win=<gl> parent=1              <- reverted: "not fullscreen"
XUnmapWindow <container> ; XIfEvent UnmapNotify ; XDestroyWindow <container>
```

Counted from `trace1-1.serial.log` (L39, 150 s): 13,336 creates, 13,335 maps,
13,332 repairs, **1** `glXSwapBuffers`. The trigger is therefore *not* a
property the client polls and *not* a ClientMessage: it is the answer to the
geometry question it asks immediately after the reparent. On the L40 tree the
same trace shows what it asks, in order — this is the row set to read when a
future client repeats:

```text
XCheckIfEvent matched type=12 (Expose)
XCheckIfEvent matched type=28 (PropertyNotify)
XGetWindowProperty  win=<gl> atom=69 (_NET_WM_STATE)      <- reads the state back
XGetWindowAttributes win=<gl>                             <- the SIZE check
XRaiseWindow <gl> ; XSync x3
XCheckIfEvent matched type=22 (ConfigureNotify) ; XQueryTree win=<gl>  <- the PARENT check
XCheckIfEvent matched type=22 (ConfigureNotify) ; XCheckIfEvent matched type=21 (ReparentNotify)
```

So the client checks three things and needs all three to agree: the state
property it asked for, its window's **size**, and its window's **parent**. A
shim that answers the EWMH `ClientMessage` with a ConfigureNotify carrying the
*content* rect (1916x1018) fails the size check; a shim that never sends a
ConfigureNotify at all fails on the missing event.

### 29.2 Four defects, each with the row that found it

1. **`XCheckIfEvent` only looked at the queue HEAD.** Real Xlib scans the whole
   queue, holding non-matching events aside and re-queueing them (rule 37's
   sibling — that rule fixed `XIfEvent` and left this one). With a
   PropertyNotify in front, a queued ConfigureNotify was invisible, so the
   decision "the WM did not resize me" was made with the event already in the
   queue. Rows now carry `scanned=`; a match behind other events prints
   `[XC] XCheckIfEvent matched type=22 ... scanned=2`.
2. **The container's created size was overwritten by the desktop's content
   rect.** `set_geometry()` writes the rect it got from `ESC ] G` into the
   *window record* of the surface, and the container IS the surface while it
   is mapped — so a container created at 1920x1080 read back as 1916x1018.
   The grant refused every real container:
   `[XE] FSDECL why=4 win=2097154 parent=2097156 pp=1 root=1 cw=1916 ch=1018
   sw=1920 sh=1080 cp=2097156`. Fixed by keeping the client's **requested**
   size (`req_w/req_h`, set by create and `XResizeWindow`) apart from the
   laid-out one.
3. **The reparent got no grant.** A reparent into a screen-sized top-level
   container is a fullscreen request; a WM answers it by resizing the client to
   the screen, setting `_NET_WM_STATE(_NET_WM_STATE_FULLSCREEN)` on the client
   window and sending one ConfigureNotify with the screen geometry. The shim
   did none of that, and an unanswered reparent is exactly the loop. The row is
   one per handshake, bounded, with `rep=` set when the same child is granted
   twice at the same size (a repeat is the client rejecting the answer):
   `[XE] FSGRANT n=1 rep=0 win=2097154 parent=2097156 w=1920 h=1080`.
   The revert half logs `[XE] FSREV`, and a refusal logs `[XE] FSDECL why=`.
4. **Properties were readable only on "the window created last".** A read on
   the ROOT answered `BadWindow`, so `_NET_WORKAREA` — which this client
   interns and reads straight after the reparent — read as "there is no window
   manager here". The root now publishes `_NET_SUPPORTED` (with
   `_NET_WM_STATE`, `_NET_WM_STATE_FULLSCREEN`, `_NET_WM_STATE_ABOVE`),
   `_NET_WORKAREA` = the screen, `_NET_CURRENT_DESKTOP` and
   `_NET_NUMBER_OF_DESKTOPS`, and any live window or the root may read the
   store. Row: `[XE] GETPROP win=1 atom=72 found=1` (72 = `_NET_WORKAREA`).

Two smaller defects were fixed on the way and are independently correct:
`XMapWindow` and a reparent now emit `VisibilityNotify` when the window
selected `VisibilityChangeMask`, `XSetInputFocus` emits `FocusIn`, and
`win_clear()` no longer throws away a **deferred** frame when an unrelated
window is created — which is what dropped the splash's only frame
(`[XE] PUTDEFER 0;0;1280;720` then `[XE] PUTLATE held=1 defer=1280x720`).

### 29.3 Measured result

Same command, 240 s runs, this tip. Churn counts come from the traced runs
(`RW_TRACE_X11=1`, `[XC]` rows); every run also reports its handshake and its
presents:

| run | trace | `XCreateWindow` | `XMapWindow` | `FSGRANT` | `glXSwapBuffers` | final shot |
|---|---|---|---|---|---|---|
| `d1` | no | — | — | `n=1 … w=1920 h=1080` | 1 | `nonblack=65,534 colors=16,589` |
| `d2` | no | — | — | `n=1`, `n=2` (both `rep=0`) | 1 | `nonblack=65,534 colors=16,589` |
| `d3` | yes | **12** | **3** | `n=1`, `n=2` (both `rep=0`) | 1 | `nonblack=65,534 colors=16,589` |
| `d4` | yes | **12** | **3** | `n=1`, `n=2` (both `rep=0`) | 1 | `nonblack=65,534 colors=16,589` |
| L39 §28.5-28.6 | yes | 3,336-14,120 | 3,335-14,120 | `grant=0` | 1 | 94.1 % `#000000` |

The `[XSW]` row is the same in every run:

```text
[XSW] n=1 swap=1 put=1 putok=1 flush=83 crc=0x940850f19945c95b changed=0
      w=1280 h=720 ready=0 nz=921600/921600 box=0,0,1279,719 copy=0 glflush=15
```

`ready=0` is the splash swapping before the desktop's `ESC ] G` arrives; the
frame is kept (`[XE] PUTDEFER 0;0;1280;720`) and presented by the next
`set_geometry` (`[XE] PUTLATE held=1 x=2 y=34 w=1916 h=1018 defer=1280x720`),
which is why the desktop shows the picture from the 4th screenshot on.


The screenshot is **RimWorld's loading screen**: a brown-orange planet with
polar ice and scattered water, a white fighter and a boxier orange cargo ship,
a small moon at the lower right, a dark box with three white dots ("…") in the
middle of the planet, inside a desktop window titled `RIMWORLD`. That is a
game frame, not the launcher's stdout window (rule 61).

The dance is gone in every run of this tip: `FSGRANT` totals 1-2 per run (one
for the splash window, sometimes one for the game window), `rep=0` every time,
and the create/map counters stay at 12/3 instead of climbing at ~89 Hz.

### 29.4 Route A was already in place and is not enough

`-force-glcore -screen-width 1280 -screen-height 720 -screen-fullscreen 0`
has been in `rwlaunch`'s argv since `eda232bc`, and the game does read it:

```text
Command line arguments: -force-glcore -screen-width 1280 -screen-height 720 -screen-fullscreen 0 -logFile /RW_HOME/Player.log
requesting resize 1280 x 720
resizing window to 1280 x 720
Using native desktop resolution 1920 x 1080
requesting fullscreen 1920 x 1080 at 0/1 Hz
```

So the documented mode arguments work and are honoured for the *windowed*
window, but RimWorld then makes its own fullscreen call ("using native desktop
resolution"), so the argv route cannot avoid the transition — the game simply
wants the whole screen. The transition was the thing that had to work.

### 29.5 The startup stall was a STALE KERNEL, and it is not the dance

Three runs in this lane produced `Assertion at
../../mono/arch/amd64/../x86/x86-codegen.h:410, condition 'offset ==
(gint32)offset' not met` (two of them) and `tramp-amd64.c:641` (the seed's own
run) — i.e. **both** JIT assertions that §27 closed — with no `[XHB]` at all.
The cause was not the code: the seeded `disk.img` carried a **pre-L38 kernel**
(`strings` of `::/boot/hobbyos.elf` matched `JITA` **0** times while the tree's
`hobbyos.elf` matched it twice). `RW_SKIP_BUILD=1` re-stages the DSOs but not
`/boot`, so every run booted the old kernel. After rebuilding the image
(`TOOLS_SKIP=1 make -j8 ARCH=intel MODE=desktop DESKTOP=hobby hobbyos.elf
disk.img`, then proving `JITA` is in `::/boot/hobbyos.elf`), the banner comes
back 4 of 4 with `- Loaded All Assemblies, in 9.2-11.1 seconds`.

So the answer to §28's open question is: the "no `[XHB]`" runs are **process
deaths**, and the row that names them is the Mono assertion line, which only
reaches the console because the abort flushes the block buffer. It does not
correlate with the fullscreen dance: `d1`/`c3` reach the loading screen with no
assertion and still have no `[XHB]` row (see 29.6).

### 29.6 What still does not happen

One `glXSwapBuffers` per run, and after the last `XQueryTree` the client makes
**no further X call at all** — `XCreateWindow` 12, and then nothing. The guest
goes idle at ~30 s (`[WATCHDOG] console silent` 8 times from 30 s to 240 s) and
`[NFSIO] bytes` freezes at 19,903,177 (0x130B4C9), so it is not asset loading:
the process parks with the loading screen on screen and the window handshake
complete.

That is the next lane's target, and it is **not** the window system: the sizes
(`XGetWindowAttributes` → 1920x1080 at 0,0), the tree (`XQueryTree` → the
container), the state property (read back and found), the ConfigureNotify and
the ReparentNotify were all consumed before the client stopped. The row to
start from is the last block of `[XC]` rows in `d1`/`c3`: everything the client
asked was answered, and then it stopped asking.

### 29.7 Gates at this tip

```text
TOOLS_SKIP=1 ./run_unit_tests_intel.sh   413/0 PASS
bash tools/gate/t3_linux_probe.sh redzone PASS — redzone reached its done marker
bash tools/gate/m2_mono.sh               PASS — Mono 6.12 threaded C# HELLO-OK
python3 tools/gate/m3_window_probe.py    PASS — SDL XCreateWindow ladder + WM_DELETE ClientMessage
python3 tools/gate/m4_gl_probe.py        PASS — GL 3.3 core triangle PRESENTED in the guest
```

Also measured in this lane: the Mesa DSO the harness stages must be *checked*,
not assumed — the seeded `obj/mesa/lib/libGL.so.1` predated patch 0008, so
`XcPresentMark` was an undefined symbol in **no** build (`readelf --dyn-syms`
showed no such import) and `[XSW]` could not appear at all. Rebuilding Mesa
(`MESA_PYTHON=<a python with mako> bash tools/mesa/build-hobbyos.sh`, 43 s with
the TU cache warm) puts `UND XcPresentMark` back in `--dyn-syms`.

### 29.8 Evidence

`obj/rw-desktop/evidence-l40/`:

* `d1.{Player,serial}.log` + `d1.final.png` — first 240 s run of 29.3: the
  handshake, one `[XSW]` row and the loading screen, with no trace DSO;
* `d3.*`, `d4.*` — the same run with `RW_TRACE_X11=1`: the `[XC]` counts of
  29.3 and the X-level order of 29.1 (`XCheckIfEvent matched … scanned=`), plus
  the two `[XE] FSGRANT` rows;
* `c2.harness.log`, `c3.harness.log`, `d1..d4.harness.log` — the harness
  output of every run (screenshot sizes/colour counts per shot);
* `obj/rw-desktop/l40/a3.harness.log`, `a4.harness.log`, `a5.harness.log` — the
  stale-kernel runs of 29.5 (`Assertion at …x86-codegen.h:410`; then the banner
  after the rebuild);
* `tools/rw/l40-summary.py <Player.log> [serial.log]` — regenerates every
  number in this section (churn counts, `[XHB]`, `[XSW]`, handshake rows, the
  per-iteration signature), so none of them is typed by hand;
* `tools/rw/l40-runs.sh <tag> <n> [run-timeout]` — the ladder runner
  (sequential by construction: two desktop harnesses in one worktree fight over
  `disk.img`, rule 5).

## 30. 2026-11-05 — M6/L42: the park is Mono's ThreadPool, not the joiner

§30 answers L41's open question with a minimal reproducer rather than another
theory. The managed wait is real (`mono_wait_uninterrupted` ->
`mono_os_cond_timedwait`), but the waiter's signal word is **0**: no `Set()`
ever ran. The signaller is a `ThreadPool.QueueUserWorkItem` callback, and
Mono's ThreadPool does not execute work items in this guest at all.

### 30.1 The managed side, as far as it can be named

- The parked object is a `MonoW32Handle` at `key-0x38`: `signal_status` at
  `+0xc` is **0**, `signal_mutex` at `+0x10`, `signal_cond` at `+0x38`
  (`mono_w32handle_set` writes `1` to `+0xc` then broadcasts `+0x38`).
  `status=0` with the waiter parked means the setter never ran; it is not a
  lost broadcast.
- `Loading.Preload` (pid 27) waits in `mono_os_event_wait_multiple`, whose
  per-waiter `pthread_cond_t` is allocated **on the waiter's own stack**
  (`mono_os_event_wait_multiple` builds it at `0x38(%rsp)`): the key sits
  next to the thread-name string in the 16 MiB anon region. It is a Mono
  event wait, not a raw Unity semaphore.
- Mono's own SIGQUIT dump was attempted (`/SIGQUITON` -> `kill(pid,3)`):
  `Full thread dump:` appears in Player.log, then Mono aborts the Finalizer
  with `Cannot transition thread ... from RUNNING with FINISH_ASYNC_SUSPEND`.
  The dump machinery is unusable here; the CFI backtrace + the w32handle
  status word are the managed-side evidence instead.

### 30.2 The reproducer: Mono ThreadPool never runs a work item

`tools/rw/mono-probe/tp.cs` runs under the guest's own Mono 6.12:

```text
TP START cpus=8
TP phase before-join
TP TIMEOUT parallel done=0
TP OK join v=7
TP phase after-join
TP TIMEOUT parallel done=0   (x6)
```

`Thread.Join` works. `ThreadPool.QueueUserWorkItem` + `AutoResetEvent.Set()`
never runs a single callback: `done=0` after 5 s, every round, **before any
managed Thread.Join**. `[TEXIT]` shows the workers being created and exiting
with `st=3` (RUNNING), `key=0x0` — they never park and never execute a work
item. That is exactly RimWorld's `Verse.GenThreading.ParallelFor` shape
(AutoResetEvent + one work item per slice + `WaitOne` loop), so the game parks
in the loading screen with the event status word still 0.

A/B controls that did not move the park: `GC_DONT_GC=1`, and
`MONO_THREADS_PER_CPU=50` (the threadpool's worker ceiling). The stall byte
count is identical in every run (`0x130B4C9` = 19,903,177), so this is a
deterministic missing signal, not a timing race.

### 30.3 The join contract itself matches Linux

`src/user/linux_probe_join.c` (host glibc vs guest compat, same static-PIE
source) covers park-first join, wake-before-wait, join-after-exit and
CLONE_CHILD_CLEARTID; all four graded rows agree with the host (see
`obj/linux_probe_join.log`). `[TEXIT]` rows show every exiting compat thread
with `touch=0`, `pre=<tid>`, and `woke=1` whenever a joiner was parked on its
clear word. So pthread_join/clear_child_tid is **not** the lost-wakeup site;
the missing signal is one level above it, in Mono's threadpool.

### 30.4 Ladder at this tip (3 runs, 110 s each)

| run | NFS bytes | main (pid 4) | Preload (pid 27) | screenshot |
|---|---|---|---|---|
| r1 | `0x130B4C9` (19,903,177) | futex, `hs=0`, waits=208/wakes=117 | futex, waits=13/wakes=0 | loading screen |
| r2 | `0x130B4C9` | futex, `hs=0`, 216/115 | 13/0 | loading screen |
| r3 | `0x130B4C9` | futex, `hs=0`, 206/117 | 13/0 | loading screen |

All three runs are the same park, byte for byte — the deterministic signature
of a missing signal, not a race. The ladder did **not** pass the loading
screen: the blocker is the ThreadPool reproducer of 30.2, which is the next
lane's target. The fixes this lane ships are diagnostic (the `[TEXIT]` rows,
the syscall ring, the w32handle status word, `/SIGQUITON`, `/TPTRACE`,
`/NOGC`, `/TPCPU`) plus the probe `linux_probe_join` and the managed
reproducer `tools/rw/mono-probe/tp.cs`.

Gates at this tip: unit `413/0`; `t3_linux_probe join` host/guest rows agree
for A-D; `redzone`, `m2_threads`, `m12`, `m2_mono`, `m3_window_probe`,
`m4_gl_probe` PASS.

## 31. 2026-11-05 — M6/L43: `gettimeofday` was uptime, so every Mono threadpool park expired 56 years early

§30 named the park as Mono's ThreadPool on the strength of `tp.cs` under the
guest's official Mono 6.12 (`mono-sgen`). Both halves of that claim needed
re-measuring, and one of them was wrong:

- The **reproducer was invalid as evidence about this kernel**. The official
  `mono-sgen` is a monolithic build that does not export four symbols
  `libmono-native.so` imports (`mono_add_internal_call_with_flags`,
  `mono_marshal_set_last_error`, `mono_threads_enter_gc_safe_region_internal`,
  `mono_threads_exit_gc_safe_region_internal`), so `System.Native` cannot load.
  Every threadpool worker then dies in
  `ThreadPoolWorkQueueThreadLocals..ctor` -> `new Random()` -> `Interop.Sys`,
  and the probe prints `TP TIMEOUT parallel done=0`. Measured on the **host**,
  with the same binary and the same mscorlib: identical failure. A probe that
  fails identically off-kernel cannot name a kernel defect.
- The **underlying defect was real but is a clock bug, not a ThreadPool bug**:
  `gettimeofday(2)` answered **uptime** while `clock_gettime(CLOCK_REALTIME)`
  answered the boot-anchored RTC epoch. Mono's `mono_os_sem_timedwait()`
  builds an ABSOLUTE deadline as `gettimeofday() + timeout_ms` and passes it to
  `sem_timedwait()`, which glibc implements as
  `futex(FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, &deadline)`. The kernel
  compares that against `hb_clock_realtime_ms()`, so a 5..60 s worker park was
  a deadline ~1.79e9 s in the past -- `-ETIMEDOUT` on the first futex call.
  `worker_park()` returns "timed out", `worker_thread` sets
  `worker_timed_out = TRUE` and `break`s: the worker never parks and exits.
  That is exactly §30's `[TEXIT] ... st=3 key=0x0`.

### 31.1 The clock contract, host vs guest (one static-PIE probe)

`src/user/linux_probe_clock.c` / `tools/gate/t3_linux_probe.sh clock` asks the
raw syscalls (musl implements `gettimeofday()` on top of `clock_gettime()`, so
the libc wrapper cannot see a broken `gettimeofday`):

| row | host | guest, before | guest, after |
|---|---|---|---|
| `raw_gettimeofday sec` | 1,793,891,631 | **3** | 1,793,891,693 |
| `raw_realtime sec` | 1,793,891,631 | 1,793,891,651 | 1,793,891,693 |
| `delta_gettimeofday_realtime` | 0 | **-1,793,891,648** | 0 |
| `sem_timedwait` deadline = `gettimeofday()+50 ms` | 50 ms, ETIMEDOUT | **0 ms**, ETIMEDOUT | 58 ms, ETIMEDOUT |
| `sem_timedwait` deadline = `CLOCK_REALTIME+50 ms` | 50 ms, ETIMEDOUT | 55 ms, ETIMEDOUT | 59 ms, ETIMEDOUT |
| `pthread_cond_timedwait` 50 ms | 50 ms, ETIMEDOUT | 56 ms | 55 ms |
| `nanosleep` 50 ms | 50 ms | 58 ms | 60 ms |
| `pthread_cond_timedwait` 5 s | 5000 ms | 5009 ms | 5009 ms |

The two `sem_timedwait` rows are the whole diagnosis: the *same* 50 ms wait is
instant when its absolute deadline is built from `gettimeofday` and correct
when it is built from `CLOCK_REALTIME`. `cond` and `nanosleep` are controls --
they were already right, which is why only semaphore-based parks broke.

### 31.2 The fix

`gettimeofday` is `CLOCK_REALTIME`, so it must answer the same clock `time(2)`
already did:

- `src/kernel/linux_compat.c` `l_core_gettimeofday()`:
  `timer_get_ms()` -> `hb_clock_realtime_ms()`.
- `src/kernel/arch/x64/trap.c` raw `case 100` (native/non-table path): the same
  change, and both `timeval` words are now written as 64-bit (the legacy arm
  wrote `tv_sec` as 32-bit and left `tv_usec` untouched).

Nothing else in the kernel changed. `clock_gettime` was already correct for
every clock the target uses (`CLOCK_REALTIME` 0, `MONOTONIC` 1, `RAW` 4,
`COARSE` 6, `BOOTTIME` 7, `REALTIME_COARSE` 5).

### 31.3 A probe that tests the game's runtime, not mono-sgen

`tools/rw/mono-probe/mono_main_launcher.c` `dlopen`s
`MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so` (the exact runtime Unity runs)
with `RTLD_GLOBAL` and calls `mono_main`, after `mono_set_dirs` and
`MONO_PATH`. `System.Native` resolves against that same runtime, so the probe
exercises the threadpool the game actually uses. `tools/rw/run-tp-probe.sh`
stages glibc 2.31 + the game's runtime/mscorlib/config and boots it as a
MODE=test wave.

`tp.cs` under it, host and guest (both after the fix):

```text
TP START cpus=32|8
TP phase before-join
TP OK parallel done=8
TP OK join v=7
TP phase after-join
TP OK parallel done=8   (x6)
TP-OK
```

Under `mono-sgen`, host **and** guest print `TP TIMEOUT parallel done=0` x7 with
`Unhandled Exception: ... DllNotFoundException: .../libmono-native.so`. The
L42 sequence is a property of that binary, not of HobbyOS.

With `/TPTRACE` staged the same probe also proves the *parking* half, which is
what the clock bug broke (counts from one guest run):

```text
worker starting 40   worker parking 39   worker unparking 39
worker timed out 39  worker executing 104              TP-OK
```

Before the fix every one of those 39 parks returned "timed out" on its first
futex call, so the count was `worker parking 0`, `worker timed out 0` and the
worker exited after its first callback.

### 31.4 The ThreadPool was never dead -- and worker exit is normal

With `/TPTRACE` staged, a desktop run shows the pool really running:

```text
Mono: [0x...] try create worker, created 0x..., now = 34 count = 1
Mono: [0x...] worker starting
Mono: [0x...] worker executing
Mono: [0x...] worker running in domain 0x11100c2d80 (outstanding requests 1)
```

Four workers enter the managed `ThreadPoolWorkQueue.Dispatch()` and stay there
(it is an infinite loop; it does not return "worker finishing" until shutdown).
`[TEXIT] ... Thread Pool Worker` rows are the hill climber retiring an idle
worker after its randomized 5..60 s park -- normal on Linux too. Neither row
says anything about a work item being stranded.

### 31.5 What still parks the game

The ladder is unchanged: all three runs read exactly `0x130B4C9` = 19,903,177
bytes of 729 MiB and stop (table below). The thread state at the stop is *not*
a signaller that never ran in Mono's pool:

| pid | name | state | waits/wakes | note |
|---|---|---|---|---|
| 4 | main | futex, `to=4294967295` | 1249/115 | wakes repeatedly, re-parks |
| 27 | `Loading.Preload` | futex, `to=4294967295` | **13/0** | the only thread nobody wakes |
| 25 | `Loading.AsyncRe` | futex | 19/18 | healthy ping-pong |
| 26 | `UnityGfxDeviceW` | futex | 271/270 | healthy ping-pong |
| 29..32 | `Thread Pool Wor` | 2 futex (0x1084AD5278) + 2 blocked | 14/12 | inside managed Dispatch |

The waiting object at `0x10D08BAA00` is a per-thread structure -- the words
below the key hold the inline ASCII name `Loading.PreloadManager` -- i.e.
`MonoThreadInfo::wait_cond`, the object rule 91 already resolved to
`mono_wait_uninterrupted`. So the strand is one waiter on
`MonoThreadInfo::wait_cond` that nobody signals, while the pool it was
suspected of waiting on is alive and its workers park. The next lane's first
question is therefore *which park on `MonoThreadInfo::wait_cond` this is and
who is supposed to `mono_thread_info_install_interrupt()` it*, not "does the
ThreadPool run".

### 31.6 Ladder at this tip (3 runs, 150 s each)

| run | NFS bytes | rpcs | `[XSW]` | screenshot |
|---|---|---|---|---|
| r1 | `0x130B4C9` (19,903,177) | `0x3B2D` | 0 | loading screen |
| r2 | `0x130B4C9` | `0x3B2D` | 0 | loading screen |
| r3 | `0x130B4C9` | `0x3B2D` | 0 | loading screen |

The kernel inside `disk.img` was proved to be this build (`mcopy ::/boot/hobbyos.elf`
md5 == `obj/intel/hobbyos.boot.elf`). No `[XSW]` row appears at all, which per
rule 89 means the staged `libGL.so.1` has no `XcPresentMark` probe, not that the
client stopped swapping.

### 31.7 Gates

`m2_tp.sh` is the new regression: it builds `tp.cs` against the game's
`mscorlib`, runs it under the game's runtime in the guest with `/TPTRACE` and
requires 7x `TP OK parallel` + `TP OK join` + `TP-OK`, zero `TP TIMEOUT`, and
at least one `worker parking`/`worker unparking` pair; then it runs
`t3_linux_probe.sh clock` and requires `[LP] done rc=0`. The old `m2_mono.sh tp`
mode (mono-sgen) is not a gate for this: it cannot distinguish a kernel defect
from its own missing `System.Native`.

Regression at this tip: unit `413/0`; `redzone`, `m2_mono`, `m2_threads`,
`m12`, `m3_window_probe`, `m4_gl_probe` PASS; `m2_tp` PASS.
