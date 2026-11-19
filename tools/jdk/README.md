# Running OpenJDK on HobbyOS

The x86_64 build of HobbyOS carries a Linux-ABI compatibility layer that runs
real glibc binaries.  This directory is the lane that proves it can run a full
JDK, and the acceptance gate for that claim.

Measured with **Adoptium OpenJDK 27 (27.0.1.1+4), linux/x64 hotspot**, glibc
**2.31** (the pin the compat layer was proven against; the JDK itself asks for
no more than GLIBC_2.17).  27.0.1.1 -- not 27.0.1 -- because of
CVE-2026-114938, the AVX-512 VBMI Base64 decoder overflow; `docs/java.md`
("The pin is a security decision") carries the evidence, and
`tools/jdk/fetch-jdk.sh` reproduces the pin from its published SHA-256.

```
$ tools/jdk/sdktest.sh --no-build         # 4 boots, ~15 min under TCG
   version  PASS   openjdk version "27.0.1.1" 2026-11-17
   javac    PASS   Hello.class written; javac exited
   run      PASS   HELLO-JAVA: java.version=27.0.1.1
   suite    PASS   SUMMARY pass=18 fail=0 skip=0
   all checks passed
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
| `nettest.sh` | java.net **TCP over the NIC**: starts an echo server on the host (the guest reaches it at `10.0.2.2` through slirp), runs `TcpProbe`, grades 6 checks. `--no-build`, `--stage-jdk`, `PORT=` |
| `fetch-jdk.sh` | reproduces the pinned runtime: Temurin **27.0.1.1+4** (SHA-256 checked before unpack) -> `third_party/jdk-27`, plus `--glibc` for the 2.31 pin `stage.sh` needs |
| `stage.sh` | rebuilds `disk.img`: `--jdk DIR`, `--dir SRC:/DEST`, `--probe SRC` (8.3 name from the source basename, so probe sources must be hyphen-free), `--wave "CMD ARG..."` |
| `cycle.sh` | build + repack + boot in one step. **Use this, not hand-run make+boot** -- `boot.sh` loads `hobbyos.elf` from *inside* `disk.img`, so a kernel you did not repack is a kernel you did not test |
| `boot.sh` | boot and wait for a marker. `--timeout`, `--log`, `QEMU_DEBUG=`/`QEMU_DEBUG_LOG=` |

`tools/jdk/tests/Verify.java` is the suite: 18 checks over class loading,
reflection, jimage reads, threads, monitors, NIO files/directories, sockets,
process exec, GC, compression and crypto.  It is one JVM so a single boot can
report all of them.

`tools/jdk/tests/Base64Probe.java` is the CVE-2026-114938 regression probe: it
feeds the decoder the published trigger shapes (65-byte padded tails and
friends), asserts `IllegalArgumentException`, then asserts that well-formed
padded groups still decode and that objects allocated before the burst are
byte-identical after it.  Stage it as its own boot:

```bash
tools/jdk/stage.sh --dir obj/jdk/classes:/C --wave "JDK/bin/java -cp /C Base64Probe"
tools/jdk/boot.sh --timeout 420 --log obj/jdk/base64.log
```

`tools/jdk/tests/TcpProbe.java` (+ `nettest.sh`) is the wire test: the suite's
`sockets-loopback` only covers the in-kernel `127.0.0.1` pair, so this dials a
host-side echo server at slirp's `10.0.2.2` and round-trips real segments
(connect, 22 B line, 8192 B, reconnect, and a refused port).  It found two ABI
bugs on its first run -- `TCP_NODELAY` rejected with `ENOPROTOOPT`, and
`getsockname`/`getpeername` reporting `127.0.0.1` for a NIC connection -- both
fixed in `fs.c`.  See `docs/java.md` for the table.

```bash
tools/jdk/nettest.sh --no-build
```

## What the port actually needed from the kernel

Each of these was a real failure with a real log, not a speculative patch.

| Kernel change | Symptom it fixed |
|---|---|
| `fs.c`: `file_socket_setopt` accepts IPPROTO_TCP options (NODELAY/MAXSEG/KEEP*) as no-ops, and `getopt` answers them | every `java.net.Socket` threw `SocketException: Protocol not available`: NioSocketImpl sets `TCP_NODELAY`, which the ABI layer answered with `ENOPROTOOPT` (`tools/jdk/nettest.sh`) |
| `fs.c`: `file_sockname` answers the pcb's real local/peer address (`ntohl`, `net_get_ip()` fallback) instead of a hardcoded `127.0.0.1` | `Socket.getLocalAddress()`/`getInetAddress()` reported `127.0.0.1` for a connection to `10.0.2.2` -- the value TLS hostname checks read |
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
