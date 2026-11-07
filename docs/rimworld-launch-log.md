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

## 32. 2026-11-05 — M6/L44: the stranded park is Unity's, not Mono's; the joiner is the stall

§31 left one row unexplained: pid 27 `Loading.Preload`, **13 parks / 0 wakes**, on a
per-thread object whose inline name is `Loading.PreloadManager`, read as
`MonoThreadInfo::wait_cond`. Two things were wrong with that reading, and both
are decidable from the object itself.

### 32.1 The park: Unity's PreloadManager semaphore, not Mono

The `[FUTEX]` dump prints the key's owner (`at = key-0x38`) and the user stack.
For pid 27 in a fresh run at this tip:

```text
[FUTEX] q slot=27 pid=27 st=8 key=0x10D08BAA00 waits=13 wakes=0 sc=202 hs=0 Loading.Preload
[FUTEX] h pid=27 at=0x10D08BA9C8 0x200000001 0x40000 0x2E676E6964616F4C 0x4D64616F6C657250
                                 0x726567616E61 0x2 0xE00000001 0x0 0x0 0xFFFFFFFF 0x0 0x0
[FUTEX] st pid=27 sp=0x1142A6AD98 0x1082BE84AE ... word=0x0
```

- The ASCII is at **key-0x28**: `0x2E676E6964616F4C` = `Loading.`, `0x4D64616F6C657250`
  = `PreloadM`, `0x726567616E61` = `anager`. That is a **thread name buffer inside
  the Unity thread object**, not a MonoThreadInfo (mono 6.12's struct has no name
  field and no `wait_cond` at all; upstream `mono_thread_info_install_interrupt`
  only CASes `info->interrupt_token` at `+0x478` and wakes nothing).
- `[rsp] = 0x1082BE84AE` is `UnityPlayer+0x18E94AE`: the instruction after
  `call syscall@plt` in UnityPlayer's own futex helper at `+0x18E9460`
  (`edi=0xca`, `edx=0x80` = `FUTEX_WAIT|FUTEX_PRIVATE_FLAG`, timeout from the
  caller). The caller frame `0x1081F0F56C` is `UnityPlayer+0xC1056C`, i.e. the
  loop at `+0xC1055D`: `if (*(int*)r14 <= 0) futex_wait(r14, 0, -1)` then
  CAS-decrement. **It is a "wait until the counter is positive, then take one"
  semaphore with an infinite timeout** — Unity's PreloadManager job gate.
- `word=0x0`: the counter is zero and no producer ever incremented it. The
  wake count is 0 because **no POST happened**, not because a pulse was lost.
  The thread is idle: it is the consumer waiting for a preload job.

So the park is named, and its unblocker is a producer: the Unity thread that
posts preload work (`Loading.AsyncRead`/the main thread's preload queue). It is
not a Mono interrupt, not `mono_thread_info_install_interrupt`, and not a
managed `Pulse`.

### 32.2 The thread that decides the stall is the main thread, in a managed wait

The same dump shows the process main thread (pid 4) parked on a **heap** key
(`0x105A117368` in one run, `0x104D8039E8` in another) with an infinite timeout
and `waits=215 wakes=113` frozen across every later dump. `[FUTEX] bt` finds the
Mono chain `mono_os_cond_timedwait+0xAF` <- `mono_w32handle_timedwait_signal_handle`
<- `mono_w32handle_wait_one(+0x101)` <- `mono_join_uninterrupted` (a faithful
`while (ret == ALERTED) ret = wait_one(...)` loop), and `[PARK]` rows show the
same thread's earlier waits being woken by pids 30/31/32/34 (`[FUTEX] wake4`),
so the machinery works and the last wait's signaller simply never ran.

The managed caller is not a guess: the game's own assemblies are decompilable
from `/srv/nfs/rimworld/RimWorldLinux_Data/Managed`. The only `Thread.Join()` in
`Assembly-CSharp` is `Verse/DirectXmlLoader.cs:80` (`XmlAssetsInModFolder`
spawns two `DirectXmlLoader Thread N of 2` workers, drains the same queue on
the calling thread, then joins both). Above it, `PlayDataLoader.DoPlayLoad`
runs `Parallel.ForEach(typeof(Def).AllSubclasses(), ...)` and each iteration
enters `DefDatabase<T>.AddAllInMods` -> `Verse/GenThreading.cs:110/142`,
`ParallelForEach`:

```csharp
AutoResetEvent taskDoneEvent = new AutoResetEvent(false);
foreach (batch) ThreadPool.QueueUserWorkItem(_ => { ...; taskDoneEvent.Set(); });
while (Interlocked.Read(ref tasksDone) < count) taskDoneEvent.WaitOne();
```

`taskDoneEvent.WaitOne()` is `mono_w32handle_wait_one(AutoResetEvent, -1, TRUE)`
on a heap `signal_cond` — exactly the park in the dump. The unblocker is
`taskDoneEvent.Set()` in the work item, i.e. **Mono's ThreadPool must run an item
queued from inside a pool worker while every worker is blocked in the same
nested wait.**

### 32.3 What the rows say, in one line each

| row | meaning |
|---|---|
| pid 27 `Loading.Preload` 13/0 on a Unity private-futex semaphore | idle consumer; not the stall |
| pid 4 infinite park on `AutoResetEvent.signal_cond` (`wakes` frozen) | `GenThreading.ParallelForEach`'s done-event |
| `[FUTEX] wake4` rows by 30/31/32/34 then none | the same wait works until workers starve |
| 19 `mono_thread_internal_set_priority` rows at the end of `Player.log` | pool workers being created in the last second |
| `[XSW] n=1 crc=0x940850f19945c95b changed=0` | one frame presented, then nothing |

The UI agrees: the last frame is RimWorld's loading art with the animated `...`
box (`LoadingDefs` is the event text `PlayDataLoader` sets immediately before
the nested parallel work), frozen.

### 32.4 Ladder at this tip (150 s and 60..75 s runs)

| run | NFS bytes | `[XSW]` | screenshot |
|---|---|---|---|
| r1 (150 s) | `0x130B4C9` = 19,903,177 | n=1, crc `0x9408…c95b`, changed=0 | loading screen, static |
| r3 (75 s) | `0x130B4C9` | n=1 | static |
| r4 (70 s) | `0x130B4C9` | n=1 | static |

Byte-for-byte the same read boundary as §31.6, but for a different reason: the
game now presents a frame, and the stop is a ThreadPool starvation inside
RimWorld's nested parallelism, not a clock or a lost signal.

### 32.5 Instrumentation added (kept)

`src/kernel/linux_compat_thr.c`: a `[PARK]` one-shot row for pid 4/27 infinite
parks (key, expected value, `tf->elr`, the three stack words at the park); a
`[FUTEX] dstk`/`obj` pair (32 raw stack words and 24 raw words from the key) and
a 1024-word/10-hit `bt` scan for those two pids. `tools/rw/mono-probe/tp_starve.cs`
is the guest/host reproducer for the nested-parallelism shape.

### 32.6 Gates

unit `413/0` at this tip. `m2_tp` PASS (ThreadPool parks and wakes), `m2_threads`
PASS, `redzone`, `m2_mono`, `m12`, `m3_window_probe`, `m4_gl_probe` PASS.

## 33. 2026-11-05 — M6/L46: Boehm's suspend set was short by one, because a `tgkill` to the main thread could be claimed by any thread

§32 left the stop-the-world as the thing to explain: `world_stopped=1
stop_count=44` for the rest of a run, 4-6 threads parked in
`GC_suspend_handler_inner`'s `sigsuspend`, and a collector sleeping in
`sem_wait`. The hand-off said Boehm's live set was **one greater** than the
number of `SIGPWR`s that reached their targets. That is exactly what the rows
show, and the missing delivery is a kernel routing bug, not a Mono bug.

### 33.1 The oracle first: `tp_starve` died at `GC_lookup_thread()==NULL`

`tools/rw/mono-probe/tp_starve.cs` under the game's own runtime, *before* any
kernel change (MODE=test, `[LCOMPAT]`/`[SGS]`/`[SIGD]` rows):

```text
[LCOMPAT] clone flags=0x3D0F00 stack=0x10C1C2CFB0 tls=0x10C1C2D700 -> 5
[SGS] pid=1 sig=30 st=8 susp=0 inh=0 pend=0x20000000 mask=0x0
[TGTK] op=tgkill tgid=2 tid=1 sig=30 st=2 th=1 ptgid=2 gen=1 rc=0
[SGS] pid=3 sig=30 st=8 susp=0 inh=0 pend=0x20000000 mask=0x0
[TGTK] op=tgkill tgid=2 tid=3 sig=30 st=2 th=1 ptgid=2 gen=1 rc=0
[SGS] pid=2 sig=30 st=8 susp=0 inh=0 pend=0x20000000 mask=0x0
[TGTK] op=tgkill tgid=2 tid=2 sig=30 st=2 th=0 ptgid=2 gen=0 rc=0
[SIGD] pid=1 sig=30 ...
[SIGD] pid=3 sig=30 ...
[SIGD] pid=5 sig=30 ...          <- no [SGS] pid=5: nobody signalled pid 5
[KERNEL] pid=5 (MONO.EXE) memory fault VA=0x0000000000000020
         PC=0x0000001080478793 rw=r in=HOLE region=NONE
         rbx=0x0 rdi=0x1 ... ret=0x1080478706 -> killed
```

`0x1080478793 - 0x27c793 = 0x10801FC000` (the DSO base), i.e.
`GC_suspend_handler_inner+0x6F`, the `cmp QWORD PTR [rax],0x0` after
`lea rax,[rbx+0x20]`; `rbx = 0` is `GC_lookup_thread(pthread_self())` returning
NULL, and `ret=base+0x27c706` is the instruction after `call
GC_suspend_handler_inner` in `GC_suspend_handler`. **pid 5 took a `SIGPWR` it
was never sent.**

### 33.2 The tid diff: counted, signalled, delivered, and the thread in between

`[GCT]` (new, §33.5) walks Boehm's 256 `GC_threads` buckets with
`GC_suspend_all`'s own filters and matches each entry to our PCB by TCB
(`clear_child_tid - 0x2d0`, i.e. `&pd->tid`; `pthread_kill` reads
`[pthread_t+0x2d0]`, disassembled in libpthread-2.31). For the run above:

| Boehm entry (`GC_threads`) | flags `+0x28` | `scat +0x10` | in the live set? | SIGPWR planted | delivered to it | our PCB state |
|---|---|---|---|---|---|---|
| `id=pid 1` | `0x0` | `0` | yes | `[SGS] pid=1` rc=0 | `[SIGD] pid=1` | `st=8` FUTEX |
| `id=pid 3` | `0x0` | `0` | yes | `[SGS] pid=3` rc=0 | `[SIGD] pid=3` | `st=8` FUTEX |
| `id=pid 2` (main) | `0x0` | `0` | yes | `[SGS] pid=2` rc=0, `thr=0` | **none** | `st=8` FUTEX (`mono_thread_create_internal`'s wait) |
| — no entry — | — | — | no | no | `[SIGD] pid=5` | `st=2` READY, clone 3 ms earlier, not registered with the GC |

3 counted, 3 planted, **2 delivered**: the third `SIGPWR` was recorded in the
group's *process-directed* set (the anchor's `sig_pending`) and pid 5 — a
thread that was not in Boehm's set at all — claimed it at its first trap exit.

### 33.3 Linux semantics: neither the exit nor the create rule was violated — the DIRECTION was

The two candidates in the hand-off were:

* **exit**: a thread that leaves must be dropped from a would-be suspend set
  atomically with its exit. Our side already does this: `process_signal()`
  refuses `FREE`/`EXITED`/`THREAD_DONE` and answers `-ESRCH`, and
  `GC_suspend_all` counts `rc == 3` as "skip" (`cmp $0x3,%eax; je skip`).
  Row: a `THREAD_DONE` pid answers `[SIGIN] miss ... -> ESRCH`, and the
  unit test `test_p5_directed_signal_owner` pins it.
* **create**: a thread must be registered with the runtime before it can run
  managed code. That is Mono's contract (`GC_register_my_thread` is the first
  thing a Mono thread does), and the kernel cannot check it — pid 5 running
  before registering is legal Linux.

What *is* a Linux rule, and what we violated, is the third thing: **a signal
that names a tid is queued on that task and no other thread of the group may
take it.** `tgkill(tgid, tid=main, SIGPWR)` while the main thread is blocked in
a futex must stay pending on the main thread. Our `process_signal()` stored it
in `sig_pending` on the group anchor — the same word process-directed signals
use, which `linux_compat_signal_deliver()` merges into every member's candidate
set (`pend |= grp->sig_pending`) — so the first thread to trap won it, and
`GC_suspend_all`'s count (which includes the main thread) could never be
satisfied. In the wedge that is the whole story: the collector waits for an ack
the main thread can no longer give, the world stays stopped, and every worker
that later enters `GC_suspend_handler_inner` parks in `sigsuspend` for the rest
of the run.

### 33.4 The fix

`struct process` gains `uint64_t sig_pending_dir` — the thread-directed set.
`process_signal_directed(pid, sig, directed)` records a bit there when the
caller named a tid (`tgkill`/`tkill`, or a `kill(2)` whose pid is a thread),
and in `sig_pending` otherwise; `process_signal()` is the `kill(2)`-shaped
front door. Delivery, park-wake, `sigpending`, `rt_sigsuspend`'s pre-check and
the trap-exit fast path all read both sets; only the addressed PCB can consume
a directed bit. Native targets are untouched (`sig_pending_dir` is never set
for them).

### 33.5 Instrumentation: `[GCT]`, and what a steady state looks like

`[GCST]` now also prints the ack semaphore's waiter word and the group's
`live_threads`; a new `[GCT]` block walks `GC_threads` and prints one summary
plus a row per unaccounted entry (`ent/live/pcb/unreg/norec/self/fin/b29/susp/
epoch/anchor`). Two shapes, measured:

```text
# no stop in progress (desktop, steady state)
[GCST] world_stopped=0 stop_count=44 ack_sem=0 ackw=0 grplive=25
[GCT] sum ent=15 live=0 pcb=25 unreg=21 norec=11 self=1 ... epoch=3 anchor=8
# during a stop (tp_starve, stop=16; anchor st=8 == the collector)
[GCST] world_stopped=1 stop_count=16 ack_sem=0 ackw=0 grplive=12
[GCT] sum ent=12 live=11 pcb=12 unreg=0 norec=1 ... epoch=0
```

`unreg=21` is correct and worth stating: Unity's "Background Job." threads are
native pthreads that never call `GC_register_my_thread`, so they are in our PCB
table and not in Boehm's set. `live=11` against 12 tasks (11 thread PCBs + the
anchor) is the invariant: the collector is the anchor, which
`GC_suspend_all` skips by design, so the plant set is `pcb - 1`.

### 33.6 `tp_starve` on guest and host, 10 iterations

Host reference (`mono_main_launcher` + the game's `libmonobdwgc`, native
Linux): `STARVE-OK`. Guest, 10 consecutive boots:

| run | result | outer rings | timeout | faults | stolen `[SIGD]` (no `[SGS]` for that pid) |
|---|---|---|---|---|---|
| 1-10 | `STARVE-OK` (10/10) | 8/8 each | 0 | 0 | **0** |

"Stolen" is computed per log: the set of pids with a `[SIGD] ... sig=30` row
minus the set with a `[SGS] ... sig=30` row. Before the fix that set was
`{5}` and the process died; after it is empty in all ten.

### 33.7 The desktop ladder, 3 runs

| run | `world_stopped=1` dumps | `[GCT]` at the end | NFS bytes | Player.log | screen |
|---|---|---|---|---|---|
| r1 | 0 | `live=0 pcb=25` | `0x130B4C9` = 19,903,177 | `Loaded All Assemblies, in 11.298 s`, `RimWorld 1.6.4871 rev600` | loading art, animated |
| r2 | 0 | `live=0 pcb=25` | 19,903,177 | `... in 11.557 s`, banner | loading art, animated |
| r3 | 0 | `live=0 pcb=25` | 19,903,177 | `... in 14.479 s`, banner | loading art, animated (launcher exited before the last two shots) |

The stop-the-world now completes in every run: `world_stopped=0` and `live=0`
in every `[GCST]` row of all three runs, `stop_count` advancing to 44 and
staying there, with `[FWK] ... why=sigsuspend` / `why=futex-sa-restart`
pairs for every pid. `Loaded All Assemblies` fell from §32's 32.6 s to
11.3-14.5 s, which is what a stop-the-world that actually restarts looks like.

The frame is real and it is *moving*: `01..22_running.ppm` all differ
(SHA-1 per frame; 4..56 pixels change between shots, the `...` box), and from
shot 04 on every frame carries 1,044,712 non-black pixels over 1,280 coarse
colours. `[XE] PUT 0;0;1280;720` is the present row; **`[XSW]` does not fire in
this ladder** (0 rows in all three runs), which is its own open question —
§32's r1 had `n=1 crc=0x9408…c95b`.

### 33.8 Where it stops now

Still 19,903,177 bytes, and it is the §32 park, not a stopped world: pid 4 is
in `mono_os_cond_timedwait` -> `mono_w32handle_timedwait_signal_handle` ->
`mono_w32handle_wait_one` -> `mono_join_uninterrupted` on a heap condvar with
`waits=270 wakes=121` frozen, and the group has **no Thread Pool Worker left
alive** (11 `[TEXIT]` rows, `live=32..26`; every remaining slot is `st=8`, and
`Loading.Preload` still waits on Unity's private semaphore with `word=0`).
Player.log's last rows are eleven `mono_thread_internal_set_priority` lines,
i.e. workers being created at the deadline. So the next wall is the one §32
named — a managed joiner whose signaller never runs — but it is no longer
*inside* a stop-the-world, and `n_live == planted` holds while it happens.

### 33.9 Regression

`unit 414/0` (413 + the new `test_p5_directed_signal_owner`), `m2_mono`,
`m2_tp` (work items, parking, clock), `m2_threads`, `m12`, `redzone`,
`m3_window_probe`, `m4_gl_probe` all PASS at this tip.

## 34. 2026-11-05 — M6/L47: the pool's own counters, the misattributed Join, and an "animation" that was the panel clock

Three of §33's conclusions do not survive their own rows. This section
re-derives them from the pool's own trace and from the pixels, and it corrects
the two instruments that hid the truth.

### 34.1 The `[TEXIT]` table: the managed exit DID run in every row

`[TEXIT]`'s `mod=` column was computed by walking back over contiguous regions
of the same `kind`, and adjacent DSOs here are packed back-to-back, so all 11
rows of §33 claimed `mod=0x1083380000+0x17C6C6` — a 1.6 MiB "module" that is
really four libraries. With the boundary fixed (a module's first segment is
never writable, so a writable predecessor ends the walk; `src/kernel/process.c`),
the same rows read `libpthread+0x86C6`:

```text
[TEXIT] pid=36 pre=0x24 touch=0 woke=0 live=35 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=30 pre=0x1E touch=0 woke=1 live=35 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=37 pre=0x25 touch=0 woke=0 live=35 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=32 pre=0x20 touch=0 woke=1 live=35 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=29 pre=0x1D touch=0 woke=0 live=32 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=38 pre=0x26 touch=0 woke=1 live=32 mod=0x10834F4000+0x86C6 /NFS/RimWorldLinux
[TEXIT] pid=31 pre=0x1F touch=0 woke=0 live=30 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=34 pre=0x22 touch=0 woke=0 live=30 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=35 pre=0x23 touch=0 woke=1 live=28 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=33 pre=0x21 touch=0 woke=1 live=30 mod=0x10834F4000+0x86C6 Thread Pool Wor
[TEXIT] pid=28 pre=0x1C touch=0 woke=0 live=26 mod=0x10834F4000+0x86C6 /NFS/RimWorldLinux
```

`0x86C6` is inside `start_thread`, at the `xor %edi,%edi; mov $0x3c,%eax;
syscall; jmp` loop that glibc calls `__exit_thread` (`libpthread-2.31.so`,
disassembled: `0x86c0..0x86c6`). It is reached through the unwind handler that
`start_thread` installs — the same tail for a returned start routine and for
`pthread_exit` — and, crucially, `start_thread` runs

```text
0x8612: call __call_tls_dtors        (__cxa_thread_atexit dtors)
0x8625: call __nptl_deallocate_tsd   (pthread_key_create dtors <- Mono's)
0x862a: call __libc_thread_freeres
0x86c4: syscall 60 (exit)
```

Mono registers `thread_exited_dtor` as a `pthread_key_create` destructor, and
that dtor calls `unregister_thread(info)`. `unregister_thread` is the **only**
place `mono_threads_signal_thread_handle()` is called
(`mono-threads.c:597`), i.e. the only place a `Thread.Join` waiter is woken.
`mono_thread_info_exit()` runs the same function one step earlier via
`mono_thread_info_detach()`, so both exit paths signal.

**Verdict: "a worker that leaves without running the managed exit never signals
its joiner" is eliminated** for all 11/16 rows: every one of them ran
`unregister_thread`, signalled its `MonoThreadHandle`, and cleared its own
`ctid` (`pre` is each row's own tid, `touch=0`). If a join is stuck, its target
is not one of these threads.

### 34.2 Whose join: the main thread is NOT in `mono_join_uninterrupted`

`mono_join_uninterrupted()` in 6.12 does not touch a `MonoW32Handle` at all:

```c
ret = mono_thread_info_wait_one_handle (thread_to_join, wait, TRUE);
   -> mono_os_event_wait_one (&thread_handle->event, timeout, alertable)
      -> mono_os_event_wait_multiple (...)      /* os-event-unix.c */
           mono_cond_t signal_cond;             /* a STACK local */
           g_ptr_array_add (event->conds, &signal_cond);
           mono_os_cond_wait (&signal_cond, &signal_mutex);
```

so a Thread.Join parks on a **stack** address, inside a `pthread_cond_wait`
called directly by `mono_os_event_wait_multiple`. Two different keys are in the
ladder, and they belong to two different waits:

| thread | key | where the key lives | `waits/wakes` | decoded frame |
|---|---|---|---|---|
| pid 4 main | `0x105B6707F8` | heap | 208 / 121, frozen | `mono_os_cond_wait` <- `mono_w32handle_timedwait_signal_naked` <- `mono_w32handle_wait_one` <- `mono_wait_uninterrupted` |
| pid 29 pool worker | `0x11A4028928` | **stack** (0x11A4000000+0x40000) | 5 / 4 | `pthread_cond_wait` <- `mono_os_event_wait_multiple` <- `mono_thread_info_wait_one_handle` = **Thread.Join** |

The heap key is decisive for pid 4: the code at `libmono+0x20dec8` is
`lea 0x38(%rbp),%rdi; add $0x10,%rbp; mov %rbp,%rsi; call
mono_os_cond_timedwait` — cond at `handle+0x38`, mutex at `handle+0x10`, which
is exactly `MonoW32Handle` (`type/ref/signalled/in_use`, `signal_mutex` at
0x10, `signal_cond` at 0x38). Its caller at `libmono+0x23ca7b` calls
`0x20db03` (that same `mono_w32handle_wait_one`) out of the alertable-wait
loop, i.e. `mono_wait_uninterrupted` — a managed `WaitHandle.WaitOne()`.

So: **main waits on the game's `taskDoneEvent` (AutoResetEvent) and one pool
worker waits in `Thread.Join`.** §33.8's "pid 4 is in `mono_join_uninterrupted`"
is a misattribution: a stack-scan candidate was named instead of the caller.
Any theory built on "the main thread's join" has to be retired.

### 34.3 The pool's own counters (Mono's threadpool trace, `/TPTRACE`)

With a *current* `rwlaunch` (§34.5), `MONO_LOG_MASK=threadpool` +
`MONO_LOG_LEVEL=debug` prints the pool's decisions. One run, in order:

```text
[0x10801f3680] try create worker, created 0x1148200700, now = 50 count = 1
...                                                          ... count = 8
[0x10801f3680] try create worker, failed: maximum number of working threads reached
[0x10801f3680] request worker, failed
[0x1148401700] try create worker, failed: maximum number of working threads reached
[0x1148401700] request worker, failed
[0x1148c05700] worker parking
[0x1148c05700] worker unparking, timeout? yes interrupted? no
[0x1148c05700] worker timed out, starting = 0 working = 2 parked = 0, setting max_working to 8
[0x1148c05700] worker finishing
[0x1148a04700] worker timed out, starting = 0 working = 0 parked = 0, setting max_working to 8
[0x1148a04700] worker finishing
```

The counters the pool reads, at the stall:

* `limit_worker_min = mono_cpu_count() * MONO_THREADS_PER_CPU = 8 * 1 = 8`;
  `max_working` starts there and the hill climber pins it there —
  `decr_max_working = MAX(limit_worker_min, MIN(num_active, max_working)) = 8`.
* 8 workers, then every 9th request answers **"maximum number of working
  threads reached"** (`counter._.working >= counter._.max_working`) and
  `request worker, failed`.
* **zero `monitor thread, created` / `monitor thread, unparked` rows**: the
  monitor is the only path that raises `max_working`
  (`counter._.max_working ++` then `worker_try_create()`), and it only runs that
  path when `work_item_count() > 0` at its 500 ms tick. At the stall the native
  hint is 0 — every item was already *dequeued* — so it does nothing.
* the last rows are `starting = 0 working = 0 parked = 0`: the pool is empty
  while the main thread still waits.

A/B with `MONO_THREADS_PER_CPU=50` (`/TPCPU`): `max_working` 8 -> 400, 16
`[TEXIT]` instead of 11, five of them pool workers that never entered
`worker_callback` (they stayed unnamed, `/NFS/RimWorldLinux`, because
`mono_threadpool_set_thread_name()` is the first thing `worker_callback` does),
and the loader stops at the same place.

**Verdict: pool capacity is not the wall.** The item the main thread waits for
is not sitting in the pool's queue; another worker would not run it. Raising the
ceiling only creates workers that park, time out and exit.

### 34.4 `[XSW]=0` vs "the frame animates": the animation was the panel clock

Consecutive `01..22_running.ppm` diff, pixel by pixel, at 1920x1080:

```text
02 diff_px=7   bbox=(1889,1065,1892,1068)
03 diff_px=28  bbox=(1881,1063,1894,1068)
04 diff_px=18  bbox=(1881,1064,1894,1069)
05 diff_px=921534 bbox=(2,34,1894,1069)     <- the game window appears
06 diff_px=36  bbox=(1881,1063,1894,1069)
...
22 diff_px=56  bbox=(1881,1063,1894,1069)
```

Every change after shot 05 is a 13x6 box in the **bottom-right corner of the
1920x1080 screen** — the desktop panel's clock — 7 to 59 pixels. The game's
loading art does not change by one pixel after it is first shown. "Every
screenshot's SHA differs" was true and meant nothing; it measured the clock.

So `[XSW] = 0` is the honest row: the client did not swap after the first
frame, exactly as §32's `n=1` said. The hook itself is not the explanation
either — `obj/mesa/lib/libGL.so.1` carries `UND XcPresentMark`, a `JUMP_SLOT`
relocation and `jmp XcPresentMark@plt` at `0xab64fd` (guarded by `cmpq $0,GOT`),
`obj/xcompat/libX11.so.6` exports `T XcPresentMark`, and the staged copies in
`disk.img` md5-match both — but no such argument is needed: the pixels settle it.

### 34.5 A stale `rwlaunch.bin` silently disables `/TPTRACE` and `/SIGQUITON`

`RW_SKIP_BUILD=1` re-stages every DSO but **not** `obj/rw-desktop/rwlaunch.bin`
(it is built inside the same `if` as the kernel). The lane's seeded binary
predated L42: `strings obj/rw-desktop/rwlaunch.bin | grep TPTRACE` was empty
while `src/user/rwlaunch.c` had the code. A `/TPTRACE` run therefore produced
**zero** trace rows and read exactly like "Mono's trace does not work" — one
boot lost. The same stale binary is why the first `/SIGQUITON` run printed
nothing at all: `open("/SIGQUITON")` never ran.

With a current launcher the SIGQUIT dump does fire, but Mono's own dump stops
after `Full thread dump:` (its first line, `"Finalizer"`, then nothing) — the
async-suspend walk dies there, so managed stacks are still not available from
this kernel.

### 34.6 Where it stops now

`Loaded All Assemblies, in 13.660 s`, banner reached, `world_stopped=0`,
`live=0`, `stop_count=44`, NFS at `[NFSRTT] ... bytes=19331072` (19.3 MB of
729 MiB), 11 `[TEXIT]`, every pool slot `st=9`, and the main thread frozen in
`WaitOne` on its `taskDoneEvent` at `waits=208 wakes=121`.

The remaining wall is one work item that never completes while the pool that
would run it is empty. §33.8 said "no Thread Pool Worker is alive" and that is
right; what it got wrong is *why*. The corpus now says: not a stopped world
(§33 fixed it), not a pool ceiling (§34.3 A/B), not a worker that skipped its
managed exit (§34.1), and not the main thread's `Join` (§34.2). The open
question is the target of the *worker's* `Thread.Join`, which needs a managed
stack; the next instrument is a working thread dump (or a breakpoint count of
`mono_threads_signal_thread_handle` against thread exits).


## 35. 2026-11-05 — M6/L48: the pool is EMPTY because the game is not using it; the dump stops inside Mono's suspend handshake

§34.6 left one question open: "one work item never completes while the pool
that would run it is empty". This lane answered it by counting work items at
the only place where an *item* exists — the managed/native boundary — and the
answer is a negative: **the ThreadPool holds no stranded item. In a 200 s run
the game enqueues seven work items and all seven complete.** The pool is empty
at the stall because RimWorld stopped using it, not because a worker lost
something on its way out.

### 35.1 Why Mono's own trace cannot see an item

Two facts from the 6.12 sources (`mono/metadata/threadpool.c`,
`threadpool-worker-default.c`, `mcs/.../threadpool.cs`) decide the instrument:

* `ves_icall_System_Threading_ThreadPool_NotifyWorkItemQueued` is a **no-op**
  (one perfcounter increment). Every managed enqueue calls it, so it is a free
  and exact enqueue counter.
* `worker.work_items_count` — the number §34.3 read as "the native hint" — is
  incremented by `mono_threadpool_worker_request()`, i.e. once per
  **`RequestWorkerThread` icall**, not once per item. `EnsureThreadRequested()`
  asks for up to `processorCount` threads, so the counter is a *thread-request*
  hint. Measured: **7 work items produced 22 requests** in one run. A zero
  there says "no outstanding thread request", never "the queue is empty".

The third boundary call is `NotifyWorkItemComplete()`, made after every item
returns, so `enq - comp` is the number of items queued and not yet run.

### 35.2 The instrument: icall accounting from the AD-9 launcher

`src/user/rwmain.c` now installs three hooks with `mono_add_internal_call()`
(a public API that *replaces* an existing registration):

| icall | what it counts |
|---|---|
| `System.Threading.ThreadPool::NotifyWorkItemQueued` | one per managed `Enqueue` |
| `System.Threading.ThreadPool::NotifyWorkItemComplete` | one per executed item |
| `System.Threading.ThreadPool::RequestWorkerThread` | one per managed thread request, and whether the native pool granted it |

The original function pointers come from `mono_lookup_internal_call()` on the
methods found with `mono_class_from_name()`/`mono_class_get_method_from_name()`,
and are still called, so the runtime's own behaviour is unchanged. The JIT
binds an icall's address when it compiles the calling method, so a watchdog
thread polls for the root domain, attaches, and registers **by name only** —
a class lookup from that thread during `mono_init` aborted the runtime
(`Assertion at object.c:1939, condition 'klass' not met`, measured), which is
why phase 1 touches no class loader and phase 2 (the two remaining icalls)
runs from inside the first enqueue, on a Mono thread with a live runtime.

Every row carries the calling thread's tid and, for the ring dump, the managed
caller resolved with `mono_jit_info_table_find()` on the icall's return
address. Gated on `/TPTRACE`; nothing runs without the marker.

### 35.3 The accounting, and the timeline

```text
[TPQT] t=0s  enq=0 comp=0 req=0  reqF=0
[TPQT] t=10s enq=0 comp=0 req=0  reqF=0
[TPQT] t=20s enq=0 comp=0 req=0  reqF=0
[TPQT] t=25s enq=7 comp=7 req=22 reqF=0     <- the whole of the game's pool use
[TPQT] t=30s enq=7 comp=7 req=22 reqF=0
...                                          (frozen to the end of the run)
[TPQSUM] enq=7 comp=7 req=22 reqF=0 stranding=0
```

Four runs agree, and the same four runs stall at the same byte:

| run | `Loaded All Assemblies` | NFS bytes at the deadline | `[XSW]` | dump |
|---|---|---|---|---|
| baseline (no probe, no dump) | 9.476 s | 19,331,072 | 0 | — |
| accounting run A | — | 19,335,720 | 0 | — |
| accounting run B | 9.990 s | 19,339,984 | 0 | — |
| accounting + `/SIGQUITON` | 9.314 s | 19,339,984 | 0 | `Full thread dump:` + `"Finalizer"`, nothing else |

**Verdict: the ThreadPool is excluded.** §34.3's "capacity is not the wall", §34.1's
"the managed exit ran", §34.2's "the main thread is not in a Join" and this
lane's "enqueue == complete" now agree: the pool neither holds nor loses the
item the loader waits for. The `starting = 0 working = 0 parked = 0` rows and
the missing `work_item_count() > 0` monitor tick are the *pool being idle*, and
an idle pool is not a bug. Any further theory that begins "the work item is in
the pool" is refuted by 7/7.

### 35.3b Who enqueues them: `ThreadPoolTaskScheduler.QueueTask`, not `GenThreading`

The ring dump resolves each event's managed caller (`from=`, the icall's
second return address, resolved with `mono_jit_info_table_find`):

```text
[TPQR] kind=enq tid=4  icall=ThreadPool.NotifyWorkItemQueued  from=ThreadPoolTaskScheduler.QueueTask
[TPQR] kind=req tid=4  icall=ThreadPool.RequestWorkerThread   from=ThreadPoolTaskScheduler.QueueTask
[TPQR] kind=cmp tid=31 icall=ThreadPool.NotifyWorkItemComplete from=_ThreadPoolWaitCallback.PerformWaitCallback
```

Every one of the run's seven items is a **`System.Threading.Tasks.Task`**
queued by Mono's `ThreadPoolTaskScheduler.QueueTask` from the main thread, and
every completion is a pool worker leaving `_ThreadPoolWaitCallback`. Not one
comes from `Verse.GenThreading.ParallelForEach`/`QueueUserWorkItem`, which was
§32.2's model of the load. So the loader's parallelism is `Task`-based, and the
wait the main thread sits in is a **Task completion wait**, not a
`QueueUserWorkItem` done-event.

That re-points the search at the wake path rather than at the queue: the seven
tasks all ran to completion (`comp=7`), so a `Task.Wait()` that never returns
means the *completion signalled nothing*, or signalled before the waiter
registered. The row §34.2 kept — pid 4's wait key `waits=208 wakes=121`,
frozen — is the shape of a lost wake, and the futex ring shows the same
asymmetry elsewhere (`WAKE ... n=1 ... woke=0`). The key lives in an anonymous
private region (`[FUTEX] rgn pid=4 key=0x105B69C708 base=0x1040000000 ... kind=2
prot=3`), i.e. the managed heap, and the `[FUTEX] bt` chain is the same
`mono_os_cond_timedwait` <- `mono_w32handle_timedwait_signal_naked` <-
`mono_w32handle_wait_one` <- `mono_wait_uninterrupted` <- JIT (`0x1086005E28`,
module `0x1086000000`) that §34.2 decoded.

That is the next thing to test: whether a `mono_os_cond_signal` for that key is
issued and our futex wake drops it (the `woke=0` rows are the candidate), or
whether the signal is never issued at all.

`Verse.GenThreading::ParallelForEach` is the only code in `Assembly-CSharp`
that builds an `AutoResetEvent` and waits on it with an infinite timeout — and
with the enqueuer now known to be `ThreadPoolTaskScheduler`, its
`QueueUserWorkItem` would have shown up in the ring if it had run. Its IL is
still worth recording, because a *nested* pool item (a Task that calls
`ParallelForEach`) would look exactly like this stall:

```text
foreach (var localBatch in SliceWork(list, maxDegree))
    ThreadPool.QueueUserWorkItem(<ParallelForEach>b__0);   // return value popped
while (Interlocked.Read(ref tasksDone) < list.Count) taskDoneEvent.WaitOne();
```

with the lambda doing `Interlocked.Add(ref tasksDone, localBatch.Count)` after
running the batch (exceptions inside the batch are caught and logged, so a
throwing element is not silently skipped from the count). The counts above
say all its queued batches finished, so an unresolved `ParallelForEach` would
need `list.Count` to exceed what `SliceWork` covered — not proven. A hook on
`System.Threading.WaitHandle::Wait_internal` (the icall this mscorlib's
`WaitOneNative` calls) registered at 2.5 s never fired in a 200 s run, so the
managed wait site is still un-named; that is the next instrument.

### 35.4 The managed dump: SIGQUIT lands, then Mono's own handshake stops

`/SIGQUITON` fires at 60 s. The kernel now prints one row per delivery for
SIGQUIT and the RT range (the global `[SIGD]` cap of 48 is always exhausted by
Boehm's GC signals, which is why no earlier lane could see this), and the
`[ALIVE-SET]` rows carry the saved user PC and its module+offset:

```text
[SIGR] pid=4 sig=3  ipc=0x1083503376 mask=0x0      <- SIGQUIT, delivered, mask empty
[SIGR] pid=6 sig=35 ipc=0x10800DD23D mask=0x0      <- Mono's async suspend (SIGRTMIN+1)
                                                      ... and then no further signal
```

The dump's own output stops after its first thread:

```text
Full thread dump:

"Finalizer"
```

`[ALIVE-SET]` at the deadline (module offsets resolved against the pinned
glibc 2.31 in `/tmp/glibc231`):

```text
slot=4 pid=4 st=8 pc=0x1083503376 mod=0x10834f4000+0xf376  pthread_cond_wait+0x??
slot=6 pid=6 st=3 pc=0x108004348a mod=0x1080000000+0x4348a  sigsuspend+0x4a
```

i.e. the interrupted thread returned from the handler into its `WaitOne` cond
wait, the one thread the dump suspended is sitting in **`sigsuspend`** — which
is exactly what a suspended Mono thread does — and it is spinning there at
100 % of a CPU (`[IDLESTUCK] claims: ... c6=6/9949`). No thread is parked
inside `libmonobdwgc` at all, so the dumper is not waiting on a Mono lock in a
way a native PC can name: it is inside `mono_thread_info_safe_suspend_and_run`
-> `mono_threads_begin_global_suspend()`, i.e. the *global* suspend handshake,
after having suspended one target. The failure is therefore in Mono's
handshake, not in our SIGQUIT delivery (mask 0, delivered, handler ran).

Two smaller findings belong with it. First, a suspended thread busy-spinning in
`sigsuspend` is itself anomalous — a correctly parked suspend loop blocks — and
`[IDLESTUCK]` shows it holding a whole vCPU for the rest of the run; either our
`rt_sigsuspend` returns `-EINTR` on a wake that carried no deliverable signal,
or a real signal is being re-delivered. Second, `[SIGB]` (sent-but-blocked) has
not fired once, so no dump signal is being swallowed by a mask.

### 35.5 Instruments added (all marker-gated)

* `src/user/rwmain.c`: the icall accounting probe (35.2) and a stack-walk
  primitive (`mono_jit_info_table_find` over raw stack words whose preceding
  byte is `0xE8`, the §33/L46 filter), used for the ring dump and for waits.
* `src/kernel/linux_compat_thr.c`: `[SIGR]` per-pid rows for SIGQUIT and
  signals 34..40, with the target's mask.
* `src/kernel/process.c`: `[SIGB]` (a signal the sender left pending because
  the target blocks it) and `pc=`/`mod=` on every `[ALIVE-SET]` slot.

### 35.6 Where it stops now

Same ladder as §34.6 — banner, `world_stopped=0`, NFS frozen at 19.33-19.34 MB
of 729 MiB, `[XSW]=0`, one static loading screen — but the pool is now
*excluded by measurement* rather than suspected, and the dump's stopping point
is localized to Mono's global-suspend handshake. The next instrument is a
managed wait-site hook that actually binds (the `Wait_internal` registration
never fired) or a kernel-side user-stack dump of the dumping thread.

## 36. 2026-11-05 — M6/L49: the wait site is PLINQ spooling, the completion signal is NEVER ISSUED, and the timers were a real gap

§35 left two questions: *who* the main thread waits for (its managed wait site
was un-named because the `Wait_internal` icall hook never fired) and *whether*
the completion signal is issued at all. Both are answered here by measurement,
and a third finding — the kernel had **no implementation of any timer syscall**
— is closed as a real gap while being excluded as this stall's cause.

### 36.1 The wait site, named: `System.Linq.Parallel` spooling

The icall hook cannot be made to bind, and the reason is now a row rather than
a guess. `mono_add_internal_call("System.Threading.WaitHandle::Wait_internal")`
does replace the table entry — after it,

```text
[TPQW] bind arity=4 orig=0x10846d1701 hook=0x10000048c0 verify=HOOK
```

— but the JIT-compiled managed wrappers were compiled *earlier* and do not
carry that pointer anywhere a patcher can find it:

```text
[TPQP] tag=WaitHandle/WaitOneNative4 code=0x1085531280 orig=0x10846d1701 sites=0
       calls=+11:0x1075c89bdc +51:0x1085531479 +58:0x103d989e07 +79:0x108553146b ...
[TPQP] tag=WaitHandle/WaitMultiple4  code=0x10855314c0 sites=0
[TPQP] tag=NativeEventCalls/SetEvent1 code=0x1085531910 sites=0
```

`sites=0` for every one of them: the body's `call` targets are neither the
registered icall address nor a `movabs` of it — the runtime reaches the icall
through an indirection this patcher does not model. (The first patch attempt
also named the wrong bodies: `WaitOne`/`WaitAny` compile with `sites=0` because
they call the **private managed helpers** `WaitOneNative(SafeHandle,uint,bool,bool)`
and `WaitMultiple(WaitHandle[],int,bool,bool)`, which is where the icall lives.)

So the stack is taken from the thread itself instead of from the icall:
SIGUSR2 → the parked thread's handler copies `[rsp, rsp+2048)` from its own
ucontext (async-signal-safe: copy + flag only) → the watchdog thread resolves
every word with `mono_jit_info_table_find`. Frame names, not addresses, and it
works for **any** parked thread. Two samples, 30 s apart, identical:

```text
[TPWS] tgkill SIGUSR2 t=45s rc=0
[TPWS] stack sp=0x17ffffa080 words=2048 bt=Monitor.ObjWait <- Monitor.Monitor_wait <-
  Monitor.ObjWait <- Monitor.ObjWait <- Monitor.Wait <- Monitor.Wait <-
  ManualResetEventSlim.Wait <- Task.AddTaskContinuation <- ManualResetEventSlim.Wait <-
  Task.SpinThenBlockingWait <- TaskScheduler.TryRunInline <- Task.InternalRunSynchronously <-
  Task.RunSynchronously <- SpoolingTask.SpoolStopAndGo <-
  DefaultMergeHelper`2.System.Linq.Parallel.IMergeHelper<TInputOutput>.Execute <-
  MergeExecutor`1.Execute
```

**The main thread is inside PLINQ's merge**, not in the loader, not in a Unity
thread, and not in `GenThreading.ParallelForEach`. `SpoolingTask.SpoolStopAndGo`
ran its merge task synchronously on the calling thread; `Task.RunSynchronously`
→ `Task.InternalRunSynchronously` → `Task.SpinThenBlockingWait` is waiting on
that task's **completion `ManualResetEventSlim`** (`SetOnInvokeMres`), and
`ManualResetEventSlim.Wait` has fallen back to `Monitor.Wait` on its lock
object — which is exactly the `pthread_cond_wait` park the kernel sees.

`Assembly-CSharp.dll` does reference `AsParallel` (one MemberRef), so this is
the game's own PLINQ query (RimWorld 1.6 uses PLINQ in its loading path). The
same PLINQ shapes complete on the **host under the game's own runtime**
(`Where(...).ToArray`, `Select(...).ToList`, `ForAll`, 32 CPUs, 8-17 ms), so
PLINQ itself is not broken in this Mono build; the guest is what differs.

### 36.2 Signal vs wake: the completion signal is NEVER ISSUED

`[FUTXW]` (new, `make RW_FUTXW=1`) records the futex word before/after every
wake, the number of queued waiters the wake found, and — at the scheduler's
"no READY work" dump, where it cannot be capped away — which thread is parked
on which key and how long it has been there:

```text
[FUTXW] KEY #62 key=0x104D7F0C68 w32handle=0x104D7F0C30 waiter_pid=4
[FUTXW] NOW pid=4 key=0x104D7F0C68 parked_ms=3341    waits=128 wakes=107 main=1
[FUTXW] NOW pid=4 key=0x104D7F0C68 parked_ms=61363   waits=128 wakes=107 main=1
[FUTXW] NOW pid=4 key=0x104D7F0C68 parked_ms=116126  waits=128 wakes=107 main=1
[FUTXW] KEYSUM #62 key=0x104D7F0C68 by_pid=4 waits=1 wakes=0 woke=0
```

One park on that key (the stall), **zero wakes for 116 s**, while the same
instrument shows the wake path working everywhere else in the same run
(`WAKE ... parked=1 woke=1 pids=4,` on the startup keys, and `parked=2 woke=1`
under contention). The word's value is irrelevant here: no `mono_w32handle_set`
/ `mono_os_cond_signal` ever reached that handle's condvar.

**Verdict: the signal is never issued. This is not a dropped wake.** Every
theory that begins "the kernel wakes the wrong waiter" is refuted for this key.

### 36.3 The arithmetic reconciled: 7/7 has nothing to do with the wait

`enq=7 comp=7` is right and the wait is still real, because the task the main
thread awaits **was never enqueued at all**:

```text
[TPQT] t=25s enq=7 comp=7 req=20 reqF=0
[TPQT] t=30s enq=7 comp=7 req=20 reqF=0
...                                (frozen through the whole 116 s park)
[TPQT] t=135s enq=7 comp=7 req=20 reqF=0
```

The park begins at ~t=34 s and no enqueue, no request and no completion happens
after t=25 s. So:

* the seven `ThreadPoolTaskScheduler.QueueTask` items completed (§35), and the
  main thread does **not** wait on any of them;
* the PLINQ merge task it waits on was neither queued to the pool
  (`enq` would be 8) nor completed — it is a `Task` that
  `Task.InternalRunSynchronously` is waiting for while nothing can complete it;
* therefore the missing signal is **upstream of the ThreadPool**: no work item
  is stranded, because no work item for this task exists. §34.6's "one work
  item never completes while the pool is empty" is the same fact seen from the
  wrong end.

### 36.4 Timers: a real gap, closed — and excluded as this stall's cause

The kernel had **no** `timerfd`, `setitimer`, `alarm`, `timer_create` or
`epoll` implementation: `getitimer(36)` was in no table, and 37/38, 222-226 and
283/286/287 were not even named in `linux_syscall.h`, so all of them answered
ENOSYS. A `Task` whose continuation needed a timer would hang exactly like
§36.1, so the gap had to be closed and then excluded by measurement.

Implemented (`src/kernel/linux_compat_timers.c`, one 100 Hz tick, no per-timer
thread): `setitimer`/`getitimer`/`alarm`/`pause`, POSIX
`timer_create/settime/gettime/getoverrun/delete` (SIGEV_SIGNAL and SIGEV_NONE),
`timerfd_create/settime/gettime` with a real `FILE_TYPE_TIMERFD` (read returns
the accumulated u64 and drains it, poll/select report it readable, a read with
none pending parks until the next expiry), and a level-triggered `epoll`
(`create/create1/ctl/wait/pwait`) built on the kernel's one readiness engine,
`file_poll`.

`src/user/linux_probe_timers.c` (ordinary musl program, runs on the host and in
the guest) is the differential. Host vs guest, same binary:

| row | host | guest |
|---|---|---|
| `timerfd_create/settime/gettime` | ok, left=49ms | ok, left=49ms |
| `timerfd read` (50 ms) | 50 ms | 60 ms |
| `poll(timerfd)` (20 ms) | 20 ms | 25 ms |
| `setitimer` + SIGALRM (50 ms) | 50 ms | 67 ms |
| `alarm(1)` | 1000 ms | 1020 ms |
| `timer_create` + SIGALRM (60 ms) | 60 ms | 76 ms |
| `epoll_wait` empty (60 ms) | 60 ms | 65 ms |
| `poll(NULL,0,60)` / `select` / `nanosleep` | 60/60/60 ms | 68/69/69 ms |
| `done rc=` | 0 | 0 |

The guest's extra 5-20 ms is the 100 Hz tick, not a defect. Gate:
`bash tools/gate/t3_linux_probe.sh timers` (PASS, `done rc=0`).

Managed side, under **the game's own runtime** (`tools/rw/mono-probe/timers.cs`
via `tools/rw/run-tp-probe.sh`):

```text
A Task.Delay(50).ContinueWith OK
B Timer(50ms) OK
C Task.Run().Wait OK
D chained continuations OK
E Task.Delay(50).Wait OK
F Task.Delay(300).Wait ms=319
TIMERS-OK
```

So Mono's own Timer/Task machinery works in this guest, and `Task.Delay` fires
on time. **Timers are excluded** — and the ENOSYS list they used to sit on is
now empty for this family.

### 36.5 Instruments added (all gated)

* `src/user/rwmain.c`: the SIGUSR2 stack-capture handler + `mono_jit_info_table_find`
  resolver (`[TPWS]`), the corrected `Wait_internal` ABI (it is
  `(IntPtr* handles, int n, bool waitAll, int ms)`, not `(handle, ms, ...)`;
  L48's hook would have mis-read its arguments even if it had fired), the
  `bind/verify` + `[TPQP] calls=` row, and `[TPQE]` enqueue stacks.
* `src/kernel/linux_compat_thr.c`: `[FUTXW]` (RW_FUTXW) — per-key WAIT/WAKE
  timeline with the futex word before/after, waiters found, and an uncapped
  `[FUTXW] NOW`/`KEYSUM` snapshot from the scheduler's no-work dump
  (`src/kernel/process.c`).
* `src/kernel/linux_compat_timers.c`, `src/user/linux_probe_timers.c`,
  `tools/rw/mono-probe/timers.cs`, `tools/gate/t3_linux_probe.sh timers`.

### 36.6 Where it stops, and the next single step

The ladder is unchanged (banner, `world_stopped=0`, NFS frozen at 19.3 MB of
729 MiB, `[XSW]=0`, one static loading screen) but the wall is now named
precisely: **the main thread is in PLINQ's `SpoolingTask.SpoolStopAndGo`, waiting
on a task-completion `ManualResetEventSlim` that nothing ever sets, and the
task was never enqueued.**

Two candidates remain, and they are separable in one boot:

1. `Task.InternalRunSynchronously` queued the task to a scheduler that dropped
   it (the enqueue would then be invisible to `NotifyWorkItemQueued` only if it
   went through a path that does not call it — worth checking by hooking
   `TaskScheduler.InternalQueueTask`'s callers instead of the pool's icall);
2. the task completed *before* `Task.AddTaskContinuation(mres)` registered the
   wait, and the waiter therefore blocks on a signal that was issued earlier and
   consumed by nobody — the classic Mono `SpinThenBlockingWait` window, made
   wide by this guest's timing.

The discriminator is the awaited task's state at the park: print
`task.IsCompleted`/`Task.Status` for the task `RunSynchronously` was given
(a hook on `Task.InternalRunSynchronously` plus one field read), or hook
`Task.AddTaskContinuation` and print its return value on that thread. If it
returns false, the signal was issued before the wait — a lost wake *inside
Mono*, and the kernel is not the cause.

### 36.7 The 3-run ladder (deterministic, not intermittent)

`bash tools/rw/run-rimworld-desktop.sh --timeout 400 --run-timeout 180`, three
times, nothing staged but the build:

| run | NFS bytes at the deadline | `[XSW]` | frame content |
|---|---|---|---|
| 1 | 19,344,568 | `n=1 swap=1 put=1 putok=1 flush=83 crc=0x940850F19945C95B changed=0` | `nz=921600/921600 box=0,0,1279,719` |
| 2 | 19,344,568 | identical (same CRC, `changed=0`) | identical |
| 3 | 19,344,568 | identical (same CRC, `changed=0`) | identical |

Byte-for-byte the same stall in 3 of 3: one present, the same buffer checksum,
no second swap, NFS frozen at 19.34 MB of 729 MiB, and the loading screen still
on screen (the `[XSW]` row proves the client presented exactly one full-opaque
1280x720 frame; there is no main menu, so no fps number is meaningful). This is
the §35 ladder reproduced exactly — the stall is deterministic, which is what
makes the single remaining question in §36.6 worth one more instrument.

### 36.8 Regression, and the one row that re-opens the pool question

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **414 run, 0 failed**. Gates:
`t3_linux_probe redzone` PASS, `m2_mono` PASS, `m2_tp` PASS (clock row
included), `m2_threads` PASS, `m12_thread_ceiling` PASS, `m3_window_probe`
PASS, `m4_gl_probe` PASS. `tools/rw/check-no-game-files.sh` OK.

One row in the 180 s run deserves to be carried forward, because it is the only
thing left that contradicts "the pool is idle":

```text
[IDLESTUCK] slot=31 pid=31 st=8 futex=0x11AC005078 rip=0x1083503376 Thread Pool Wor
[FUTEX] q slot=31 pid=31 st=8 key=0x11AC005078 waits=5 wakes=3 ... Thread Pool Wor
[FUTEX] rgn pid=31 key=0x11AC005078 base=0x11AC000000 len=0x23000 kind=3 prot=3
```

`0x11AC005078` is inside a **thread stack** (`base=0x11AC000000`, 140 KiB,
`kind=3`), which per rule 98 is the signature of `mono_join_uninterrupted`'s
`mono_os_event_wait_multiple` — i.e. a **Thread Pool Worker is in
`Thread.Join`** at the stall (5 waits, 3 wakes, still parked), not idle. If that
worker is the one holding the PLINQ merge task, the task cannot complete while
the worker joins a thread that never exits, and §36.6's candidate 1 becomes
"the merge task IS being run, by a worker blocked in a Join". The next
instrument is therefore cheap and specific: at the stall, resolve the *join
target* of that worker (its `MonoThreadHandle`, one word on its stack) and print
the target's pid/name, plus `Task.Status` for the task the main thread's
`RunSynchronously` was given. Both are reads of state the two threads already
carry; neither needs a new boot-time mechanism.

## 37. 2026-11-05 — M6/L50: the awaited task is COMPLETE and its event IS set — the lost wake is inside `Monitor.Wait` itself

§36 named the wait (`SpoolingTask.SpoolStopAndGo` → `Task.RunSynchronously` →
`Task.SpinThenBlockingWait` on a completion `ManualResetEventSlim`) and left two
candidates: the task was queued to a scheduler the blocked main thread must
pump, or `AddTaskContinuation` lost the completion. **Both are refuted by rows
here.** The task ran, the continuation ran, the event is signalled — and the
main thread is still asleep inside `Monitor.Wait` on that event's lock.

### 37.1 Reading the managed objects in place

Private managed fields are read through Mono's own metadata API, not at guessed
offsets: `mono_class_get_field_from_name` + `mono_field_get_value`
(`mono_class_vtable` for the exact type test). `Task`'s inventory in this
mscorlib is `m_taskId/m_stateFlags/m_taskScheduler/m_action/
m_continuationObject/m_contingentProperties`, and `ContingentProperties` holds
`m_completionEvent`/`m_completionCountdown` — printed once at arm time:

```text
[TPWI] Task id=0x104c9fdc50 flags=0x104c9fdcf0 sched=0x104c9fdcb0
       cont=0x104c9fdf10 cp=0x104c9fdf70 action=0x104c9fdc70
       CP.ev=0x104ca03ad0 CP.cd=0x104ca03b70 Thread.it=0x10400c1ff8
       IT.tid=0x10400c11b8 vt_task=0x104ca149a0 vt_thr=0x10400b9970 ok=1
```

The objects are found **without unwinding**: the parked thread's own stack still
points at `this` for `SpinThenBlockingWait` and at the `SetOnInvokeMres` local,
so every 8-aligned word in the captured 16 KiB is tested with
`*(void**)word == MonoVTable(Task)` (exact isinstance) and each deref is guarded
by the same `rw_readable()` (mincore-backed) test the stack capture already uses.

### 37.2 The awaited task, at the stall, four samples over 90 s

`[TPWS]`'s own chain is unchanged (§36.1), and the object it waits for is:

```text
[TPWT] tid=4 task=0x11100e4960 id=0 flags=0x1830000
       st=started|delegate|children|ran
       sched=0x111ffc16a0(ThreadPoolTaskScheduler) action=0x0
       cont=0x11100d03e0(Object) cp=0x11100e4820 cd=0 ev=0x0 set=0
[TPWQ] tid=4 mres=0x11100e9210 combined=0x80000001 set=1 waiters=1
```

Decoded against this mscorlib's constants (`Task.cs`: `TASK_STATE_STARTED`
0x10000, `DELEGATE_INVOKED` 0x20000, `WAITING_ON_CHILDREN` 0x800000,
`RAN_TO_COMPLETION` 0x1000000; `ManualResetEventSlim`:
`SignalledState_BitMask` **bit 31**, waiter count in bits 0..18):

| fact | row | consequence |
|---|---|---|
| `m_taskScheduler` = `ThreadPoolTaskScheduler` | `sched=0x…(ThreadPoolTaskScheduler)` | **not** a `SynchronizationContextTaskScheduler` and not the game's; §36.6 candidate 1 (a scheduler only the blocked thread pumps) is eliminated |
| `RAN_TO_COMPLETION` set, `m_action` null, `m_continuationObject` = the completion sentinel (class `Object`) | `st=…|ran action=0x0 cont=Object` | the task finished; `FinishStageThree` nulls the action and `FinishContinuations` consumed the continuation list |
| `m_completionCountdown = 0`, `m_completionEvent = null` | `cd=0 ev=0x0` | no children outstanding, no separate event object |
| `SetOnInvokeMres.m_combinedState = 0x80000001` | `set=1 waiters=1` | **the completion was signalled** (`SetOnInvokeMres.Invoke` → `Set()`) **and one waiter is still registered** |
| same values at t=40/70/100/130 s | 4 samples | the state is frozen, not transitional |

So: the completer did its job. `AddTaskContinuation` did not lose anything (its
caller `AddCompletionAction` invokes the action inline when the task is already
complete, and here the continuation was added *and* the task then completed, so
`Set()` ran). There is **no thread to hand the awaited task to** — nothing is
missing on the task-scheduler side. `[TPQT]`'s frozen `enq=7 comp=7` is a true
but irrelevant row: the spooling task never needed the pool.

### 37.3 What is actually stuck: `Monitor.Wait` on a set event

`ManualResetEventSlim.Wait`'s blocking tail is

```csharp
lock (m_lock) { while (!IsSet) { Waiters = Waiters + 1;
                                 if (IsSet) { Waiters--; return true; }
                                 Monitor.Wait (m_lock, realMillisecondsTimeout); ... } }
```

and `Set()` is `IsSet = true; if (Waiters > 0) lock (m_lock) Monitor.PulseAll (m_lock);`.
`IsSet` and `Waiters` live in the **same word** (`m_combinedState`, bit 31 and
bits 0..18), and both sides use `Interlocked.CompareExchange` on it, so the
"set then read waiters" / "bump waiters then re-read set" handshake is totally
ordered. With `set=1` **and** `waiters=1` observed for 130 s, the waiter must
have seen `IsSet == false` after its increment — i.e. `Set()` ran afterwards,
read `Waiters == 1`, and took `lock (m_lock)` to `PulseAll`. **The pulse was
issued. The waiter never returned.**

Two independent interventions, both negative:

| intervention | row | result |
|---|---|---|
| `Monitor.Enter(m_lock); Monitor.PulseAll(m_lock); Monitor.Exit(m_lock)` from the launcher watchdog, 10 times, through `mono_runtime_invoke` | `[TPWK] n=1..10 lock=0x1146c2ce80 exc=0x0 0x0 0x0` | no exception (the lock was free, so the setter was **not** stuck in `lock(m_lock)`), and the waiter stayed parked |
| one spurious wake on **every** queued futex key, 12 rounds from t=31 s (`make RW_WAKEALL=1`) | `[FWAKE] t=31 keys=24 woke=24` … `t=181 keys=25 woke=25` | machine untouched: NFS bytes 19,996,505 → identical, `[XSW] n=1`, same CRC |

The second row is *not* conclusive on its own — glibc's `pthread_cond_wait`
re-checks its own sequence counter, so a bare `futex_wake` on a condvar word is
absorbed. The first row is the interesting one: a genuine managed
`PulseAll` on the very object, from a thread that provably took the lock,
did not release the waiter.

### 37.3a The object's own lock word says the monitor has no inflated sync

`MonoObject` is `{ MonoVTable *vtable; MonoThreadsSync *synchronization; }`, so
`m_lock+8` is the monitor's own word. Sampled at the stall, four times, same
value (`/TPTRACE` only; `RW_FUTXW=1` for the kernel side):

```text
[TPWQ] tid=4 mres=0x111ffc4060 combined=0x80000001 set=1 waiters=1
[TPWM] tid=4 lock=0x1146c2dec0 vtable=0x10400b93e0 sync=0x104d7e79a2 synccls=thin/tagged
[PARK] pid=4 key=0x105B657AB8 val=0x0 to=-1 bitset=0xFFFFFFFF rip=0x1083503376
```

`0x104d7e79a2` is not 8-aligned: it is a **tagged thin-lock value** (the low
bits carry the lock state), not a pointer to an inflated `MonoThreadsSync`.
Meanwhile the waiter is queued on a condvar at `0x105B657AB8`, in Mono's own
data area, and `MRES.Set`'s `lock (m_lock)` + `PulseAll` has already run and
returned. So at the moment of sampling the object **has no inflated sync block
to broadcast on**, while a waiter is still registered on a condvar inside one.
That is the concrete shape of "the pulse was issued and reached nobody", and it
is a *monitor* bookkeeping state, not a task, pool or futex state.

The encoding needs Mono's `monitor.h` to decode exactly (which tag bit means
"thin", and where `MonoThreadsSync::cond` sits), and that is the first step of
the next lane — but the pair above already says the waiter and the pulser are
not on the same sync block at the stall.

### 37.4 What this rules out, and what it leaves

Ruled out by rows:

* **thread-pool starvation / missing worker** — the item the main thread waits
  for was never a pool item; the task ran and completed;
* **a scheduler only the main thread pumps** — `m_taskScheduler` is
  `ThreadPoolTaskScheduler`;
* **`AddTaskContinuation` losing the signal** — the continuation ran
  (`m_continuationObject` is the sentinel, `m_combinedState` bit 31 set);
* **our futex compare/park race** — `l_thr_futex` compares the word and parks
  under `proc_lock`, and the wake path takes the same lock
  (`src/kernel/linux_compat_thr.c`, "Compare under the lock: the waker cannot
  slip between the compare and the park");
* **a dropped kernel wake on that key** — a wake-all of all 24-26 queued keys
  leaves the machine bit-identical, and the park itself is one `FUTEX_WAIT` on
  one key with `val=0` (a condvar word, not a mutex word);
* **§36.8's `Thread.Join` row being on the critical path** — in three sampled
  runs no managed thread was in `Thread.Join` (`[TPWY]` empty; every non-main
  stack resolves to native frames only), and it cannot be the completer anyway:
  the awaited task had already completed.

What is left is the monitor's own bookkeeping: the waiter is queued on a condvar
(`[PARK] pid=4 key=0x105B657AB8 val=0x0 to=-1 rip=0x1083503376`) while the
object's lock word is a tagged thin lock and not an inflated sync (§37.3a), so
`mono_monitor_pulse_all` has nothing to broadcast on. The next steps, in order:

1. **Decode the lock word and the sync layout from Mono's own headers**
   (`monitor.h`: which tag bit means "thin", `MonoThreadsSync::cond`'s offset),
   then print, in one sample, `lock+8`, the sync it points at, and `sync+off(cond)`
   next to the kernel's `[FUTXW] NOW` key. §37.3a already shows the two do not
   match; this makes the mismatch a byte-exact statement instead of a structural
   one, and says whether the waiter is on a *stale, deflated* sync.
2. **A second waiter.** Run the same PLINQ shape in a probe under the guest's
   runtime (`tools/rw/mono-probe/plinq.cs` — seven of eight shapes pass in the
   guest; `openmp-style-nested` hangs, and that hang is the cheapest reproducer
   we have: `bash tools/rw/run-tp-probe.sh --exe obj/tp-probe/plinq.exe`).
   Given a one-minute reproducer, the same `[TPWT]`/`[TPWQ]`/`[TPWM]` reads on a
   *probe* thread would say whether the signalled-bit-with-live-waiter state is
   reachable there too.

No kernel fix is indicated by any row in this section, and inventing one would
be guessing: the compat layer's futex, the pool, the scheduler and the monitor's
*inputs* are all measured correct. The stall is one managed wait that the
runtime cannot break.

### 37.5 The ladder (unchanged, and that is the honest number)

Three runs, `bash tools/rw/run-rimworld-desktop.sh --timeout 400-500
--run-timeout 150-220`:

| run | extras | NFS bytes at deadline | `[XSW]` | frame |
|---|---|---|---|---|
| 1 | `/TPTRACE` (first Task/Thread sample) | 19,996,505 | `n=1 … crc=0x940850f19945c95b changed=0` | `nz=921600/921600 box=0,0,1279,719` |
| 2 | + `/TPPOKE` (10 managed PulseAll attempts) | 19,996,505 | identical | identical |
| 3 | + `RW_WAKEALL=1` (12 spurious wake-all rounds) | 19,996,505 | identical | identical |
| 4 | + `RW_FUTXW=1` (lock-word sample, §37.3a) | 20,003,673 | identical | identical |

Run 4's 20,003,673 bytes is the same loader position read in a slightly different
order (3 more RPCs); it is not progress — the frame, the CRC and the missing
second swap are identical in all four.

One present, one buffer checksum, no second swap, and the loading screen still
on screen: there is no main menu, so no fps number is meaningful. The stall
moved *no* distance, which is the point — the fix is not in the layers this lane
can reach.

### 37.6 Instruments added (all gated)

* `src/user/rwmain.c`: `[TPWI]` field inventory, `[TPWT]` Task dump, `[TPWQ]`
  `SetOnInvokeMres` dump, `[TPWY]` Thread dump, `[TPWA]` per-tid managed stack
  (any live tid, `SIGUSR2`), `[TPWD]` raw-word fallback, and `/TPPOKE`'s
  `[TPWK]` (one `Monitor.Enter/PulseAll/Exit` on the stalled lock). All of it
  runs only under `/TPTRACE`; the poke needs `/TPPOKE` as well.
* `src/kernel/linux_compat_thr.c` + `Makefile`: `RW_WAKEALL=1` →
  `[FWAKE]` and one spurious wake of every queued futex every 8 s from t=30 s
  (rule 24b). Compiled out unless asked for.
* `tools/rw/mono-probe/taskinv.cs` (field/method inventory under the game's
  runtime), `moninv.cs` (Monitor arity), `plinq.cs` (the PLINQ shapes, host and
  guest).
* `tools/rw/run-tp-probe.sh`: stage `System.dll` — without it every
  `ParallelEnumerable` probe dies in `TypeLoadException` on
  `SortHelper\`2._sharedBarriers` before the first query.
* `tools/rw/run-rimworld-desktop.sh`: `RW_TPPOKE=1` stages `/TPPOKE`.

### 37.7 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **414 run, 0 failed**. Gates:
`t3_linux_probe redzone` PASS, `m2_mono` PASS, `m2_tp` PASS (clock row
included), `m2_threads` PASS, `m12_thread_ceiling` PASS, `m3_window_probe`
PASS, `m4_gl_probe` PASS.

## 38. 2026-11-05 — M6/L51: the PLINQ "reproducer" is a 200× latency artefact, and Mono's monitor cannot lose a wake

§37 left one lever: `openmp-style-nested` "hangs in the guest only", which would
be a one-minute reproducer of the game's lost wake. **This lane measured that
lever and it does not exist.** The probe shape completes; it is simply ~200×
slower in the guest, and the probe's deadline expires first. The monitor path
was then tested directly: an ordered `Monitor.Wait`/`PulseAll` storm loses no
pulse in the guest, and the `ManualResetEventSlim` Dekker handshake -- the one
place a *stray* wake could be dropped in managed code -- ran 1,000,000
iterations in the guest with **zero** lost windows.

### 38.1 `openmp-style-nested` is slow, not hung

`min3.cs` runs each shape inside a `Task` and awaits it with a timeout, so one
boot produces the whole table instead of stopping at the first failure. Guest
(8 vCPU) rows, `--exe obj/tp-probe/min3.exe`:

| row | guest | host |
|---|---|---|
| `seq-sum-64`, `flat-outer-dop8-300k`, `par-sum-300k/64/1024/16384/131072`, `par-toarray-64/4096` | **done=True** | done=True |
| `nested-seq-body-64-dop8` (body uses sequential LINQ) | **done=True** | done=True |
| `nested-par-body-64-dop8`, `-dop1`, `-4096-dop8` (body starts a PLINQ query) | done=False `status=Running` at 8 s | done=True |

`Running` is not a deadlock: `min6.cs` counts nested-query completions and
prints progress every second. With 40 nested queries and a 150 s budget:

```text
PROGRESS t=2754ms nested_done=1 status=Running
PROGRESS t=9800ms nested_done=15 status=Running
PROGRESS t=18915ms nested_done=35 status=Running
PROGRESS finished at 21568 ms
ROW nested40 done=True nested_done=40 ms=21569 status=RanToCompletion
```

Steady progress, and the outer task reaches `RanToCompletion`. The cost is
**~500 ms per nested query**: `min7.cs` measures 527 ms/query in the guest and
2 ms/query on the host, and it is **unchanged by `MONO_THREADS_PER_CPU=8`**
(`/TPMIN8`, i.e. a 64-thread pool minimum: `per_query_ms=527`). So it is not
thread-pool starvation and not thread-injection hill climbing either; every
nested query pays the same fixed ~500 ms, and `openmp-style-nested` (which runs
1000 of them) needs ~500 s -- longer than any probe deadline this track uses.

**Consequence: `openmp-style-nested` cannot be used as a reproducer.** Its
`SHAPE ... start` with no `OK` is a deadline expiry, and no task in it ever
reaches §37's state (complete task + signalled event + parked waiter).

### 38.2 §37.3a's "thin lock" is a misdecode: the monitor IS inflated

`mono/metadata/monitor.h` defines the lock word by its **low two bits**, not by
alignment:

```text
LOCK_WORD_FLAT = 0, LOCK_WORD_HAS_HASH = 1, LOCK_WORD_INFLATED = 2
64-bit: LOCK_WORD_INFLATED = [sync:62 | status:2]
```

so `sync=0x104d7e79a2` is `0x…a2 & 3 == 2` = **inflated**, with
`MonoThreadsSync *mon = 0x104d7e79a0`. §37.3a read it as "a tagged thin lock"
because `(sync & 7) != 0`; the object in fact has exactly the fat monitor a
queued waiter requires, and `mono_monitor_pulse_all` had a `wait_list` to walk.
That premise -- "no inflated sync block for `PulseAll` to broadcast on" -- is
withdrawn.

### 38.3 The monitor has no lost-wake mechanism to lose

Mono 6.12's `Monitor.Wait` is not a condvar broadcast on the monitor at all
(`mono/metadata/monitor.c`, 6.12.0.122):

* `mono_monitor_wait` creates a **private auto-reset `MonoW32Handle` event per
  wait**, does `mon->wait_list = g_slist_append (mon->wait_list, event)` **while
  it still owns the monitor**, *then* saves `nest`, releases, and blocks in
  `mono_w32handle_wait_one (event, ms, alertable)`.
* `mono_monitor_pulse` runs only while the caller owns the monitor, walks
  `mon->wait_list` and calls `mono_w32event_set (node->data)` -- i.e. sets that
  event's **sticky** `signalled` flag and signals its `signal_cond`.
* `mono_w32handle_wait_one` loops `if (own_if_signalled (handle)) return SUCCESS;`
  → `mono_w32handle_timedwait_signal_handle(...)` (a `pthread_cond_wait` on
  `handle->signal_cond` under `handle->signal_mutex`) → re-check. Because the
  flag is sticky and re-checked under the same mutex the signaller takes, an
  early pulse, a late pulse and a lost `pthread_cond_signal` all end in
  `SUCCESS`; the waiter cannot be stranded by a wake that went missing.

The only remaining hole would be the managed handshake: if `Set()` read
`Waiters == 0` it would skip `lock (m_lock) { PulseAll }` entirely. Mono's
`ManualResetEventSlim` closes that with a CAS-then-read on both sides -- and the
comment in its own source says so (`ManualResetEventSlim.cs:649-663`: "we must
ensure there cannot be any reordering of the assignment to Waiters and the read
from IsSet ... guaranteed as Waiters{set;} involves an
Interlocked.CompareExchange"). **That guarantee holds in this guest**, measured
two ways:

| probe | guest | host |
|---|---|---|
| `min7` ordered `Monitor.Wait`/`PulseAll`, 3000 rounds × 4 waiters = 12,000 waits, a pulse only after the waiter published its phase | `STORM ... ms=16054 joined=True` (no `STORM-HANG`) | `ms=85 joined=True` |
| `min8` the MRES handshake exactly: waiter `CAS(state, s\|1)` then read bit 31; setter `CAS(state, s\|0x80000000)` then read bits 0..18; both with CAS retry and plain reads | **1,000,000 iterations, `lost_windows=0`, `DEKKER-OK`** | 300,000 iterations, `lost_windows=0` |

So neither the monitor's own bookkeeping, nor the managed CAS/read pairing that
would drop a pulse, nor a condvar wake, can produce "pulse issued, waiter still
asleep" here. What the guest *does* show is that everything is slow (the storm's
12,000 waits take 16 s in the guest against 85 ms on the host) -- latency, not
loss.

### 38.4 What the stall actually looks like: a quiescent process

The game run with `/TPTRACE` + `RW_FUTXW=1` stopped earlier than §37's runs (see
38.6), and its scheduler dump is the useful part:

```text
[ALIVE-SET] no READY work, yet the scheduler counts live work ...
  slot=28 pid=28 st=8 ... pc=0x108011895d Loading.Preload
  slot=5  pid=5  st=8 ... pc=0x1083506454 SDLTimer
  slot=7  pid=7  st=8 ... pc=0x1083506454 Finalizer
  (25 live slots, every one st=8 = parked)
```

Every thread in the process is parked and the scheduler has no ready work. That
is a **deadlock or an unfulfilled wait**, not a wake dropped in flight: a wake
that was lost in the kernel would leave the waker runnable and the process
otherwise busy. It also means the main thread's completer is either gone or
parked, so the *next* question is not "did the pulse get lost" but "which wait is
the main thread in, and is its event on that monitor's list at all".

### 38.5 Instruments added

* `src/user/rwmain.c`: `[TPWM]` now decodes the whole monitor from Mono's own
  header -- `cls={free|flat|hash|inflated}`, `status`, `owner`, `nest`, `ent`,
  `wbit`, `entcond`, `entmutex`, `nwl` (wait_list length) -- and `[TPWH]` prints
  the first queued event's `type/ref/sig/cond/mutex`. With `RW_FUTXW=1`'s
  `[PARK] key=` in the same log, the four cases are one row: `nwl=0` (the waiter
  is not on the pulser's list), `sig=1` (the event was set and the waiter is
  still parked → below Mono), `sig=0` (the pulse never ran), or `cond !=`
  `[PARK] key` (a different wait entirely).
* `tools/rw/mono-probe/condtrace.c` + `tools/rw/run-tp-probe.sh --condtrace`:
  condvar flight recorder linked **into** the launcher (`-rdynamic` + a version
  script, the rule-28 pattern). Two traps, both measured: a full `-rdynamic`
  export breaks Boehm (`Wrong __data_start/_end pair`), and
  `dlsym(RTLD_NEXT, "pthread_cond_timedwait")` resolves **libc's `GLIBC_2.0`
  compat symbol**, whose pre-2.3.2 condvar layout makes every timed wait return
  `ETIMEDOUT` at once (`Task.Wait(5000)` on a 100 ms task returned in 3 ms).
  `dlvsym(..., "GLIBC_2.3.2")` is required.
* `tools/rw/ct-analyze.py`: pairs every wait with its return and reports waits
  that never returned, plus whether their cond was signalled after the wait
  began.
* `tools/rw/mono-probe/min.cs` … `min8.cs`: the ladder -- pool-free shapes,
  nested-query latency, ordered storms, and the Dekker test.
* `tools/rw/run-tp-probe.sh --tpmin8`: stages `/TPMIN8`, which makes
  `mono_main_launcher.c` set `MONO_THREADS_PER_CPU=8` before `mono_main`, the
  pool-minimum differential.

### 38.6 Ladder and regression

`/TPTRACE` + `RW_FUTXW=1` (`bash tools/rw/run-rimworld-desktop.sh --timeout 400
--run-timeout 200`) reached **23,643,577 bytes** of 729 MiB and then the
quiescent stall of 38.4, with `[XE] PUT` **0** and `[XE] FLUSH` 1 -- i.e. this
run stalled *before* the first present, one step earlier than §37's one-frame
runs. A second, **clean** run (no `/TPTRACE`, no `RW_FUTXW`) stopped in the same
place: **19,999,577 bytes**, `[XSW]` rows **0**, and the same "no READY work"
dump with every live slot `st=8`. So the earlier stop is not caused by the trace
flags; whether the launcher rebuild (whose only behavioural change is a
`/TPTRACE`-gated dump) moved the stop point is **not** separated here, and the
integrator must re-run the ladder on its own tree before reading 20.0 MB as this
lane's frontier.

The screenshots show the loading screen (65,534 non-black pixels, 16,589
colours) and there is no main menu, so no fps number exists. The per-futex
instrumentation is therefore not free: `RW_FUTXW=1` alone changes where the run
stops, and any ladder row taken with it must say so.

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **414 run, 0 failed**, gates green
(`redzone`, `m2_mono`, `m2_tp`, `m2_threads`, `m12`, `m3_window_probe`,
`m4_gl_probe`). `RimWorldLinux.ad9` was rebuilt and the new rows verified in the
binary (`strings ... | grep -E 'TPWH|nwl='`).


## 39. 2026-11-05 — M6/L52: the monitor was never missing; a realtime-deadline clamp killed every pool park, and every wake waited for the tick

The brief for this lane was "the pool never grows past 8 because its
hill-climbing monitor thread is never created". **That premise does not
survive Mono's own trace.** The monitor is created on the *first* worker
request (`monitor_ensure_running()` -> `mono_thread_create_internal(...,
MONO_THREAD_CREATE_FLAGS_THREADPOOL | MONO_THREAD_CREATE_FLAGS_SMALL_STACK)`),
and `monitor_thread()` prints `monitor thread, started` from inside the new
thread -- so a later `created`/`unparked` row is *growth*, not existence. The
row is present in every archived run that had `/TPTRACE` (L44, L45) and in all
three L52 runs:

```text
Mono: [0x1142fde700] monitor thread, started
Mono: [0x1142fde700] monitor thread, finished      <- 60 s after the last item
```

`monitor_thread, created` is zero in the game because the 8-worker ceiling is
never hit there (`try create worker, failed: maximum ... reached` = 0 in the L52
runs), not because the monitor cannot grow. Driven past the minimum, the
monitor does grow: `min9.cs` with eight workers held blocked logs
`monitor thread, created` and `try create worker, created ... count = 9, 10` in
the guest. So "creation attempted -> rc" is answered by `started`, and the
growth path by `created`.

### 39.1 The real defect: every absolute CLOCK_REALTIME futex deadline expired at once

`mono_os_sem_timedwait()` builds `gettimeofday() + timeout_ms` and hands the
absolute timespec to glibc's `sem_timedwait()`, which passes it to
`futex(FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME)`. `l_timespec_to_ms()` clamped
any `tv_sec > 1e8` to `1e8 ms` **before** the absolute subtraction. Every
realtime deadline is ~1.79e9 s, i.e. always over that bound, so a 5..60 s pool
park became `1e8 ms - 1.79e12 ms` -> negative -> 0, and the futex returned
`-ETIMEDOUT` without ever parking. Measured in the guest, `min10.cs`:

| row | before (L52, pre-fix) | after |
|---|---|---|
| `worker timed out` | 79 of 79 parks | **0** |
| `worker unparking, timeout? no` | 0 | **186** |
| `try create worker, created` | 80 (one per item) | 10 (startup only) |
| `[FTMO] ... to=` | never printed (to was 0) | `to=45886 now=1793928074574` |

The clamp now bounds at 1e10 s (so realtime deadlines survive) and the 27.8 h
cap is applied *after* the absolute subtraction, where it belongs. The 5 s
semaphore case was added to `linux_probe_clock.c` (`sem_mono_5s_waits_5000ms`
PASS), and that probe also measures a two-thread semaphore ping-pong:
**365 us per wake+switch** (200 rounds, 73 ms).

### 39.2 A wake marked the thread READY but poked no CPU

`l_futex_wake_locked()` only set `p->state = PROC_STATE_READY`. Every other CPU
was halted in `safe_wfi()` and returned through `schedule()` only on the 100 Hz
timer broadcast, so a queued work item started 10-30 ms later (host: 0 ms).
`min10.cs` B (`QueueUserWorkItem` + busy-spin, no blocking wait at all) showed
it was scheduling, not waiting: med 30 ms, max 898 ms.

`sched_wake_idle_cpu()` (src/kernel/process.c) now sends one directed 0x81
reschedule IPI to an idle CPU after a futex wake, a `process_wakeup`, a
`process_wake_all`, `process_create` and `process_clone`. It pokes at most one
CPU per wake and only when idle, so a busy machine is unchanged. Effect on the
12,000-wait monitor storm: **16,054 ms -> 983 ms**; on the nested PLINQ oracle
(`min7.cs`): **527 ms/query -> 132 ms/query** (host 1-2 ms).

The residual ~12 ms per task round trip is *not* the futex wake (365 us
measured); it lives in Mono's managed dispatch and is not yet named. It is the
next lever, not a regression: the two fixes above took the guest from
"workers cannot park at all" to "workers park and unpark in 0.2-1 ms".

### 39.3 Ladder, 3 runs on this tree

`RW_TPTRACE=1 bash tools/rw/run-rimworld-desktop.sh --run-timeout 240`, three
consecutive runs, no `/TPCPU`, no `/RW_FUTXW`:

| run | NFS bytes | `[XSW]` | Player.log | pool |
|---|---|---|---|---|
| r1 | 23,643,577 (0x168CDB9) | 0 | `Loaded All Assemblies, in 11.990 s`, `RimWorld 1.6.4871 rev600` | started 1, created 0, 14 workers, 58 unparks, 15 retire-timeouts |
| r2 | 23,643,577 | 0 | `Loaded All Assemblies, in 11.001 s` | same shape |
| r3 | 23,643,577 | 0 | `Loaded All Assemblies, in 11.025 s`, `RimWorld 1.6.4871 rev600` | same shape |

The stop is deterministic and 3.7 MB *later* than §38's 19.9 MB, but it is the
same barrier: the last `[NFSIO]` row is frozen for the final 60 s, `[XSW]` is
0, and the screen is the static loading art (50.4% non-black, 75,883 colours,
identical from shot 05 to `99_final` -- `[XSW]=0` is the honest row). No main
menu, so no fps number exists.

### 39.4 What the quiescent dump says now

All three runs end with `[ALIVE-SET] no READY work` and:

* every live Unity thread `st=8`, `pc=0x108011895d` (`UnityPlayer+0x11895d`) --
  the job-system wait -- including `Loading.Preload`, `Loading.AsyncRe`,
  `BatchDeleteObje`, `UnityGfxDeviceW` and 20 `Background Job.` threads;
* the main thread `pid=4`, `st=8`, `pc=0x1083503376` = `libpthread+0xf376`
  (`pthread_cond_wait`) -> `mono_wait_uninterrupted` on a WaitHandle;
* the pool is **empty but healthy**: 14 `Thread Pool Wor` slots are `st=9`
  (THREAD_DONE, retired after their 5..60 s park), zero `request worker,
  failed`, and `[IDLESTUCK] claims: c0..c7 = -1/...` (every CPU idle).

So the pool is no longer the suspect: it grew to 14, parked and unparked 58
times, and retired normally. The remaining wait is one Unity job that is never
handed work -- the main thread's `taskDoneEvent` waiter and the Unity job
threads are all parked, and nothing in the process is READY. This is the
"unfulfilled wait" of §38.4 with the pool exonerated by its own rows; naming
the Unity-side owner of `0x108011895d` is the next instrument (that PC is not
in `UnityPlayer.so`'s file-backed `.text` at the same offset, so the module
base in the dump needs re-deriving first).

### 39.5 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` -> **414 run, 0 failed**; gate ladder
`redzone`, `m2_mono`, `m2_tp`, `m2_threads`, `m12`, `m3_window_probe`,
`m4_gl_probe` re-run on this tree (see obj/l52-gates.log for the raw rows).

The exact rows, r3: `[IDLESTUCK] slot=4 pid=4 st=8 wake=0
futex=0x105b762d28 rip=0x1083503376 /NFS/RimWorldLinux` (an infinite
`pthread_cond_wait` on a heap MonoW32Handle+0x38 -- the managed WaitHandle
path of §34.2), while the Unity job threads wait with
`wake=0xFFFFFFFF+now` (49 days: `MONO_INFINITE_WAIT` passed as a relative
timeout). Nothing in the process holds a runnable state, so this is an
*unfulfilled* wait, not a dropped wake: the completer is either gone or was
never queued. The next test is on the managed side, not the kernel: Mono's
`ThreadPoolWorkQueue` keeps a work-stealing queue per worker thread, and a
worker that exits after its 5..60 s park can orphan the continuations in that
local queue -- no global request is made for them, which is what an empty pool
with `request worker, failed = 0` looks like. Instrument
`ThreadPoolWorkQueueThreadLocals` teardown (does it transfer the local queue to
the global one?) before touching the scheduler again.

## 40. 2026-11-05 — M6/L53: there is no local-queue orphan; the PLINQ event WAS set when the waiter stopped

The brief was §39.4's hypothesis: a Thread Pool worker exits after its 5..60 s
park and orphans continuations in its managed `WorkStealingQueue`, so no global
request is ever made for them. **That hypothesis is false on this tree, and the
pool is exonerated by its own read state, not by its trace.**

### 40.1 How the managed queue state is read

The field names were measured on the *host* under the game's own runtime
(`tools/rw/mono-probe/tpq.cs`, `tpq3.cs`, run through
`mono_main_launcher.c` with the game's `libmonobdwgc` and `mscorlib`):

```text
ThreadPoolWorkQueue: queueHead, queueTail (QueueSegment, INSTANCE fields),
                     allThreadQueues (SparseArray<WorkStealingQueue>, static),
                     numOutstandingThreadRequests (instance)
ThreadPoolWorkQueue+WorkStealingQueue:
                     m_array, m_mask, m_headIndex, m_tailIndex
ThreadPoolWorkQueue+QueueSegment:
                     nodes, indexes (lo16 = head, hi16 = tail), Next
SparseArray<T>:      m_array (16 slots, T[])
ThreadPoolGlobals:   workQueue (static; the instance the three instance
                     fields live on)
```

`tpq3.cs` validates the index semantics: three `Task` enqueues from a worker
moved that worker's `tail 0 -> 3` and the steals moved `head 0 -> 3`. So
`used = tail - head` is the deque depth, `m_headIndex` counts steals, and
`m_tailIndex` is local pushes minus local pops.

The launcher reads this with the same metadata API as the Task dump (rule 102,
`mono_field_get_value` / `mono_field_static_get_value` with the class vtable,
`mono_array_addr_with_size`), gated on `/TPTRACE`, and prints:

* `[TPWP] why=… n=… nonempty=… items=… unowned_items=… glob=… outreq=…`
* `[TPWL] i=… q=0x… head=… tail=… used=… cap=… owner=… dhead=… dtail=…`
  (owner is the tid whose captured stack holds the matching
  `ThreadPoolWorkQueueThreadLocals` object, exact-vtable test);
* `[TPWG]` per QueueSegment (global chain), and one `[TPWO]` row per queue
  found on a worker stack.

### 40.2 The per-worker table at the stall

Three runs on the L52 kernel, `/TPTRACE` only (no `RW_FUTXW`), sampled
from t=35 s onward (r4 reached t=175 s; r3/r5 sampled fewer times and never
showed a non-zero queue):

| run | NFS bytes | `[XSW]` | queues `n` | nonempty | items | global | outreq | enq/comp/reqF/stranding |
|---|---|---|---|---|---|---|---|---|
| r3 | 23,653,953 (0x168EE41) | 1 | 7 | 0 | 0 | 0 | 0 | 14/14/0/0 |
| r4 | 23,654,097 (0x168EED1) | 1 | 9 | 0 | 0 | 0 | 0 | 14/14/0/0 |
| r5 | 23,654,097 (0x168EED1) | 1 | 8 | 0 | 0 | 0 | 0 | 14/14/0/0 |

Every queue in every sample is `head=0 tail=0 used=0 cap=32`; `dhead=dtail=0`
between consecutive samples. `head=0` means no steal ever touched these
deques. `tail=0` is the deque's **net** local depth, not a cumulative push
counter — `LocalPop` decrements `m_tailIndex` — so the indices cannot
reconstruct how many items were ever pushed and locally popped; what they prove
is that no item remains in any local queue at any sample and that no item was
stolen. The global `QueueSegment` chain is empty and
`numOutstandingThreadRequests` is 0, so `EnsureThreadRequested()` would ask for
a worker if an item existed. Every row also reports `owner=-1`: the
queue-to-tid match found no live thread whose captured stack still holds that
`ThreadPoolWorkQueueThreadLocals` object (most pool workers are `st=9`
THREAD_DONE by then, and a parked `Dispatch` may keep `tl` in a register). A
retired worker that had exited with an item would still show it as `used>0` in
its registered queue; none does.

Consequences, in one line each:

* no continuation is left in any local queue at the stall, so there is nothing
  for a retired worker to have orphaned; and because every enqueued item
  completed (`enq == comp`, `stranding=0`), no item was lost on a teardown
  path either;
* `enq == comp` (14/14) and `stranding=0` mean no work item is queued,
  running, or unaccounted for;
* the pool is empty because there is no work for it, not because it declined
  to run work.

### 40.3 What the waiter actually waits for

The main thread's self-sample (`SIGUSR2`, rule 107) is identical in every
sample of every run:

```text
Monitor.ObjWait <- Monitor.Monitor_wait <- Monitor.Wait <- Monitor.Wait
<- ManualResetEventSlim.Wait <- Task.SpinThenBlockingWait
<- Task.InternalRunSynchronously <- Task.RunSynchronously
<- SpoolingTask.SpoolStopAndGo <- DefaultMergeHelper`2.Execute
<- MergeExecutor`1.Execute
```

i.e. the same PLINQ merge site as §36/§37, not a Unity job wait. The awaited
object is a `Task.SetOnInvokeMres` (ManualResetEventSlim subclass) at
`0x…d3f9c0` with:

```text
combined=0x80000001   set=1 waiters=1
m_lock=0x…42ca0  sync=0x…7c82 cls=inflated  mon=0x…7c80
status=0x7fff0000 (ENTRY_COUNT_ZERO: owner 0, entry_count 0 = released)
wait_list = 0 (at the CORRECT offset, see below)
```

The status is a released monitor, as it must be while its owner is inside
`Monitor.Wait`. The bit decode is this mscorlib's own IL
(`tools/rw/mono-probe/mresil.cs`): `get_IsSet` masks `0x80000000`,
`get_Waiters` masks `0x7FFFF`. The `set=1` bit plus `waiters=1` is already
decisive about the setter: this mscorlib's `ManualResetEventSlim.Set(bool)` IL is
`set_IsSet(true)`; `if (Waiters > 0) lock (m_lock) Monitor.PulseAll(m_lock)`.
A parked waiter with `waiters=1` can only have incremented its count (under
`m_lock`) before `Set` read it, so `Set` saw a waiter and ran `PulseAll`.

**Monitor layout correction (this build).** Unity's Mono 6.12 is compiled with
`HAVE_MOVING_COLLECTOR`: `MonoThreadsSync` carries an extra `gint32 hash_code`
at +8, so `wait_list` is at **+16**, `data` +24, `entry_mutex` +32,
`entry_cond` +40; `GSList` is `{data at +0, next at +8}`. This is not a guess:
the game's own `mono_monitor_wait` (the `Monitor.Monitor_wait` icall resolves to
`libmonobdwgc+0x1ae0ee`, which calls `0x23c961`) does
`mov 0x10(%rbx),%rdi; call g_slist_append; mov %rax,0x10(%rbx)` at
`0x23c9fe..0x23ca53`, and the same function calls
`mono_w32handle_wait_one(event, ms, TRUE)` at `0x23ca7b`. So:

* rule 105's `wait_list = mon+8` is the **non-moving** layout; every earlier
  `nwl=0` on this build (including §37/§38) was reading `hash_code`, not the
  list. The launcher now prints `nwl` from +16.
* `mono_monitor_pulse` sets the first wait_list event and removes its node.
  `wait_list == NULL` at the stall therefore means **the pulse already ran and
  dequeued this waiter's private event**. `mono_monitor_wait` only removes the
  node itself on the failure path (no pulse), so an empty list with a parked
  waiter is not "never signalled": it is "signalled and dequeued".

So the wait graph has no missing producer and no cycle:

```text
main (pid 4)
  SpoolingTask.SpoolStopAndGo
    -> Task.RunSynchronously -> SpinThenBlockingWait
       -> ManualResetEventSlim.Wait (set=1, waiters=1)
          -> Monitor.Wait on m_lock (wait_list drained by PulseAll)
             -> mono_w32handle_wait_one(private event)
                -> pthread_cond_wait   [parked, st=8]
```

The event was set; the only thread that could consume it is the one that is
parked on it. That is a lost wake **below** the managed event, not an orphaned
continuation and not a missing signal.

The in-guest handle scan corroborates the set state: a `MonoW32Handle` of
`type=3` (`MONO_W32TYPE_EVENT`), `ref=3`, **`signalled=1`**, `in_use=1` is
present at `0x104005fdd8` in five stack slots of the main thread in every
sample. (Its `signal_cond` is `0x104005fe10`; the kernel's park key is a
different heap address, so the exact handle↔key identity is the one row still
to be pinned — see 40.6.)

### 40.4 The Unity native threads are idle, and the "UnityPlayer+0x11895d" label was wrong

`[ALIVE-SET]` prints `mod=<base>+<off>` on this kernel, and the module walk
merges contiguous read-only file regions (rule 97's class). The real load bases
from the launcher's own `dl_iterate_phdr` row are:

```text
libc.so.6          base=0x1080000000  end=0x10801f1660
UnityPlayer.so     base=0x10812ff000  end=0x108337fc00
libmonobdwgc-2.0   base=0x1084522000  end=0x1084cf3048
libpthread.so.0    base=0x10834f4000
```

`0x108011895d` is **libc.so.6+0x11895d**, not `UnityPlayer+0x11895d`: it is the
`cmp $0xfffffffffffff001,%rax` **immediately after `syscall`** in glibc's
`syscall@@GLIBC_2.2.5` (0x118940..0x118976). All 25 Unity job/preload threads
(`Job.Worker 0`, 20x `Background Job.`, `BatchDeleteObje`, `Loading.AsyncRe`,
`UnityGfxDeviceW`, `Loading.Preload`) are parked in that same generic syscall
wrapper — i.e. in raw futex/semaphore waits on their own per-thread heap keys
(`0x10D0…`), not in a UnityPlayer wait loop. They are idle job consumers; none
of them is a producer for the main thread's managed event, and none should have
been woken: there is no job queued for them.

`Loading.AsyncRe` (pid 26) has **no managed stack**: 2048 captured words
resolve to zero JIT frames and its top words are the same libc `syscall()`
return point. It is a native Unity thread, so "get its managed stack" has the
answer "it has none".

### 40.5 Ladder, 3 runs

`RW_TPTRACE=1 RW_SHOT_EVERY=10 bash tools/rw/run-rimworld-desktop.sh
--run-timeout 240`, three runs on the L52 kernel:

| run | NFS bytes | `[XSW]` | assemblies | queues at stall | `set/waiters` | verdict |
|---|---|---|---|---|---|---|
| r3 | 23,653,953 | 1 | 10.851 s | 7, all empty | 1/1 | stall |
| r4 | 23,654,097 | 1 | 10.841 s | 9, all empty | 1/1 | stall |
| r5 | 23,654,097 | 1 | 8.930 s | 8, all empty | 1/1 | stall |

All three: banner `RimWorld 1.6.4871 rev600`, one 1280x720 present
(`[XSW] n=1 put=1 putok=1 nz=921600/921600 box=0,0,1279,719`), then NFS frozen
and no second swap. The final PPM is byte-identical to shots 18..22:
**1,044,712 / 2,073,600 non-black pixels (50.38%), 75,884 colours** — the
static loading art, no main menu, so no fps number exists. (r5's watchdog
sampler stops after its t=65 s sample; the game state it had printed is
identical to r3/r4 and the run still stalled at the deadline, so the row is
kept and the sampler limit noted.)

### 40.6 The layer verdict and the next single instrument

* **Local-queue orphan: no.** No local queue holds an item at any sample, no
  queue was ever stolen from (`head=0`), and every enqueued item completed
  (`enq == comp == 14`, `stranding=0`), so nothing was left behind by a
  worker exit. Mono's `ThreadPoolWorkQueue` teardown is not implicated, and no
  engine-side worker-exit change is warranted.
* **Unwoken Unity native thread: no.** The 25 native threads are idle job
  consumers in raw futex syscalls; the park PC that looked like UnityPlayer is
  libc's `syscall()` return point, and the main thread's wait is the managed
  PLINQ completion event.
* **The waiter**: `SpoolingTask.SpoolStopAndGo` -> `Task.RunSynchronously` ->
  `SpinThenBlockingWait` on a `Task.SetOnInvokeMres` whose event is
  `set=1, waiters=1`, with its monitor's `wait_list` already drained by
  `PulseAll`. The setter is the task-completion path
  (`Task.SetOnInvokeMres.Invoke`), and it already ran.
* **Remaining defect**: the private event's signal did not wake the thread
  parked in `pthread_cond_wait` — one layer below the managed event. The next
  instrument is the §38 condtrace on that event's `signal_cond` (count
  `pthread_cond_signal`/`broadcast` calls after the wait began): signal count 1
  proves a kernel condvar wake delivery loss; count 0 means the pulse walked an
  event other than the parked one and the handle↔key identity must be resolved
  first. Do not re-open the pool, the scheduler, or Unity's job threads.

### 40.7 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` -> **414 run, 0 failed**. Gate ladder
on this tree, all PASS:

| gate | row |
|---|---|
| `redzone` | PASS (32 `KEPT`, 0 `CLOBBERED`) |
| `m2_mono` | PASS — official Mono 6.12 ran a threaded C# program |
| `m2_tp` | PASS — clock and ThreadPool park/unpark |
| `m2_threads` | PASS — 8 pthreads, mutex+condvar+TLS+join+dlopen TLS |
| `m12` | PASS — 150 threads + TLS + shared futex + SIGCANCEL |
| `m3_window_probe` | PASS — whole SDL XCreateWindow sequence |
| `m4_gl_probe` | PASS — GL 3.3 core triangle presented and torn down |

The browser and WebKit tiers were not touched; the only changed userspace file
is `src/user/rwmain.c` (launcher instrumentation, `/TPTRACE`-gated) plus the
host-only probes under `tools/rw/mono-probe/`.

## 41. 2026-11-05 — M6/L54: the private-event condvar was never the one Mono signalled — our own `GLIBC_2.2.5` interposer built a second one

The brief was the last hop of §40.6: the pulse woke *an* event, that event
was `signalled=1`, and the thread was still in `pthread_cond_wait`. **The
identity row says the thread was never parked on the event the pulse set.**
The waiter's cond was a private 0x30-byte heap object created by glibc's
`GLIBC_2.2.5` compat entry for `pthread_cond_wait`, which this launcher
resolved from its own `-rdynamic` wrapper. Signal/broadcast were not
interposed, so Mono lifted the handle's real cond while the thread slept on
the compat copy.

### 41.1 Handle→cond identity: the parked cond is a malloc'd 0x30-byte block

The two offsets come from the binaries, not from source:

* glibc 2.31 (pinned `/tmp/glibc231`), `pthread_cond_wait@@GLIBC_2.3.2` at
  `f160`: `f1f5 lea 0x28(%rbp,%r9,1),%r15` and then the futex syscall with
  `rdi=r15`.  The futex key is **`cond + 0x28 + 4*g`**.
* Unity's `libmonobdwgc`, `mono_w32handle_wait_one` at `0x20de36`:
  `20dec8 lea 0x38(%rbp),%rdi; add $0x10,%rbp; mov %rbp,%rsi; call
  24f404`.  `signal_cond` is **`handle + 0x38`**, `signal_mutex`
  `handle + 0x10`.

So a w32handle event's key must be `handle + 0x60 + 4*g`, and the 16-byte
header at `key - 0x60` must read `{type, ref, signalled, in_use}` with small
sane values.  The L53 stall fails that test:

| row | value | meaning |
|---|---|---|
| parked key | `0x105B68DB18` | from the kernel `[FUTEX] q slot=4` |
| `key - 0x38` | `0x0 0x41` | glibc **malloc chunk header**: prev_size 0, size 0x41 = a 0x30-byte request + 8 + PREV_INUSE |
| `key - 0x60` | `0x105B68DAB8` | not a handle: that address is 8 bytes before the chunk header, inside the previous allocation |
| the signalled EVENT | `0x104005FDD8` `sig=1` `in_use=1` | a real, different handle; its cond would be `0x104005FE10` → key `0x104005FE38` |

The `0x41` is the giveaway: `0x30` is exactly `sizeof(pthread_cond_t)`, and
nothing in `MonoW32Handle` allocates its cond separately.  The thread was
parked on a cond that lives in its own malloc block, while the handle's
inline cond (at `handle+0x38`) was the one `mono_w32event_set` signalled.

### 41.2 The mechanism: `pthread_cond_wait@GLIBC_2.2.5` allocates a private "2.0" cond

`src/user/rwmain.c` used to define

```c
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  real = dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.2.5");
  return real(c, m);
}
```

libpthread 2.31's `pthread_cond_wait@GLIBC_2.2.5` (`0x10650`) is
`__pthread_cond_wait_2_0`:

```text
1065b  mov (%rdi),%r12          ; r12 = *(void**)cond
1065e  test %r12,%r12
10661  je 10678                 ; zero -> allocate
10678  calloc(1, 0x30)          ; the "2.0" cond
10692  lock cmpxchg %rdi,(%rbx) ; store it into the REAL cond's first word
106a9  jmp f160                 ; wait on the private copy
```

Every freshly `calloc`'d `MonoW32Handle` has a zeroed `signal_cond`, so the
first wait takes the `calloc` path: the real cond's `__wseq` is overwritten
with a pointer to a private 0x30-byte cond, and the thread parks on
`private + 0x28`.  Mono's `pthread_cond_signal`/`broadcast` were **not**
wrapped, so they act on the real cond (`handle+0x38`): different futex word,
nobody queued there, permanent sleep.  An early pulse is equally fatal — the
`signalled` flag is set, but the waiter is not on the cond the pulse wakes.

This also explains why the whole pool was healthy: timed parks go through
`pthread_cond_timedwait`, which already resolved `GLIBC_2.3.2`; only the
infinite `Task.SpinThenBlockingWait` path uses plain `pthread_cond_wait`.

Host proof with the same pinned libpthread (`fakecond.c`, cond at offset 0x38
of a zeroed 0x70-byte block, signal on the real cond):

| wrapper version | first word after park | signal wakes waiter? |
|---|---|---|
| `GLIBC_2.2.5` | heap pointer to private cond | **no** (`waiter_out=0`) |
| `GLIBC_2.3.2` | `0x2` (a real `__wseq`) | yes (`waiter_out=1`) |

### 41.3 The fix

All four cond functions are now interposed by
`tools/rw/mono-probe/condtrace.c`, which resolves `GLIBC_2.3.2` for
`pthread_cond_wait` and `pthread_cond_timedwait` and the same version for
signal/broadcast; rwmain's two broken wrappers are gone (rule 108's warning
applied to `timedwait` only — `wait` had the same defect with a worse
symptom).  `condtrace` also keeps per-cond counters and is linked into the AD-9
launcher when `RW_CONDTRACE=1`.  The counters distinguish "no wait was ever
entered on this object" from "a wait was entered and never woken": in the
pre-fix run the cond under the kernel's parked key reported `waits=0`.  In
the fixed run (ladder r3) one handle produced both halves of the same wait:

```text
[CT] S tid=34 cond=0x000000104005fe10 inwait=1 k0=0x104005fe38 k1=0x104005fe3c
[CT] w tid=4  cond=0x000000104005fe10 mutex=0x000000104005fde8
```

`mutex + 0x10 = 0x104005fdd8` and `cond - 0x38 = 0x104005fdd8`: the wait and
the signal are on the **same real MonoW32Handle** — the very handle §40 saw
`signalled=1` while the thread stayed asleep — and the main thread's wait
returned and the loader resumed.  That is the last hop closed, with rows.

### 41.4 What the fix exposed: three NFS walls, in order

The stall was hiding real work.  Each wall below reported the next one as
soon as the previous was fixed; all three are new, and none is a condvar:

1. **`open(O_DIRECTORY)` on `/NFS/Data` → ENOENT.**  `file_open` routed every
   `O_DIRECTORY` to `fat16_open_dir`, so `Verse.ModLister.RebuildModList`
   threw `DirectoryNotFoundException`.  Added `FILE_TYPE_NFSDIR`, a
   getdents64 walk over the existing `nfs_list_dir` (READDIRPLUS) cache, and
   `nfs_dir_entries()` so end-of-directory answers 0 instead of `nfs_list_dir`'s
   -1 (which the .NET enumerator saw as EIO).  The `[LNFSD]`/`[LGD]` rows name
   every open and every enumeration.
2. **`lseek` on a directory fd → EINVAL.**  .NET's enumerator probes the
   directory with `lseek`; Linux permits it (`SEEK_END` is 0).  `file_seek`
   now moves the getdents cursor for FAT16DIR/PROCFD/NFSDIR.
3. **`MemoryMappedFile` over `/NFS` → "Could not map file".**  RimWorld wraps
   every `Data/**/*.xml` in a `MemoryMappedFile`; mmap of an NFS fd was
   `-ENOTSUP` (M5b).  Added `VM_OBJ_NFS` (`nfs_obj_get`/`nfs_obj_page`):
   one object per (mount, NFSv3 file handle), pages filled on first touch
   with `nfs_read_file` outside every lock, sharing frames like
   `VM_OBJ_FILE`.  `VM_OBJ_MAX` went 64 → 2048: the XML loader holds one
   mapped view per `LoadableXmlAsset`, and the tree has 1,209 XML files; at
   64 the pool died after 8 files and every later mmap returned `ENOMEM`.
   The NFS share slots evict cache-only objects when full, and the source
   identity lives on the object so a live mapping keeps filling afterwards.

### 41.5 Ladder, 3 runs

`RW_SKIP_BUILD=1 RW_SKIP_EXPORT=1 RW_CONDTRACE=1 RW_SHOT_EVERY=10 bash
tools/rw/run-rimworld-desktop.sh --run-timeout 150`, three consecutive runs
on this tree:

| run | NFS bytes | `[XSW]` max | map exceptions | end |
|---|---|---|---|---|
| r1 | 43,754,953 (0x2AA1B1A) | 70 | 1 | virtio-blk `STILL SPINNING` after ~150 s |
| r2 | 43,753,790 (0x2A6E674) | 80 | 2 | deadline kill; last rows a `[FUTEX] map` dump, no panic |
| r3 | 43,761,745 (0x2A6981A) | 80 | 0 | virtio-blk `STILL SPINNING` after ~150 s |

All three are **20.1 MB past §40's 23.65 MB**, and all render at 1920x1080
(`[XSW] n` 70-80, the `changed=` flag toggling 0/1).  A longer run on the
same build (`--run-timeout 220`) reached `[XSW] n=160` and a full-bleed
1920x1080 scene: 100% non-black, **132,577 colours**, 95% different from the
old loading art, with only a small per-frame delta — a static menu-style
scene, not the old loading art.  The residual failures are **not** the
condvar: two runs wedge the virtio-blk queue (`submit_io_cmd: STILL
SPINNING`) after the def load, and every run also logs 9-12 `Could not open
file` failures, consistent with fd exhaustion (`MAX_OPEN_FDS` 64) and not
investigated further here.  Both are named, not fixed, in this lane.

### 41.6 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **414 run, 0 failed**.  Gate
ladder `redzone`, `m2_mono`, `m2_tp`, `m2_threads`, `m12`,
`m3_window_probe`, `m4_gl_probe` re-run on this tree (raw rows in
`obj/l54-gates.log`).

## 42. 2026-11-06 — M6/L55: the "virtio-blk" wedge is an NVMe ring with two consumers; the fd ceiling; and the NFS RPC buffers were shared

Two residual walls were named in §41.5.  Both are fixed, and each fix exposed
the next wall rather than a new theory.

### 42.1 There is no virtio-blk driver on x86 — it is the NVMe path

`src/kernel/virtio_blk.c` compiles two different drivers.  The `#else` half is
the virtio-MMIO one (AArch64); the `#ifdef __x86_64__` half finds a PCI device
of class 01/08 and drives **NVMe**: admin SQ/CQ with opcodes 0x05/0x01 to
create IO queue 1, `submit_io_cmd` filling a 2-entry SQ and polling
`io_cq[io_cq_head].cid == cid`.  Every `STILL SPINNING!` row came from that
function (`virtio_blk.c:177` at the merged tip), not from a virtqueue.

### 42.2 Why a completion never arrived: the ring had two consumers

The pre-fix request path held `blk_request_lock` **across the IRQ-off device
wait**:

```
virtio_blk_read_raw:  flags = spinlock_acquire_irqsave(&blk_request_lock);
                      ... submit_io_cmd(&cmd) ... 100 x 1e6-spin windows ...
                      spinlock_release_irqrestore(&blk_request_lock, flags);
```

The waiter runs with IF=0, so it takes no timer interrupt; its
`cpu_heartbeat_ms[]` therefore stops advancing.  `spinlock_acquire()`'s own
stall breaker (`arch/x64/locks.c`, `lock_stallbreak_ok`) frees any lock whose
owner has not taken an interrupt for `LOSTWAKE_DEAD_OWNER_MS` (2000 ms).
Measured in the L54 r3 log, one second before the first wedge row:

```text
[STALLBREAK] lock=0x000000000745a2878 owner_cpu=5 stale_ms=2314 pc=0x0700eeee
```

From then on two CPUs were inside the driver at once, sharing `io_sq_tail`,
`io_cq_head`, the cid counter and the (then single) `call`/completion slots.
The rows say exactly that:

| row | meaning |
|---|---|
| `io_cq[0].cid=65535 io_cq[1].cid=54432 io_cq_head=1` | a completion for another thread's cid is stranded in a slot nobody is waiting on |
| `io_cq_head` alternating 0/1 across consecutive `STILL SPINNING` rows | something is still advancing the head — a second consumer |
| `CSTS=0x1` | the controller is ready; the device is not the problem |
| `[STALLBREAK] ... stale_ms=2314` | the lock was force-freed, which is how the second consumer got in |

With a 2-entry ring, one consumer taking the other's CQE (`cid -> 0xFFFF`,
head advanced) makes the victim wait for a cid that can never appear again.
The old `BLKSTALL` safety net then returned an error **without restoring the
ring**, so every later request inherited the desync.

### 42.3 The mechanism now

1. **A single-in-flight token outside any lock.** `blk_token_acquire()` is a
   plain flag under `blk_request_lock` (held for a few instructions); waiters
   `safe_wfi()`.  No lock is held across the device wait, so no stall breaker
   can admit a second entrant.  `blk_inside` counts entrants and prints
   `[BLKRACE]` if it ever exceeds 1.
2. **Phase-tagged completions.** The CQ tracks `io_cq_phase`, the only valid
   "this CQE is new" signal in NVMe; a stale/abandoned CQE is drained and
   logged (`[BLKQ] drain stale cqe`) instead of blocking the ring.
3. **A bounded wait that recovers.** At 2000 ms: one `[BLKQ] stall` row
   (slot, sq_tail, cq_head, phase, both head CQEs' cid/status, `inside`,
   CSTS/CC/INTMS/INTMC), then `nvme_reinit()` — CC.EN=0, wait RDY=0, rebuild
   admin queues, zero BOTH IO rings, phase=1, recreate CQ/SQ — and the same
   command is resubmitted once.  A completion the device really lost becomes a
   completed transfer, and the ring is left consistent either way.
4. **Depth 16, not 2**, so a completion can never find the CQ full behind one
   in-flight command, and the poll refreshes this CPU's heartbeat (it *is*
   alive — it is polling the device) so a peer's reaper cannot age it out.

Recovery is provable without waiting for a natural stall: `RW_BLK_DROP_AT=N`
silently loses submit #N's doorbell.  Measured (N=600, 90 s run):

```text
[BLKTEST] dropping doorbell for submit #600 (timeout-recovery self-test)
[BLKSLOW] pending cid=699 slot=7 ms=251 sq_tail=8 cq_head=7 phase=0
[BLKQ] stall cid=699 slot=7 sq_tail=8 cq_head=7 phase=0 cqe[cq_head].cid=65535
       .status=0x1 next.cid=65535 next.status=0x1 inside=1 n=1
       CSTS=0x1 CC=0x460001 INTMS=0x0 INTMC=0x0
[BLKQ] controller reset #1
```

…and the loader kept running: the launcher spawned and the game reached
`[XSW] n=1` after the reset.  In the shipped build `RW_BLK_DROP_AT` is 0 and
the whole block compiles out.

### 42.4 The fd ceiling: 64 -> 256, and the mask that pinned it

`MAX_OPEN_FDS` was 64 because `struct process.fd_cloexec` was ONE `uint64_t`
(with a `_Static_assert` to keep it honest).  It is now
`fd_cloexec[FD_CLOEXEC_WORDS]` addressed only through
`fd_cloexec_test/set/clear/reset/copy`, `MAX_OPEN_FDS` is **256** (which is
exactly `K_FD_SETSIZE`, so select/poll agree), and `MAX_GLOBAL_FILES` went
128 -> 512 so the wider per-process table is actually reachable.  Native
processes use the same helpers, so nothing is special-cased.

`LOCKDBG_REGISTRY` had to grow with it (`MAX_GLOBAL_FILES + 128`): it was a
hard 256 covering `fs_lock + one lock per global file slot + test locks`, and
at 512 file slots the registry filled up and `test_lock_order_debug`'s own
locks were silently unregistered (0 violations where it wants 1).

Local-fd allocation was also a check-then-set on shared group state
(`open_fds[i] == -1` then assign, no lock).  Four `DirectXmlLoader` threads
opening at once could take the same slot — one file object orphaned, and the
winner's fd number naming the other file.  `fd_install()` now does scan+install
under one leaf lock.

### 42.5 What actually produced `Could not open file`: shared NFS RPC buffers

The fd work did not change the exception count, and a new row made that plain:
`[LOPENF]` printed **nothing** for `/NFS` and all 107 failing paths existed on
the host export.  The failures were `nfs_lookup` returning -1, and `nfs.c` had:

```c
static uint8_t call_buf[2048];      /* request bytes  */
static uint8_t reply_raw[2048];     /* reply datagram */
```

Both file-scope, shared by every caller — while the XML loader issues LOOKUPs
from four threads and page-in faults issue READs from more.  One thread's
request was overwritten between `rpc_build_call` and `net_socket_send`; one
thread's reply was overwritten before another parsed it.  The loser sees a
foreign xid, `rpc_xfer` skips it as stale, and the RPC times out:
`[NFS] rpc: no reply from server` **x108** in one run, i.e. one per failing
path.  `nfs_lookup` can only report that as ENOENT.

Every RPC now builds and receives on its own kernel stack and the reply BODY is
copied into a caller-owned buffer (`rpc_call(..., body_buf, body_cap, &body,
&blen)`), so its lifetime is the caller's.  `nfs_read_serial`,
`nfs_rd_issue` and `nfs_read_pipelined` got the same treatment.

Result: **0 `Could not open file`, 0 `FileNotFoundException`, 0 `no reply from
server`** in three consecutive 220 s runs (was 12 and 108).

### 42.6 New wall: concurrent mmap base selection

8 XMLs per run still fail, now as `System.IO.IOException: Could not map file`
from `MemoryMapImpl.Map`, `[LCOMPAT] mmap fail ... -> -12`.  It is not the fd
table (`nfd` is 4-20), not `nfs_obj_get` (its new `[NFSMAP] refuse` row never
fires) and not the region-table capacity (raising `VM_REGION_MAX_CAP`
1024->2048 changed nothing; growing by a 1-frame step instead of doubling
changed nothing).  The failing set is **disjoint run to run** (0-1 common paths
across four runs), i.e. a race: two threads' first-fit pick the same base,
`region_overlaps()` (`vm.c:520`) rejects the second insert, and `vm_mmap`
answers -ENOMEM (`vm.c:2266`).  It blocks 8 of 1,209 defs.  Fix direction:
make base selection + `vm_region_insert` one `vm_lock` section (or retry at a
fresh base), not a bigger table.

### 42.7 Ladder, 220 s runs, final build

| run | NFS bytes | `[XSW]` max | `Could not open file` | `Could not map file` | blk stalls | `no reply` |
|---|---|---|---|---|---|---|
| r3 | 45.04 MiB | 150 | 0 | 20 | 0 | 0 |
| r4 | 44.93 MiB | 150 | 0 | 18 | 0 | 0 |
| r5 | 45.21 MiB | 150 | 0 | 24 | 0 | 0 |
| r6 | 44.46 MiB | 140 | 0 | 8 | 0 | 0 |

All four are **past §41's 43.76 MB**, present continuously at 1920x1080
(`nz=1950488/1950488`, `box=0,0,1915,1017`), and none wedge the block layer
(`[BLKQ] stall` = 0, `STILL SPINNING` = 0).  The frame itself is **static**:
two screenshots 10 s apart differ in **41 pixels** (bottom-right desktop
chrome), 132,577 colours, 100 % non-black, and OCR at 2x over three crops
finds no text at all — a full-bleed planet/starfield still, not an interactive
menu.  So the loader finishes its def pass (NFS reads plateau at ~45 MiB after
~90 s, then ~25 RPC/s of preload) and the game sits on its loading art at
~0.7 fps; no menu buttons are on screen yet.

### 42.8 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` -> 414 run, 0 failed; `redzone`,
`m2_mono`, `m2_tp`, `m2_threads`, `m12`, `m3_window_probe`, `m4_gl_probe`,
`tools/rw/run-rwnfs-probe.sh` re-run on this tree.

## 43. 2026-11-06 — M6/L56: the mmap base-select race, proven with rows and closed with one critical section

§42.6 named it correctly: 8 defs per run died as `IOException: Could not map
file` from `MemoryMapImpl.Map`, `[LCOMPAT] mmap fail ... -> -12`, with a
run-to-run-disjoint failing set.  This section proves the race with rows,
closes it structurally, and reports the state it exposes behind it.

### 43.1 First: the seeded launcher was five revisions stale

Every run of the seeded lane reproduced a *different* state than §42's ladder:
NFS reads stopped at 22.5 MiB, `[XSW] n=1` (one 1280x720 opaque frame), zero
`[NMAP]` rows, `Loading.AsyncRe`/`Loading.Preload` threads parked, and the
launcher then returned from `PlayerMain` by itself.  The cause is rule 99's
trap, measured: `obj/rw-desktop/RimWorldLinux.ad9` was dated **Nov 5 01:23**
while `src/user/rwmain.c` was Nov 6 00:08, i.e. the launcher predated L54's
`pthread_cond_*` interposers.  `tools/rw/rebuild-launcher.sh` — the helper
whose whole purpose is to rebuild *only* the launcher — carried the pre-L54
link line and did not compile `tools/rw/mono-probe/condtrace.c` at all, so
"rebuild the launcher" silently produced a launcher without the fix.  Fixed
the helper (one line); the rebuilt AD-9 exports all four wrappers
(`pthread_cond_wait/signal/broadcast/timedwait`, 43,368 -> 95,520 bytes).

After that the same kernel reaches §42's state and beyond: NFS **47 MiB**,
`[XSW] n=160-170`, `[NMAP] ok` rows (NFS XML mappings), and the def pass runs
to its cross-reference phase.  A seeded lane must therefore rebuild the
launcher before believing any loading-state result.

### 43.2 The race, with rows

Instrumented the pre-fix kernel (`[MR]` on every chosen base, `[MR]` FAIL with
the blocking region, `[MRACE]` naming the peer from a 64-entry selection ring)
and drove it with `src/user/linux_probe_mmap_race.c`: 8 threads x 64 rounds of
the same 8 KiB `mmap`, every mapping held.

```text
[MR] s=9  pid=1 len=0x2000 base=0x1080118000 w=1
[MR] s=10 pid=3 len=0x2000 base=0x1080118000 w=1
[MR] FAIL pid=3 len=0x2000 base=0x1080118000 blk=0x10800F7000+0x23000 kind=3
[MRACE] same-base chosen-by pid=1 s=9 base=0x1080118000 len=0x2000 || refused pid=3 base=0x1080118000 len=0x2000
[MRACE] same-base chosen-by pid=3 s=10 base=0x1080118000 len=0x2000 || refused pid=4 base=0x1080118000 len=0x2000
```

`kind=3` is `VMK_ANON`: the winner's record merged into the adjacent anonymous
region, which is what the loser then collided with.  Totals for that boot:
**520 refusals, 546 same-base pairs, 252 spurious ENOMEM out of 512 requests**,
and every surviving base distinct (260/260) — the arena was empty, so no
refusal was genuine exhaustion.  The row proves both halves: the two threads
chose the same base, and the base was taken by the time of the insert.

### 43.3 The fix: selection and reservation in ONE `vm_lock` section

`vm_hole_find()` is a reader and `vm_region_insert()` is the writer; the
pre-fix `vm_mmap` called them as two steps with the backing-object creation in
between (hundreds of instructions).  The invariant now is *a chosen base is in
the region list before the lock drops*:

1. **`vm_region_alloc_hole(as, start, limit, len, prot, kind, flags, obj, off,
   &base)`** — find + insert under one `vm_lock` (`src/kernel/vm.c`).
   `vm_mmap` uses it for the first-fit path and mremap for `MREMAP_MAYMOVE`.
2. **`vm_region_install_fixed(...)`** — remove + insert under one lock for
   `MAP_FIXED`/`MREMAP_FIXED`.  The old pair was two acquisitions: a peer
   could take the range between them, so the remove destroyed a live mapping
   and the caller saw `-ENOMEM`.
3. **`vm_jit_alloc_locked()`** — the JIT arena's placeholder carve used to
   take the lock, drop it, and let `vm_mmap` insert much later; between the two
   the span was absent from the list, so first-fit could claim it (same bug one
   layer down).  Now the carve and the insert share the caller's section; the
   `[JITA]` rows are emitted after the lock is dropped.
4. The backing object (`vm_object_create` / `file_obj_get` / `nfs_obj_get`) is
   created **before** the section: all three take `vm_lock` themselves, and
   none of them needs the base.  Any failure path unrefs it.

### 43.4 Tests

- Kernel unit tier: `test_vm_alloc_hole_atomic` (`src/kernel/vm_test.c`) runs
  the production primitive 32 times: every returned base is already covered by
  a region when the call returns, no two bases are equal, a window too small
  for the request still answers `-ENOMEM` (the fix must not turn exhaustion
  into a silent success), and `vm_region_install_fixed` replaces a live record.
  Unit tier: **415 run, 0 failed**.
- Linux-ABI probe: `src/user/linux_probe_mmap_race.c`, run as
  `t3_linux_probe.sh mmap_race`.  Pre-fix: `mapped=260 enomem=252`, `done
  rc=1`; post-fix: `mapped=512 enomem=0`, `distinct-bases OK (512/512)`,
  `done rc=0`.
- Gate hazard found while doing the A/B: when the tier's *build* fails, the
  t3 gate reads the previous run's console (`T3_LOG` line) and a stale
  `[LP] done rc=0` reads as PASS — it did exactly that once here.  Check the
  gate log's head for the boot marker (`=== t3_linux_probe: ... ===` followed
  by a fresh `T3_LOG=` and a this-run `Tests`/`[LP]` line) before believing a
  PASS, and make the tree link before running the gate.

### 43.5 Ladder, three 220 s runs (fixed kernel, rebuilt launcher)

| run | NFS bytes | `[XSW]` max | `Could not map file` | insert FAIL | `[MRACE]` | SIGSEGV | shot colours |
|---|---|---|---|---|---|---|---|
| fix-1 | 47.27 MiB | 170 | 0 | 0 | 0 | 0 | 132,577 |
| fix-2 | 47.25 MiB | 160 | 0 | 0 | 0 | 0 | 132,577 |
| fix-3 | 47.26 MiB | 160 | 0 | 0 | 0 | 0 | 132,577 |

All three are 1920x1080 full-bleed (`nz=1950488/1950488 box=0,0,1915,1017`),
`changed=1` between rows, 8 `[XE] PUT` rows, `[NMAP] ok` = NFS XML maps, and
**zero** `Could not map file`, `Could not open file`, `no reply from server`,
`[BLKQ] stall`.  Present rate is ~0.75 fps (160-170 marks in 220 s), the same
softpipe cost as §42.  The screenshot is the planet/starfield loading art with
the `Loading defs.` overlay (OCR; 132,577 colours = §42's number exactly), i.e.
**the mmap wall is gone and the def pass now maps all 1,209 XMLs** — the run
simply does not finish the def pass inside 220 s (reads continue to the last
sample instead of plateauing at 45 MiB).

### 43.6 New wall: `LoadedLanguage`'s parallel read dies on `Invalid handle`

A 360 s run goes past the def pass (99 "Could not resolve cross-reference"
warnings, the last phase of def loading) and then dies in language loading:

```text
System.IO.IOException: Invalid handle to path "/NFS/Data/Ideology/Languages/English/Keyed/MainTabs.xml"
  at System.IO.FileStream.ReadData ... at RimWorld.IO.FilesystemFile.ReadAllText
  at Verse.LoadedLanguage+<>c.<LoadData>b__42_1 ... System.Linq.Parallel ...
[KERNEL] pid=4 (/NFS/RimWorldLinux) memory fault VA=0x0000000000000000
         PC=0x0000001085A28522 rw=r in=HOLE region=NONE
```

`LoadedLanguage.LoadData` reads every Keyed XML through PLINQ; the read comes
back as `EBADF` (Mono maps it to "Invalid handle to path"), the exception
propagates, and the main thread then takes a NULL dereference inside a
just-created JIT chunk (`[JITV] ... base=0x1085A27000 len=0x10000` immediately
before).  `MainTabs.xml`/`Menus_Main.xml` are the two paths in the first
observation; whether they are the same two every run is not yet measured.  The
next lane should instrument `read()`/`close()` (pid, tid, fd, global slot, and
who cleared it) rather than the loader — this is an fd-lifetime question, not a
mapping one.

### 43.7 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` -> **415 run, 0 failed** (414 + the
new test); `t3_linux_probe.sh redzone` 18/18 KEPT; `m2_mono`, `m2_tp`,
`m2_threads`, `m12`, `m3_window_probe`, `m4_gl_probe`, `rwnfs_probe` and
`t3_linux_probe.sh mmap_race` all PASS on this tree.


## 44. 2026-11-06 — M6/L57: `Invalid handle to path` is a transport error wearing a descriptor's name

§43.6 asked the next lane to instrument `read()`/`close()` and name the pair
(closer tid, reader tid, fd, path) that produced `EBADF` on a Keyed XML.  The
answer is that there is no such pair: **no fd was ever closed**.  Mono's
message names a descriptor fault for what `file_read()` reports as a failed
NFS READ, and two transport defects of ours — both now fixed — produced it.

### 44.1 The message does not mean EBADF-on-a-closed-fd

Three layers turn an I/O failure into that sentence:

| layer | code | what it does |
|---|---|---|
| `src/kernel/arch/x64/trap.c:sys_read` | `regs[0] = (ret == -1) ? -EBADF : ret` | **every** `file_read()` failure that returns `-1` becomes `EBADF`; only `< -1` errnos pass through |
| `mono/metadata/w32error-unix.c` | `case EBADF: case EIO: return ERROR_INVALID_HANDLE` | Mono maps **EBADF *and* EIO** to the same Win32 code |
| `MonoIO.GetException` (`MonoIO.cs`) | `ERROR_INVALID_HANDLE` -> `"Invalid handle to path \"{0}\""` | the sentence in Player.log |

`file_read()` returns `-1` on its NFS branch for a failed READ RPC
(`nfs_read_file() < 0`) and for a mount that disappeared — neither involves
the fd table.  So the exception's text cannot distinguish "someone closed my
fd" from "the read timed out", and §43.6's EBADF reading was the wrong half of
the fork.

### 44.2 The fd table is not the cause (flight recorder, kept in the tree)

`fs.c` now keeps a bounded in-kernel ring of every `open`/`close`/`dup`/
`dup2`/`close_range` on a `/NFS/` fd — `(seq, kind, fd, global slot, slot
generation, tid, tgid, path tail)` — and prints it only when a `/NFS` read
fails (`[FDR] fail fd=.. why=..`, then that fd's history, then the group's last
events).  It is the instrument §43.6 asked for: `[FDR]` rows carry the reader
tid and every closer tid for the fd, and a slot reused under a live reader is
a generation mismatch.  Cost is a few stores per open/close; rows only on
failure (bounded to four dumps per boot).

Ruled out by rows: `[FDR] close_range` never fires (nothing in the game closes
by range), `[NFSNOSOCK]` never fires, and `[NSOCK] ... inuse=` stays at 2-9 of
16, so the socket pool is not exhausted.  The failing read is named by the
managed stack as `FilesystemFile.ReadAllText` -> `FileStream.ReadData`, i.e. a
`read()` on the fd that same lambda opened; nothing else in the process holds
that number (`[FDR]` prints a slot-reuse generation and would flag it).
Ownership of this wall is therefore **ours**, not the game's: a real Linux
kernel answers the same failed read with `EIO`, and a `close()` the game never
issued cannot be the cause of a `read()` that its own stack frame owns.  The
recorder stays in the tree so a future `Invalid handle` settles the question in
one run: it prints the closer's tid and the slot generation.

### 44.3 Mechanism 1 (measured): the ephemeral-port counter re-issued live ports

`net_socket_create()` allocated `local_port = next_port++` from a **16-bit**
counter starting at 49152 — 16,384 sockets and it wraps onto ports that live
sockets still hold.  UDP demux is **first-match on `local_port`**
(`net.c:handle_udp` `break`s at the first `in_use` socket with that port), so
two live sockets share one receive queue: the reply one is waiting for is
consumed by the other (and skipped as a stale xid), it waits out
`NFS_RPC_TIMEOUT_MS` (500 ms), `nfs_read_serial()` returns `-1`, `file_read()`
returns `-1`, and trap.c calls that `EBADF`.

Reachability, measured by counting the skips a collision-free allocator has to
make (one run, unmodified read path):

```text
[NSOCK] created=16384 port_dups_skipped=0 inuse=2
[NSPORT] skipped=1 for port=49160 created=16392
[NSOCK] created=20480 port_dups_skipped=1 inuse=2
```

That `[NSPORT]` row is the exact allocation at which the old code would have
handed out **port 49160 a second time**; 49160 is one of the first ports of the
boot, i.e. the NFS mount's cached READ socket, which lives for the whole
session.  It lands at ~16.4k sockets in **every** run, and a 300 s load creates
28k-30k sockets (`[NSOCK] created=28672`), so the wrap is steady state, not a
corner case.

Fix: allocate a port **no live socket holds** (scan the 16 PCBs, skip and
count).  `[NSOCK]`/`[NSPORT]` stay in the tree: a future wrap is a skip count
that keeps climbing.

### 44.4 Mechanism 2 (measured): one lost READ reply failed the whole read

`nfs_read_serial()` did **one** `rpc_xfer()` per block with no retry (unlike
`rpc_call()`, which retries `NFS_RPC_TRIES` times), and `nfs_read_pipelined()`
returned `-1` when the **first** block's reply did not arrive.  One lost or
>500 ms-late datagram therefore became `EBADF` for a file whose fd was open
the whole time.  The stalls are not rare: `[NFSPIPE] stall` fires 2-7 times per
run, including in the pre-fix run that threw the exception:

```text
[NFSPIPE] stall inflight=1 issued=23 replies=22 contig=21440 end=31361 us=590266
[NFSPIPE] stall inflight=1 issued=24 replies=23 contig=22176 end=32962 us=587052
```

Each row is a READ whose reply arrived after the 500 ms window or not at all.
Fix: one retry on a **fresh** socket (a READ is idempotent; the xid is reused),
and the pipelined path falls back to the serial path instead of failing when
its first reply goes missing.  `[NFSRD] fail` remains as the row for a
genuinely dead transport; it is 0 in every post-fix run while stalls still
occur, i.e. the retry is doing the work.

### 44.5 Honesty: `EIO`, not `EBADF`, for a transport failure

`file_read()`'s NFS branch now answers `-EIO` when the transport fails and when
the mount is gone.  trap.c passes `< -1` through unchanged, so a future
transport failure reaches Mono as `EIO` — still "Invalid handle to path" per
44.1, but the truthful errno in `[FDR]`/`[NFSRD]` and in any strace-class
trace, instead of a descriptor fault that never happened.

### 44.6 The NULL-deref was downstream of the EBADF (intervention proof)

Pre-fix run (base-1, seeded kernel, no instrumentation): the early language
load threw `AggregateException -> IOException: Invalid handle to path
".../Keyed/Alerts.xml"`, RimWorld logged `Exception loading language data.
Rethrowing.`, then `Exception from asynchronous event` (the second `LoadData`,
from `PlayDataLoader.DoPlayLoad -> Translator.Translate`), and the next line
was

```text
Tried to use an uninitialized DefOf of type ShaderTypeDefOf. DefOfs are
initialized right after all defs all loaded. ...
[SIGDUMP] SIGSEGV code=1 addr=0x0
[SIGDUMP] uctx rip=0x1085a36022 ... rdi=0x0 rsi=0x1 rdx=0x8
```

so the managed load aborted before `DefOf` binding, `ShaderTypeDefOf` stayed
null, and JIT code read `[null+8]`.  With the EBADF fixed, `Exception loading
language data` is 0 in every run, and neither the `ShaderTypeDefOf` warning nor
the VA=0 fault appears anywhere — same binary, same data, one wall removed,
fault gone.  That is the intervention, not an inference from adjacency: the
deref is a **consequence of the aborted load**, not a second bug.

### 44.7 New wall behind it: a *different* SIGSEGV in implied-def generation

With the language data loading, the def pass completes (577 `Could not resolve
cross-reference: No Verse.SoundDef named ...`, one `XML error: Could not find
parent node named "MusicalInstrumentBase"`), and the game dies at a different
site while generating implied defs:

```text
Tried creating build blueprint for thing that has no blueprint class assigned!
[KERNEL] pid=40 memory fault VA=0x338 PC=0x1085AA6C4B rw=r in=HOLE region=NONE
[SIGDUMP] uctx rip=0x1085aa6c4b rcx=0x0 rdi=0x31a68 rdx=0x20000000000
```

`rcx=0` plus a `0x338` field read is a null object whose field is being read;
the `Log.Error` string immediately before it is at `IL_02d0` of
`RimWorld.ThingDefGenerator_Buildings.CreateBuildBlueprintDef` (decompiled from
`Assembly-CSharp.dll`), i.e. a `ThingDef` that is null or missing when the
blueprint/frame defs are generated.  This one is **not** a read or fd failure:
the same runs have 0 `[NFSRD]`, 0 `[FDR]`, 0 `[NFSNOSOCK]`, and

```text
[NFSLS] n=93 dirs=93 complete=1 rel=/Data/Core/Defs
```

— 93 entries, 93 of them directories, matching the host exactly.  The def
*files* are enumerable; what is absent is defs inside the database: the 577
missing SoundDefs are defined in files that exist on the export (e.g.
`Data/Core/Defs/SoundDefs/World_Oneshots_ProjectileImpacts.xml` defines
`BulletImpact_Metal`).  The ladder build's `[NFSCHK]` self-check (hash of the
first read of `World_Oneshots_ProjectileImpacts.xml`, a SoundDefs file) never
fired in any run either — consistent with that subtree not being read, though
one file is not proof.  The next lane should widen that trigger to *any* file
under a `/SoundDefs/` directory (one line in `nfs_selfcheck()`) so one run
answers it directly, and print the SoundDefs directory's `[NFSLS]` row by name
rather than relying on the 40-row cap.  So the next
lane's question is the loader's *file set* (which directories it recurses
into) and the `MusicalInstrumentBase` parent, not the fd layer.

One more, seen once in three ladder runs and unrelated to the above: a Thread
Pool Worker took a **write** fault inside a writable anonymous region
(`VA=0x11B8051020 PC=0x10846E9645 rw=w in=PROT region=0x11B8000000+0x53000
prot=3 kind=3`) and was killed.  `vm_handle_fault` answered `PROT` for a write
to a region whose `prot` includes write — worth a row of its own next time.

### 44.7a Evidence hazard found while reading the wall: Player.log has 512-byte holes

The `XML error: Could not find parent node ...` row in Player.log ended
mid-word and was padded with NUL bytes.  That is not the XML read: the NUL run
is **159 bytes starting at file offset 79360, exactly 512-aligned**, and the
text resumes mid-word after it (`"...for tribal r" <159 NULs> "ituals and
parties..."`).  A FAT16 write left a hole at a sector boundary, so Player.log
itself can lose 1-511 bytes of a line.  Two consequences: (a) read Player.log
with `\x00`-aware tooling (`grep -a`, or `strings`), and (b) a truncated-looking
log line is not evidence about the guest's I/O — check the alignment before
believing it.  The write path is not fixed here; it is recorded as a row of its
own for the next lane (the same hole would corrupt saved games).

### 44.8 Ladder

Three 220 s runs on the final build (`[NMAP]` rows present, launcher rebuilt
from `src/user/rwmain.c` + `condtrace.c`), plus two 300 s runs that go past the
def pass:

| run | s | NFS bytes | `[XSW]` max | `Invalid handle` | language exception | memory fault | `[NFSRD]` fail | `[NSPORT]` skip | shot colours |
|---|---|---|---|---|---|---|---|---|---|
| fix-1 | 220 | 47.26 MiB | 170 | 0 | 0 | 0 | 0 | 1 | 132,577 |
| fix-2 | 220 | 47.26 MiB | 170 | 0 | 0 | 0 | 0 | 1 | 132,577 |
| fix-3 | 220 | 47.23 MiB | 160 | 0 | 0 | 1 (44.7 note, `rw=w`) | 0 | 1 | 132,577 |
| diag-1 | 300 | 47.52 MiB | 210 | 0 | 0 | 2 (0x338) | 0 | 1 | 132,577 |
| diag-2 | 300 | 47.52 MiB | 220 | 0 | 0 | 2 (0x338) | 0 | 1 | 132,577 |

Before the fix, the same harness (base-1, 420 s) produced 13 `Invalid handle`
occurrences (one IOException echoed through the nested AggregateException
stacks), one `Exception loading language data`, and the VA=0 fault, at
47.72 MiB and `[XSW]` 170.  After it: **0 / 0 / no VA=0** in five runs, with the language
phase reached and passed in both 300 s runs.

Two facts the reader should not over-read.  (1) `[XSW]` is 160-170 at 220 s and
210-220 at 300 s, i.e. ~0.73 fps.  The def pass had not reached its
cross-reference phase at 220 s and had completed it by 300 s, so it needs
somewhere in 230-300 s at that rate; a 220 s run therefore cannot show the
language phase, and the 300 s runs are what show the EBADF is gone.  (2) The 132,577-colour screenshot is the planet/starfield
loading art (§42's exact number), **not** the main menu: the run dies at 44.7
before the menu exists, so "reaches the main menu" is not yet met — what is met
is "0 EBADF, 0 Invalid handle, no VA=0 fault, defs and language data load".

### 44.9 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh` -> **415 run, 0 failed** on this tree
(run twice: once on the ladder build, once on the final one).

### 44.10 Gates

| gate | result |
|---|---|
| `t3_linux_probe.sh redzone` | PASS — every slot KEPT, `[LP] done rc=0` |
| `t3_linux_probe.sh mmap_race` | PASS — `mapped=512 enomem=0`, `distinct-bases OK (512/512)` |
| `m2_mono.sh` | PASS — official Mono 6.12 ran a threaded C# program |
| `m2_tp.sh` | PASS — ThreadPool 7x parallel + join, workers PARK and wake |
| `m2_threads.sh` | PASS — 8 pthreads, mutex+condvar+TLS+join |
| `m12_thread_ceiling.sh` | PASS — 150 threads created + joined, TLS, futex, cancel |
| `m3_window_probe.py` | PASS — full XCreateWindow sequence + WM_DELETE_WINDOW |
| `m4_gl_probe.py` | PASS — GL 3.3 core triangle rendered and presented |
| `tools/rw/run-rwnfs-probe.sh` | PASS — 20/20 rows incl. 2915-byte single read, 102-entry listing |

### 44.11 What is left

1. The 0x338 crash in implied-def generation (44.7): find why a def family that
   exists on disk (SoundDefs) and a parent def that exists on disk
   (`MusicalInstrumentBase`) are absent from the database.  The rows to start
   from are `[NFSLS] n= dirs= complete=` (per directory; 93/93 already match
   for `/Data/Core/Defs`), `[NFSCHK]` (digest of the first SoundDefs read) and
   `[NFSRD]`.  This is a def-file-set question, not an fd or read question.
2. The one-off write fault into a writable anon region (44.7 note) — a
   `vm_handle_fault` PROT answer where `prot=3 kind=3` says the write is legal.

## 45. 2026-11-06 — M6/L58: the missing defs were 31-char-truncated dirent names; the Player.log holes were two writers racing one file cursor

§44.7 asked why a def family that exists on the export (`SoundDefs`) and a
parent def that exists on the export (`MusicalInstrumentBase`) are absent from
the database while `[NFSLS]` says every directory of `/Data/Core/Defs` matched
the host.  The blocker is a **name-length cap in the NFS directory entry**, not a read, an
fd or a load-order filter: `NFS_NAME_MAX` was 32, so the dirent64 name field
held 31 characters while 62 XML files in `Data/` have longer names.  The game
enumerated them, stat'ed the truncated name, got `ENOENT`, and dropped the file
— silently, because a file that vanishes during enumeration is normal for
`Directory.GetFiles`.  The missing parent is then exact: `MusicalInstrumentBase`
lives in the one 32-character file in `ThingDefs_Buildings`, and the six
long-named `SoundDefs` files hold 61 defs.  The 577 sound cross-reference rows
are larger than those 61 defs' textual reference count (132) and smaller than
every Core SoundDef's (1162), so the pre-fix loss was not limited to the
truncated set; what is measured is that with the cap raised the database
resolves **everything** (0 rows) in four runs.  The pre-fix `[XO]` trace that
would have named the exact set does not exist.

### 45.1 The instruments (both kept)

| instrument | where | what it answers |
|---|---|---|
| `[W]` walker (`src/user/rwwalk.c`) | `tools/rw/run-guest-walk.sh` | every directory and **file** with its `stat()` size under `/NFS/Data`, over raw `getdents64` (the same kernel path the game's glibc `readdir` uses), then `Core/Defs/SoundDefs` re-listed at 64/128/4096/8192-byte buffers |
| `[XO] <rel>` | `vfs_open_routed()` | every successful XML open, so "the walker saw it" and "the game opened it" are two lists that can be diffed |
| `[GDT] rel= total=` | `file_getdents64()` | the entry count the kernel hands the game for each directory it starts reading |
| `[RDP] rel= cin= got= last= eof= count=` | `dir_cache_fill()` | one row per READDIRPLUS page — a short listing is a short page, a bad continuation cookie, or a cache-full break, named |
| `[NFSLS] n= dirs= complete=` | `nfs.c` | raised from 40 rows to 400 so every directory is in the log |

The walker is a *native* program on purpose: it mounts the export itself and
speaks `SYS_GETDENTS64` (95), so it cannot be accused of using the game's code
path, and its `[W] f` rows diff directly against `find Data -type f`.

### 45.2 The enumeration is correct; the names were not

`[GDT]` from a game run settles the count question in one line each:

```text
[GDT] rel=/Data/Core/Defs total=93            (host: 93)
[GDT] rel=/Data/Core/Defs/SoundDefs total=35  (host: 35)
[GDT] rel=/Data/Core/Defs/ThingDefs_Buildings total=22 (host: 22)
```

so the *counts* were never wrong — the names inside them were.  The pre-fix
`[W] f` rows show the truncation directly:

```text
[W] e /NFS/Data/Place official expansions here. nlen=31 type=8
```

(host: `Place official expansions here.txt`), and the pre-fix diff against
`find` reported **truncated names** for the 62 affected files, including
`World_Oneshots_ProjectileImpacts.xml` (36), `Building_Sustainers_Ambiences.xml`
(33), `Interact_Oneshots_PickupDrop.xml` (32) and
`Buildings_MusicalInstruments.xml` (32).

The last one is the missing parent: `MusicalInstrumentBase` is defined in
`Data/Core/Defs/ThingDefs_Buildings/Buildings_MusicalInstruments.xml`, and it is
the **only** file in that directory whose name exceeds 31 characters.  With the
file invisible, every `<ThingDef ParentName="MusicalInstrumentBase">` in Core,
Royalty and Ideology lost its parent, the building defs that survive have no
blueprint class, and `ThingDefGenerator_Buildings.CreateBuildBlueprintDef`
dereferences the null def at `VA=0x338` — §44.7's SIGSEGV, now gone.

`SoundDefs` had six long-named files (61 `SoundDef` defs) across Core, Biotech
and Ideology.  A textual count of references to those 61 names is 132, not the
577 §44.7 reported, so the pre-fix loss was larger than the truncation set
alone; the pre-fix `[XO]` trace does not exist and cannot be reconstructed.  What
*is* measured is the post-fix state: **1209 of 1209 XMLs opened**, 0 `Could not
resolve cross-reference`, 0 `No Verse.SoundDef`, 0 `MusicalInstrumentBase` in
four 300 s runs (§45.5).

### 45.3 The fix

`src/include/nfs.h`: `NFS_NAME_MAX` 32 → 64 (the dirent64 path is not bound by
the native `struct sys_dirent.name[32]` cap, and the longest shipped XML name is
47 characters).  Every XML now enumerates, stats and opens; the `[W]` walker's
diff against the host is **1379/1379 files and 389/389 directories, 0 missing,
0 truncated**.  Cost: `dir_cache.names[256][64]` is 16 KiB of BSS.

`lookup_one()`'s request buffer is sized from `NFS_NAME_MAX`, so it grew with it
(108 → 140 bytes); it was already large enough for the 47-char names, but the
buffer is now consistent with the widest component `nfs_lookup` accepts.

### 45.4 The FAT16 NUL holes: two writers, one file cursor

§44.7a's 159-byte NUL run at a 512 boundary is real and reproducible: every long
run's `Player.log` has 2-3 holes, always **chunk-sized and non-sector-aligned**
(22, 19, 102, 159 bytes), each replacing the head of a log record.

`fat16_write()` deliberately drops `fat_lock` across the sector read/write (a
blocking device op must not freeze every FAT operation).  That is right for the
filesystem and wrong for the *file*: the cursor is file state, so two threads
writing one file interleave as

```text
A: cur=100 -> patch sector -> (lock drops)
B: cur=100 -> patch sector -> write           <- A's chunk lost
A: write   -> cur=100+len(A)
B:         -> cur=100+len(B)                  <- cursor is now len(B) past the data
```

and the next write starts `len(B)` bytes past the data, leaving a NUL hole of
exactly one chunk.  `Player.log` has exactly that shape: Unity's `-logFile`
turns fd 1 into the log, and the launcher's `[XSW]` swap rows are written to the
same fd from the rendering thread.  The instrument confirms it in every run:

```text
[FATW] CONC path=/RW_HOME/Player.log busy=2 n=22
[FATW] CONC path=/RW_HOME/Player.log busy=2 n=44
```

`n=22` is literally one of the hole sizes measured before the fix.

Fix (all in `fat16_write`):
- a hashed per-path `wr_lock` held across the whole call (order `wr_lock >
  fat_lock`, nowhere reversed) so one file's cursor + patch + device write are
  one critical section;
- the read-modify-write **read** is zero-initialised and checked — a failed read
  can no longer publish uninitialised stack over a live sector;
- the sector **write** is checked, and `written_bytes` is only advanced after it
  lands — a transient failure is now a short write, never "advanced past bytes
  that did not land";
- `[FATW] RMWREAD` / `[FATW] ZERORD` / `[FATW] CONC` name the event instead of
  leaving a hole.  Every exit path — including the `start_cluster == 0`
  allocation failure — drops both locks; a leaked `wr_lock` would wedge that
  hash slot for the rest of the boot.

Result: three 300 s runs with **0 zero runs** in `Player.log` (86 168, 85 600
and 83 773 bytes) where every pre-fix long run had 2-3.

Regression: `test_fat16_partial_sector_no_holes` (`src/kernel/fat16_test.c`)
writes an 8 KiB non-zero pattern in 137-byte chunks — each ending mid-sector —
overwrites two bytes mid-sector, reads it back and fails on any byte mismatch,
any all-zero 512-byte block and any 32-byte zero run.

### 45.5 Ladder: three 300 s runs of one build, plus a fourth after a lock-release fix

| run | NFS bytes | `[XSW]` max | fps | `Player.log` NUL holes | unresolved SoundDefs | `MusicalInstrumentBase` | `no blueprint class` | memory faults | `Invalid handle` | `[FATW] CONC` | `[XO]` rows |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 50,506,396 (48.17 MiB) | 220 | 0.73 | **0** | **0** | 0 | 0 | 1 | 0 | 2 | 1205 |
| 2 | 53,990,935 (51.49 MiB) | 190 | 0.63 | **0** | **0** | 0 | 0 | 1 | 0 | 2 | 1197 |
| 3 | 55,857,141 (53.27 MiB) | 180 | 0.60 | **0** | **0** | 0 | 0 | 0 | 0 | 2 | 1208 |
| 4 | 57,075,004 (54.43 MiB) | 170 | 0.57 | **0** | **0** | 0 | 0 | 0 | 0 | 3 | **1209** |

Run 4 is the same build plus one lock-release fix (the `alloc_cluster() == 0`
path of `fat16_write` used to return without dropping `wr_lock`, which would
wedge that hash slot on a full disk; it never fired in runs 1-3).  It is the
cleanest run: **1209 of 1209** `[XO]` rows — every XML in the tree opened — with
0 holes, 0 faults and 0 unresolved references.

Before the fix, the same harness at 300 s produced 577 `Could not resolve
cross-reference: No Verse.SoundDef named ...`, one `XML error: Could not find
parent node named "MusicalInstrumentBase"`, the `VA=0x338` SIGSEGV and 2-3 NUL
holes per `Player.log`.  After it: **0 / 0 / no fault of that shape / 0 holes**
in three runs, at 48-53 MiB (the def pass now reads the six long-named SoundDefs
files too).

The screen at the 300 s deadline is the loading screen, OCR `Initializing.` in
all three runs — **not** the main menu.  The def pass and its cross-reference
phase are done by then (0 unresolved references proves the database resolved),
so what remains is the phase after it: the thread pool is at 36-37 live workers
with `[TEXIT]`/`[TEXIT]` churn and `[ALIVE-SET] no READY work` dumps, i.e. the
PLINQ/`LoadedLanguage` cost of §39/§104, which 300 s does not cover.  A 220 s
run cannot show the def pass at all (~0.7 fps), so the ladder is 3x300 s.

One memory fault remains, and it is §44.7's one-off, not this lane's:
`pid=32 (Thread Pool Wor) VA=0x11AC0910C0 PC=0x10846E9645 rw=w in=PROT
region=0x11AC000000+0x93000 prot=3 kind=3 -> killed`.  A write to a region whose
`prot` already includes write answered `PROT` by `vm_handle_fault`; it kills one
pool worker and the load continues.  It appeared in 2 of 3 runs and 0 of 1
before, so it is a real intermittent, still unfixed.

### 45.6 The game's file set vs the walker's

`[XO]` (every successful XML open) against `find Data -name '*.xml'`: 1205-1208
of 1209 distinct paths per run, and **the missing set differs run to run and
comes in contiguous directory runs** (`Buildings_Ancient_*` + `Concepts_*` +
`Terrain_Water` + `Weathers` in one run; `BabyPlay.xml` + `Abilities.xml` +
`RangedMechanoid_Medium.xml` + `Script_PawnLend.xml` in another).  A loader
filter cannot move between runs; a dropped serial burst can, and the guest
writes ~2.3 MB of serial per run.  Those rows are welded onto kernel rows
(`[NFSRTT] ...[XO] /Data/...`) in the same log, i.e. the capture is lossy under
load.  The functional answer is the def database: 0 unresolved cross-references
in all three runs means every def file that matters was read.

The walker's own totals, after the fix, are the host's totals exactly:

```text
[W] TOT dirs=389 files=1379 bytes=309174297 bad=0
host:      389 dirs   1379 files   309174297 bytes
```

— every directory, every file, every size, and **0** `stat()` failures (a file
whose name had been truncated shows up as `[W] e ... nlen=31`; there are none
left).  The per-row diff still reports 3 files "missing" in one run, and it is
the *capture*, not the walker: the same directory's own summary row says
`[W] S .../Shuffled n=13 dirs=0 files=13` (host: 13), and the "extra" row is a
path with a kernel row welded into its middle
(`Offworld_Nonspecifi[cN_AFdSuIlOt]. xbmylt`).  QEMU's serial-to-file capture
drops bursts under a ~1400-row walk; the per-directory counters are the
authoritative count, which is why the walker prints them.

### 45.7 Regression

`tools/rw/l58-gates.sh` runs the whole set sequentially (never two boots at
once) on the final build:

| gate | result |
|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **416 run, 0 failed** (415 + `test_fat16_partial_sector_no_holes`) |
| `t3_linux_probe.sh redzone` | PASS |
| `t3_linux_probe.sh mmap_race` | PASS |
| `m2_mono.sh` | PASS |
| `m2_tp.sh` | PASS |
| `m2_threads.sh` | PASS |
| `m12_thread_ceiling.sh` | PASS |
| `m3_window_probe.py` | PASS |
| `m4_gl_probe.py` | PASS |
| `tools/rw/run-rwnfs-probe.sh` | PASS |

### 45.8 A 600 s run: past the defs, past the language, then a managed SIGSEGV

One 600 s run of the same build (after the three 300 s ones) goes much further
and is the next frontier's evidence:

| measure | 300 s ladder | 600 s run |
|---|---|---|
| NFS bytes | 48-53 MiB | **225.35 MiB** (0xE14Axxx, 47,794+ RPCs) |
| unresolved SoundDefs | 0 | 0 |
| `no blueprint class` | 0 | 0 |
| kernel memory-fault rows | 0-1 | **0** |
| mid-file NUL holes | 0 | 0 |
| end | loading screen, `Initializing.` | `waitpid=4 status=11` at ~410 s |

225 MiB means the load got well past the defs and the language data (the extra
~170 MiB is the asset/audio phase), and then the child died at ~410 s with
`status=11` — **SIGSEGV delivered to the process's own handler**, not a kernel
fault: there is no `[KERNEL] ... memory fault` row and no `[SIGDUMP]`, because
Unity's handler (`[RWMAIN] sigaction sig=11 ... KEEP-OURS`) took it and exited.
`Player.log` ends in a long run of
`mono_thread_internal_set_priority: pthread_setschedparam failed` rows (the
thread count was 36-37 in the 300 s runs and still climbing), and the desktop is
back on screen afterwards (the final screenshot is the Apps menu, OCR `Apps`).
The last 272 bytes of `Player.log` are all zero and sit at the file's *end*
(offset 84 480, a sector boundary) — the chunk that was in flight when the
process died, not a mid-file hole; every run before the fix had 2-3 mid-file
holes.

### 45.9 What is left

1. That ~410 s SIGSEGV after the asset phase: it is inside the managed runtime
   (no kernel row, Unity's handler exits 11) and follows a thread-count climb
   in `mono_thread_internal_set_priority`.  Next lane: `MONO_THREADS_PER_CPU`
   A/B (marker `/TPCPU` already exists) and a managed thread dump at 380 s, so
   "the pool is injecting a thread per work item" and "a worker dies in native
   code" are different rows.
2. The main menu is still not reached: the 600 s run died before it.  The
   remaining work after the asset phase is unknown; a 900 s run with the same
   instrumentation is the cheapest next measurement.
3. The `vm_handle_fault` PROT answer for a write into a `prot=3 kind=3` region
   (45.5) — a pool worker dies each time it happens.
4. `O_APPEND` is recorded in `open_flags` (and `F_GETFL` reports it) but
   `fat16_write()` ignores it: a second handle on a file starts at its own
   cursor.  Player.log's two writers share one fd today, so the per-path write
   lock covers it; a caller that opens the same file twice with `O_APPEND`
   would still interleave.  Not fixed here.

## 46. 2026-11-06 — M6/L59: the ~410 s death is #UD inside UnityPlayer's text, and the pool-worker kill was a retry budget every thread shared

§45.8 left one death and one wound: a 600 s run that reached 225 MiB and then
died at ~410 s with `waitpid status=11` and no `[SIGDUMP]`, and a Boehm pool
worker killed in a `prot=3 kind=3` region where a write was legal. This lane
reproduced both, and they are different bugs.

### 46.1 The ~410 s death is a #UD in a DSO text page, not a fault

The 600 s run reproduces: the group leader dies at ~410 s, and the kernel's
own row names the vector before any handler is involved:

```text
[KERNEL] User process fault! Vector: 6 RIP: 0x0000001081ac8c00
[LCOMPAT] fault sig=11 pid=4 tgid=4 ...
```

Vector 6 is `#UD`, not `#PF` — so there is no `[KERNEL] ... memory fault` row to
find, which is exactly why §45.8 saw "no kernel fault row" and no `[SIGDUMP]`.
The next run added the mapping and the JIT bounds in one row:

```text
[KERNEL] fault-pc region=0x0000001081A66000+0x00000000017A4000 prot=5 kind=7 jit=0 live=36 err=0 rsp=0x00000017FFFFA748
```

`kind=7` is `VMK_LIB` (a DSO's text, mapped by the interpreter), `prot=5` is
R|X, the span is 24.7 MiB = UnityPlayer.so's `R E` segment
(`0x733000..0x1ED7000`), and `jit=0` retires the JIT-chunk hypothesis
outright: the PC is not in the arena (the arena claimed
`0x1088FE0000+0x7020000` in the same run).

Two more measurements decide what the CPU was pointed at:

* the registers at the fault (`[KREG]`, printed by the kernel because the user
  frame could not be planted): `rip=0x1081AC8C00` and `r11=0x1081AC8C00` — the
  target arrived in `r11`, i.e. through a computed jump/call, and `rsp` is on
  the main thread's stack (`0x17FFFFA7A8`), not on a guard page;
* the file bytes at that offset. RIP is `0xAC9E0` into the text segment in one
  run and `0x799C00` in another; at `0x799C00` the file holds byte 2 of a
  `call cosf@plt` (an address that cannot fault with `#UD`), and at `0xAC9E0`
  the file holds `53` (`push %rbx`). A #UD at a byte that decodes means the
  page in memory is not the page in the file.

The text page at the faulting address reads back **all zeros** while the file
holds `41 57 41 56 53 48 83 ec 20` (`push %r15; push %r14; push %rbx; sub
$0x20,%rsp`) — measured with a kernel-side page probe, `[UDCHK] off=0x795000
pagecmp=match mem=00.. file=00..` (the probe's own file read was itself
zero-filled by the bug it was measuring, see 46.4). So the mechanism is: a
DSO text page was published with the wrong contents (zero), the CPU executed
into it, and the instruction stream desynchronised.

That is the "real kernel VM bug" branch of the four the lane was asked to
decide between, and the three others are retired with rows:

| hypothesis | row that retires it |
|---|---|
| stack overflow (guard-page fault below a thread stack) | no `#PF` at all: `erc=0`, vector 6; `rsp` is inside the main stack; `[SIGX]`/`[VMK]` absent |
| heap corruption (fault at a plausible-but-wrong address) | the fault is an instruction fetch in `VMK_LIB prot=5`, not a data access |
| JIT code fault (rip in an exec chunk) | `jit=0`, and the region is the DSO's own text span |
| real kernel VM bug | the page content differs from the file: `[UDCHK]` |

### 46.2 The pool-worker kill was a retry budget that every thread shared

§45.5's intermittent kill reproduces in run 1 of this lane, and the new #PF
error-code field shows the page was **not present** when the hardware walked
it:

```text
[KERNEL] pid=33 (Thread Pool Wor) memory fault VA=0x1130059050 PC=0x10800BBFC2
  rw=w in=PROT region=0x1130000000+0x5B000 prot=3 kind=3 live=40 inH=0
[SIGX] pid=33 tgid=4 sig=11 code=1 addr=0x1130059050 alt=1 live=40
```

`prot=3` says the VMA allows the write, so `in=PROT` could only come from the
present-leaf retry branch. `[VMRT]` (new) shows why that branch never made
progress:

```text
[VMRT] va=0x113000D048 leaf=0x80000000482E4067 present=1 prev=-1 prot=3 kind=3 w=1
[VMRT] va=0x113000D078 leaf=0x80000000482E4067 present=1 prev=1  prot=3 kind=3 w=1
```

Three separate things were wrong, and each is a row:

1. `retry_va` was the **page**, so three different stores into one page counted
   as three "retries of the same address". A page that a peer thread had just
   mapped (stale local translation) therefore accumulated a count from
   unrelated instructions and the fifth store killed the worker.
2. The state lived in the **address space**, so every thread of the group
   shared one counter. Five workers each taking one legitimate stale-translation
   fault on the same page killed the fifth.
3. Nothing ever **reset** it: the comment says "consecutive retries", the code
   only ever incremented. Four episodes spread over minutes armed an instant
   kill on the fifth.

The fix is the budget the comment always claimed: per thread (keyed by pid),
reset by every fault that is actually resolved, with a CR3 reload on a repeat
(INVLPG alone does not always reach a parent-table entry installed by another
CPU). Rows: `[VMRT] pid=.. prev=..`, `[VMK] retry-budget`, `[VMFIX] prot-up`,
`[VMFIX] remap`.

The same instrumentation then found a second, independent defect on the same
path: a **present READ-ONLY leaf inside a writable anonymous VMA**
(`leaf=0x800000004922D007 prot=3 kind=3`, RW bit clear). With no object to copy
from, the old code could only retry until it killed the writer — measured as a
pool worker dying in the 1 GiB brk heap at `VA=0x105B7E3060` and freezing the
def pass at 48-63 MiB in two runs. `vm_handle_fault` now upgrades such a leaf in
place (`do_wp_page` semantics), and a present+writable leaf the hardware still
refuses after a retry+CR3 reload is replaced once with a private copy of its
contents instead of killing the thread.

### 46.3 A short page read is not a short page

`file_obj_page()` zero-filled everything past the bytes the device returned, so
a chain break or a transient device error published **a page of zeros as file
content** — and for a DSO text page that page is mapped executable. That is the
mechanism of 46.1. The fill now requires the page's real byte count
(`min(4096, size - off)`), retries twice, and refuses to publish a short page;
`[ZPFILL] short read off=.. n=.. want=.. -> not published` names it.

Executable file pages are also no longer shared with the page cache: a
MAP_PRIVATE text page is copied into an AS-owned frame (`vm_file_obj_materialize
(..., cow=1)` for `exec || prot & EXEC`). The old shared read-only leaf was
correct only while the cache frame was immutable, and this kernel invalidates
the file-object cache on **every FAT16 write** (Player.log is written
continuously) and recycles frames on the last unref. Cost: one private copy per
text page (UnityPlayer's text is 24.7 MiB); correctness of executed code is not
negotiable.

### 46.4 The instruments this lane added (all kept)

| row | where | what it answers |
|---|---|---|
| `[THRS] pid= live= gt= hw= slots=n/191 regs= anon= anon_mib= big_mib= stk= res_mib= pk_mib=` | `rw_thr_sample()` from the PIT tick, every 5 s | the thread curve, its high-water mark, PCB-slot usage, VM region count, anon/stack totals and resident MiB — the leak test (`regs` falling with `live` means stacks are freed; `slots` rising with `live` flat means PCB tombstones) |
| `[SIGX] pid= tid= tgid= sig= code= addr= ipc= rsp= h= alt= live=` | `l_sf_plant()`, per-pid bounded | one row per delivered SIGSEGV/BUS/ILL/ABRT, with the thread count at delivery and whether the frame went on an altstack — the row §45.8 did not have |
| `[SIGPL] FAIL pid= sig= why= arg= rsp= alt_sp= alt_sz= alt_st= live=` | `l_sf_plant()` failure points | *why* a handler could not run (`restorer`, `sp-low`, `sp-tight`, `touch`) — the silent `status=11` path |
| `[LCOMPAT] fault-no-frame` / `fault-in-handler` / `fault-no-handler` | `linux_compat_fault()` | the three ways a fault dies without a dump |
| `[KREG] pid= vec= erc= rip= rsp= rax..r15` | x64 user-fault path, bounded | the register file when no user dump is possible (the jump target's provenance) |
| `[KERNEL] fault-pc region=.. prot=.. kind=.. jit=.. live=.. rsp=` | x64 user-fault path | DSO vs JIT-chunk vs hole, and the thread count, for any non-#PF user exception |
| `[VMRT]` / `[VMK]` / `[VMFIX]` | `vm_handle_fault()` | the present-leaf retry branch: leaf value, repeat count, and which recovery fired |
| `[THRS]` + `[ZFILL]`/`[ZPFILL]` | `file_obj.c` | a page that could not be read whole (never published) |

The launcher's own dump was rebuilt for the same reason: one 2 KiB `write(2)`
can come back **short** on this console (measured: only the last of eight
`[SIGDUMP]` lines reached the serial), so the summary line is written first on
its own, `rb_flush()` loops until every byte is out, and the dump carries all
16 GPRs, `cr2`/`err`/`trapno`, the 16 loaded modules with base/end, the thread
count and a chained call to the handler we refuse to install (Unity's), ours
first. `SA_ONSTACK` + a 32 KiB altstack per thread (armed by the
`pthread_create` interposer, which also counts live threads) is what makes a
stack overflow dumpable at all.

### 46.5 `O_APPEND` is honoured by `fat16_write`

§45.9 item 4: `O_APPEND` was recorded (and `F_GETFL` reported it) but ignored,
so a second handle on a file started at its own cursor. `fat16_write()` now
seeks to `entry.file_size` **inside** the per-path `wr_lock` and under
`fat_lock`, which is what makes append atomic between two appenders; doing it at
`open()` would leave the second writer using an end that was already stale.

Two supporting changes make that safe:
`fat16_open_ex()` zeroes `open_flags` on every handle it returns (a bare
`struct file` on a test's stack used to leak a random `0x400` bit into the new
append test) and `file_open()` re-applies the VFS flag word immediately after
that call. Regression: `test_fat16_append_two_handles` — 24 alternating records
from two `O_APPEND` handles must read back as the exact concatenation, a seek on
an append handle must not move the write position, and the control (same handle,
flag cleared, seeked to 0) must overwrite.

### 46.6 Thread-count characterisation (the answer to §45.9 item 1)

`[THRS]` every 5 s for the whole 600 s run:

* live threads: 23 at 5 s, 41 at 50 s (the parallel def pass), 26-28 through the
  cross-reference phase, a second burst to 40 at 210 s, then a plateau of 25-31
  to the end. **High-water 43.** The climb §45.8 saw at 300 s was this plateau
  being approached, not an unbounded leak.
* `gt=` (a direct count of the group's non-tombstone PCB slots) equals `live=`
  in every sample: the group counter is right.
* `slots=` rises 25 → 89 of 191 over 600 s while `live` stays at 25: the excess
  is `PROC_STATE_THREAD_DONE` tombstones, which are reclaimed lazily. At this
  rate a 600 s load uses 47% of `MAX_PROCESSES`; a much longer load would need
  the tombstone reaper to keep up.
* `regs=` (VM regions) tracks the thread count down as well as up (356 at the
  peak, 327 at 600 s) and `anon_mib` follows it: **thread stacks are freed**,
  there is no stack leak. `stk=` stays 3 (the main stack and two kernel-made
  ones) — thread stacks are ordinary anonymous mmaps, so `regs` is the
  stack-count proxy.
* `res_mib` reaches ~1.1 GiB at 600 s with `big_mib=255` (the Boehm heap's
  largest anon region), i.e. the 264 MiB of NFS reads is not the memory story;
  the GC heap and the JIT arena are.

So the answer to "which subsystem creates the threads" is: not Mono's thread
pool alone and not Unity's job system alone — the count is bounded at 43 and is
the def pass's PLINQ + `Loading.AsyncRead` + the Boehm GC's own workers. Nothing
in this lane's rows supports "a thread per work item".

### 46.7 Ladder: three 600 s runs

The ladder is `tools/rw/run-rimworld-desktop.sh --run-timeout 600` on one build,
plus `tools/rw/l59-summary.py` per run. One attempt (labelled run 5) never
booted — the previous loop still held a write lock on `disk.img`
(`qemu: Failed to get "write" lock`) — so the three runs below are the three
that actually ran the game.

| run | NFS bytes | `[XSW]` last | fps | `[THRS]` live/hw | regions | mid-file NUL runs | end |
|---|---|---|---|---|---|---|---|
| A | 264.73 MiB | n=180 | 0.42 | 25 / 43 | 333 | 0 | #UD at ~430 s, `RIP=0x1081a8a570` |
| B | 264.74 MiB | n=180 | 0.42 | 25 / 43 | 325 | 0 | #UD at ~430 s, same RIP |
| C | 264.73 MiB | n=180 | 0.42 | 25 / 43 | 332 | 0 | #UD at ~430 s, same RIP |

Every run: 0 `[SIGX]`, 0 `[VMK]`, 0 kernel `#PF` rows, 24 `[VMRT]` rows with
`prev` never above 1, `Player.log` 84 902-85 280 bytes with **0 NUL runs**.
The two kills of §45.5 and §46.2 are gone from the ladder: the pool worker is
never killed, and the def pass no longer freezes at 48-63 MiB.

The screen at the 600 s deadline is the desktop in all three runs: the app is
gone by then (`99_final` OCRs as the taskbar clock, exactly the shape §45.8
described), so **the main menu is still not reached** — what the ladder proves
is that the 264 MiB / 180-frame load is now clean up to the deterministic #UD.

The ~410 s death is not gone, and it is deterministic: all three runs fault with
`Vector: 6 RIP: 0x1081a8a570`, `region=0x1081A66000+0x17A4000 prot=5 kind=7
jit=0`, `rsp=0x17FFFFA728`, `rax=0 rcx=0xFFFFFFFF rdx=0x1080212340
rdi=r8=0x1040068C10 r10=4 r11=0x1081a8a570 r12=1 r14=0 r15=0x10400C54A0`, and
only `rbx`/`r13` differ between the runs. `0x977570` (file offset, i.e.
`0x733000 + 0x24570`) is a cold block that ends in a `jmp` to an assert with
`$0x403` in `%ecx` — a valid instruction stream, entered through `%r11`.

So the remaining fault is *not* explained by the page-contents theory of 46.3
either: the faulting instruction decodes. What the rows now prove is where the
PC came from (an indirect branch through `%r11`) and that the mapping is
`VMK_LIB R|X` with `jit=0`; what they do not yet prove is why the CPU raised
`#UD` at a decodable instruction. Two candidates remain, and §46.9 says how to
separate them in one run:

1. the reported PC is not the executed instruction (a trap-frame reporting
   question for non-#PF vectors — `#PF` rows have always been sane, and every
   user `#PF` in this lane named a plausible VA/PC), or
2. the executed bytes differ from the file's (the `[UDCHK]` probe that was
   meant to answer this read the file through a static buffer, which is not the
   identity-mapped physical address `fat16_read_direct_pos` requires, so its
   `pagecmp=match mem=00..` row is the probe failing, not the page).

### 46.8 Regression

`tools/rw/l58-gates.sh` (sequential, never two boots at once) on the final
build:

| gate | result |
|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **417 run, 0 failed** (416 + `test_fat16_append_two_handles`) |
| `t3_linux_probe.sh redzone` | PASS |
| `t3_linux_probe.sh mmap_race` | PASS |
| `m2_mono.sh` | PASS |
| `m2_tp.sh` | PASS |
| `m2_threads.sh` | PASS |
| `m12_thread_ceiling.sh` | PASS |
| `m3_window_probe.py` | PASS |
| `m4_gl_probe.py` | PASS |
| `tools/rw/run-rwnfs-probe.sh` | PASS |

### 46.9 What is left

1. **The deterministic `#UD` at UnityPlayer+0x977570.** One run answers it:
   fix `[UDCHK]` to read the file page into a `frame_alloc()` buffer (physical,
   as `file_obj_page()` does) and print the 16 bytes at the PC from both
   sides, *and* print `tf->elr` next to the frame's `cs`/`rflags`. A match
   retires the page-contents theory and makes the trap frame the suspect; a
   difference names the fill. The register row (`[KREG]`) is already there and
   is what makes either answer usable.
2. `slots` reaches 89/191 at 600 s with only 25 live threads: `PROC_STATE_THREAD_DONE`
   tombstones are reclaimed lazily, and a 900 s load would meet
   `MAX_PROCESSES` before it met the menu. The reaper (not this lane's file set)
   is the next stop if the run gets longer.
3. `mono_thread_internal_set_priority: pthread_setschedparam failed, error:
   "Operation not permitted"` still ends `Player.log`: our `sched_setscheduler`
   refuses a policy the runtime asks for. It is not fatal, but it is the last
   row before the fault in every run and deserves its own ladder.

## 47. 2026-11-06 — M6/L60: 0x1081a8a570 is `push %rax` in a **relocated thunk table**, the page under it was never the file's, and the wall behind it is a Mono JIT assert

§46 left two candidates for the deterministic `#UD`: "the reported PC is not
the executed instruction" and "the executed bytes differ from the file's", with
a one-run experiment to separate them.  This lane ran that experiment, and the
first thing it found is that **§46's fault offset was off by 0x220000**: the
address was decoded at `0x977570` instead of `0x757570`, so the "cold block that
ends in a `jmp` to an assert" the lane described belongs to a different
function.  The instruction at the real target is `push %rax`; there is no
invalid opcode anywhere near it.  The page under it, however, was **not the
file's page**, and the row that proves it also names the moment it happened:
the fill of that page, immediately after the block layer wedged and reset.

### 47.1 The address, and what is actually there

The launcher prints the load bias itself, so nothing has to be inferred:

```text
[RWMAIN] dlopen ok /RWDSO/UnityPlayer.so -> 0x10400008d0 base=0x1081333000 ...
```

`0x1081a8a570 - 0x1081333000 = 0x757570`.  UnityPlayer's first `PT_LOAD` has
`p_offset = p_vaddr = 0` and the text segment is `off/vaddr 0x733000`, so vaddr
== file offset for every page and the target is file offset **0x757570**.
§46.7/§46.9 wrote "`0x977570` (file offset, i.e. `0x733000 + 0x24570`)" — but
`0x733000 + 0x24570 = 0x757570`, and 0x977570 is 2.2 MiB further into `.text`.
The other ladder's RIP decodes the same way: `0x1081AC8C00` is file offset
`0x795C00`, which holds `41 57 41 56 53 48 83 ec 20`
(`push %r15; push %r14; push %rbx; sub $0x20,%rsp`) — exactly the prologue
§46.1's probe printed as its "file" side.

At 0x757570 the file holds:

```text
00757570: 50 e8 8a 14 ee 00 48 89 c7 58 e9 e1 18 ee 00 cc
          push %rax
          call 0x1638a00
          mov  %rdi,%rax
          pop  %rax
          jmp  0x1638e60
          int3
```

a 16-byte **adapter thunk**; the sibling at 0x757560 is byte-identical in shape,
and both are 16-byte aligned entries.  What they call is a getter and a
handler:

```text
1638a00: 50                push %rax
1638a01: 31 ff             xor  %edi,%edi
1638a03: e8 98 64 33 ff    call 0x96eea0
1638a08: 59                pop  %rcx
1638a09: c3                ret
96eea0:  48 63 c7          movslq %edi,%rax
96eea3:  48 8d 0d 06 c1 62 01  lea %rcx,[rip+0x162c106]   # 0x1f9afb0
96eeaa:  48 8b 04 c1       mov  (%rcx,%rax,8),%rax
96eeae:  c3                ret
```

i.e. "fetch slot 0 of the pointer table at 0x1f9afb0, pass it as the first
argument, tail-call the handler at 0x1638e60".

There is **no symbol** for it: the DSO is stripped to a single defined dynamic
symbol (`_Z10PlayerMainiPPc` at 0xe638b0).  The identity is structural instead:
`0x757570` is one of ~250 slots of a function-pointer table in `.data.rel.ro`
(`0x1edff00 .. 0x1edffd8`), every slot filled at load time by an
`R_X86_64_RELATIVE` relocation, and the slot that holds this thunk is
`0x1edffc8` (`addend 0x757570`).  A scan of all 24.7 MiB of `.text` finds
**zero** rip-relative references to that table, because its address only ever
lives in data — which is why the target arrives in a register.

So the answer to "ud2? a pure-virtual stub? an IFUNC resolver tail? padding?"
is: **none of those.  It is a legitimate function entry reached through a
relocated function pointer**, and its first byte is `0x50`.

### 47.2 Why `r11 == rip`

`r11 == rip` is the normal signature of `call *%r11`: the CPU pushed the return
address and set RIP to the register's value, so the fault frame carries the
target in both.  The register file also says where the *caller* was: the qword
at the faulting `rsp` is in the JIT arena (`0x1089E9DA88`, `0x1089E9DF58` in
two runs; the arena claims `0x1088FE0000+0x7020000`), not in UnityPlayer.  The
call is emitted by **Mono-generated (JIT) code** that loaded the native entry
point out of UnityPlayer's table — consistent with 47.1's "no code reference".

`r11` held `base + 0x757570`, exactly what the table slot contains after
relocation.  **The pointer was right; the bytes at the pointer were not.**

### 47.3 IRELATIVE / IFUNC: audited, and not this

The hypothesis "our loader never ran the resolver" is retired with a table
(host oracle, `ifunc_audit.py`: `.dynsym` + `.rela.dyn` + `.rela.plt` parsed
directly):

| DSO | `STT_GNU_IFUNC` | `R_X86_64_IRELATIVE` | IFUNC `JUMP_SLOT`/`GLOB_DAT` |
|---|---|---|---|
| UnityPlayer.so | 0 | 0 | 0 |
| libmonobdwgc-2.0.so | 0 | 0 | 0 |
| libmono-native.so | 0 | 0 | 0 |
| lib_burst_generated.so | 0 | 0 | 0 |
| libpthread-2.31.so | 0 | 0 | 0 |
| ld-2.31.so | 0 | 0 | 0 |
| libc-2.31.so | **58** | 0 | 0 |

There is no resolver in the game's own DSOs to run.  The one library that has
them (glibc) is relocated by **glibc 2.31's own `ld.so`** — the AD-9 launcher
names it as `PT_INTERP` — not by this kernel: `elf_apply_relocations()` accepts
only `R_*_RELATIVE` (8 / 1027) and returns `ELF_ERR_RELOC` for anything else,
and it never runs on the game's DSOs (they are `dlopen`ed by the launcher, so
`ld.so` maps and relocates them in userspace).  Runtime confirmation, printed by
the launcher after `dlopen`:

```text
[IFUNC] memcpy=0x... [IFUNC] strlen=0x... [IFUNC] memcmp=0x... [IFUNC] memmove=0x...
```

and the process executes memcpy/strlen for 180 frames, which an unresolved
resolver could not survive.

### 47.4 The page was not the file's — and the fill is when it happened

Three probes, each of which had to be fixed before it could be believed:

* the staged DSO is **byte-identical to the host export**
  (`md5 45408fc19d21120d73e89d7bf81fbadd` both sides, 33 010 672 bytes), so the
  file on `disk.img` is not the variable;
* the host file has **no all-zero 4 KiB page anywhere in `.text`** (0 of 6 296
  pages), and page 0x757000 begins `50 f3 0f 11 44 24 04 e8 04 92 ec 00`;
* the fault-time probe reads the page **through the faulting page tables**
  (`vm_arch_walk` on the faulting VA), asks the frame allocator whether the
  frame is still owned, snapshots the executed bytes *before* allocating
  anything (a probe that allocates first can be handed the very frame it is
  inspecting), and reads the file into a **real frame** (a `.bss` array is not
  a physical address, which is what made §46.9's `pagecmp=match` meaningless).

Rows (identical in shape in all three ladder runs):

```text
[WATCH] armed off=0x757000 phys=0x81262000
[KERNEL] User process fault! Vector: 6 RIP: 0x1081a8a570
[UDX] rip=0x1081A8A570 off=0x757000 rd=4096 leaf=0x81262025 pa=0x81262000
      alloc=1 objfr=0x81002000 same=1 wf=0
[UDX] pghd=00 00 ... (12)  ripb=00 00 ... (12)  file=00 00 ... (12)
```

* `leaf=0x81262025` — present, user, **RW clear, NX clear** (executable), so the
  mapping is a correct read-only text leaf;
* `pa=0x81262000`, `alloc=1` — the frame is still owned by the allocator;
* `wf=0` — the one-frame tripwire (`frame_watch`, armed on this page at map
  time) counted **zero** `frame_free()` calls for it, so this is **not** a
  use-after-free, not a recycled frame, and not a stale leaf;
* `same=1` — the page and a fresh read of the same file offset agree... on
  zeros.  The file holds code there.  So the *read* is what is wrong, and it is
  wrong the same way twice.

And the row above the fault names the moment:

```text
[BLKSLOW] pending cid=42982 slot=1 ms=251 ...
[BLKQ] stall cid=42982 ... CSTS=0x0000000000000000 ...
[BLKQ] controller reset #1
[WATCH] armed off=0x757000 phys=0x81262000        <- this fill
[KERNEL] User process fault! Vector: 6 RIP: 0x1081a8a570
```

The page is materialised **for the first time** at that instant (that is what
`[WATCH] armed` means), with the controller reporting `CSTS=0` (RDY clear, i.e.
the device had already fallen over) and the driver rebuilding its queues.  The
page-in published a page the device never delivered, and the CPU executed it.

That is the mechanism §46.3 only half-closed: it stopped *short* reads from
being zero-filled, but a read that returns a **full 4 KiB of the wrong bytes**
(or of zeros) is indistinguishable from a good one at the FAT16 layer, and for
an executable page it is fatal.

### 47.5 The fix

1. **An executable page is verified before it is published**
   (`vm_file_obj_materialize`).  For `exec` pages of a `VM_OBJ_FILE`: read the
   page twice, publish only when two independent reads agree (and, for a DSO,
   when the page is not all zero — a shared object's executable page never is:
   0 of 6 296 in UnityPlayer), buy a third read on disagreement (majority wins,
   so one bad read is re-read instead of executed), back off 3×10 ms and retry
   when every answer was zero, and otherwise refuse with `-EIO` —
   `[TXT] ZERO` / `[TXT] UNVERIFIED` name it.  Cost: one extra 4 KiB read per
   executable page (24.7 MiB of a 1.3 GiB load).
   The "not all zero" half is deliberately **DSO-only**: a native image
   legitimately contains zero pages (`DESKTOP.BIN` has 107), so that path tests
   agreement and nothing else.  Getting that wrong made the desktop unbootable
   in one gate run — which is what the gate is for.
2. **The image path gets the same rule** (`v2_image_materialize`), and it
   needed it more: it accepted *any* short read and zero-filled the tail.  The
   launcher's own `.text` is demand-paged through it, which is why the ladder
   produced `#UD RIP=0x1000010970 region=0x1000000000+0x3C000 prot=7 kind=1`
   (RWX image text) in 2 of 3 runs: the same class of bug in the other loader.
   A page **entirely past** the image's EOF is not a short read — it is `.bss`
   and the reserved tail of the IMG span, legitimately zeros, and must still be
   published: getting that wrong made every native program with `.bss` beyond
   its last file page die at its first bss touch (`m3_window` caught it in one
   gate run, which is exactly what the gate is for).
3. **`MAP_FIXED` replaces instead of inheriting** (`vm_mmap`): the old code
   removed the region records and left the resident leaves, so the new mapping
   silently served the previous occupant's frames (and, when the old object's
   record was the last reference, frames that had already been freed).  It now
   zaps the replaced span first, exactly like `vm_munmap_range`, and
   `[MMF] replace base=.. len=.. resident=N -> zapped` records it (N = 0 for
   every DSO segment map measured, which is the correct answer).
4. **The launcher waits longer than the harness.**  `RW_WAIT_SECS` was 420
   while the ladders run at 600, so the launcher exited mid-run, the child was
   reparented and the game window vanished from the screen just when a 600 s
   run needed it.  900 now.

### 47.6 Ladder: three 600 s runs

| run | `[XSW]` frames | NFS bytes | end | `[TXT]` |
|---|---|---|---|---|
| 1 | 180 | 271,405,137 (258.8 MiB) | `#PF VA=0xFFFFFFFFFFFFFFFF PC=0x1088A76564` (JIT arena) at ~405 s | 3 584 pages, 0 bad |
| 2 | (not captured) | 277,598,968 (264.7 MiB) | `fault-in-handler sig=11 pc=0x108210E804` at ~410 s, then the launcher's own image `#UD 0x1000010970` | 3 584 pages, 0 bad |
| 3 | 180 | (not captured) | same `fault-in-handler` + same image `#UD`, `Player.log` ends in the Mono assert | 3 584 pages, 0 bad |

**The deterministic `#UD` at `UnityPlayer+0x757570` is gone: 0 occurrences in
3/3 runs** (it was 3/3 before).  `[TXT] bad=0` on 3 584 materialised executable
pages per run, and **no `[TXT] ZERO` row appeared**, so the gate itself never
fired: the disappearance is *not* attributable to it.  What the ladder does
establish is that the fault site moved — the three runs now end in the Mono
assert below (and in the launcher's image-page `#UD` in 2 of 3, which the image
gate in 47.5 addresses) — so the next lane inherits a named runtime abort
instead of a mystery `#UD`.  Frames and I/O are unchanged (180 frames,
0.42-0.44 fps, 259-265 MiB), so the verification cost is inside the noise.

### 47.7 The wall behind it: Mono's JIT asserts at `x86-codegen.h:403`

With the #UD gone, `Player.log` ends with a line that §46 could never see
because the process died before flushing it:

```text
* Assertion at ../../mono/arch/amd64/../x86/x86-codegen.h:403, condition `0' not met
```

in 2 of 3 runs (the third dies in the same phase, at a `-1` pointer read in JIT
code).  That is Mono's **code generator** calling `g_assert_not_reached()` —
the `x86-codegen.h` size-dispatch family, i.e. the JIT was asked to emit a
memory operand of a size it does not implement.  It is not a kernel fault: it
is a managed-runtime abort, it happens at the same point in the load
(~405-420 s, 180 frames, ~260 MiB), and it explains the `fault-in-handler`
rows: the abort's signal is delivered, and the delivery itself faults.

So the next lane's question is not VM at all: *which method makes Mono's amd64
JIT ask for that operand size?*  The cheap first measurement is the managed
stack at the assert (SIGQUIT to Mono, `/SIGQUITON` already exists), plus
`MONO_LOG_LEVEL=debug` around the last assembly loaded.  §46.9 item 3's
`pthread_setschedparam` rows are still the last thing written before it.

### 47.8 Regression

`tools/rw/l60-gates.sh` / `l60-gates2.sh`, sequential (never two boots at
once).  The first four were run on the build that carries the exec page-in gate
and the image-path gate; the last four (unit, `m3`, `m4`, `rwnfs` — the ones
that exercise the image path this lane changed) were re-run on the final build
after the `.bss`/past-EOF correction:

| gate | result |
|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **417 run, 0 failed** |
| `t3_linux_probe.sh redzone` | PASS |
| `t3_linux_probe.sh mmap_race` | PASS |
| `m2_mono.sh` | PASS |
| `m2_tp.sh` | PASS |
| `m2_threads.sh` | PASS |
| `m12_thread_ceiling.sh` | PASS |
| `m3_window_probe.py` | PASS (final build) |
| `m4_gl_probe.py` | PASS (final build) |
| `tools/rw/run-rwnfs-probe.sh` | PASS (final build) |

### 47.9 What is left

1. **Mono's JIT assert** (`x86-codegen.h:403`) is now the wall that hides the
   main menu, and it is a runtime question, not a VM one: the code generator is
   asked to emit an operand size it does not implement.  The cheap first row is
   the managed stack at the abort (`/SIGQUITON` already sends SIGQUIT to Mono at
   60 s; it needs to fire at ~400 s instead, or on the assert itself), plus
   `MONO_LOG_LEVEL=debug` around the last assembly loaded.
2. **A read that "succeeds" without delivering its bytes** is still possible:
   the verification above catches a read that disagrees with another read, and
   refuses all-zero DSO code pages, but a *consistently* wrong read is
   indistinguishable at this layer.  The row that names it is `[BLKQ] stall ...
   CSTS=0` + `controller reset #1` immediately before a page-in; the fix
   belongs in the block layer (never complete a request whose DMA did not land).
3. **The launcher's own image text is RWX** (`prot=7 kind=1` in the 47.6
   `#UD` row).  Native images are mapped writable+executable; the W^X work that
   DSOs got in §19/§20 has not been done for `VMK_IMG`.
4. `slots` still climbs and `pthread_setschedparam` still fails (§46.9 items 2
   and 3); neither changed here.

## 48. 2026-11-06 — M6/L61: the wall is **not** the code generator — `:403` is `mono_x86_patch_inline`'s `default:`, the `VA=-1` is Boehm's free list, and the interpreter control dies before the menu

§47.7 named `x86-codegen.h:403` as "the code generator asked for an operand size
it does not implement".  This lane decoded the assert against the exact source
line, disassembled the fault that §47 counts as its sibling, and ran the
interpreter control.  The first two are now precise; the third does not give the
clean answer the plan expected, and the rows below say why.

### 48.1 `x86-codegen.h:403` is `default: g_assert (0)` in `mono_x86_patch_inline`

The string `../../mono/arch/amd64/../x86/x86-codegen.h` appears at `.rodata`
0x29ea39 and is referenced from exactly **six** sites in all 24.7 MiB of text,
all inside one function at **libmonobdwgc+0xf5600**.  That function is
`amd64_patch()` (`mono/mini/mini-amd64.c`), and it is identifiable from its own
assert strings, which are line-for-line the 2020-02 source:

```text
libmonobdwgc+0xf5600  switch on code[0]; REX skip; b8-r / 8b modrm00 rm5 /
                      ff 15|25 / else x86_patch(code,target)
   f5650  assert file:380  "code [1] >= 0x80 && code [1] <= 0x8F"
   f5670  assert file:403  "0"                    <- the row Player.log printed
   f5695  (jcc rel8 patch)
   f56c7  (e9)
   f56ef  assert file:414  "offset == (gint8)offset"
   f5707  assert file:397  "code [1] == 0x15 || code [1] == 0x25"
   f571f  assert file:410  "offset == (gint32)offset"
```

and the two `mini-amd64.c` asserts next to it print `!1` (line 204, the
`mov 0(%rip),%reg` case) and `!2` (line 212, `call *off(%rip)`), which is this
build's `g_assert (!1)` / `g_assert (!2)` pair.  So the containing function is
`amd64_patch`, not a code emitter and not `mono_x86_patch` itself.

`x86-codegen.h:403` is the last line of the opcode switch:

```c
	switch (*code) {
	case 0xe8: case 0xe9: ...            /* call/jmp rel32 */
	case 0x0f: g_assert (code [1] >= 0x80 && code [1] <= 0x8F); ...
	case 0xe0 ... 0xe2: case 0xeb: case 0x70 ... 0x7f: ...
	case 0xFF: g_assert (code [1] == 0x15 || code [1] == 0x25);
		   g_assert (0);
	default:
		g_assert (0);            /* <-- line 403, condition string "0" */
	}
```

So the check is **not** a size dispatch and **not** the rel32 branch-range
check of L38: it is the *default* arm, reached when `amd64_patch()` is asked to
patch a code address whose first byte (after an optional REX) is none of
`e8 e9 0f e0 e1 e2 eb 70-7f ff` and none of `amd64_patch`'s own `b8-r`,
`REX 8b modrm00 rm5`, `ff 15/25`.  Its condition string is `"0"`, which is the
exact text in `Player.log`.  Line 410 — `offset == (gint32)offset`, the check
the L38 JIT arena was built for — is a different, *later* assert and did not
fire.

What value it saw was not captured: the abort is a userspace `abort()`, not a
kernel fault, so there is no `[KREG]` row, and Mono's `g_assert` clobbers `rdi`
before the message.  The register file is not recoverable from the SIGABRT
path.  What the row does establish is the class: the byte at the patched address
was not a branch/call opcode — i.e. the code buffer did not contain the
instruction the emitter was patching.  (A probe build with the assert site
replaced by `ud2` was staged — `RW_MONO_SO` — but the JIT-mode run to trigger
it was pre-empted by the moving failures in 48.4.)

### 48.2 The `VA=-1` is **not** in a JIT chunk; it is Boehm's `GC_malloc_kind_global`

§47.6 lists run 1 as `#PF VA=0xFFFF... PC=0x1088A76564 (JIT arena)`.  The
launcher's own SIGDUMP resolves that PC against its module list:

```text
[SIGDUMP] rip=0x1088a76564 (libmonobdwgc-2.0.so+0x275564) rsp=0x17ffffa8f0 ...
[SIGDUMP] greg rax=0xffffffffffffffff rbx=0x2c rcx=0x1088dbf760 rdx=0x3
          rsi=0x1088dc0020 rdi=0x2c
```

`libmonobdwgc` was mapped at 0x1088801000 in that run, so PC = base+0x275564,
*15 MiB below* the arena the same run claimed (`[JITA] claim base=0x1088FE0000
size=0x7020000`).  `jit=` was never true for this PC; §47.6's parenthetical is
wrong and is corrected here.

The instruction is `mov (%rax),%rdi` at +0x275564, inside
**`GC_malloc_kind_global`** (the function starts at +0x275502):

```text
275536  lea   GC_arrays,%rcx
27553d  mov   0x14d8(%rcx,%rbx,8),%rdx   ; rdx = GC_size_map[lb]  (lb=44 -> 3 granules)
275550  lea   GC_obj_kinds,%rsi
275557  mov   (%rsi,%rax,1),%rsi         ; rsi = ok->ok_freelist   (0x1088dc0020)
27555b  mov   (%rsi,%rdx,8),%rax         ; rax = ok_freelist[3] == 0xFFFFFFFFFFFFFFFF
27555f  test  %rax,%rax
275562  je    2755b0                    ; (NULL would mean "refill", -1 does not)
275564  mov   (%rax),%rdi                ; <-- fault, VA=-1
```

So the 48-byte free-list head was **0xFFFF_FFFF_FFFF_FFFF**: not NULL (the empty
list), not a heap address.  That is corruption of Boehm's own freelist array
(§48.4 says what kind).  No JITed method is involved: the frame chain in the
same dump is entirely native Mono (`mono_jit_info_get_method+0x664`,
`mono_gc_register_root+0x408`, `mono_string_new_size+0xd8`,
`mono_object_get_virtual_method+0x1eb`, ...), which is what an allocation from
runtime code (metadata/string/class setup) looks like.

### 48.3 The interpreter control: the runtime ignores `MONO_ENV_OPTIONS`; the flag can be forced, and the interpreter has its own wall

| run | how the interpreter was requested | `mono_use_interpreter` | outcome |
|---|---|---|---|
| L61-a | `MONO_ENV_OPTIONS=--interp` in the child env | **0** | JIT run; `tramp-amd64.c:641` right after the banner |
| L61-b | same + `mono_jit_parse_options(2,{"mono","--interp"})` before `mono_jit_init` | **0** | JIT run; Unity's own `archive:/CAB-...` CRC failure + `Caught fatal signal signo:5` |
| L61-c | `*(int *)mono_use_interpreter = 1` (exported global) before `mono_jit_init` | **1** | SIGSEGV at `libmonobdwgc+0x116e57` (interp frame setup) 30 s after launch, one line after `RimWorld 1.6.4871 rev600` |
| L61-d | as c + the driver's second global (0x5b9cb8 = 0x100000000) | **1** | `Stack overflow in unmanaged` at the same point |

Two things are established and one is not:

1. **`MONO_ENV_OPTIONS` is inert in Unity's embedded Mono.**  The launcher
   prints the runtime's own flag: `[RWMAIN] mono_use_interpreter=0
   asked=--interp`.  `mono_jit_parse_options()` is the embedder's parser and its
   own "Available options" list has no `interp` entry, so passing it there is
   also inert.  Any future interpreter control must set the exported global (or
   the driver's `-v`-style argv plumbing), not the environment.
2. **With the interpreter actually on, the `x86-codegen.h:403` assert does not
   occur.**  The process dies earlier and elsewhere: a NULL/foreign class
   pointer read in interp frame setup (`movzwl (%rax)` with `rax` read from a
   frame the native→interp transition had not built), i.e. a path this Unity
   build never exercises.
3. **The control therefore does not certify "game logic is correct".**  The
   interpreter is not a working execution mode in this runtime, so a menu under
   `--interp` is not available to prove the JIT is the only wall.

What the control *does* settle is the negative: the failure is **not confined to
the code generator**.  An execution mode with no amd64 code generation at all
still dies in the same phase, in runtime code.

### 48.4 Five runs, five sites: the ladder is a moving fault, not one codegen bug

| run | end | where |
|---|---|---|
| L60-1 | `#PF VA=0xFFFFFFFFFFFFFFFF PC=libmonobdwgc+0x275564` | Boehm `GC_malloc_kind_global`, freelist head = -1 |
| L60-2 | `fault-in-handler` + image `#UD 0x1000010970` | launcher image page |
| L60-3 | `x86-codegen.h:403` | `amd64_patch` (a JIT code buffer) at ~405 s |
| L61-a | `tramp-amd64.c:641` (`g_assert_not_reached`) | `mono_arch_create_specific_trampoline`, rel32 range |
| L61-b | `The file 'archive:/CAB-...' is corrupted!` + `signo:5` | Unity asset archive read |

The variance is not only in *where* a run dies but in *whether* it does: two
L61 JIT-mode runs (same build, same image) were still alive at their boundaries
(600 s and 900 s), resident 1138 and 1625 MiB with 26-37 threads, still in the
def/asset load and with no fault row — the L60 runs died at 265 MiB at ~405 s.
A deterministic codegen abort cannot produce that spread.

A deterministic codegen bug produces one site.  Five sites in the same phase,
with the same build and the same inputs, is the signature of **state-dependent
corruption**, and two of the five (48.2, 48.3) are outside the code generator
entirely.  §47.9 item 2 named the remaining candidate: a read that "succeeds"
without delivering its bytes (the `[BLKQ] stall ... CSTS=0` → `controller reset
#1` immediately before a page-in, and `[FATW] ZERORD` in the FAT16 write path)
is still the one mechanism that can touch every one of these sites.  The next
lane should treat that as the primary hypothesis, not the JIT.

### 48.5 What this lane changed

1. **`/INTERP` — a documented, real interpreter switch.**  `rwlaunch` exports
   `RW_MONO_INTERP=1`; `rwmain` loads `libmonobdwgc` before UnityPlayer,
   resolves `mono_use_interpreter`, sets it, and prints
   `[RWMAIN] INTERP: handle=.. parse=.. mono_use_interpreter=1` so every run
   says which engine it used.  This is the only way to run this runtime without
   the amd64 JIT, and it is the control §48.3 is built on.  It is **not** a path
   to the menu yet (the interpreter faults in its own entry path).
2. **`RW_MONO_SO`** stages a byte-patched `libmonobdwgc` from a named file
   instead of the shared export, so assert-site probes never modify
   `/srv/nfs/rimworld` (`RW_MONO_SO=obj/l61/libmonobdwgc.ud2.so`).
3. **`RimWorldLinux.l61`** — `rwlaunch` prefers this private name when present,
   because the NFS export is shared with other lanes and a concurrent harness
   installs its own AD-9 launcher over `/NFS/RimWorldLinux` while this guest is
   booting (measured: one run started with the other lane's 96,432-byte build,
   without the switch under test).
4. `tools/rw/l61-gates.sh` — the §47.8 tier, unchanged, for a delta.

### 48.6 Regression

## 48. 2026-11-06 — M6/L62: the 900 s soak fires the block-layer and zero-page tripwires at 430 s, the VM stops inside `fxsave`, and the present path measures 9.40 Mpx/s

§47 closed the deterministic `#UD` at `UnityPlayer+0x757570` and handed the next
lane a Mono JIT assert.  This lane was told to stop watching the loader and
start watching the *machine*: one long soak with the page-in tripwires armed,
plus a measured cost for a frame at each plausible window size.  Both answers
are worse than hoped and sharper than expected: **the soak reproduced the §47.4
mechanism on a different page (0x9AD000) at 430 s**, and the death that followed
took the whole VM down inside `fxsave` — so the soak has 430 s of evidence, not
900 s.  The performance half is clean: the present path is a fill-rate wall at
**9.40 Mpx/s**, and 30 fps at 1920x1080 needs **62.2 Mpx/s**.

### 48.1 The soak, and where it stopped

`tools/rw/run-rimworld-desktop.sh --run-timeout 900`, disk image at the merged
tip (`2ad691bf`), the launcher's default request (1280x720), no diagnostic
markers.  Counted by `tools/rw/l62-soak-check.py` over the 3.47 MB serial
capture and the 86 234-byte `Player.log`.

| measure | value |
|---|---|
| guest time before the stop | **430 s** (of the 900 s window) |
| `[XSW]` presents | **0** |
| NFS bytes | 277 457 914 (264.60 MiB) |
| `[TXT]` exec pages verified | **3584, bad=0** |
| `[TXT] ZERO` / `UNVERIFIED` rows | **0** |
| `[ZPFILL]` short reads refused | 0 |
| `[BLKQ] stall` / `controller reset` | **1 / 1** |
| `[BLKQ] giving up` / `reinit failed` | 0 |
| `STILL SPINNING` | 0 |
| `[BLKSLOW]` (>250 ms) | 1 |
| `[VMRT]` present-leaf retries | 24 (every one `prev=-1`) |
| `[VMK]` retry-budget kills / `[VMFIX]` | 0 / 0 |
| `[SIGX]` delivered signals | **0** |
| kernel `#PF` rows | 0 |
| non-`#PF` user exception (vector row) | **1 (vector 6)** |
| `Player.log` mid-file NUL runs ≥32 B | **0** |
| `[THRS]` live / high-water / slots | 25 / **43** / 89 of 191 |
| VM stops (KVM) | **1** |

So of the six assertions this lane was given, **two fired**: the block layer
wedged and a zero text page was executed; a third (the VM) died as a
consequence.  The rest are zero on rows that can report presence — `[TXT]`
counted 3584 pages, `[VMRT]` 24, `[THRS]` 93 samples.

### 48.2 The tuple: the wedge, the page, the instruction

```text
[BLKSLOW] pending cid=43110 slot=1 ms=251 sq_tail=2 cq_head=1 phase=1
[BLKQ] stall cid=43110 slot=1 sq_tail=2 cq_head=1 phase=1
       cqe[cq_head].cid=65535 .status=0x0 next.cid=65535 next.status=0x0
       inside=1 n=1 CSTS=0x0 CC=0x0 INTMS=0x0 INTMC=0x0
[BLKQ] controller reset #1
[KERNEL] User process fault! Vector: 6 RIP: 0x1081ce0ea0
[KERNEL] fault-pc region=0x1081A66000+0x17A4000 prot=5 kind=7 jit=0 live=36
         err=0x0 rsp=0x17FFFFA718
[KREG]  pid=4 vec=6 erc=0x0 rip=0x1081CE0EA0 rsp=0x17FFFFA718
        rax=0x10D089D5C0 rbx=0x10D14FDE20 rcx=0x108323F2C8 rdx=0x314 ...
[UDX]   rip=0x1081CE0EA0 off=0x9AD000 rd=4096 leaf=0x8120D025
        pa=0x8120D000 alloc=1 objfr=0x80D1F000 same=1 wf=0
[UDX]   pghd=00 00 00 00 00 00 00 00 00 00 00 00
[LCOMPAT] fault sig=11 pid=4 tgid=4 tls=0x108021D740
KVM internal error. Suberror: 1   extra data[1]: 0x66c35d07ae0f480f
```

* the region is UnityPlayer's `R E` span (`base + 0x733000`, 24.7 MiB),
  `prot=5`, `kind=7`, `jit=0` — the DSO's text, not the JIT arena;
* `0x1081CE0EA0 - 0x1081333000 = 0x9ADD0EA0`, i.e. file offset **0x9AD000**
  (`inpage 0xEA0`); the staged DSO holds `ff ff ff ff c7 40 18 ff ff ff ff`
  there, so the page in memory is **not the file's**;
* the leaf is present, user, read-only, executable, `pa=0x8120D000` still owned
  by the allocator (`alloc=1`), and the one-frame watch counted **zero**
  `frame_free()` calls (`wf=0`) — the same "not a lifetime bug, a fill bug"
  shape as §47.4, at a page 2.3 MiB further into `.text`;
* `same=1` with `rd=4096` means the *fresh full-page read agrees with the
  zeros* — a read that returned a whole page of wrong bytes, which §47.9 item 2
  named as the remaining hole;
* and it happened on the fill that immediately follows `CSTS=0` + `reset #1`,
  exactly the ordering §47.4 recorded.

### 48.3 Why `[TXT] bad=0` while a zero page was executed

The gate counted 3584 executable pages and refused none, and no `[TXT] ZERO`
row exists.  `vm_file_obj_materialize` cannot publish an all-zero DSO text page
(3 reads, back-off, `-EIO`); so either this page never entered that block, or
it was published non-zero and the zeros arrived later.  Both are open, and the
rows do not separate them: `pa != objfr` says only that the leaf is a private
copy (expected since L59).

The one path that *can* publish an exec page without the block is
`vm_touch()`'s object-file branch, which computes
`cow = write && (r->flags & VM_MAP_PRIVATE)` — no exec clause — while
`vm_handle_fault()` computes `cow = (write && PRIVATE) || (is_text && PRIVATE)`
(§46.3).  A read-touch prefault of a private text mapping therefore takes the
shared path and never meets the gate.  One provenance row at publish time
(`off=`, `cow=`, `zero=`, `src=share|copy`) decides it; do not "fix" the gate
before that row exists.

### 48.4 The VM stop: `fxsave` into a signal frame

The `[LCOMPAT] fault sig=11` row is the *attempt* to deliver §48.2's `#UD`.
The delivery died one instruction in:

```text
RIP=0x70090a44   code: 55 48 89 e5 <48> 0f ae 07 5d c3
                  push %rbp; mov %rsp,%rbp; fxsave (%rdi); pop %rbp; ret
RDI=0x1083eb1df0
```

`nm` on `hobbyos.elf` puts `0x70090a44` at `arch_fpu_save_to+4`; the operand is
the `fpstate` area of the signal frame (`l_sf_addr(base, L_SF_FP)` in
`l_sig_frame_plant`).  QEMU took an exit it could not emulate
(`KVM internal error. Suberror: 1`, "emulation failure") and **stopped the
vCPU**, so the guest froze: no handler ran, hence **0 `[SIGX]` rows** — the
same absence §46 and §47 reported for this death, now with a mechanism.

Two consequences worth keeping:

1. a guest `#UD` escalates to a **whole-VM stop**, not one process death.  The
   desktop's last frame stays on screen for the rest of the window, which is
   why a 900 s run yields 430 s of evidence and why `99_final` looks like a
   live desktop;
2. `RDI` is a *user* VA (0x1083EB1DF0 is above `USER_VA_BASE` 0x1000000000),
   i.e. the frame is on the per-thread altstack — so the write lands in a
   mapping the guest's page tables describe but the host has no memslot for.
   Either the altstack VMA is not what the PTE says, or the frame base is stale.
   That is a real lead for the signal/altstack work; it is not this lane's fix.

### 48.5 Window size: the WM ignores the request (row), and the cost of a frame

The WM stores `pix_pref_w/h` from `ESC ] X` and never reads them: layout is a
grid of tiles (`update_layout`), so a client gets `win->w-4 x win->h-36` of the
tile it was given.  The row is gl_bench's own handshake line:

```text
[PB] window content rect 1916x1018 (requested 320x240) at 2,34
```

A 320x240 request is answered with 1916x1018 — **25.4x** the pixels asked for.  So
`-screen-width/-screen-height` on the launcher changes **what the client
renders into**, not what the WM gives it, whenever the WM is the desktop's.

An opt-in switch (`/WMSIZE`, content-gated, staged only by the fps ladder)
makes the same client get its request exactly:

```text
[PB] window content rect 320x240 (requested 320x240) at 2,34
```

Default behaviour is unchanged (`wm_honour_pixel_pref(0)` unless the marker
says `on`), so every existing app and gate keeps the tiled layout.

`-force-glcore` + `-screen-fullscreen 0` is stable at both grants.  No run wrote
`_NET_WM_STATE`/FULLSCREEN (`FSREQ`/`FSGRANT`/`FSDECL`/`FSREV` = **0** in every
log), one window id served the whole run, and the presented surface never
changed size: 115 `[XSW]` rows at `w=320 h=240` with
`n=1090 swap=1090 put=1090 putok=1090` — **no lost presents** at the honoured
size, and the same shape at the tiled 1916x1018.  The game contributes no row to
that check because it never swapped.

**The game itself presented no frame** in either size configuration: 0 `[XSW]`
rows in the 430 s soak (1916x1018) and 0 in a 440 s run at 960x540 with the WM
honouring the request.  The render thread is alive and spinning, not deadlocked:

```text
[IDLESTUCK] slot=26 pid=26 st=8 wake=4294987342 futex=0x112000a460
            rip=0x108011895d UnityGfxDeviceW
[FUTEX] q slot=26 pid=26 ... key=0x112000A460 waits=14 wakes=13 UnityGfxDeviceW
```

so "fps at size X" cannot be read from the game today — it is 0 everywhere, and
the number is a loader/render-init property, not a fill-rate one.  The fill-rate
answer comes from the same DSO with RimWorld's measured scene shape
(`gl_bench`: 110 textured, alpha-blended screen-space quads, ~2 screens of
overdraw — `docs/rimworld-evidence.md` §6), guest softpipe, one boot:

| surface | Mpx | raster only (FBO) | fps | via FBO + blit + swap | fps | Unity's path (direct) | fps |
|---|---|---|---|---|---|---|---|
| 640x480 | 0.307 | 49.0 ms | 20.41 | 149 ms | 6.71 | — | — |
| 960x540 | 0.518 | 75.0 ms | 13.33 | 177 ms | 5.65 | — | — |
| 1280x720 | 0.922 | 120.0 ms | 8.33 | 231 ms | 4.33 | — | — |
| 1916x1018 | 1.951 | 225.0 ms | 4.44 | 366 ms | 2.73 | **221 ms** | **4.52** |
| 320x240 | 0.077 | — | — | — | — | **20.0 ms** | **50.0** |

The `present` column is the FBO→window copy path (a ~100-135 ms full-surface
blit the *direct* path does not pay); Unity uses `direct`, where the swap and
`XPutImage` cost only **2.28 ms** at 1916x1018.  The best measured configuration
is therefore the smallest window: **50 fps at 320x240, 4.5 fps at 1916x1018**,
and nothing in between reaches 30 fps until the surface is ≤ ~0.14 Mpx.

### 48.6 The performance model — and the quantified virgl requirement

Least squares over the four raster points (`tools/rw/l62-model.py`, R²=0.999):

```text
ms/frame = 19.0 + 106.4 * Mpx        (19.0 ms fixed = 110 draws at 197 us/draw)
                                     (106.4 ms/Mpx = 9.40 Mpx/s sustained)
```

The fixed term is per-draw CPU (197.4 µs/draw measured at 1280x720, fill-free),
the slope is softpipe fill.  Consequences, in the numbers a renderer has to beat:

* **1920x1080 at 30 fps needs 62.2 Mpx/s of blended fill** plus ≤33.3 ms/frame
  total.  Measured: **9.40 Mpx/s → 6.6x** short on fill; 4.52 fps at
  1916x1018 → 6.6x short on frame rate (the two agree because both are the same
  wall).
* at today's cost the 33.3 ms budget buys **0.135 Mpx** (~367x367); measured
  crossing: 320x240 → 50 fps, so 30 fps lands near 0.10-0.14 Mpx — the model is
  confirmed at both ends.
* the per-frame CPU floor is 19-20 ms even with zero pixels, i.e. **~48 fps is
  the ceiling of the whole present path at any size** unless the per-draw cost
  drops too.  A virgl port that only fixes fill gets to 30 fps at 1080p; it does
  not get past ~48 fps anywhere.
* the present-mode blit is a separate 100-135 ms tax (fixed 108.6 ms +
  132.2 ms/Mpx, R²=1.0000).  Any path that renders to an FBO and then copies
  pays it; the game's direct path does not.

So "virgl is needed" is not a preference: the software rasteriser sustains
**9.4 Mpx/s** and a playable 1080p frame needs **62.2 Mpx/s of blended,
2x-overdraw fill with ≤14 ms of per-frame CPU**.  Anything less is a smaller
window, not a faster game.

### 48.7 Regression

| gate | result |
|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **417 run, 0 failed** |
| `t3_linux_probe.sh redzone` | PASS |
| `t3_linux_probe.sh mmap_race` | PASS |
| `m2_mono.sh` | PASS |
| `m2_tp.sh` | PASS |
| `m2_threads.sh` | PASS |
| `m12_thread_ceiling.sh` | PASS |
| `m3_window_probe.py` | PASS |
| `m4_gl_probe.py` | PASS |
| `tools/rw/run-rwnfs-probe.sh` | PASS |

### 48.7 What is left

1. **The codegen row is precise but not the wall.**  `x86-codegen.h:403` is
   `default: g_assert (0)` in `mono_x86_patch_inline`; it fires when
   `amd64_patch()` finds a non-branch byte where the emitter expected one.  Its
   exact byte and method are still unmeasured (the `ud2` probe build is staged
   and ready: `RW_MONO_SO=obj/l61/libmonobdwgc.ud2.so`, which traps at both this
   site and `tramp-amd64.c:641` and leaves the code pointer in `rdi`/`rax` for
   `[KREG]`).
2. **The interpreter is not a fallback.**  Setting `mono_use_interpreter`
   starts the interpreter (`=1` verified) but its native→interp entry path
   faults at `+0x116e57`; the driver's second global only converts that into an
   unmanaged stack overflow.  Do not present `--interp` as a shipped mode until
   that entry path is fixed.
3. **The storage layer is the leading hypothesis for the moving ladder**
   (§48.4): one bad page-in can produce a corrupted freelist, a JIT code buffer
   with a wrong byte, a CAB that fails its own CRC, and a trampoline that is
   4 GiB from its target.  The row to fix is still "never complete a request
   whose DMA did not land"; the verification in §47.5 covers executable pages
   only.

### 48.8 Two JIT-mode runs that did not fail — and still did not reach the menu

Because §48.4 makes "where it dies" a coin flip, the lane ran the same build for
600 s and then 900 s with no `/INTERP`:

| run | wall | resident | threads | `[XSW]` frames | NFS bytes | end |
|---|---|---|---|---|---|---|
| 600 s | 600 s (harness boundary) | 1138 MiB | 37 | **0** | 264.5 MiB | child still alive, no fault row |
| 900 s | 900 s (launcher `RW_WAIT_SECS`) | 1625 MiB | 26 | **0** | 264.5 MiB | child still alive, no fault row |

Both runs were still in the def/language load when the harness cut them (the
last `[XO]` rows are Biotech/Royalty `Languages/English/Keyed/*.xml`), and
neither ever opened a swap path: `[XSW] n=0` for the whole window.  So even when
the corruption does not fire, the load does not converge on the menu inside
15 minutes — 264.5 MiB of NFS reads and 1.6 GiB resident with no frame.  That
is a second, independent blocker: the load is either re-walking the def tree or
growing without bound, and it is not the code generator either.

Conclusion for the plan: **the wall is not codegen-only, and a JIT bailout
cannot reach the menu today.**  The two rows that settle it are 48.2 (the
`VA=-1` is Boehm's freelist, not JIT code) and 48.3 (an execution mode with no
codegen still dies in the same phase).  The codegen assert of 48.1 is a real
bug and is now precisely located, but fixing it removes one of five sites.

| `m3_window_probe.py` | PASS |
| `m4_gl_probe.py` | PASS |

### 48.8 What is left

1. **The publisher of the zero text page.**  `[TXT]` saw 3584 pages and refused
   none, yet `0x9AD000` executed as zeros.  Add the publish-time provenance row
   (`off/cow/zero/src`) and re-run; the `vm_touch` `cow` clause is the suspect
   (§48.3).  A gate that is not on every publish path is worse than no gate,
   because it reports `bad=0`.
2. **A guest `#UD` must not stop the VM.**  `fxsave` in `arch_fpu_save_to`
   reached an address KVM had to emulate and the whole guest stopped (§48.4).
   Until delivery is safe, every long soak ends where the first fault lands.
3. **The game still presents nothing.**  0 frames at 1916x1018 and at 960x540,
   with `UnityGfxDeviceW` spinning (`wake=4294987342`).  That is the JIT/loader
   lane's wall (§47.9 item 1); this lane's rows only bound it: no fill-rate
   number can be taken from the game until the first swap happens.
4. `slots` reached 89/191 at 430 s (§46.9 item 2) with 25 live threads — a
   900 s load still needs the tombstone reaper.

## 49. 2026-11-06 — M6/L63: the VM stop was `fxsave` against a **user** address, not the 568 offset; the frame's FP image is staged and a guest fault can no longer stop the machine

§48.4 handed this lane a one-line mechanism: the `#UD` delivery died at
`arch_fpu_save_to+4`, `fxsave (%rdi)`, `rdi=0x1083EB1DF0`, and QEMU stopped the
vCPU (`KVM internal error. Suberror: 1`).  The brief's hypothesis was that
`L_SF_FP 568` is `8 (mod 16)`, so a 16-aligned frame base makes the FXSAVE
operand misaligned and the `#GP` kills the VM.  **The arithmetic half of that is
wrong, and the measurement says so**: the frame base is *not* 16-aligned.  It is
8 (mod 16) by construction (`base = ((sp - L_SF_SIZE) & ~15) - 8`, because the
x86-64 psABI enters a handler with `%rsp % 16 == 8`), so `base + 568` is
16-aligned — `0x1083EB1DF0` itself is `0 mod 16`.  Nudging the constant to 576
would have put the fpstate at 8 (mod 16) on **every** delivery and turned a
one-off stop into a guaranteed one.  What is real is the second half of the
mechanism: **the kernel executed FXSAVE with a user operand**, and a fault on
that instruction is unrecoverable.

### 49.1 The alignment table, measured from ring 3

The rule is about the *address*, not the offset, so the probe
(`src/user/linux_probe_fpu_sig.c` P1) measures the four combinations with a
`SIGSEGV` handler that skips the faulting instruction (`48 0f ae /r`).  Same
binary, workstation and guest:

| base | off | dst | dst % 16 | result (host) | result (guest) |
|---|---|---|---|---|---|
| `…1030` (0 mod 16) | 568 | `…1268` | 8 | **#GP** → SIGSEGV | **#GP** → SIGSEGV |
| `…1030` (0 mod 16) | 576 | `…1270` | 0 | ok | ok |
| `…1438` (8 mod 16) | 568 | `…1670` | 0 | ok | ok |
| `…1438` (8 mod 16) | 576 | `…1678` | 8 | **#GP** → SIGSEGV | **#GP** → SIGSEGV |

That is the whole argument in one table: 568 is correct *for the base this
kernel plants*, 576 is correct only for a base it never plants, and the kernel
side now derives the offset from the base (`l_sf_fp_off`) so the two can never
drift apart.  Every plant also prints the arithmetic:

```text
[FPUA] pid=2 sig=11 base=0x0x17FFFFF878 b16=8 off=568 fp=0x0x17FFFFFAB0 fp16=0 rc=0 bad=0 rows=1
[SFP]  pid=3 gen=2 sig=11 kind=0 sp=0x0x17FFFFFD50 sp16=0 base=0x0x1080000BB8 b16=8 fp_off=568 fp16=0 alt=1
```

`b16=8`, `off=568`, `fp16=0` in every row of both 900 s soaks (24 `[FPUA]`, 48
`[SFP]`, `bad=0`): the operand was never misaligned, before the fix either.

### 49.2 What actually went wrong

`arch_fpu_save_to()` executed `fxsave64` with `%rdi` = a **user** VA, from ring
0.  Two consequences follow, and only the second one is about the offset:

1. FXSAVE takes an implicit 512-byte operand; any fault it takes is a
   kernel-mode fault in the middle of signal delivery.  KVM cannot emulate the
   instruction, answers `KVM_INTERNAL_ERROR_EMULATION`, and QEMU's handler for
   that is `vm_stop(RUN_STATE_INTERNAL_ERROR)` — the vCPU is gone, so the guest
   cannot run the handler, print a row, or die politely.  That is why §48 saw
   `[SIGX] = 0` and a desktop frozen at 430 s.
2. The destination page had already been written by the plant (pretcode,
   ucontext, gregs — all in the same 4 KiB page as `base+568`), and the page was
   mapped `prot=3 anon`, so a plain store there worked.  Why the *aligned*
   FXSAVE then faulted is not fully explained by the rows this lane has: the
   candidates are a per-core `CR4.OSFXSR=0`/`CR0.TS` (→ `#UD`, which KVM also
   tries to emulate) or a translation the host could not back at that instant.
   The fix does not depend on which it was, and that is the point: the kernel no
   longer hands a user address to FXSAVE/FXRSTOR at all.

### 49.3 The fix

| site | before | after |
|---|---|---|
| `linux_compat_thr.c` plant | `arch_fpu_save_to(base + 568)` | `arch_fpu_save_user(p, base + l_sf_fp_off(base))` |
| `linux_compat_thr.c` rt_sigreturn | `arch_fpu_restore_from(fpregs)` | `arch_fpu_restore_user(p, fpregs)` + fallback, logged `[FPUX]` |
| `process.c` native plant | `arch_fpu_save_to(f->fpu)` (user VA) | `arch_fpu_save_user(grp, f->fpu)` |
| `process.c` sigreturn | `arch_fpu_restore_from(f->fpu)` | `arch_fpu_restore_user(grp, f->fpu)` |

`src/kernel/fpu_user.c` owns the staging: a 16-byte-aligned 512-byte **kernel**
buffer (a stack local, so two CPUs taking signals cannot share it), `FXSAVE`
into it, then `vm_kwrite` / `vm_user_read` — `vm_touch` + direct-map copy under
`vm_lock`, the same path every other user-pointer write already uses.  The
alignment/range check runs **before** the instruction, so a misaligned or
unbacked destination is `-EFAULT`:

```text
[FPUA] … fp16=0 rc=0        a good plant
[FPUA] … fp16=8 rc=-14      would now be an EFAULT, not a #GP        (guard)
```

A failed plant runs the signal's **default action** for that process (the compat
engine kills the group with the signal, the native engine returns 0 → same), and
a failed restore leaves the live FPU state alone instead of feeding garbage to
FXRSTOR.  The v1 (statically mapped 32 MiB) window keeps working through a
validate-then-byte-copy arm, so native programs' signals are unaffected.

Kernel-side regression tests (`src/kernel/arch/x64/fpu_sig_test.c`) pin the
offset arithmetic (568 for `base%16==8`, 576 for `base%16==0`) and the guard
(4 misaligned/out-of-window addresses → `-EFAULT`; the misaligned call would
`#GP` on the spot if the guard ever moved behind the instruction).  The unit
tier went 417 → **421 run, 0 failed**.

### 49.4 The frame now survives the address that killed it

Soak A (tree at `e10f8ab3`), at the same wall as §48 — a `#UD` (vector 6) in
UnityPlayer text, `[UDX] off=0xACB000`, page header all zeros — reached the
delivery and this time it **completed**:

```text
[SIGX] pid=4 tid=4 tgid=4 sig=11 code=1 addr=0x0x0 ipc=0x0x10825FFF80
       rsp=0x0x1083EB1BB8 h=0x0x100000D450 alt=1 live=41
[KERNEL] pid=4 memory fault VA=0xFFFFFFFFFFFFFFFF … inH=1
[LCOMPAT] fault-in-handler pid=4 sig=11 ipc=0x0xFFFFFFFFFFFFFFFF -> default action
```

`0x1083EB1BB8 + 568 = 0x1083EB1DF0` — **exactly** §48.4's faulting `RDI`.  The
frame that used to stop the machine was planted, the handler at `0x100000D450`
ran on the altstack (`alt=1`), it faulted itself on `-1`, and the *process* was
killed.  The VM kept printing for the rest of the window.

### 49.5 The 900 s soak

Soak B (final tree, `RW_KEEP_RUNNING=1` so the window is the full 900 s even
after the launcher exits), `tools/rw/run-rimworld-desktop.sh --run-timeout 900`,
`tools/rw/l62-soak-check.py` over the 4.58 MB serial capture:

| measure | value |
|---|---|
| serial capture | **4 582 706 bytes** (soak A: 3 633 810 at launcher exit) |
| screenshots / window | 105 (103 running shots × 8 s ≈ the full 900 s) |
| **KVM emulation failures / VM stops** | **0 / 0** |
| NFS bytes | 277 643 386 (264.78 MiB) |
| `[TXT]` exec pages verified | **3584, bad=0** |
| `[XSW]` presents | 0 (unchanged: the game never swaps, §48.5) |
| `[SIGX]` delivered | **1** (SIGABRT, `alt=0`) |
| fault inside the handler | **1** (`VA=0xE`, `inH=1` → default action) |
| `[FPUA]` plants / `fp16≠0` / `rc≠0` | 24 / **0** / **0** |
| `[BLKQ]` stall + reset | 1 + 1 (unchanged from §48, separate defect) |
| `[THRS]` live / high-water / slots | 26 / 44 / 91 of 191 |

The game's own death moved to the SIGABRT path (`ipc=0x108004300B`, then a
SIGSEGV at `0x108290F804` reading `VA=0xE` inside the handler) and happened at
~450 s — but **the machine outlived it by ~380 s**: 47 more screenshots of a
live desktop and 1.05 MB more console (the SIGX row sits at byte
3 534 506 of 4 582 706).  That is the difference §48 could not
show: a guest fault now costs one process.

### 49.6 Gates

`tools/rw/l63-gates.sh` (10/10):

| gate | result |
|---|---|
| `run_unit_tests_intel.sh` | **421 run, 0 failed** (417 + 4 FPU) |
| `t3_linux_probe.sh redzone` | PASS |
| `t3_linux_probe.sh fpu_sig` (with `PROBE.ELF nest`) | PASS |
| `m2_mono.sh` / `m2_tp.sh` / `m2_threads.sh` | PASS / PASS / PASS |
| `m12_thread_ceiling.sh` | PASS |
| `m3_window_probe.py` / `m4_gl_probe.py` | PASS / PASS |
| `run-rwnfs-probe.sh` | PASS |

The probe's own rows, identical on the workstation and in the guest: P1 the
four-case alignment table, P2 a SIGUSR1 on the normal stack with xmm0..15 +
MXCSR + the x87 file (ST0..ST7, FCW) **bit-identical** after a handler that
clobbers all of them, P3 the same on a deliberately tight 4 KiB altstack plus
`uc_mcontext.fpregs` non-NULL, 16-aligned, and usable by the guest's own
`fxrstor64`, and P4 a fault *inside* the `SIGSEGV` handler (the §48 shape) which
kills the second wave instance and leaves the graded one to print its marker.

### 49.7 What is still open

1. **The zero text page** (§48.3) is untouched and is what produces these
   `#UD`s: `[UDX] off=0xACB000 … pghd` all zeros, `[TXT] bad=0`.  Killing the
   process instead of the machine makes it debuggable, not fixed.
2. **Why the aligned FXSAVE faulted at all** (§49.2) is still unexplained; a
   per-core CR0/CR4 dump at the delivery site would settle it.  The staging
   removes the exposure either way.
3. The block-queue stall/reset at ~430 s is unchanged and still the first
   tripwire to fire.

## 49. 2026-11-06 — M6/L63: the zero text page has a name (`vm_touch`'s exec-less `cow`), every publish carries its provenance, and the image path needed a checksum

§48.3 left one question open: `[TXT]` counted 3 584 executable pages and refused
none, yet `0x9AD000` executed as zeros.  Was the page published by
`vm_file_obj_materialize` (which would have refused it) or by something else?
It named the only site that could bypass the gate — `vm_touch()`'s object-file
branch, `cow = write && MAP_PRIVATE` with no exec clause — and asked for a row
at publish time instead of another theory.  This lane built that row, and it
answers the question in two parts: the `vm_touch` branch is real (and is now
fixed and tested), and the rows then found a **second** publisher on the image
path that two agreeing reads cannot catch.

### 49.1 The provenance row: one per publish, naming the caller

`vm_pub_note()` (vm.c) is called at every site that installs a page's BYTES
into a user mapping:

| `who=` | site | source |
|---|---|---|
| `touch_obj` | `vm_touch` -> `vm_file_obj_materialize` | file / cache |
| `fault_obj` | `vm_handle_fault` -> same | file / cache |
| `touch_anon` | `vm_touch` anonymous branch | zero |
| `fault_anon` | `vm_handle_fault` anonymous branch | zero |
| `img` | `v2_image_materialize` (demand + eager first page) | file / zero |
| `img_pf` | `v2_image_prefetch_run` (batch) | file |
| `elf` | `elf_loader`'s staged image | file (count-checked) |
| `cow` | `vm_cow_page_locked` | copy |
| `vmfix` | the present-but-faulting remap | copy |

Each row is `va, off, prot (R/RW/RX), priv, src (file|zero|cache|copy),
zero_bytes (counted from the frame, not inferred), ver, who`.  Executable
pages from `src != zero` are printed unconditionally; everything else is
sampled (first rows per site, then every 1024th), with a `[PUBS]` total every
4 096 publishes.  `src=zero` is the DEFINED content of anonymous memory
(`.bss`, heap, a JIT arena page the writer fills), so it is counted but never
alarmed — that is the one place a zero page is legal.

### 49.2 The publisher, and the fix

`vm_touch`'s branch now reads:

```c
int is_exec = (r->prot & VM_PROT_EXEC) != 0;
int cow = (write || is_exec) && (r->flags & VM_MAP_PRIVATE);
```

and `vm_file_obj_materialize` no longer puts the gate inside `if (cow)`:

* `vm_exec_page_verify()` is the L60 two-read gate lifted out of the COW
  branch.  It runs for every exec publish from a file-backed object — private
  or shared, touch or fault — and fills the published frame from the file.
  Two agreeing reads that are not all-zero are the page; a disagreement buys
  a third; a short read is never zero-extended.  Failure is `-EIO`.
* A private exec mapping always gets a private frame (L59's rule), so the
  verified bytes cannot be recycled under executing code; a shared one keeps
  the object's frame when it agrees with the file and publishes the verified
  copy when it does not.
* `nfs_obj_page()` no longer zero-extends a short NFS read: only the file's
  real tail may be shorter than a page.

### 49.3 The second publisher the rows found: the image path

The soak's rows named a page the image path published with
`zero=4096 ver=1` — verified, because two independent reads agreed — and the
CPU then executed `0x1000010EA0` while `RWLAUNCH.BIN` holds
`55 48 89 e5` (`push %rbp; mov %rsp,%rbp`) there.  Agreement is not proof when
the device can answer the same zeros twice (§48.2 measured exactly that), and
the image path could not use the DSO rule ("no text page is all zeros")
because `DESKTOP.BIN` legitimately has 107 zero pages.

So the image now has a witness instead of a second opinion: `v2_map_image`
reads the file ONCE at exec (`vm_image_sums_build`) and keeps a sum32 per
page (4 B/page, a contiguous frame run, freed at teardown, copied at fork).
Every exec image fill — demand, eager first page, and the coalesced prefetch
batch — must match its page's sum; a mismatch is `[TXT] IMG SUM ... -> not
published` and `-EIO`.  An exec page entirely past the image is refused; a
`.bss` tail page stays legal.

### 49.4 The kernel test: a fault injected into the file object

`test_vm_exec_page_eio` (vm_test.c) injects faults into
`fat16_read_direct_pos()` (`fat16_test_inject`, unit tier only) and asserts
the mapping gets an I/O error and stays ABSENT:

1. short read -> `vm_touch` = -1, page absent;
2. full-length zeros (the §48 delivery) -> -1, page absent;
3. injection cleared -> the same page publishes the FILE's bytes (the gate
   replaces the poisoned cache frame instead of publishing it);
4. `vm_handle_fault` (exec=1) refuses the same shape and resolves when the
   device is healthy;
5. a fresh address space with a HEALTHY cache still gets a PRIVATE frame —
   the publisher property itself.  **Negative control:** with the exec clause
   removed, this assertion fails (`leaf == objfr`, `SHARED`) and the unit tier
   reports 2 failures; with it, the leaf differs (`private`).

`test_vm_img_page_eio` does the same for the image: build the sums, fill page
1, unmap, inject a full page of zeros -> `[TXT] IMG SUM pos=0x1000
got=0x76EFDDC5 want=0x106D33C5 -> not published`, absent; healthy -> bytes
match `HO_BIGLOAD_30MB`.  Unit tier: **419/0** (was 417; the two new tests).

### 49.5 Lane isolation: one export per worktree

L61 measured a run booting with another lane's 96 432-byte launcher: every
lane was served from `/srv/nfs/rimworld`, and each harness installs its own
AD-9 launcher at `<export>/RimWorldLinux`, so a concurrent lane overwrote it
mid-boot.

`tools/rw/lane.sh` now derives `RW_LANE` (the worktree's dirname) and
`RW_EXPORT=/srv/nfs/rimworld-<lane>`; `nfs-export.sh` exports that directory
and REFUSES the shared path unless `RW_ALLOW_SHARED_EXPORT=1`; every harness
uses `$RW_EXPORT`; and the guest is told which one by `/NFSEXPORT`
(`src/user_include/rwnfs_lane.h`, read by rwlaunch and the NFS probes).  The
serial log carries the proof: `[RW] rwlaunch: export
/srv/nfs/rimworld-l64-zeropage mount 10.0.2.2:/srv/nfs/rimworld-l64-zeropage
-> /NFS rc=0`.

Two lanes mounted concurrently (loopback, TCP for the host client; the guest
uses UDP):

| lane | export | `LANE.TXT` | `RimWorldLinux` sha256 |
|---|---|---|---|
| l64-zeropage | `/srv/nfs/rimworld-l64-zeropage` | `lane=l64-zeropage ...` | `5b22a833...c017d68` |
| l63-proof | `/srv/nfs/rimworld-l63-proof` | `lane=l63-proof ...` | `734e3221...c557b58` |

With both mounted, lane B replaced its own `RimWorldLinux` (the L61 defect
replayed): B's mount changed to `3469565e...d3682d`, **A's stayed
`5b22a833...c017d68`**.  The `.l61` private-alias workaround is gone: with a
per-lane export `/NFS/RimWorldLinux` is already this lane's binary.

### 49.6 The soak

`tools/rw/run-rimworld-desktop.sh --run-timeout 900` on this tree, counted by
`tools/rw/l63-soak-check.py`: every `[PUB]` row is one publish, and the verdict
columns are the ones §48.8 item 1 asked for.  Two runs are reported because
they sample two different halves of the mechanism: run 3 (the final tree) never
hit the block wedge, and run 2 (same code, before a cosmetic `off=` fix) hit it
and is where the gate's refusal is visible.

| measure | L62 (430 s) | L63 run 3 | L63 run 2 (wedge) |
|---|---|---|---|
| guest time in the window | 430 s (VM stop) | **902 s** (104 running shots) | 903 s (103 shots) |
| `[PUB]` rows printed / alarm rows | n/a | 4 450 / **0** | 4 386 / **0** |
| zero-byte exec publishes, file said non-zero | (unmeasurable) | **0** | **0** |
| unverified exec publishes | (unmeasurable) | **0** | **0** |
| `[UDX]` rows | 1 | **0** | **0** |
| `#UD` in UnityPlayer text | 1 | **0** | **0** |
| `[TXT]` exec pages verified (bad) | 3 584 (0) | 3 584 (0) | 3 584 (0) |
| gate refusals (`... -> not published`) | 0 | 0 | **1** at `off=0x757000` |
| `[BLKQ] stall` / `controller reset` | 1 / 1 | 0 / 0 | 1 / 1 |
| `[SIGX]` delivered | 0 | 0 | 0 |
| non-`#PF` user exceptions | 1 (vector 6) | 0 | 0 |
| VM stops (KVM) | 1 | 0 | 1 (`fxsave`, delivery) |
| NFS bytes | 277 457 914 | 277 583 480 (264.72 MiB) | 277 467 064 |
| `[XSW]` presents | 0 | 0 | 0 |
| resident peak | — | 1 353 MiB | 1 137 MiB |

The provenance table from run 3 (rows printed / exec / exec-zero where the file
says non-zero / exec-zero the file really holds / unverified / anon-zero /
alarms):

```text
caller         rows   exec exec0bad exec0ok  unver  anon0  alarm
cow               7      0        0       0      0      0      0
elf              53     51        0       0      0      0      0
fault_anon      345      1        0       0      0      1      0
fault_obj      3978   3963        0       0      0      0      0
img              48     48        0       9      0      0      0
touch_anon       14      0        0       0      0      0      0
touch_obj         5      2        0       0      0      0      0
```

`exec0ok=9` is the boot image's own all-zero pages (the sum table confirms
them against the file); `anon0` is `.bss`/stack, which is legal by definition.
Every DSO text page in the load (`fault_obj`, 3 963 publishes) was published
`src=file ver=1`, i.e. from a verified read — zero of them from the cache.

**Run 2 is the one that reproduces §48's tuple**, and it ends with the right
outcome instead of an executed zero page:

```text
[BLKSLOW] pending cid=44171 ... ms=251
[BLKQ] stall cid=44171 ... CSTS=0x0
[BLKQ] controller reset #1
[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published
[KERNEL] pid=4 (/NFS/RimWorldLinux) memory fault VA=0x108228B570
         ... in=PROT region=0x1082267000+0x17A4000 prot=5 kind=7 -> killed
```

`0x108228B570` is base+**0x757570**, the §47 thunk page — the page the L60 and
L62 ladders died executing.  This time the gate read it three times, got a full
page of zeros each time, refused to publish it, and the process took a real
protection fault instead of executing zeros.  A guest `#UD` no longer happens
on that path at all; what remains is the **FXSAVE delivery stop** (§48.4): the
SIGSEGV is being delivered when `arch_fpu_save_to` reaches an address KVM must
emulate.  That is the other lane's fix.  **When it lands, this soak must be
re-run on the merged tree** — the page-in verdict stands on this tree, but a
900 s window that contains a fault will only complete end-to-end once delivery
is safe.

### 49.7 What is left

1. **The FXSAVE stop** (other lane): a guest fault still ends the whole VM, so
   every long soak ends where the first fault lands.  On this tree that is now
   the *refusal* of the zero page, not the zero page itself.
2. **The block layer still wedges** (one `[BLKSLOW]` + `[BLKQ]` stall + reset
   in run 2; none in run 3) — the page-in gates make it survivable, not
   absent, and a wedge is still a 250 ms stall every time it happens.
3. **No presents** (`[XSW] n=0`): the game still never swaps, so the fill-rate
   wall of §48.5 is unchanged.

## 50. 2026-11-06 — M6/L65: the 47.8 vs 264 MiB spread is a **sampling-time artifact**; all three 1800 s runs sit at 47.7 MiB with `Example_Traits.xml` last at t≈280 s

§49 left one number unexplained: a soak that read **47.8 MiB** while an
earlier run on the same code read **264 MiB**, with no menu in either.
Was that a latent race or scheduling?  This lane ran three fresh **1800 s**
windows with a 30 s sampler (`tools/rw/l65-soak-sample.py`) and a timeline
builder (`tools/rw/l65-timeline.py`).  The answer is (b) **benign variance
that the window absorbs** — 47.8 MiB is what every run has read at
t≈240-330 s, and the artifact that carries the number is a **280 s** run.

Full evidence: `docs/l65-variance-report.md`; the 30 s tables:
`docs/l65-variance-timelines.md`.

### 50.1 Method

`RW_KEEP_RUNNING=1 RW_WAIT_SECS=1800 --run-timeout 1800` on this worktree's
export (`/srv/nfs/rimworld-l65-variance`).  The launcher's wait was hard-coded
900 s, i.e. below the new window, so `RW_WAIT_SECS` is now a compile-time
override in `run-rimworld-desktop.sh` and the launcher (`#ifndef` guard); run 1
prints `child still running after 1690s`, so the override took effect.  Every
30 s the sampler records NFS bytes (last `[NFSRTT] periodic`), presents (last
`[XSW] n=` from a live `mcopy` of `/RW_HOME/Player.log`), live threads and
resident MiB (last `[THRS] pid=4`, 5 s cadence), the last `[XO]` open, and the
tripwire counts (`wait_max_us>=500000`, `[BLKQ] stall`, `[NFSPIPE] stall`,
`[NFSRD] fail`, `[TXT] ZERO`, `KVM internal error`).

### 50.2 All three curves are the same curve

Cumulative NFS MiB:

| t (s) | run 1 | run 2 | run 3 | 47.8 artifact |
|---:|---:|---:|---:|---:|
| 210 | 46.7 | 46.7 | 46.7 | 46.7 |
| 240 | 46.7 | 47.7 | 46.7 | **47.7** |
| 270 | 47.7 | 47.7 | 47.7 | 47.7 |
| 300 | 47.7 | 47.7 | 47.7 | 47.7 (ends 280) |
| 330 | 47.7 | 48.7 | 47.7 | — |
| 390 | 54.2 | 67.8 | 52.3 | — |
| 450 | 129.0 | 179.8 | 126.2 | — |
| 510 | 264.7 | 264.7 | 264.6 | — |
| final | **264.72** | **264.74** | **264.65** | — |

The completed totals are deterministic to 0.035 %: 277 577 677 / 277 598 238 /
277 500 762 bytes.  Run 2 is the fastest (the burst starts ~30 s earlier) and
runs 1/3 the slowest; that spread is the only run-to-run difference in the
load.

### 50.3 The 47.8 MiB artifact is a 280 s run

`l66-virgl/obj/rw-desktop` (byte-identical to the l65 copy of the same run) has
**34 running shots** at `RW_SHOT_EVERY=8` (12:13:21 → 12:18:09), a last row
`child still running after 280s`, and `make: *** [Makefile:2828: run] Killed`.
It is a **300 s** run.  Every artifact on record that lasted past 400 s read
264.6-264.8 MiB (l59 410 s, l61 860 s, l63 470 s, l64 850 s, run 1 1690 s,
run 2 490 s).  The §49.6 "900 s / 47.8 MiB" pairing is therefore most plausibly
the 300 s verification run's byte count (its log overwrote the 900 s soak's at
12:13) crossed with the longer run's resident figure.

At its last sample the artifact is in the *same state* as the fresh runs at
t≈281 s:

| at t≈281 s | NFS MiB | `[XSW]` | live | res MiB | last `[XO]` |
|---|---:|---:|---:|---:|---|
| 47.8 artifact (t=280) | 47.7 | 230 | 30 | 752 | `Core/.../DefInjected/TraitDef/Example_Traits.xml` |
| run 1 | 47.7 | 210 | 34 | 755 | same |
| run 2 | 47.7 | 230 | 31 | 752 | same |
| run 3 | 47.7 | 210 | 40 | 751 | same |

### 50.4 There is no branch point before 330 s

The first file a fast run opens after the common state is the mod language
set; the artifact's window ends inside the gap:

| run | last `Core/.../DefInjected` | first mod `Keyed` |
|---|---|---|
| 47.8 artifact (ends 280 s) | `Example_Traits.xml` at 240 s | never — window ends |
| run 1 | `Example_Traits.xml` at 260 s | `Royalty/Keyed/Alerts.xml` at 320 s |
| run 2 | `Example_Traits.xml` at 230 s | `Royalty/Keyed/Alerts.xml` at 300 s |
| run 3 | `Example_Traits.xml` at 260 s | `Ideology/Keyed/Dialog_StatsReports.xml` at 330 s |

The transport rows are *retries that work* in every run: the artifact has two
`[NFSPIPE] stall` rows at 230 s (`issued=4 replies=2/3`, ~520 ms) and the retry
returns the page (`got=4096`); run 1 has eight more between 250-350 s, run 2
seven between 230-320, run 3 seven between 250-350 — every one recovers, and
the burst follows.  A stall row before the burst is the normal shape, not the
slow run's signature.

### 50.5 Verdict

**(b) benign, absorbed by the window.**  (1) 47.8 MiB is the loader's position
at t≈240-330 s in every run; the 1800 s window carries it through the def-parse
plateau into the asset burst (330-390 s) and to the deterministic total by
491-541 s.  (2) The only 47.8 MiB artifact is 280 s long; no 900 s / 47.8 MiB
artifact exists.  (3) It is not (c) load-order nondeterminism: the `[XO]` order
after the first four files differs between every pair of runs, yet the byte
total reproduces to 0.035 %.  (4) It is not (a) a race on this evidence: no run
lost a wake at the plateau and no retry row is followed by a stop.  A future
soak must quote its wall time and shot count with its byte count.

### 50.6 The real bug in the window: the block wedge is deterministic

Runs 2 and 3 died **after** the load, at 491 s and 511 s, at the **same** VA
(`0x10824E1EA0`), after the **same** refusals:

```text
[BLKSLOW] pending cid=43767 ... ms=251
[BLKQ] stall ... CSTS=0x0 ... inside=1 n=1
[BLKQ] controller reset #1
[TXT] ZERO off=0x9AD000 reads=3 n0=4096 -> not published
[TXT] IMG SUM pos=0x10000 got=0x76EFDDC5 want=0xDC910CE7 -> not published
[KERNEL] pid=4 ... memory fault VA=0x10824E1EA0 ... in=PROT ... -> killed
[SIGX] pid=4 ... sig=11 ... alt=1 live=37
[KERNEL] pid=4 ... memory fault VA=0x0 PC=0x0 ... inH=1 -> killed
[KERNEL] pid=3 (RWLAUNCH.BIN) memory fault VA=0x1000010EA0 ... -> killed
```

The page-in gate turned silent zero execution into an I/O error (no `#UD`, no
`KVM internal error`, no `vm_stop`), which is the L63 result holding on the
merged tree.  But the wedge itself is reproducible and still kills the process
tree — that, not the byte count, is what to fix next.  Run 1 never hit it: it
parked after the load with 260 presents and grew resident to 1 899 MiB by the
end of its window (launcher t=1690 s; the launcher's loop counter runs ~6 %
behind wall time under load).

### 50.7 No menu, no frame to quote

All three runs present 260-290 frames at 1920×1080, `box=0,0,1915,1017`,
`crc` alternating `0xcab88ca9/0x2e2650f4/0xbc23863d` (run 3 ends on
`0xbb5ae76f`), `changed=1`; OCR of the best frame is loading art (132 577
colours), no menu text.  Run 1's framebuffer drops to 8×1 at t≈500 (later
screendumps are 8×1); runs 2/3 lose the game window at 504/491 s.  There is no
menu evidence and no game fps.

### 50.8 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **423 run, 0 failed, UNIT TESTS
PASSED**.  `m3_window`: **PASS** (`m3w: PASS — the shim served SDL's whole
XCreateWindow sequence and delivered WM_DELETE_WINDOW`).  `m4_gl`: **PASS**
(`m4: PASS — libGL.so.1 loaded in the guest, rendered a GL 3.3 core triangle,
PRESENTED it to a window, and tore the stack down`).

## 51. 2026-11-06 — M6/L67: the block wedge is a **host-side controller reset**, and the page that killed the loader was an **aborted transfer**; run 1's 1 899 MiB was a livelock, not a load

§50 named the block wedge as the real bug in the window and left the question
the L55 row could not answer: *is the device not completing, or are we missing a
completion?*  This lane instrumented the request, the ring **and the device's own
registers**, traced the host side of the same device, and got a decisive answer.
It is neither a lost completion nor a bookkeeping bug: **the controller is reset
out from under the request, by QEMU, and the transfer that was in flight is
aborted and later completes with success and no DMA — which is the page of zeros
that kills pid 4.**

Full evidence: `docs/l67-wedge-report.md`.

### 51.1 The stalled-request tuple

Run 1 (1800 s window, load complete at `bytes=277581512`), t≈590 s:

```text
[BLKQ] device off cid=44230 op=0x2 lba=0x7B41B since_ms=2
       cc=0x460001 csts=0x0 cap=0xF0107FF/0x400820 vs=0x10400 cc0_writes=0
[BLKQ] wedge-device-off cid=44230 op=0x2 lba=0x7B41B nlb=0 prp=0x748B6000
       slot=1 sq_tail=2 cq_head=1 phase=1 cqe[cq_head].cid=65535 .status=0x0
       at_slot.cid=65535 .status=0x0 last_cid=44229 last_sqhead=1
       ms_since_done=8 inside=1 stalls=0 devoff=1 reqs=109665 cc0=0 cpu=3
       CSTS=0x0 CC=0x0 CAP=0xF0107FF VS=0x10400 INTMS=0x0 INTMC=0x0
[BLKQ] controller reset #1
[BLKQ] admin timeout cid=2 CSTS=0x0
[BLKQ] create SQ failed status=0x7FFF
[BLKQ] reinit failed cid=44230 -> I/O error
[BLKQ] device off cid=44231 op=0x2 lba=0x7B418 since_ms=2019 cc=0x0 csts=0x0 ... cc0_writes=1
[BLKQ] controller reset #2
[BLKQ] recovered cid=44231 us=5000 resets=2
[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published
[KERNEL] pid=4 memory fault VA=0x000000108228B570 ... -> killed
```

The row decides every candidate:

* `cqe[cq_head].status=0x0` with `phase=1` — **no valid completion at the CQ
  head**, and nothing at the command's own SQ slot either;
* `last_sqhead=1` — the device's own SQ head (from the previous CQE) matches
  ours, `ms_since_done=8` — it was completing normally 8 ms earlier;
* `inside=1` — one driver entrant, so not the L55 two-consumer desync;
* **`cc0=0`** — this kernel had not written CC=0 yet in the whole run;
* `CC=0x460001` (EN=1) with `CSTS=0x0` (RDY=0) on the detection read, and
  `CC=0x0` on the same row's later re-read — the controller was **inside**
  QEMU's `nvme_ctrl_reset()` (which zeroes CSTS, INTMS and INTMC first, drains
  the namespaces, and only then frees the queues and clears CC) and the reset
  **finished two milliseconds later**;
* `CAP`/`VS` read correctly at that instant, so the BAR mapping and
  `cpu_pd7[480]` are not the explanation.

So: **(A), and it is not even "slow" — the device was reset.**  The host trace
agrees: `pci_nvme_pci_reset` (FLR) appears only at boot, and no
`pci_nvme_mmio_stopped` (guest CC write) precedes the wedge; the only doorbell
gap is the one our own resubmission makes.

### 51.2 The death was an aborted transfer, not the reset itself

L55's recovery was written for a *lost completion*: bound the wait, reset,
resubmit.  Against a device that is **mid-reset** that sequence is exactly
wrong, and the log shows it: reinit #1 writes CC=0 *into* QEMU's running reset,
QEMU then finishes the reset and frees the queues we just created, so the admin
`Create SQ` times out (`cid=2`, 2 s), `reinit failed`, and a good read is turned
into `-EIO`.  Reinit #2, with no reset in flight any more, takes **5 ms**.

The read that kills the process is the *retry*: QEMU aborts the requests that
were in flight when it reset, and an aborted AIO can complete with **success and
no DMA**, so `bounce_buf` still holds the previous command's bytes (here, zeros).
The exec gate reads a full 4096 zeros three times (`[TXT] ZERO off=0x757000
reads=3 n0=4096`) — the first from the device, the second and third from the
block cache, which had published the zero frame as valid because the status was
0 — refuses to publish, and the instruction fetch faults in `PROT` and kills
pid 4.  That is why the VA is the same every run: it is the page the aborted
read was for.

### 51.3 The fix

`src/kernel/virtio_blk.c`:

1. **Detect, do not wait.** The wait loop polls `CC.EN`/`CSTS.RDY` once the
   command has been outstanding for 20 ms; either clearing means the controller
   was reset and the cid can never be posted.  No 2 s bound, no blind reset.
2. **Repair without racing the host.** `nvme_reinit()` waits (≤500 ms) for a
   half-reset device to finish, retries the whole sequence up to 4 times, and
   re-checks CC/CSTS 20 ms after a successful enable.
3. **Never trust the first transfer after a repair.** The completion that
   arrives after a recovery is discarded and the same command is issued once
   more (`[BLKQ] re-read after recovery`); only the second transfer is used, so
   an aborted-AIO "success" cannot become data.  At most 3 repair+re-read cycles
   per request, then `-EIO`.

Cost: one extra command per repaired request (two in a run), and no MMIO in the
fast path (`dt > 20 ms` gate).

### 51.4 Run 1's 1 899 MiB was a livelock, not a load

Run 1 of §50 never hit the block wedge at all: `0 [BLKQ] stall`, `0 [BLKSLOW]`,
`0 [TXT] ZERO`, `0 [NFSRD] fail`.  Its rows (`obj/l65-run1.serial.log`):

| t (s) | NFS bytes | `[XSW] n` | live | res MiB |
|---:|---:|---:|---:|---:|
| 421 | 77 809 096 | **260** | 26 | 879 |
| 541 | **277 577 677** (final) | 260 | 26 | 1148 |
| 1041 | 277 577 677 | 260 | 26 | 1476 |
| 1690 | 277 577 677 | 260 | 25 | **1899** |

* presents stop at **260** at t≈420 s, *before* the load finishes, and the
  framebuffer is 8×1 by t≈500 (§50.7) — the same window loss runs 2/3 show at
  491/504 s;
* the load then reaches the deterministic total and stops dead (no NFS read
  after 541 s);
* `res_mib` climbs **linearly at 0.64 MiB/s** for 19 minutes, matched by the
  publish counter: `[PUBS] n` 307 200 → 503 808 (+196 608 first-touch anon pages
  = 768 MiB, one page per 6.1 ms);
* the futex recorder's last-64 window (re-dumped by the idle hang report, up to
  line 145 766) is a **three-thread WAIT/WAKE cycle** — pid 4 waits on
  `0x1083B17624`, 26 wakes it and waits on `0x1100000334`, 4 wakes it and 27
  waits on `0x113000A460`, 26 wakes it … all `to=-1`, `bitset=0xFFFFFFFF`; the
  record index reached 190 182 in 1690 s ≈ **112 futex ops/s**;
* the scheduler's verdict: `[ALIVE-SET] no READY work, yet the scheduler counts
  live work ... so no halt`, 25 live slots — 24 in `PROC_STATE_FUTEX` and pid 4
  `st=3` (RUNNING) at `pc=0x10B8043D72`, a JIT arena.

So run 1 parked in a **post-load frame-handshake livelock with an unbounded
per-iteration allocation**, not on a file, a dead thread, or the block layer.
`1 899 MiB` must not be read as "still loading": the load is over at
277 577 677 bytes / t=541 s.

### 51.5 The fast reproducer

`BLKTEST.BIN` gained an opt-in **P6 device-churn** phase (`/CHURN.ON` must
exist, so the wave is untouched): 4 readers streaming 6 × 256 KiB files in
random 512…4096 B chunks, 3 writers rewriting a random file's first sector each
iteration, one grower appending 256 B × 400 under the readers, parent as a fifth
reader.  [CHURN-RESULT]

### 51.6 Ladder

| run | window | `[BLKQ] stall` | host resets seen | repair | end state |
|---|---:|---:|---:|---:|---|
| instrumented (QEMU trace on) | 1800 s | **0** | 1 (cid 44230) | 5 ms after a 2 s failed repair | killed on a zero page, load complete (277 581 512 B) |
| ladder 1 (fresh cid + drain) | 1800 s | **0** | 1 (cid 44202) | **56 ms**, no admin timeout, no `-EIO` | killed on a zero page, load complete (277 468 532 B) |
| ladder 2 (same build) | 1800 s | **0** | **0** | - | killed on a zero page at t~270 s, load at 50 MB |

The wedge itself is dead: no run produces a `[BLKQ] stall`, the controller is
repaired in tens of milliseconds when the host resets it, and the L55 failure
mode (`create SQ failed` -> `reinit failed` -> `-EIO` on a good read) is gone.

But the **process still dies**, and ladder 2 is the run that separates the two
bugs for good: it died at 50 MB of load with `devoff=0 resets=0 cc0=0` - the
controller was enabled and ready the whole time - on

```text
[FATW] ZERORD sector=656610 off=82944 size=83099
[TXT] ZERO off=0xBC000 reads=3 n0=4096 -> not published
[KERNEL] pid=40 memory fault VA=0x00000010890BE00C ... in=PROT -> killed
```

So the page of zeros is **not** the wedge's aftershock: a healthy controller and
a healthy ring still hand the exec gate a full 4 KiB of zeros for a page that
must hold code (`n0=4096` means FAT16 did not clamp, so this is not an
end-of-file short read).  That is a separate page-in defect, and it is the one
between the loader and the menu.

### 51.7 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **423 run, 0 failed, UNIT TESTS
PASSED**.  `tools/rw/l67-gates.sh`: **redzone, fpu_sig, m2_mono, m2_tp,
m2_threads, m12, mmap_race, m3_window, m4_gl, rwnfs — 10 passed, 0 failed**
(one tier at a time; the unit wave and a desktop boot never share `disk.img`).

## 52. 2026-11-06 — M6/L69: the zero page is a **poisoned block-cache frame**; the gate now asks the device, and a refused page is a SIGBUS, not a protection fault

Full evidence and the refuser inventory: `docs/l69-pagein-report.md`.

§51 separated the page-in defect from the block wedge (ladder 2 died with
`devoff=0 resets=0 cc0=0`) but could not say *which* of four delivery paths
produced the zeros, because every read the exec gate makes goes through the
block cache. This lane built the row that separates them, and the answer is the
cache: **the device holds the page, the cache held a page of zeros, and the
cache was right there in the middle of every "independent" read.**

### 52.1 The refuser inventory

Two gates refuse an executable page; both fired in the instrumented window:

| row | file | off | VA | mapping |
|---|---|---|---:|---|
| `[TXT] ZERO off=0x757000 reads=3 n0=4096` | `/RWDSO/UnityPlayer.so` (33 010 672 B) | `0x757000` | `0x108228B570` | base `0x1082267000`+`0x17A4000`, `prot=5`, `kind=7` (`VMK_LIB`), `MAP_PRIVATE`, `obj_off=0x733000`, obj `VM_OBJ_FILE` |
| `[TXT] IMG SUM pos=0x10000 got=0x76EFDDC5 want=0xDC910CE7` | `/RWLAUNCH.BIN` (246 520 B) | `0x10000` | `0x1000010EA0` | base `0x1000000000`+`0x3D000`, `prot=7`, `kind=1` (`VMK_IMG`), `MAP_PRIVATE` |

### 52.2 The seed row

```text
[BLKQ] device off cid=44153 op=0x2 lba=0x7B6DE cc=0x0 csts=0x0 cc0_writes=0
[BLKQ] controller reset #1 / recovered us=53000 resets=1
[BLKQ] untrusted completion re-read lba=0x7B6DE status=0x0
[BLKC] distrust unit=63195 (reset during fill) -> frame dropped, re-reading
[TXT] PROBE off=0x757000 lba=0x7B6D8 len=4096
       | cache found=1 fstate=0 valid=0xFF c_nz=0 c_sum=0x0
       | raw rc=0 r_nz=3754 r_sum=0x6A3B1 | dropped=8 refill n=4096 f_nz=0
[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published
[EIO] pid=4 page-in failed VA=0x108228B570 -> SIGBUS
```

* LBA `0x7B6DE` is sector 6 of the page at `0x7B6D8` — the **same unit the gate
  was validating**, i.e. the page that dies is the page the aborted transfer
  was for (the §47 thunk page).
* `c_nz=0`, `fstate=0`, `valid=0xFF`: the cache held a **fully valid, all-zero
  frame**.
* `r_nz=3754 r_sum=0x6A3B1` is the file's page (`/srv/nfs/.../UnityPlayer.so`
  at `0x757000`: `nz=3754 sum=0x6A3B1`, head `50 f3 0f 11 44 24 04 e8` =
  `movss %xmm0,0x4(%rsp); call`). The two agree byte-for-byte.

So (a) sparse file, (b) NFS short read and (d) a mis-indexed image sum are all
retired with numbers: the file is code, the read was full-length, and
`want=0xDC910CE7` is the host-computed FNV-1a of `rwlaunch.bin[0x10000:0x11000]`.
The failing delivery is (c): a frame published into the page cache from a
transfer QEMU aborted with status 0 and no DMA, then served to every reader —
including all three of the gate's "independent" reads, which is why they agreed.

### 52.3 The fix, in three layers

1. **Driver.** `blk_dev_epoch` is bumped by every reset performed or detected;
   `virtio_blk_read_raw` re-issues a sector whose completion overlapped a bump
   (fresh cid) and returns `-EIO` rather than stale bytes if it moves again.
2. **Block cache.** A fill compares the epoch across its device I/O; a fill
   that straddled a reset is **unlinked and its frame freed** instead of
   published (`[BLKC] distrust …`), and `nvme_reinit` now flushes the whole
   cache (`[BLKC] post-reset flush dropped=N`) so nothing filled around a reset
   survives. This is the invariant "an aborted DMA never publishes a page, and
   a page published before is invalidated".
3. **Exec gates.** `fat16_read_direct_pos_raw()` reads a page straight from the
   device. `vm_exec_page_verify` and `v2_image_materialize` use it — with two
   raw reads that must agree — whenever their cached answer is zeros, short or
   torn, and publish the device's bytes (dropping the poisoned unit). A page the
   device really holds as zeros, or a page whose bytes do not match the
   load-time sum, is still refused.

`vm_handle_fault` now returns 2 for an `-EIO` page-in and marks an executable
region `VM_MAP_IOERR`; the trap layer delivers **SIGBUS** (`[EIO] … -> SIGBUS`)
instead of SIGSEGV, and every later fault/touch answers the same EIO without
touching the device. SIGSEGV was the wrong signal: Mono installs a handler for
it (null checks), so returning from the handler re-faulted the same page for
ever — the §51 "process died on a protection fault at the same VA".

### 52.4 Tests

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **431 run, 0 failed**. New:
`test_blk_cache_aborted_dma_not_published` (aborted transfer is re-issued; a
persistent abort fails the read; a fill that straddled the event is not
published) and `test_vm_poisoned_cache_page` (installs the exact poisoned frame
over a mapped exec page and asserts the gate publishes the file's bytes and
drops the unit). `test_vm_exec_page_eio` now asserts the EIO/SIGBUS answer, the
sticky region, and that the retry costs **zero** device sectors.

### 52.5 Ladder

| run | window | host resets | final `[TXT] ZERO` / `IMG SUM` | page-fault death | NFS bytes | presents | end state |
|---|---:|---:|---:|---|---:|---:|---|
| f1 | **1800 s** | 0 | **0 / 0** | none | **277 529 012** (264.67 MiB) | 260 | load complete; the §51.4 post-load park (alive, `res_mib` 1 926) |
| f2 | 510 s | 1 | 0 / 1 | none | 221 036 013 | 240 | runtime `g_assert` → SIGABRT (`x86-codegen.h:403`), message lost to a NUL hole `[FATW] ZERORD sector=657286` |
| f3 | 470 s | 6 | 1 / 0 | none | 263 653 604 | 190 | `[TXT] UNVERIFIED off=0x757000 n0=0` (0-byte read, chain/metadata) after reset #1 → EIO → SIGBUS |
| f4 | 460 s | 1 | 0 / 1 | none | 229 367 841 | 230 | `read_fat` reads every FAT sector raw and twice; the only refusal left is the launcher's exit-path image page, where the device itself returns zeros twice |

The windows that matter for this lane's question are f1 (nothing but time on the
clock: 1800 s, the full 277.5 MB load, **zero** refusals of any kind, zero
deaths, 3 584 exec pages verified, 260 presents with changing CRCs) and f3 (six
host resets, and the DSO gate still refuses **nothing**: the one refusal is a
0-byte read whose *chain walk* failed — a metadata victim of a reset, not a
page of zeros, and not the defect this lane was chartered to fix).

With page-in clean the 1800 s window now ends in the known §51.4 park rather
than on a zero page: the load completes, presents freeze at 260, and the main
thread allocates one anonymous page per ~6 ms while no thread is in READY. The
screenshot at t≈480 s shows the game's own window ("RLAUNCH") drawing RimWorld's
loading artwork with an "Initializing." box — the first time the ladder has real
game content on screen; no menu is reached, so none is claimed.


## 53. 2026-11-06 — M6/L72: the park is a **three-party completion handshake that
never terminates**, the 8×1 screen is QEMU's **text fallback after a VGA reset**,
and the page that killed every run was the device **replaying zeros**

Full evidence: `docs/l72-postload-report.md`. This lane instrumented the park,
the allocator and the display mode, and found the wall that was actually
killing the ladder: not the park, but a **device replay at the exec gate**.

### 53.1 The park, named

Three threads, their keys (from the flight recorder; `[FUTEX] rgn` is new and
resolves each key against the process's own regions):

| thread | pid | key(s) | region |
|---|---:|---|---|
| `/NFS/RimWorldLinux` (main) | 4 | `0x1083B17624` | `0x1083AAF000+0x106000` kind=3 |
| `Loading.AsyncRe…` | 26 | `0x1100000334` | `0x1100000000+0x1000000` kind=3 |
| `UnityGfxDeviceW…` | 27 | `0x113000A460`, `0x10E080EBB0` | `0x1130000000+0x1000000`, `0x10E0000000+0x3000000` |

The ring repeats a six-op round (`WAIT 27 → WAKE 26→main → WAIT 4 → WAKE 26`)
at 112 ops/s for 19 minutes, and **every wake lands** (`woke=1 of=<pid>`;
`waits` and `wakes` grow in lockstep). It is a protocol livelock, not a lost
wake: each side completes its half and re-arms, so `LongEventHandler` never
observes “done”. The SIGUSR2 self-sample resolves **zero JIT frames in 2048
words** for all three — they park in native `mono_os_cond_wait`, not in a
managed `WaitHandle`.

### 53.2 The allocation

`[ANONF]` (new: sampled anonymous-fault publisher, with the faulting thread,
instruction and region) names it: `tid=28 UnityGfxDeviceW`, a growing anonymous
arena (`0x11D8000000`, 58 MB and climbing), `rip=0x10800BBE1F` =
`movntdq %xmm3,0x30(%rdi)`, glibc **`memcpy`**'s non-temporal store loop (other
samples: `__bzero`, `memset`) — **one 4 KiB managed buffer allocated and
copied per handshake round** (171 pages/s = 5.85 ms/page in L65 run 1). The driver is visible in the
main thread's managed stack at t=380 s: `<GetAllInFolder>d__2.MoveNext →
Resources.LoadAll → ResourcesAPI.LoadAll → ResourcesAPIInternal.LoadAll →
String.wstrcpy`, i.e. RimWorld's `InitializingInterface` walking folders and
allocating a path/asset buffer per iteration.

### 53.3 The 8×1 screen

`[BGA]` (new, 5 s, change-triggered) reads the Bochs dispi registers and the
two CRTC fields QEMU's text path uses:

```text
[BGA] xres=1920 yres=1080 bpp=32 en=0x0x41 hdisp=239 vdisp=55 -> mode ok
```

One row per boot, **no change** in three runs: the guest's mode never left
1920×1080×32. QEMU computes 8×1 only when VBE is off and the CRTC is zeroed —
`(cr[1]+1)*8 × (cr[0x12]+1)` — i.e. a VGA device reset with no mode set since,
which is a *guest reboot* in this tree (the only 8×1 PPMs are the shots after a
second `BdsDxe` in L65's rw-desktop log). Not the WM (1916×1018 content rect),
not Unity minimising. `virtio_gpu_health_tick()` now restores the mode after a
reset.

### 53.4 What was killing every run: the device replay

Three runs died at **exactly `[PUBS]=307200`**, the moment the game first
executes `UnityPlayer.so`'s text page at off `0x757000`/`0x9AD000`:

```text
[TXT] PROBE off=0x0x757000 … lba=0x0x7B858 | cache c_nz=0 valid=0xFF
       | raw rc=0 r_nz=3754 r_sum=0x6A3B1 | dropped=8 refill n=4096 f_nz=0
[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published
[EIO] … -> SIGBUS
```

The host's disk at that LBA holds **3754 non-zero bytes** — read 1 is the
file. Read 2 and later return 4096 zeros. L69's gate requires two *agreeing*
raw reads, so it refused a page the device had answered correctly and the
process died with SIGBUS (and in the two runs where it did not die that way,
the same point produced Mono's `x86-codegen.h:403` assert and a libmono NULL
deref — one wall, three shapes). The gate now publishes the probe's first read
when the second agrees **or is the all-zero replay** (`[TXT] PROBEGET`,
`[TXT] REPLAY`); different non-zero content is still a torn page. Back-off is
2/10/30/60 ms, on the controller-repair timescale.

### 53.5 Ladder

| run | window | presents | `[PUBS]` | res MiB | end |
|---|---:|---:|---:|---:|---|
| L65 run 1 | 1800 s | 260 | 503 808 | 1899 | park (112 ops/s, 19 min) |
| diag | 780 s | — | — | 1133 | `[TXT] ZERO 0x9AD000` → SIGBUS, t≈135 s |
| diag2 | 780 s | — | 307 200 | — | Mono `g_assert` (`:403`) |
| diag3 (ud2 probe) | 900 s | — | 307 200 | — | libmono+0x1EB680 NULL deref |
| pdz | 1200 s | — | 307 200 | — | `[TXT] ZERO 0x757000` → SIGBUS |
| final1 (replay rule) | 1200 s | 1 | 176 128 | **627 flat** | alive; no park growth, no assert, no death; still in the def pass |

So the **page-in wall is closed** (RSS bounded at 627 MiB for the whole
1200 s window, no 0.64 MiB/s climb) and the handshake wall remains, now at the
def-plateau (`[XO] …RangedSpacer.xml`, 43.6 MiB NFS) with main ↔
`UnityGfxDeviceW` and `Loading.AsyncRead` idle. No menu, so none is claimed.

### 53.6 Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **431 run, 0 failed**. Gates: see
`docs/l72-postload-report.md` §7.
