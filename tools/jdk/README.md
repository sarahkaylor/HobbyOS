# Running OpenJDK on HobbyOS

The x86_64 build of HobbyOS carries a Linux-ABI compatibility layer that runs
real glibc binaries.  This directory is the lane that proves it can run a full
JDK, and the acceptance gate for that claim.

Measured with **Adoptium OpenJDK 27 (27.0.1.0.2+1), linux/x64 hotspot**, glibc
**2.31** (the pin the compat layer was proven against; the JDK itself asks for
no more than GLIBC_2.17).

```
$ tools/jdk/sdktest.sh                     # build + 4 boots, ~15 min under TCG
PASS version              openjdk version "27.0.1.0.2" 2026-10-21
PASS javac                /T/Hello.java -> /JDKOUT/Hello.class (1528 bytes)
PASS run                  java -cp /JDKOUT Hello prints 27.0.1.0.2 / Linux / amd64
PASS suite                SUMMARY pass=18 fail=0 skip=0, "System halt"
```

## Why each check is a separate boot

`ARCH=intel MODE=test` runs the programs listed in `/WAVE.CFG` **concurrently**,
and the guest console is the only witness.  A check therefore has to be one
program, one boot, one log, with a real deadline: `boot.sh` escalates TERM to
KILL because a spinning TCG guest ignores TERM, and a wave that hangs must fail
on its own deadline instead of poisoning the next check.  `/JDKOUT` is
deliberately *not* reset between checks -- `javac` writes the class in one boot
and `java` reads it in the next, which is also a durability test of the FAT
write path.

Staging and booting must never overlap.  The guest reading a half-written FAT
image looks exactly like an infinitely slow program.

## Tools

| Script | Does |
|---|---|
| `sdktest.sh` | the gate: version, javac, run, suite. `--no-build`, `--only=name` |
| `build.sh` | downloads/pins JDK 27 + glibc 2.31 under `third_party/` |
| `stage.sh` | rebuilds `disk.img`: `--jdk DIR`, `--dir SRC:/DEST`, `--probe SRC` (8.3 name from the source basename, so probe sources must be hyphen-free), `--wave "CMD ARG..."` |
| `cycle.sh` | build + repack + boot in one step. **Use this, not hand-run make+boot** -- `boot.sh` loads `hobbyos.elf` from *inside* `disk.img`, so a kernel you did not repack is a kernel you did not test |
| `boot.sh` | boot and wait for a marker. `--timeout`, `--log`, `QEMU_DEBUG=`/`QEMU_DEBUG_LOG=` |

`tools/jdk/tests/Verify.java` is the suite: 18 checks over class loading,
reflection, jimage reads, threads, monitors, NIO files/directories, sockets,
process exec, GC, compression and crypto.  It is one JVM so a single boot can
report all of them.

## What the port actually needed from the kernel

Each of these was a real failure with a real log, not a speculative patch.

| Kernel change | Symptom it fixed |
|---|---|
| `vm.h`: `VM_OBJ_MAX_BYTES` 64 MiB -> 4 GiB | `lib/modules` is a 140 MiB jimage mapped in one call; the JVM also reserves MaxHeapSize with one `MAP_NORESERVE` mmap. ENOMEM at VA 0x12E0C |
| `fs.c`: `file_read_user()` bounce + `vm_kwrite` | a 4 MiB `pread` into a fresh `malloc` faulted **in kernel mode** (`[BADVEC] not-a-frame`) because `file_read` wrote through an unmapped user VA. Destinations are now copied through a 32 KiB per-CPU buffer |
| `fs.c`: `file_pread_user`/`file_pwrite_user` | `pread` was seek-read-restore over a shared cursor; concurrent class loading read each other's offsets -> `ClassFormatError: Unknown constant tag 47` |
| `linux_compat_misc.c`: `getcwd` returns `len+1` | `java.lang.Error: Properties init: Could not determine current working directory` on every JVM start |
| `fs.c`: plain `open(dir, O_RDONLY)` succeeds | every `Files.list()` threw `NoSuchFileException`, even for `/` |
| `fat16.c`: getdents dot-filter | `.`/`..` were emitted twice (implicit pair + on-disk pair): `/JDK` listed `. .. . .. BIN CONF` |
| `trap.c`: `exit_group(231)` -> `SYS_EXIT_GROUP` | `System.exit()`/`Runtime.halt()` hung with 13 parked threads; 231 was remapped onto `SYS_EXIT`, which only ends a clone(2) child, so the JVM's group never died |
| `net.c`/`fs.c`: loopback stream sockets | `new Socket()` -> `SocketException: Operation not supported`: no AF_INET endpoint existed |
| `trap.c`: `badvec_frame_window()` per-CPU stacks | the window tested `p < 0x70000000` while the image *links at* 0x70000000 and stacks live at 0x75Cxxxxx, so every real frame was rejected |
| `lockscale.c`: `lock_audit_site_override()` | `[BLOCKHOLD]` blamed `spinlock_acquire` itself instead of the line that took the lock |

## Loopback sockets: what is and is not modelled

Two ends that are both this kernel need a transport, not a TCP stack.  A
connection to `127.0.0.1` pairs the client pcb with the listener's accepted pcb
and reuses the rx ring the wire path already has: `send()` pushes into the
peer's ring with backpressure, `close()` marks the peer `SOCKET_CLOSED` so a
parked reader wakes and returns 0 (EOF), and a loopback reader blocks without
the 5 s wire timeout.

Deliberately absent: segment boundaries, retransmission, windows, half-close,
TIME_WAIT.  None are observable on loopback, and stubbing them would only add
states nothing exercises.  `bind()` to any address other than 127.0.0.1/0.0.0.0
is refused, because there is no NIC-bound listener to back it.

## Known limits

- **`tools/t3_tier.sh intel` is RED on this host, with and without this work.**
  The baseline kernel wedges the same way: `[IDLESTUCK]` with
  `lock_addr=0x748D8148` (`proc_lock`) in the default wave, and
  `[YLOCK-WEDGE] fat_wr_lock` with a writer parked in `SYS_WRITE`.  That is a
  pre-existing SMP lock bookkeeping bug on TCG, not a JDK regression -- which
  is why this lane runs one JVM per boot instead of the wave.
- `sockets-loopback` proves the stream socket contract, not a network stack.
  Real TCP (non-loopback connect, `select()` over many sockets) is untouched.
- The guest JVM runs with a 3 GiB max heap and CDS enabled; `-Xshare:off`
  works but exercises more concurrent class loading, which is how the `pread`
  race was found.
