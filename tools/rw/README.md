# tools/rw — the RimWorld harness

Everything here runs RimWorld (Unity 2022.3 + Mono) under HobbyOS's Linux-ABI
compat class.  The game tree is never copied into the repository: the host
serves it over NFSv3/UDP and the guest mounts it at `/NFS`.

## One NFS export per worktree (M6/L63)

**Every worktree gets its own export directory.**  It used to be one shared
`/srv/nfs/rimworld`, and every harness installs its own AD-9 launcher as
`<export>/RimWorldLinux`; two lanes booting at the same time therefore raced
on that one path, and a run could start with the *other* lane's binary
(measured: a 96,432-byte build with a switch the lane was not testing).

The identity is computed by `tools/rw/lane.sh`, which every harness sources:

| variable    | default                     | meaning                          |
|-------------|-----------------------------|----------------------------------|
| `RW_LANE`   | the worktree's dirname      | filesystem-safe lane name        |
| `RW_EXPORT` | `/srv/nfs/rimworld-<lane>`  | the directory this lane owns (output) |
| `RW_EXPORT_OVERRIDE` | unset              | use a different directory        |

The guest is told which directory to mount by a marker file: the harness
writes `RW_EXPORT` to `/NFSEXPORT` on `disk.img`, and `rwlaunch` (plus the
`rwnfs_probe` / `rwnfs_bench` probes) reads it through
`src/user_include/rwnfs_lane.h`.  The serial log shows the result:

```text
[RW] rwlaunch: export /srv/nfs/rimworld-l64-zeropage mount 10.0.2.2:/srv/nfs/rimworld-l64-zeropage -> /NFS rc=0
```

Nothing else writes into that directory, so:
- a lane may install/overwrite `RimWorldLinux`, `RimWorldLinux.orig` and its
  own probe binaries without affecting any other lane;
- the `.l61` private-alias workaround is gone — `/NFS/RimWorldLinux` is
  already this lane's binary.

`tools/rw/nfs-export.sh` refuses the legacy shared path unless
`RW_ALLOW_SHARED_EXPORT=1` is set deliberately.  To use a second export by
hand (the isolation proof does), set `RW_LANE` or `RW_EXPORT`:

```bash
RW_LANE=other tools/rw/nfs-export.sh sync     # /srv/nfs/rimworld-other
RW_EXPORT_OVERRIDE=/srv/nfs/rimworld-scratch tools/rw/nfs-export.sh sync
```

Each export is an independent `rsync -a --delete` copy of the Steam tree
(~265 MiB), so `sync` is cheap after the first one and no two lanes can share
an inode.

## Running

```bash
export HOME=/home/sarah
tools/rw/nfs-export.sh sync                      # this worktree's export
tools/rw/run-rimworld-desktop.sh --run-timeout 900
```

Never `pkill -f qemu-system` (other lanes' guests die with it); the harness
kills its own QEMU by disk image (`fuser -k disk.img`).
