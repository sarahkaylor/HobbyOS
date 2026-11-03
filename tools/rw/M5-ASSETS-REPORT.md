# M5 assets — RimWorld over NFS (lane L6) — measured report

Date: 2026-11-02. Worktree: `/home/sarah/hobbyos-rw-lanes/l6-assets`, branch
`rw-l6-assets`. Every line below is copied from a real run on this host/guest;
the commands are in `tools/rw/` and Appendix A.

**Verdict in one line:** the real 729.4 MiB RimWorld tree mounts read-only over
NFSv3/UDP in a HobbyOS guest through QEMU slirp, lists with its real long names,
and a 64 KiB read of `UnityPlayer.so` is byte-identical to the host; exec of a
native ELF *from* NFS now works through a VFS image-read path (patch included);
`mmap` of an NFS file does **not** work yet (`-EOPNOTSUPP`), and FAT16 LFN
open-by-long-name **already works** (F3: no 8.3 alias map needed).

---

## 1. Export (host side)

`tools/rw/nfs-export.sh sync` — copy outside the repo, read-only export,
verification mount, guest mount line.

```
== copy: /mnt/fast/games/steamapps/common/RimWorld -> /srv/nfs/rimworld
   rsync wall=0.55s  files=1581  bytes=764826083 (729.4 MiB)
   729 MiB would NOT fit the 1 GiB FAT16 image (627 MiB free) — that is why M5 is NFS.
   modes: chmod -R a+rX (guest presents uid 0, export root_squashes to nobody)
== export: /srv/nfs/rimworld (ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)
   /etc/exports already lists /srv/nfs/rimworld
   exportfs -ra done
== verify: showmount -e 127.0.0.1
   Export list for 127.0.0.1:
   /srv/nfs/rimworld *
   /srv/nfs/export   *
== verify: mount -t nfs -o ro,vers=3,proto=udp,nolock 127.0.0.1:/srv/nfs/rimworld /tmp/...
mount.nfs: an incorrect mount option was specified ...
   (host kernel refused NFSv3/UDP client — retrying over TCP; the guest uses UDP)
   mounted; root entries=19
   ...
   present: RimWorldLinux_Data
   present: start_RimWorld.sh
   present: UnityPlayer.so
   host sha256(head -c 65536 UnityPlayer.so) = 176614408f117bf6abdf9f962cfb701cdba9d259604bd7fea7f8c63b86d3e125
   unmounted

== guest mount line (HobbyOS shell / probe constant):
   mount "10.0.2.2:/srv/nfs/rimworld" "/NFS"
nfs-export: OK
```

Notes that matter:

* **The tree is 729.4 MiB, not 436 MB.** `du -sh` on the source NFS4 mount says
  436M (server-side allocation/compression); `find -printf %s | sum` says
  764,826,083 bytes over 1581 files, and rsync reproduces exactly that. The
  "assets do not fit the image" argument is stronger than the plan states.
* The copy is 0.55 s warm / 4.13 s cold. `rsync -a` is enough (source has no
  hardlinks: `find -links +1` = 0).
* `rsync -a` copies the source root's `0770` mode; the guest's AUTH_UNIX
  credential is `uid=0,gid=0` (`src/kernel/nfs_proto.c:174-181`) and the export keeps root_squash, so the script does
  `chmod -R a+rX` on the copy. Without it the guest gets EACCES on the export
  root (measured).
* Host kernels ≥ 6.5 dropped the NFSv3/UDP **client**, so the host-side check
  falls back to TCP; the server still serves UDP and the guest uses UDP. The
  guest probe is the real UDP test.

### Fixed mountd port (optional, verified)

`RW_FIXED_MOUNTD=1 tools/rw/nfs-export.sh export-only` writes
`/etc/nfs.conf.d/90-hobbyos-l6-mountd.conf` (`[mountd] port=20048`) and
restarts `nfs-mountd`. Measured:

```
100005    1   udp  20048  mountd
100005    1   tcp  20048  mountd
100005    2   udp  20048  mountd
100005    2   tcp  20048  mountd
100005    3   udp  20048  mountd
100005    3   tcp  20048  mountd
```

**The fixed port is not required for slirp**: the guest's portmapper GETPORT ->
MOUNT v3 handshake works against the distro's random mountd port (first two
probe runs, `rc=0`). It is provided because the plan asked for it and because a
host whose rpcbind is unreachable would need it.

---

## 2. Guest evidence (serial log, `obj/rwnfs_probe/serial.log`)

Probe: `src/user/rwnfs_probe.c`, run by `tools/rw/run-rwnfs-probe.sh`
(T3 boot, single-wave member, `T3_MIN_LINES=1`).

```
[RW] rwnfs_probe start
[RW] PASS FAT16 open+read by long mixed-case name '/RimWorldLinux_Data'
[RW] PASS FAT16 open by upper-cased long name '/RIMWORLDLINUX_DATA'
[RW] PASS FAT16 read_dir("/") shows the long name RimWorldLinux_Data
[RW] net ip=251789322
[RW] PASS network came up (DHCP)
[RW] MOUNT source=10.0.2.2:/srv/nfs/rimworld target=/NFS rc=0
[RW] PASS mount the RimWorld export
[RW] MOUNTS n=1
[RW]   point=/NFS source=10.0.2.2:/srv/nfs/rimworld type=1
[RW] PASS sysinfo(8) mount table shows /NFS as an NFS mount
[RW] LIST /NFS
[RW]   Mods size=4096 dir
[RW]   Data size=4096 dir
[RW]   Licenses.txt size=12751
[RW]   Source size=4096 dir
[RW]   RimWorldLinux size=14720
[RW]   Notes for Linux Users.txt size=804
[RW]   start_RimWorld.sh size=844
[RW]   Readme.txt size=3715
[RW]   hdyn.bin size=7808
[RW]   ModUpdating.txt size=17464
[RW]   UnityPlayer.so size=33010672
[RW]   SteamInputDefaultConfiguration_ size=9032
[RW]   RimWorldLinux_Data size=4096 dir
[RW]   SteamInputDefaultConfiguration. size=306
[RW]   ScenarioPreview.jpg size=47519
[RW]   EULA.txt size=23297
[RW]   steam_appid.txt size=6
[RW]   start_RimWorld_openglfix.sh size=3714
[RW]   SteamInputDefaultConfiguration_ size=13622
[RW]   Version.txt size=17
[RW] PASS listing shows RimWorldLinux_Data (directory)
[RW] PASS listing shows start_RimWorld.sh
[RW] PASS listing shows UnityPlayer.so with the host size 33010672
[RW] SIZE /NFS/UnityPlayer.so bytes=33010672
[RW] PASS UnityPlayer.so size matches the host
[RW] SUM /NFS/UnityPlayer.so bytes=65536 fnv1a32=0x141d2a86 sum32=0x00213361 first8=7f454c4602010100 last8=cca71a0000000000
[RW] PASS 64 KiB read of UnityPlayer.so completed
[RW] DIRCOUNT /NFS/RimWorldLinux_Data/Managed n=102 expected=102
[RW] PASS Managed lists all 102 entries (listing cache bound)
[RW] NESTED Assembly-CSharp.dll size=15767552
[RW] PASS nested open of Managed/Assembly-CSharp.dll
[RW] LONGNAME size=9728 path=/NFS/RimWorldLinux_Data/Managed/UnityEngine.RuntimeInitializeOnLoadManagerInitializerModule.dll
[RW] PASS open a 63-char path component over NFS (LOOKUP cap)
[RW] MMAP /NFS/UnityPlayer.so -> -1 errno=95
[RW] FAIL mmap a page of an NFS file (DSO page-in path)
[RW] EXEC spawn_ex("/NFS/hdyn.bin") -> 3
[RW] EXEC waitpid=3 status=0
[RW] PASS exec a native ELF from NFS (child exit 0)
[RW] done failures=1
```

Host comparison for the checksum (same file, same 64 KiB):

```
host bytes=65536 fnv1a32=0x141d2a86 sum32=0x00213361 first8=7f454c4602010100 last8=cca71a0000000000
```

The exec child's own console markers, from the same log:

```
Loading program for scheduler (v2 AS): /NFS/hdyn.bin
v2 loader: PID=3 entry=0x1008005110 sp=0x1800000000 resident=34 tables=8 abi=0x0 native=1 interp=0x1008000000
HDYN: entered (dynamic)
HDYN: libc.so symbols resolved
HDYN: done
[RW] EXEC waitpid=3 status=0
```

`HDYN.BIN` is the tree's own native dynamic pilot (ET_DYN, `PT_INTERP
/LDHOBBY.SO`, `DT_NEEDED LIBC.SO`), copied to the export as `hdyn.bin` and
spawned from `/NFS`. `interp=0x1008000000` is the interpreter mapped by the
loader; `native=1` is the native track. The binary bytes, the interpreter's
search and the child's libc calls all went through NFS/FAT exactly as they do
from the boot volume.

Remaining failure: `mmap` (section 4).

---

## 3. FAT16 long-name check — F3 verdict: **LFN works, no alias map needed**

Evidence, same serial log:

* `open("/RimWorldLinux_Data")` (mixed case, 19 chars, not 8.3) → read returned
  exactly the staged 31-byte body (`[RW] PASS ...`), i.e. the VFAT long-name
  record was matched — not an 8.3 truncation.
* `open("/RIMWORLDLINUX_DATA")` → also succeeds (case-insensitive LFN compare,
  `fat16.c:139-155`).
* `read_dir("/")` shows the long name (`[RW] PASS ...`), i.e. `ls` is faithful
  for names up to the dirent cap.

Fixture construction: `mcopy -o -i disk.img <file> ::/RimWorldLinux_Data` — mtools
writes a real VFAT LFN run + short alias, exactly like Windows/production tools.
The driver's LFN path was already exercised by `src/user/lfn_test.c`; this run
confirms it on the disk image the game would use.

**So F3 can be settled for reads: the 8.3 alias map is unnecessary.** What is
still capped is the *dirent name field*: `struct sys_dirent.name[32]`
(`src/user_include/libc.h:180`) truncates 114 names in this tree (longest:
`UnityEngine.RuntimeInitializeLoadManagerInitializerModule.dll`, 63 chars) to
31 characters in `read_dir` output. Opening them by full path works (see
`[RW] LONGNAME ... size=9728`); only *display* is truncated. Changing that is a
user-ABI change (`struct sys_dirent`), so it is reported, not done here.

---

## 4. Exec and mmap from NFS — exact answer

### 4.1 What blocked exec (before this lane's change)

The exec path never used the VFS:

| Path | Blocking site | Behaviour |
|---|---|---|
| v2 spawn/exec loader (`load_v2_internal`) | `src/kernel/program_loader.c:895` — `fat16_open(filename, &f)` | an NFS path missed, `fat16_open` **created a 0-byte FAT entry** and returned "Failed to open file", so `spawn_ex("/NFS/...")` returned `-1` |
| v1 flat loader (`load_and_run_program_in_scheduler_args`) | `src/kernel/program_loader.c:288` | same FAT-only open; flat `.bin` only |
| POSIX `execv`/`SYS_EXEC` (`process_exec_current`) | `src/kernel/program_loader.c:1245` | `fat16_open(abs)` + `v2_map_image()` (flat image keyed by start cluster) — cannot load an ELF at all |
| ELF mapper | `src/kernel/elf_loader.c:539` `elf_loader_load(as, uint16_t cluster, ...)` | takes a FAT16 **cluster**, not a path or buffer |

### 4.2 What was implemented (tested, works)

* `src/kernel/fs.c` — new `fs_read_image(path, &buf, &size, &pages)` (fs.c:2017):
  routes the path exactly like `open()` (`vfs_abs_path` + `vfs_route` +
  `vfs_open_routed`), returns 1 for an NFS path with the whole image in
  contiguous frames, 0 for a FAT16 path (legacy loader untouched), <0 on error.
* `src/kernel/elf_loader.c` — `elf_loader_load()` split into
  `elf_loader_load_image(as, buf, len, flags, info)` (elf_loader.c:534) plus a
  thin FAT16 wrapper that keeps the old signature and behaviour; the PT_INTERP
  image is still resolved from the FAT16 root (that is where `/LDHOBBY.SO`
  lives). `elf_loader_probe_native_buf()` is the buffer form of the track probe.
* `src/kernel/program_loader.c` — `load_v2_internal` reads through
  `fs_read_image()` and hands the buffer to `elf_loader_load_image()`. Flat
  `.bin` images from NFS are refused with an explicit line (their lazy mapping
  is keyed by FAT cluster identity).

Proof: `spawn_ex("/NFS/hdyn.bin")` -> pid 3, exit 0, `HDYN: done` on the
console (section 2). Before the change the same call returned -1.

No kernel regression from the loader refactor: `TOOLS_SKIP=1
./run_unit_tests_intel.sh` -> `Tests run: 397 / Tests failed: 0 / UNIT TESTS
PASSED` (that mode re-runs `elf_loader_test.c` against `elf_loader_load`).

**Ownership caveat:** `elf_loader.c`, `program_loader.c` and
`src/include/program_loader.h` are L2-owned (§7.4). The change is committed on
this lane branch as its own commit **and** exported as
`patches/L6-exec-from-nfs.patch` (315 lines) so L2/integrator can take it,
rewrite it, or drop it if L2's own loader work lands first. `fs.c`/`fs.h`/
`nfs.c`/`nfs.h` are this lane's files.

### 4.3 What still does not work: `mmap` a DSO from NFS

`[RW] MMAP /NFS/UnityPlayer.so -> -1 errno=95` (`EOPNOTSUPP`).

Exact site: `src/kernel/fs.c:2076` `file_fat16_ident()` returns `-ENOTSUP` for
every non-FAT16 fd, and `src/kernel/vm.c:1663` uses it to key the file-backed
`VM_OBJ_FILE` page cache by `(start_cluster, file_size)`. There is no object
type that can page an NFS file in on fault.

That is **not** a small change: it needs a `VM_OBJ_FILE` backend that holds
`(mount_idx, file handle, size)` and calls `nfs_read_file()` from the fault
path, plus cache invalidation and teardown. It is the one M5 bullet that needs
L2/VM design work rather than an L6 patch. The game's `ld.so` will hit it: every
DSO `mmap` of `UnityPlayer.so`/`libX11.so.6` would fail until this exists.

For contrast, `exec` does not need `mmap`: the ELF loader copies the image into
anonymous frames (section 4.2), which is why exec-from-NFS works today.

---

## 5. NFS-over-slirp: possible, no fallback needed

Task 5's precondition did not occur. QEMU slirp forwards guest UDP to
`10.0.2.2` -> host loopback, so:

* portmapper GETPORT (mountd, NFS) succeeded — MOUNT v3 returned a file handle;
* the mount table shows `10.0.2.2:/srv/nfs/rimworld` on `/NFS`;
* READDIRPLUS, LOOKUP (including a 63-byte component), READ (64 KiB) and FSSTAT
  all work.

Both the distro's random mountd port and the pinned 20048 were exercised; both
work. No tap/bridge or hostfwd alternative is needed.

---

## 6. Defects this run found (and fixed) in the NFS client

1. **Directory listings silently stopped at 64 entries.**
   `NFS_DIR_CACHE_MAX` was 64 and `nfs_list_dir()` returns -1 for any index past
   the cache — indistinguishable from end-of-directory. `RimWorldLinux_Data/Managed`
   has 102 entries, `Data/Core/Defs` 93, `Data/Biotech/Defs` 67. Raised to 256
   (`src/include/nfs.h`); `[RW] DIRCOUNT ... n=102` is the proof.
2. **LOOKUP truncated path components to 31 bytes.**
   `nfs_lookup()` used `char comp[NFS_NAME_MAX]` (32) and advanced `p += len`,
   resuming mid-component, so every component >31 chars failed. 114 names in
   this tree are longer than 31 (max 63). New `NFS_COMP_MAX 256`
   (`src/include/nfs.h`, `src/kernel/nfs.c:466-479`); `[RW] LONGNAME ... size=9728`
   is the proof.
3. **Read-only export + root_squash needs a world-readable tree** (see §1) —
   handled by the export script's `chmod -R a+rX`, not a kernel issue.

Open (reported, not fixed): `struct sys_dirent.name[32]` truncates 114 names in
listings; `mmap` from NFS (§4.3); `execv`/`SYS_EXEC` remains a flat-image path
(§4.1) so a launcher must use `spawn_ex` for ELF images.

---

## 7. Files

| File | State |
|---|---|
| `tools/rw/nfs-export.sh` | new — host export/copy/verify, fixed-port option |
| `tools/rw/run-rwnfs-probe.sh` | new — builds and runs the probe, grades the log |
| `src/user/rwnfs_probe.c` | new — the guest probe (LFN, mount, list, 64 KiB checksum, LOOKUP, mmap, exec) |
| `src/include/nfs.h`, `src/kernel/nfs.c` | cache 64->256, LOOKUP component 31->255 |
| `src/kernel/fs.c`, `src/include/fs.h` | `fs_read_image()` (VFS image read) |
| `src/kernel/elf_loader.c`, `src/kernel/program_loader.c`, `src/include/program_loader.h` | `elf_loader_load_image()` + VFS exec (L2-owned; see §4.2) |
| `patches/L6-exec-from-nfs.patch` | the L2-owned part, as a standalone patch |

## Appendix A — reproduce

```sh
# host, once (needs passwordless sudo for exportfs/mount)
bash tools/rw/nfs-export.sh sync
# optional: pin mountd
RW_FIXED_MOUNTD=1 bash tools/rw/nfs-export.sh export-only
# guest probe (boots QEMU, stages the probe + LFN fixture, grades the log)
HOME=/home/sarah bash tools/rw/run-rwnfs-probe.sh
#   log:  obj/rwnfs_probe/serial.log
#   gate: obj/rwnfs_probe/tier.log, run_final*.log
```
