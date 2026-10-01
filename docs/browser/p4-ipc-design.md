# P4 design note — IPC primitives (P4.1–P4.4: AF_UNIX sockets & socketpair, SCM_RIGHTS fd passing, poll)

Status: **design for integrator review; no code in this commit.** Milestone: browser.md §6 P4 (lane L2).
Base: **07b6c00** (main; Wave-1d battery tip after Gate P3). Consumers: the P4 implementer (kernel/L2),
L3 (`src/libc/**` + the new sysroot headers), L9 (`IPC_T.BIN`, the SOCK2TST edit), the integrator (freeze
rows + `SYS_MAX`), and P5/P7/L8, which build on the fd/IPC surface. Grounded in:
`src/kernel/{pipe.c,fs.c,net.c,process.c,program_loader.c,arch/{arm,x64}/trap.c}`,
`src/include/{fs.h,pipe.h,net.h,process.h,syscall.h,errno.h}`, `src/user/{libc.c,sock2_test.c,poll_test.c}`,
`src/user_include/libc.h` — plus a source cross-check against the pinned WebKit tree `~/webkit-hobbyos`
(WPE 2.54.0; fork commit `5220e80b97…`; `Source/WebKit/Platform/IPC/{unix,glib}`,
`Source/WebKit/UIProcess/Launcher/glib/ProcessLauncherGLib.cpp`, `Source/WTF/wtf/unix/UniStdExtrasUnix.cpp`)
and the L6-vendored GLib 2.88.3 (`third_party/glib-2.88.3`; `gio/gsocket.c`, `gio/giounix-private.c`,
`gio/gunixfdmessage.c`, `glib/gmain.c`).

## 0. Constraints carried into the design

- Kernel C stays `-mgeneral-regs-only` except the two FP TUs; no unaligned access. Every user pointer
  (pollfd arrays, msghdr/iovec/cmsg blocks, fd-number arrays) is range-checked before access the same way
  `sys_read`/`sys_select` do today (ARM: inline `USER_VIRT_BASE` compares; x64: `sys_user_range_ok()`),
  then accessed directly. P2 replaces the checks with `vm_touch` — keep them in one helper per arch so
  that swap stays mechanical.
- `MAX_PROCESSES` stays 64 (`1ULL << pid` masks), `MAX_OPEN_FDS` stays 32, `MAX_GLOBAL_FILES` stays 128.
  All new state is fixed-size, static-pool, under the **existing lock order** (`proc_lock → f->lock →
  p->lock`, pipe.c line 85 comment); no allocation; no new blocking primitives in IRQ paths.
- The **existing select() semantics and pipe blocking model must not change** — POLLTST/SOCK2TST pin
  them. P4 *extends* the shared readiness probe and reuses the park/slice engine; it does not fork it.
- Linux syscall-level behavior is the compatibility target for everything the stacked consumers
  (WK → GLib → libc) observe: same errno names/values, same EOF/EPIPE/ECONNRESET shapes, same LP64
  struct layouts, `MSG_NOSIGNAL` accepted. There is **no multi-user rights model** on HobbyOS:
  "rights validation" below means "the fd is open in the sender's group table" (§4.2).
- Scope is exactly browser.md §6 P4 (+§A.1b rows): pathless AF_UNIX via `socketpair`, fd passing,
  `poll`. Named/abstract sockets, epoll, and TCP `sendmsg` are recorded deferrals with their design
  sketch in place (§2.4, §5.3) — loud `-EOPNOTSUPP`, never silent gaps.

## 1. What exists today (what P4 extends, verbatim behavior)

### 1.1 fd table and file objects (`fs.c`, `fs.h`)
- One static `global_file_table[MAX_GLOBAL_FILES=128]` of `struct file {type, ref_count, lock, union{…}}`;
  each `struct process` (group leader, P1) holds `open_fds[MAX_OPEN_FDS=32]` mapping user fd → global
  index + `num_open_fds`. Fork copies the table 1:1 and calls `fs_reopen()` per entry
  (process.c:1406-1412); spawn2's loader maps only the three passed user fds onto the child's 0/1/2 and
  `fs_reopen()`s them (program_loader.c:250-278); group teardown closes every fd via `fs_close_global()`
  (process.c:1236-1240). `file_dup`/`file_dup2` bump `ref_count` and, for pipes, call `pipe_reopen()` so
  the per-end counts stay 1:1 with the fds (fs.c:723-775). `file_close()` decrements, calls `pipe_close()`
  for pipes per fd, and only at `ref_count == 0` runs the backend close (`fat16_close`/`net_socket_close`).
- `fd_set` width is frozen at FD_SETSIZE 256; select masks are `{uint32_t bits[8]}`.

### 1.2 Pipes: the blocking + wakeup model P4 mirrors (`pipe.c`)
- `struct pipe` = 512-byte ring + `reader_count`/`writer_count` (per-fd counts, kept in sync by
  `pipe_reopen`/`pipe_close`) + two **pid masks** (`reader_pid_mask`/`writer_pid_mask`) of blocked
  waiters. Counts reaching 0 wake the opposite mask; EOF = "read of empty pipe with `writer_count==0`
  returns 0"; write with no readers returns -1 today (P5 turns this into EPIPE/SIGPIPE).
- Blocking read/write: re-check under `p->lock`, then (dropping and retaking locks in the documented
  `proc_lock → p->lock` order) set the pid bit, set `PROC_STATE_BLOCKED`, and **return -2**. The trap
  layer rewinds the syscall and schedules (`sys_read`: `tf->elr -= 4` on ARM / `-= 2` x64); the syscall
  restarts on wake. `drain_wakeup_mask()` runs *after* releasing the pipe lock — the comment at
  pipe.c:85-97 records the 7-CPU deadlock that enforcing that order fixed.

### 1.3 The select engine (`fs.c:317-501`)
- `f1_probe_fd()` probes one fd: pipes (read end readable on data or writers==0 with the exception bit
  set; write end writable with space, or writable+exception with no readers), sockets (via
  `net_socket_ready()` + `connect_err`), FAT16/NFS (always readable). Unknown fd → the whole select
  fails `-EBADF` (Linux-shaped).
- `file_select()` walks the masks, probes only fds < nfds, rewrites masks to the ready set, returns the
  count; with nothing ready it parks the caller through the scheduler (`PROC_STATE_BLOCKED` + a 10 ms
  `wake_ms` slice) and returns **-2** — the syscall restarts and re-probes. The caller's timeout is made
  exact by a per-pid `select_wait[]` deadline table (cleared by `file_select_forget()` on group
  teardown). **No busy spin; no per-fd wake registration.**

### 1.4 Today's sockets
- `struct socket_pcb` (`net.h`) is a TCP/UDP-only object with a 4 MiB embedded RX ring; `FILE_TYPE_SOCKET`
  file objects point at it. `file_socket()` handles AF_INET only; `file_socket_connect` is
  `connect_fd(fd, ip_be, port_be)`; `file_fcntl` (F_GETFL/F_SETFL O_NONBLOCK) and getsockopt/setsockopt
  (SO_TYPE/SO_ERROR real) are socket-only. `net_socket_recv()` busy-*holds* its CPU in a 5 s `wfi` loop —
  the old model, not the park model; P4 must not copy it (AF_UNIX uses §1.2).
- `SYS_SOCKETPAIR/SENDMSG/RECVMSG/POLL` do not exist. `socket(AF_UNIX, …)` returns `-EAFNOSUPPORT` and
  `sock2_test.c:57` **asserts that**. `SYS_POLL` is unimplemented (the existing `POLLTST.BIN` is a
  select() suite).

### 1.5 The trap-layer error policy P4 must amend
`sys_read`/`sys_write` currently map **every** negative return except -2 to `-EBADF` (ARM trap.c:112-113,
476-499; x64 trap.c:146-152, 539-548). New fd types must be able to surface EAGAIN/EPIPE/ECONNRESET/
EMFILE; §6.5 records the exact amendment, constructed so every existing backend keeps its present
behavior.

## 2. AF_UNIX endpoint model (P4.1)

### 2.1 The object: a socketpair is one `struct usock` with two endpoints
**D1 — new kernel object, not `socket_pcb`.** AF_UNIX state is a new static-pool object in a new
`src/kernel/unix.c` + `src/include/unix.h`:

```c
#define K_UNIX_STREAM     1        /* byte ring per direction                 */
#define K_UNIX_SEQPACKET  2        /* message queue per direction             */
#define K_UNIX_DGRAM      3        /* message queue per direction             */
#define K_UNIX_RING       65536    /* STREAM bytes per direction              */
#define K_UNIX_MSG_MAX    8192     /* max bytes per message (2 x WK's 4096)   */
#define K_UNIX_MSGS       8        /* queued messages per direction           */
#define K_UNIX_MAX_FDS    16       /* fds per message                         */
#define K_UNIX_MAX_IOV    8        /* iovecs per sendmsg/recvmsg              */
#define MAX_UNIX_PAIRS    16       /* pool: 16 pairs, static, like pipes      */

struct usock_chan {                 /* one direction: sender -> receiver      */
  uint8_t  ring[K_UNIX_RING];       /* STREAM: byte ring                      */
  uint16_t msg_len[K_UNIX_MSGS];    /* SEQPACKET/DGRAM: queued message lengths*/
  int16_t  msg_fds[K_UNIX_MSGS][K_UNIX_MAX_FDS]; /* global fd indices, -1 empty */
  uint8_t  msg_nfds[K_UNIX_MSGS];
  uint32_t head, tail, used;        /* ring indices / message indices         */
  uint64_t rwait, wwait;            /* pid masks: blocked readers / writers   */
};
struct usock {
  spinlock_t lock;
  int   in_use;
  int   type;                       /* K_UNIX_STREAM/SEQPACKET/DGRAM          */
  uint32_t refs[2];                 /* open-fd refs per end (0 = side gone)   */
  int   nonblock[2];                /* per-end O_NONBLOCK (fcntl F_SETFL)     */
  struct usock_chan chan[2];        /* chan[d]: d=0 → A->B, d=1 → B->A        */
};
```
- `struct file` gains `FILE_TYPE_UNIXSOCK` and a union member `{ struct usock *ptr; int end; } usock;`
  (end 0|1, mirroring `pipe.end`). `MAX_UNIX_PAIRS=16` costs ~2 MiB of bss worst case and covers
  every WK connection (a WK-2 process graph is 2–4 pairs) plus tests; `MAX_GLOBAL_FILES` bounds it
  anyway (16 pairs = 32 of 128 file slots). Sizing knobs are named constants; raise with evidence.
- **Lifecycle:** `usock_alloc()` (pair, both `refs=1`) ← `socketpair()`; `usock_reopen(obj,end)` ←
  dup/fork/SCM_RIGHTS-install (mirror of `pipe_reopen`); `usock_close(obj,end)` ← `file_close` per fd;
  pair teardown (both refs 0 AND both directions fully drained *or* the receiving side gone) releases
  any queued messages' fds (`fs_close_global` each) and returns the slot to the pool. Sent data survives
  the *sender's* close (Linux): after A closes, B still drains chan[0], then sees EOF.
- **Lock order** stays `proc_lock → f->lock → usock->lock`; wakeups drain masks strictly *after*
  releasing `usock->lock` (same rule and rationale as pipe.c:85-97). Ref-count grabs for fds carried in
  a message happen *before* taking `usock->lock`, under the passed file's own `f->lock` only — no
  nested f→usock ordering (see §4.2).

### 2.2 Blocking, EOF and wakeups — the pipe model, per direction
**D2 — blocking is the pipe park, not a wait loop.** `usock_recv`/`usock_send` re-check state under
`usock->lock`, then set the caller's pid bit in the relevant mask (`chan[d].rwait` for readers waiting
on that direction, `.wwait` for writers waiting for space), set `PROC_STATE_BLOCKED`, and return **-2**
— the trap layer rewinds and schedules exactly as for pipes; the syscall restarts on wake. Mutations
that unblock: enqueue wakes `rwait`; dequeue/space frees wakes `wwait`; a close of an end wakes both
masks bit-wise for the affected directions. `O_NONBLOCK` returns `-EAGAIN` instead of parking.
- **EOF / close matrix** (D3):
  | event on end A (peer B) | B read on chan[0] | B write on chan[1] | stream vs message |
  |---|---|---|---|
  | A open | data / park | space / park | — |
  | A refs → 0, queue non-empty | drains, then EOF | **-EPIPE** | STREAM: read 0; SEQPACKET: read 0; DGRAM: read **-ECONNRESET** |
  | A refs → 0, queue empty | 0 (EOF) | -EPIPE | as above |
- Write side error is **-EPIPE with no signal** in P4 (there is no signal layer until P5; the WK code
  sets `MSG_NOSIGNAL` and treats EPIPE/ECONNRESET as connection death — matching Linux value and
  shape exactly). P5 replaces "return -EPIPE" with "EPIPE + SIGPIPE default-kill", keeping the return.
- Read side: STREAM/SEQPACKET return 0 at EOF (WK: `if (!bytesRead) connectionDidClose`, verified);
  DGRAM returns -ECONNRESET once drained (WK handles ECONNRESET → close; this is the shape its DGRAM
  fallback was written against). Pipes keep their current -1-on-write-no-readers (P5's concern).

### 2.3 Type semantics (D3)
- **STREAM**: `write()`/`sendmsg` append to the 64 KiB ring; a write that fits partially writes what
  fits and returns the byte count (POSIX); zero bytes free → park (blocking) / -EAGAIN (nonblock).
  `read()` coalesces bytes; `recvmsg` copies min(iov total, queued).
- **SEQPACKET / DGRAM**: one `write()`/`sendmsg` = one message, atomic, whole-or-nothing; queue full or
  message > `K_UNIX_MSG_MAX` (8192) → park / **-EMSGSIZE**. `read()` returns one message, discarding any
  tail beyond the buffer (POSIX socket read semantics); `recvmsg` truncates per-iov and sets
  **MSG_TRUNC**. A zero-length message is accepted (Linux-true; note the 0-return ambiguity with EOF is
  inherent to Linux too, and WK never sends empty messages — its smallest is `sizeof(MessageInfo)`).
- `lseek` → -ESPIPE; `fstat` → `S_IFSOCK|0600` (fs.h already has `K_S_IFSOCK`); `available(fd)` → queued
  bytes (stream) / total queued message bytes, -1 for a write end or invalid fd.
- Documented divergence: STREAM write-atomicity cliff (writes > free space split differently than a
  pipe's all-or-nothing 512 B); both models are legal POSIX and no consumer depends on the pipe
  shape for unix sockets.

### 2.4 Addressing, connect/listen/accept/shutdown — decided scope (D4)
**Decision: P4 ships the pathless case only, exactly as browser.md P4.1 scopes it ("socketpair(AF_UNIX)
+ pathless sockets").** Consequences, stated so nothing is ambiguous:
- `socket(AF_UNIX, SOCK_STREAM|SOCK_DGRAM|SOCK_SEQPACKET, 0)` **succeeds** and returns an *unconnected*
  endpoint (generic code constructs sockets before choosing a transport); any type/protocol else →
  `-EPROTONOSUPPORT` (the frozen errno set has no ESOCKTNOSUPPORT; same documented choice as
  `file_socket`). Operations that need a name are not represented at all: **there is no bind/listen/
  accept/connect-by-name/shutdown syscall in P4, and the libc wrappers are not provided.** Code paths
  that reach for them fail at compile or link time (loud, per the deferral rule).
- `connect_fd()` (INET, frozen ABI) must not give unix-socket fds the INET treatment; the routing is:
  INET socket + INET connect → today's behavior; unix socket + any connect → `-EOPNOTSUPP` (no named
  addresses exist); `sendmsg`/`recvmsg` on an unconnected unix endpoint → `-ENOTCONN`.
- **Filesystem-path sockets are rejected by design, not deferred silently**: FAT16 has no inodes, and
  creating a fake directory entry per socket (st_mode already prints `S_IFSOCK` for socket fds) adds
  VFS churn plus a rename/unlink lifecycle with no consumer in sight.
- **The design-in-waiting for names** (recorded so a later gate doesn't re-derive it): abstract
  namespace only (`sun_path[0] == '\0'`, Linux-compatible), a 16-entry kernel name table
  `{char path[108]; struct usock *pair; int listening}`, `bind()` registers, `connect()` resolves and
  attaches the connecting endpoint to the pair, `listen(backlog)` marks + caps a 4-deep accept queue,
  `accept()` hands out fresh endpoint fds; rows append-only after the then-current top (§6.1).
- `shutdown(2)`: not provided in P4. No consumer in the pinned WK/GLib IPC paths (`ConnectionGLib`
  closes with `g_socket_close()` → `close(2)`; `g_socket_shutdown()` exists but is not called on the
  IPC path — cited in §7). When a consumer appears, `SYS_SHUTDOWN (fd, how)` is the append.

## 3. socketpair() and the process model (P4.1) — D5, D6

**D5 — `socketpair(domain, type, proto, fds[2]) -> 0 | -errno`, row 77 (frozen §A.1b).**
- `domain` must be AF_UNIX (1) else `-EAFNOSUPPORT`; `proto` must be 0 else `-EPROTONOSUPPORT`;
  `type` = SOCK_STREAM/DGRAM/SEQPACKET, optionally OR-ed with `SOCK_CLOEXEC` (bit recorded as
  FD_CLOEXEC on both fds, §3.1); unknown bits → `-EINVAL`. Pool exhausted → `-EMFILE`; caller fd table
  full → `-EMFILE`; user `fds` pointer unchecked/overlapping → `-EFAULT` (validated first).
- On success both endpoints are in the caller's **group** fd table (lowest-free slots, like `pipe()`),
  refs=1 each, "connected" by construction. The process may keep both (self-pipe idiom) or pass one on.
- `socketpair` never allocates a `socket_pcb` — it is the only constructor of `struct usock` (§2.1).

**D6 — process-model integration.**
- **fork:** child copies `open_fds` 1:1 (unchanged, process.c:1406-1412) — extend the per-entry work so
  `FILE_TYPE_UNIXSOCK` entries also call `usock_reopen(ptr,end)`. Fd numbers survive fork; both
  processes see the same pair.
- **spawn2:** only the three fd arguments cross (program_loader.c:250-278, verified) — so
  `socketpair(); spawn2("CHILD.BIN", a, a, -1, …)` is the supported way to give a spawned child one
  end; child fd numbers are 0/1/2 by the caller's mapping, `usock_reopen` on transfer. Extend the
  *default-stderr* rule (program_loader.c:273: a parent fd-2 that is a pipe is NOT silently inherited)
  to unix-socket ends too (`file_gfd_is_pipe()` → `file_gfd_is_ipc_endpoint()`): handing a child a
  reference to the parent's protocol channel by default is the same class of surprise the pipe rule
  exists to prevent.
- **Arbitrary fd numbers (WK launch requirement):** `ProcessLauncherGLib` passes the client socket's
  fd *number* in argv and relies on inherited-fd semantics (`g_subprocess_launcher_take_fd`,
  `G_SUBPROCESS_FLAGS_INHERIT_FDS`; ProcessLauncherGLib.cpp:232-236). HobbyOS satisfies this at P5 via
  **fork + execve** (fork preserves numbers; execve honors FD_CLOEXEC) — noted here so P5's execve
  design keeps "fd numbers stable across fork" and "CLOEXEC honored at exec" in scope. SCM_RIGHTS can
  also hand the end over *after* spawn on a bootstrap channel if the port prefers.

### 3.1 FD_CLOEXEC storage (D6)
There is no per-fd flag storage today. Add `uint8_t fd_flags[MAX_OPEN_FDS]` (bit 0 = FD_CLOEXEC) to the
group PCB: init on process create, copied by fork (CLOEXEC is exec-only — fork keeps it, closes
nothing), cleared for the three fd-slot grants a spawn2 loader installs (they are the child's stdio),
honored by P5's execve. `fcntl(F_SETFD/F_GETFD)` support lands for **all** fd types (§6.4) because both
WK (UniStdExtrasUnix.cpp:33,45 `setCloseOnExec`) and GLib (gunixfdmessage.c:108 `fcntl(fd, F_SETFD,
FD_CLOEXEC)` on every received fd) call it unconditionally.

## 4. sendmsg/recvmsg + SCM_RIGHTS (P4.2) — D7, D8, D9

### 4.1 The subset (D7)
Rows 78/79: `sendmsg(fd, msghdr*, flags) / recvmsg(fd, msghdr*, flags) -> bytes | -errno`.
- **Connected endpoints only**: `msg_name` must be NULL / `msg_namelen == 0` (the two socketpair ends
  are the only peers); anything else → `-EINVAL`. `msg_iovlen` 1..`K_UNIX_MAX_IOV` (8) else `-EINVAL`.
- `flags`: sendmsg accepts `MSG_NOSIGNAL` (no-op — we never signal) and `MSG_DONTWAIT` (one-shot
  nonblock); recvmsg accepts `MSG_DONTWAIT` and `MSG_CMSG_CLOEXEC` (received fds get FD_CLOEXEC);
  any other bit → `-EINVAL`. `MSG_PEEK` → `-EOPNOTSUPP` (no consumer; appendable later).
- **Framing**: on SEQPACKET/DGRAM one sendmsg = one message: the iovecs are concatenated (total ≤
  `K_UNIX_MSG_MAX`, else `-EMSGSIZE`), the control block's SCM_RIGHTS fds ride *that* message, and one
  recvmsg yields exactly that message (data + the same fds). Order is FIFO; the pair has one producer
  per direction so no interleaving exists. On STREAM, data is bytes; **control data on STREAM →
  `-EOPNOTSUPP`** (rationale: no consumer — WK's connection type is SEQPACKET/DGRAM on every path; the
  byte-offset ancillary queue a faithful STREAM implementation needs is cost with zero current value,
  noted as the append when one appears).
- The kernel parses a **mirror struct** (`k_msghdr`, `k_iovec`, `k_cmsghdr` in `unix.h`/`ipc_proto.h`,
  keeping the sysroot's `<sys/socket.h>` macros user-side) because kernel TUs don't include the sysroot.
  The parse/validate/emit half lives in a **pure TU** (`src/kernel/ipc_proto.c`, nfs_proto.c precedent)
  so both EL1 unit tests and host tests can drive it over byte arrays.
- `sendmsg`/`recvmsg` on an **INET** socket → `-EOPNOTSUPP` in P4 (future: a thin wrapper over
  `net_socket_send/recv` if GLib's GResolver or curl ever lands on the path — flagged, not built).
  On a pipe or file → `-ENOTSOCK` (consistent with `f1_socket_file`).

### 4.2 SCM_RIGHTS: the fd lifetime machine (D8)
Grammar: a single `cmsg` `{cmsg_level=SOL_SOCKET, cmsg_type=SCM_RIGHTS, cmsg_len=CMSG_LEN(4*n)}` with
1..`K_UNIX_MAX_FDS` (16) fds; multiple SCM_RIGHTS cmsgs in one block are accepted up to the cap;
unknown level/type → `-EINVAL`; malformed lengths/alignment → `-EINVAL`. "One fd minimum, arrays
best-effort" (browser.md §6 P4.2) concretized: **1..16 fds are all-or-nothing**; the sender's >16 → `-EMSGSIZE`;
the receiver's control buffer too small for what arrived → deliver data, `MSG_CTRUNC`, close the excess
(§4.3).

The refcount machine (the part that must be exactly right):

1. **Enqueue (sendmsg):** for each fd number `u` in the cmsg — validate `p->open_fds[u]` is open
   (`-EBADF` on any failure, whole call fails, nothing enqueued), then, under that file's `f->lock`
   alone, `ref_count++` and the per-fd type reopen (`pipe_reopen` / `usock_reopen`); store the **global
   fd index** in the message slot. The message now owns those references.
2. **Install (recvmsg):** take fresh slots in the receiver's fd table; the refs already held by the
   message *transfer* to the receiver's fds — no extra increment, but the per-fd end counts are already
   correct via step 1. If the receiver cannot fit **all** fds (`num_open_fds + n > MAX_OPEN_FDS`) →
   `-EMFILE`, message **not consumed**, refs stay with the queued message (retryable).
3. **Discard:** destroying a queued message (receiver side closed, pair teardown, control-buffer
   truncation) runs the full close semantics per fd: `ref_count--`, type close (`pipe_close`/
   `usock_close`), backend close at ref 0 — i.e. exactly `fs_close_global()`.
4. **Steady state:** closing the sender's original fd after enqueue does not invalidate the message
   (ref held). Closing the receiver's installed fd releases it. Receiving an fd that is itself a unix
   socket end or pipe end keeps direction counts honest because both file types carry per-fd counts
   (step 1's reopen).

Rights: single-user OS — any fd open in the sender's *group* table may be passed (FAT16/NFS/pipe/
socket/unixsock; memfd joins at P2). No credential check is attempted; `st_uid`/`st_gid` are stubs for
everyone. The received fd number is a **fresh lowest-free slot** in the receiver (Linux semantics; the
sender's number is not preserved).

### 4.3 recvmsg tail behaviors (D9)
- Data truncation: message longer than the iovec total → copy what fits, discard the rest, set
  `MSG_TRUNC` (SEQPACKET/DGRAM; streaming recvmsg just copies min).
- Control truncation: `msg_controllen` smaller than `CMSG_SPACE` of the arrived fds → write as many
  whole fds as fit, set `MSG_CTRUNC`, close the rest (step 3). `msg_control == NULL` with fds present →
  all fds discarded (closed) + `MSG_CTRUNC`.
- `msg_flags` out: `MSG_TRUNC`/`MSG_CTRUNC` as above; `msg_controllen` rewritten to what was written.
- Blocking/nonblocking: empty queue → park/-2 (blocking) or `-EAGAIN`; `MSG_DONTWAIT` forces the
  nonblocking path for one call. Partial-message reads do not exist on message sockets.

## 5. poll() (P4.3) — D10, D11, D12

### 5.1 Semantics (D10)
Row 76: `poll(struct pollfd *fds, int nfds, int timeout_ms) -> count | -errno`.
```c
struct pollfd { int fd; short events; short revents; };   /* 8 bytes, LP64, no padding */
```
- Linux values: `POLLIN 0x1, POLLPRI 0x2 (accepted, never set), POLLOUT 0x4, POLLERR 0x8,
  POLLHUP 0x10, POLLNVAL 0x20`. `events` may only contain POLLIN|POLLOUT (+POLLPRI ignored); unknown
  bits → ignored (Linux) — documented, not an error.
- Returns the number of entries with nonzero `revents` (0 on timeout); `-errno` only for a bad argument
  (`nfds<0` or `nfds>FD_SETSIZE(256)` → `-EINVAL`; bad user pointer → `-EFAULT`). **Per-fd invalidity
  is `POLLNVAL` in `revents`, not a failed call** (Linux shape; deliberate divergence from our select's
  whole-call `-EBADF`, which does not change).
- `timeout_ms`: <0 waits forever, 0 polls, >0 waits. Implementation is the select engine verbatim:
  probe all entries, if none ready park with `PROC_STATE_BLOCKED` + 10 ms slice + the shared per-pid
  deadline (generalize `select_wait[]` into one table used by select and poll — threads have distinct
  pids, so there is no collision; `file_select_forget()` clears both), return -2, restart on wake.
- **Zero new wake plumbing:** pollers ride the 10 ms re-probe slice, like select. Blocking readers/
  writers (the -2 masks of §2.2) are the only mask-based waiters, unchanged from pipes.

### 5.2 Probe extension and the revents mapping (D10)
`f1_probe_fd` grows a `hup` out-parameter and a `FILE_TYPE_UNIXSOCK` case; **select's outputs are
computed exactly as today** (`e|hup` = exception set — POLLTST/SOCK2TST-visible behavior frozen),
while poll maps:

| probe state | select (r,w,e) | poll revents |
|---|---|---|
| pipe read end: data | r | POLLIN |
| pipe read end: writers==0 | r,e | POLLIN+POLLHUP |
| pipe write end: readers==0 | w,e | POLLOUT+POLLERR |
| unix read end: data / peer closed (drained) | r / r,hup | POLLIN / POLLIN+POLLHUP |
| unix write end: space / peer closed | w / w,hup→e | POLLOUT / POLLOUT+POLLERR |
| socket: connect_err / EOF-closed | e / r,hup | POLLERR / POLLIN+POLLHUP |
| FAT16/NFS | r | POLLIN (read may return 0 at EOF — Linux file behavior) |
| bad fd / fd ≥ MAX_OPEN_FDS / fd ≥ nfds | -EBADF call / skipped | POLLNVAL / skipped |

GLib's `GSource` derives `G_IO_HUP`/`G_IO_ERR`/`G_IO_NVAL` from exactly POLLHUP/POLLERR/POLLNVAL —
this mapping is what makes `GSocketMonitor` (the WK read path, §7) fire correctly on peer death.

### 5.3 epoll: decided out (D11)
**Not implemented and not planned for the port horizon.** Evidence: no `epoll_*` call anywhere in the
pinned WebKit IPC/WTF paths (grep in `~/webkit-hobbyos/Source/WTF` + `Source/WebKit/Platform` — zero
hits outside Android hardware-buffer code); the GLib main loop uses `g_poll` → `poll(2)` (gmain.c:4805-
4845, `ppoll` only `#if defined(HAVE_PPOLL)`, which the cross build will not have); GLib's only epoll
use is a *pollability probe* in `giounix-private.c:85` (`_g_fd_is_pollable`, used by GUnixInputStream/
OutputStream, not the IPC path) guarded by `HAVE_EPOLL_CREATE1` with a documented `#else` fallback —
so leaving `epoll_create1` absent is a supported build of GLib 2.88.3. The milestone text agrees
("poll; select stays for old code"). Revisit only if a concrete consumer appears.

## 6. Frozen interface (§A.1b amendment proposal)

### 6.1 Number reconciliation (required by the brief; document, then freeze)
- The v1 plan had `SYS_POLL 74`. P1 **inserted** `THREAD_EXIT 74` / `SET_TLS 75` (p1-threads-design.md
  §2/OQ1, consented; nothing ≥72 was implemented at the time) and shifted every later provisional row
  +2 → **POLL 76, SOCKETPAIR 77, SENDMSG 78, RECVMSG 79** — exactly what browser.md §A.1b now prints.
  P2's note (p2-vm-design.md §4.2) re-confirms "Rows 76–79 (P4) stay provisional gaps" and freezes its
  own 80/81/82.
- **P4 freezes 76–79 in place; no renumber.** At the P4 gate the single-writer header edit adds the
  four rows and sets `SYS_MAX` to `max(current, 79)` — i.e. **75 → 79** if P4 gates before P2, and a
  no-op top-up if P2's edit (→82) has already landed. Order-independent; record which happened at the
  gate (§10 OQ1(a)).
- Rows above the frozen top stay **append-only** (P2 OQ1 language). P4 needs nothing beyond 76–79
  (§2.4/§3/§4/§5); the first append candidates when a consumer exists are `GETSOCKNAME` (required by
  GLib's GSocket-from-fd, §7) then `SHUTDOWN`, then the named-socket quartet — see OQ2.

### 6.2 The rows (freeze at the P4 gate)
```
SYS_POLL         76  (struct pollfd *fds, int nfds, int timeout_ms)   -> count | 0 | -errno
SYS_SOCKETPAIR   77  (domain, type, proto, int fds[2])                -> 0 | -errno
SYS_SENDMSG      78  (fd, struct msghdr *msg, flags)                  -> bytes | -errno
SYS_RECVMSG      79  (fd, struct msghdr *msg, flags)                  -> bytes | -errno
```
Arg registers as the house ABI: ARM x0..x3 = `regs[0..3]`; x64 rdi/rsi/rdx/r10 = `regs[5]/[4]/[3]/[9]`
(no 5th/6th args; the existing 4-arg `syscall()` wrapper suffices). Dispatch: one `else-if` per row in
BOTH `arch/{arm,x64}/trap.c` (never a table). The `syscall.h` comment block records the P4 freeze.

### 6.3 Struct layouts and sysroot headers (new; LP64 Linux-compatible)
New headers under `src/libc/include/` (house pattern: `HOBBYOS_*_H` guards, `#ifdef HOST_TEST
#include_next`), because WK/GLib include real POSIX headers rather than `libc.h`:
`<sys/socket.h>` (socketpair/sendmsg/recvmsg/shutdown-when-built decls; AF_UNIX=1, sockaddr,
sockaddr_storage 128 B, `socklen_t`, SOCK_STREAM=1/SOCK_DGRAM=2/SOCK_SEQPACKET=5, SOCK_CLOEXEC=0x80000,
SOL_SOCKET=1, SCM_RIGHTS=1, SO_TYPE/SO_ERROR/SO_DOMAIN/SO_KEEPALIVE, MSG_CTRUNC=0x8/MSG_TRUNC=0x20/
MSG_DONTWAIT=0x40/MSG_NOSIGNAL=0x4000/MSG_CMSG_CLOEXEC=0x40000000, SHUT_RD/WR/RDWR), `<sys/un.h>`
(`sockaddr_un` — compilable even though no name syscall exists; GLib's `GUnixSocketAddress` includes
it), `<sys/uio.h>` (`struct iovec`), `<poll.h>` (`struct pollfd` + POLL*). `<sys/socket.h>` also carries
the CMSG macros (`CMSG_ALIGN/DATA/SPACE/LEN/FIRSTHDR/NXTHDR`) — used directly by WebKit's
ConnectionUnix.cpp:422-427 and GLib's gsocket.c — the kernel mirrors them as `k_cmsg_*` in `unix.h`.
`libc.h`'s existing frozen F1 block (AF_INET/FD_SETSIZE/socket/connect_fd/select/…) is untouched; the
new calls get their declarations in the new headers plus thin wrappers in `src/user/libc.c`
(`socketpair`, `sendmsg`, `recvmsg`, `poll` — device-only, HOST_TEST takes glibc's).

### 6.4 errno additions and existing-syscall extensions
- `errno.h` += `EMSGSIZE 90`, `ENOTCONN 107` (Linux values). Everything else needed already exists
  (EBADF, ENOTSOCK, EAFNOSUPPORT, EPROTONOSUPPORT, EINVAL, EFAULT, EMFILE, EAGAIN, EPIPE, ECONNRESET,
  ETIMEDOUT, ENOPROTOOPT, EOPNOTSUPP/ENOTSUP).
- `SYS_FCNTL (68)`: add `F_GETFD/F_SETFD` (FD_CLOEXEC bit in `fd_flags[]`) for **all** fd types, and
  `F_GETFL/F_SETFL` (O_NONBLOCK) for unix sockets; other cmds/types keep today's EINVAL (socket
  F_GETFL/F_SETFL unchanged). `FD_CLOEXEC=1`, `F_GETFD=1`, `F_SETFD=2` (Linux).
- `SYS_GETSOCKOPT (69)`: `SO_TYPE` and `SO_ERROR(0)` become real for unix sockets; `SO_DOMAIN` added
  (AF_UNIX) so GLib's `g_socket_details_from_fd` fallback (gsocket.c:528-536) can never dead-end even
  on an unnamed endpoint; `SO_KEEPALIVE` stays an error (GLib documents the failure as
  `keepalive=FALSE`, gsocket.c:588-597).
- **Not in P4, flagged for the GLib-on-OS gate:** `getsockname()` (required when `g_socket_new_from_fd`
  wraps a socketpair end — gsocket.c:513; on Linux an unnamed AF_UNIX endpoint returns
  `addrlen == sizeof(sa_family_t)` and family AF_UNIX; ours must do the same), optionally
  `getpeername()` (GLib tolerates failure, gsocket.c:580). Both append rows (`SYS_GETSOCKNAME`, …)
  when that port work starts; until then `GSocket`-from-fd is untestable on-OS — recorded in OQ2.

### 6.5 Trap-layer and read/write error policy amendment
`sys_read`/`sys_write` in BOTH trap.c files change the negative branch to:
```c
} else if (ret < 0) {
  /* P4: new fd types return -errno (< -1); -1 keeps the historical EBADF
   * mapping for the legacy backends (fat16_read, pipe_read, …).  -2 is the
   * restart marker above. */
  tf->regs[0] = (ret == -1) ? (uint64_t)-EBADF : (uint64_t)ret;
}
```
Every existing backend returns only -1/-2 today (verified: `fat16_read`, `net_socket_recv`,
`pipe_read`, `pipe_write`), so no observable change for old fds; unix sockets return real errnos
(-EAGAIN/-EPIPE/-ECONNRESET/-EMFILE). The four new syscalls validate user pointers up front (range +
alignment for the pollfd array; range for msghdr/iovec/cmsg blocks, which are then copied to kernel
scratch) exactly like `sys_select`'s mask checks.

### 6.6 Test-file amendments that ride the freeze
- `sock2_test.c:57` currently asserts `socket(AF_UNIX) == -1/EAFNOSUPPORT`; the P4 lane edits that
  check to `socket(AF_UNIX, SOCK_STREAM, 0) >= 0` plus a new `socket(AF_UNIX, SOCK_RAW, 0) ==
  -1/EPROTONOSUPPORT` (and records the change in §10 OQ1(d) as a deliberate expectation flip, not a
  regression).
- `src/user/ipc_test.c` → `IPC_T.BIN` joins the KERNEL_MODE_TEST boot list LATE (after THRD_T; it
  forks children) — integrator applies the Makefile/disk/wave diffs (browser.md §7.3, row F6).

## 7. WK cross-check — what WPE 2.54.0 actually calls (required vs deferrable)

Verified against `~/webkit-hobbyos` (commit `5220e80b97…`) and `third_party/glib-2.88.3`:

| # | Call site (file:line) | What it needs from P4 | Verdict |
|---|---|---|---|
| 1 | `IPCUtilitiesUnix.cpp:40,51` `socketpair(AF_UNIX, socketType[, SOCK_CLOEXEC], 0, …)` | socketpair; SOCK_SEQPACKET; fcntl F_SETFD/F_GETFD (`set/unsetCloseOnExec`, `UniStdExtrasUnix.cpp:33,45`) | **required** |
| 2 | `ConnectionGLib.cpp:516` / `ConnectionUnix.cpp:55-58` | `createPlatformConnection(SOCK_SEQPACKET)`; the unix fallback defines `SOCKET_TYPE = SOCK_SEQPACKET` (`#if defined(SOCK_SEQPACKET)`) — the sysroot must define it so both paths pick it; DGRAM must also exist (documented fallback) | **required** |
| 3 | `ConnectionGLib.cpp:100-106` `g_socket_new_from_fd` + `g_socket_set_blocking(FALSE)` | get fd from spawn/argv; `fcntl(F_GETFL/F_SETFL O_NONBLOCK)`; GSocket construction: `getsockopt SO_TYPE` (gsocket.c:487), `getsockname` (gsocket.c:513), `SO_DOMAIN` fallback (gsocket.c:528-536), `SO_KEEPALIVE` may fail | fcntl/SO_TYPE **required (P4)**; getsockname **required-but-deferred** (append row at the GLib port gate; OQ2) |
| 4 | `ConnectionGLib.cpp:248-284` read: `g_socket_receive_message` → `recvmsg` (gsocket.c:5746-5761, `MSG_CMSG_CLOEXEC` if defined), `MSG_CTRUNC` = fatal read failure, `g_unix_fd_message_steal_fds` + `setCloseOnExec` per fd | recvmsg; MSG_CTRUNC; SCM_RIGHTS; FD_CLOEXEC on received fds | **required** |
| 5 | `ConnectionGLib.cpp:420-509` send: `g_socket_send_message` → `sendmsg(…, MSG_NOSIGNAL)` (gsocket.c:3679-3680, 5261); CMSG/SCM_RIGHTS build (gsocket.c:4852-4869); EAGAIN → `GSocketMonitor(G_IO_OUT)` (poll-backed) | sendmsg; MSG_NOSIGNAL accepted; SCM_RIGHTS; nonblocking + poll writability | **required** |
| 6 | `ConnectionUnix.cpp:453-462` sendmsg retry loop: `EINTR`; `EAGAIN/EWOULDBLOCK → poll(&pollfd,1,-1)` with POLLOUT; `EPIPE/ECONNRESET` → close | poll(fd, POLLOUT, -1); EPIPE/ECONNRESET on send | **required** (poll) |
| 7 | `ConnectionUnix.cpp:245-271` read loop: `EAGAIN` → wait; `ECONNRESET` → close; `bytesRead == 0` → close (EOF); `MSG_CTRUNC` → failure | EOF=0 (SEQPACKET/STREAM), ECONNRESET shape, MSG_CTRUNC | **required** |
| 8 | `ConnectionGLib.cpp:356-370` `GSocketMonitor G_IO_IN/G_IO_HUP/G_IO_ERR/G_IO_NVAL` | poll revents → G_IO_* mapping incl. **POLLHUP/POLLERR/POLLNVAL** | **required** |
| 9 | `ConnectionGLib.cpp:520-533` `sendCredentials` / `remoteProcessID` (GUnixConnection credentials) | SO_PEERCRED-style credentials | **not required**: reached only from sandbox launch paths (`bubblewrap`/`flatpak`, pid wrapper — ProcessLauncherGLib.cpp:284-300); sandbox is OFF (browser.md §6 P7.4). Stub/port-fork note, out of P4 |
| 10 | `ConnectionGLib.cpp:116`/`g_socket_close`, `g_socket_shutdown` (gsocket.c:3888-3920) | `shutdown(2)` | **not required** on the IPC path (`g_socket_close` closes); deferrable until a consumer (P4 note §2.4) |
| 11 | `giounix-private.c:85` epoll probe; `gmain.c:4823` ppoll | epoll; ppoll | **not required**: `HAVE_EPOLL_CREATE1`/`HAVE_PPOLL` resolve absent at cross-build; documented fallbacks exist (D11) |
| 12 | `ProcessLauncherGLib.cpp:108,232-236` | socketpair for launch; inherited fd whose **number is passed via argv** | socketpair **required**; inheritance comes from fork+execve (P5) or a spawn2 extension — noted for P5 design (§3) |

**Sequence note for the port:** WK-1 (`jsc`) needs none of this. The first P4 consumer on the critical
path is the platform `RunLoop` (poll-backed, Appendix B.1) at WK-2/WK-3, then `ConnectionGLib` at the
process-split (WK-4/WK-5 or earlier if the port goes multi-process). That matches P4 landing in Wave 2
before WK leans on it.

## 8. Test plan

**Tier 1 — host.** (a) `src/host/ipc_proto_test.c` (new, in `make host_tests`): the pure codec TU
(`ipc_proto.c`) over byte arrays — msghdr/iovec/cmsg parse+validate matrix (offsets, alignment,
lengths, unknown cmsg, iov overflow, truncation emission, MSG_TRUNC/MSG_CTRUNC decision function,
events→revents mapping table) — nfs_proto_test precedent. (b) `src/user/ipc_test.c` also builds under
`-DHOST_TEST` against glibc's socketpair/sendmsg/recvmsg/poll (P1's thrd/tls host-variant precedent) to
validate the *test logic*; cases whose expected behavior is a HobbyOS choice (STREAM+control →
EOPNOTSUPP, "EMFILE without consuming", pool caps) are `#ifndef HOST_TEST`.
**Tier 2 — kernel units** (new `src/kernel/unix_test.c` + additions to `pipe_test.c`/`fs`-adjacent
suites, registered in `unit_test.c`, both arches): pair alloc/refcount/close matrix incl. double
close; per-direction state machine (enqueue/dequeue/backpressure/EOF/EPIPE/ECONNRESET table of §2.2);
`K_UNIX_MSG_MAX`/queue-depth bounds; fd-accounting machine with fake `struct file`s — refs return to
baseline across enqueue→install, enqueue→discard, enqueue→sender close→receiver drain; probe/revents
mapping (pure, table-driven); poll arg validation; select-probe regression (existing pipe/socket
expectations unchanged).
**Tier 3 — in-OS `IPC_T.BIN`** (`src/user/ipc_test.c`, extends the SOCK2TST output convention
`"  IPC_T <name>: PASS/FAIL"` + `ALL TESTS PASSED SUCCESSFULLY!`, self-terminating, watchdog, loaded
late):
1. STREAM socketpair echo across fork: child writes "hello", parent reads, EOF after child close,
   second read returns 0; write after peer close returns -1/EPIPE.
2. SEQPACKET framing: two sends (one 1-byte, one ~7 KiB) → exactly two recvs in order, boundaries
   intact; 0 at EOF after peer close; oversize (> 8192) → -1/EMSGSIZE.
3. fd passing: parent socketpairs, passes a **pipe write end** to a forked child via sendmsg/recvmsg;
   child writes through the received fd; parent reads it; then the reverse direction (child passes a
   pipe read end up). Lifetime check: parent closes its original fd *before* the child's recvmsg → the
   installed fd still works; receiver `fcntl(F_GETFD)` shows FD_CLOEXEC when sent with
   MSG_CMSG_CLOEXEC; `st_mode` of the received fd is S_IFIFO. Control-truncation: receiver with a
   too-small cmsg buffer gets MSG_CTRUNC and the excess fd is closed (probe via fcntl → EBADF).
4. poll multiplexing 3 fds: pipe read end, socketpair end, and the write end; assert POLLIN/POLLOUT
   sets exactly; add data → POLLIN; close peer → POLLHUP reading; unopened fd → POLLNVAL; timeout 0
   polls; positive timeout returns 0 with elapsed ≥ requested (10 ms tick tolerance); timeout -1 waits
   (fork + 100 ms child write, elapsed check — deterministic wake proof, POLLTST pattern).
5. Blocking park proof: fork a child that sleeps ~100 ms then writes; parent's reading `read()` must
   block and complete with elapsed ≥ 50 ms (proves the -2 park path, not a busy-hold).
6. Error paths: sendmsg on a pipe → ENOTSOCK; on unconnected unix socket → ENOTCONN; bad fd → EBADF;
   bad flags → EINVAL; recvmsg with msg_name set → EINVAL; STREAM + control → EOPNOTSUPP; poll nfds >
   256 → EINVAL; `fcntl(F_SETFD/F_GETFD)` round-trip on a plain file fd.
7. select() still works on unix fds (readiness through the extended probe).
**Regression guards** (green both arches before merge): `make host_tests` (F1 baseline 482/0),
`./run_unit_tests.sh` (53) + `./run_unit_tests_intel.sh` (55 KVM), full in-OS waves incl. POLLTST/
SOCK2TST (with the §6.6 edit)/PIPETST/THRD_T/TLS_T/STRESS/DNSTST, QMP E2E (`run_xcalc_test.py`), boot-
time delta by the Gate F1 method. **Waits for implementation:** everything above that needs real
threads/processes (the in-OS tier), the GLib-on-OS integration checks for `GSocket`-from-fd
(getsockname gate), the P5 execve inheritance check (fd numbers across exec), and P8's soak loops.

## 9. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | fd-ref accounting bug → table-slot leak or premature backend close | single refcount machine (§4.2) with unit accounting test returning to baseline; all releases funnel through `fs_close_global` semantics |
| R2 | lost wakeups / lock-order inversion (the pipe.c:85 deadlock class) | masks + `proc_lock → f->lock → usock->lock`, wakes drained outside locks; IPC_T's blocking-read elapsed check; kernel unit state machine |
| R3 | WK message size vs cap (8192) | WK's `messageMaxSize` is 4096 with build-time static asserts on both IPC paths; cap is 2×; EMSGSIZE test; knob is one constant |
| R4 | >16 fds per message (WK's `attachmentMaxAmount` is 254) | real traffic is 1–2 fds (SharedMemory handles); >16 → MSG_CTRUNC (fatal for WK, visible); measure at P8 torture, raise constant with evidence |
| R5 | `MAX_OPEN_FDS 32` / `MAX_GLOBAL_FILES 128` ceilings under WK multi-process | P1 OQ4 already parked this; P4 re-flags it for the P8 soak with fd-count instrumentation (OQ3) |
| R6 | select-probe refactor changes frozen select behavior | hup bit is additive; POLLTST/SOCK2TST unchanged-green is the gate; poll's POLLNVAL divergence is documented |
| R7 | trap.c error-propagation edit (-1 → EBADF carve-out) regresses old read/write paths | existing backends only ever return -1/-2 (verified); full wave + all suites re-run |
| R8 | STREAM ring semantics differ from pipes (partial writes) | documented; consumers (WK) use message types; stream tests cover partial/blocking cases |
| R9 | bss growth (~2 MiB pool) / boot-time delta | measured with the Gate F1 method; `MAX_UNIX_PAIRS` is the knob |
| R10 | A future consumer needs named sockets/getsockname sooner than planned | design-in-waiting sketches are complete (§2.4, §6.4, §7#3) so the append is mechanical; OQ2 registers the naming policy |

## 10. Decisions & open questions for integrator review

**Decisions**
- **D1** AF_UNIX = new `struct usock` pair object (static pool, 16 pairs), new `FILE_TYPE_UNIXSOCK`;
  rides the existing fd table and `fs.c` dispatch; closes/dup/fork follow the pipe per-fd-count pattern.
- **D2** Blocking is the pipe park (-2 + pid masks + `PROC_STATE_BLOCKED`), per direction; select/poll
  re-probe on the 10 ms slice; `O_NONBLOCK` → EAGAIN. No busy-hold (explicitly unlike `net_socket_recv`).
- **D3** Types: STREAM byte ring 64 KiB/dir; SEQPACKET/DGRAM message queue 8×8192 B/dir with ≤16 fds per
  message; peer close = EOF 0 (STREAM/SEQPACKET), -ECONNRESET (DGRAM read), -EPIPE (all writes); no
  signal until P5.
- **D4** Pathless only: `socket(AF_UNIX,…)` succeeds unconnected; no bind/listen/accept/named-connect/
  shutdown surface (not represented); those ops fail loud; filesystem-path sockets rejected by design;
  abstract-namespace design sketched for a later append-only gate.
- **D5** `SYS_SOCKETPAIR 77`: AF_UNIX; STREAM/DGRAM/SEQPACKET; proto 0; SOCK_CLOEXEC recorded; both
  ends in the caller's group table (pipe() shape).
- **D6** fork copies fds 1:1 (+`usock_reopen`); spawn2 maps only its 3 fds; default-stderr exclusion
  extended to IPC endpoints; `fd_flags[]` (FD_CLOEXEC) storage added, honored by P5 execve; arbitrary
  fd-number inheritance for the WK launcher = fork+execve (P5) — recorded, not solved here.
- **D7** sendmsg/recvmsg subset: connected only; ≤8 iovs; flags = MSG_NOSIGNAL/MSG_DONTWAIT send,
  MSG_DONTWAIT/MSG_CMSG_CLOEXEC recv; control data on STREAM → EOPNOTSUPP; INET sockets → EOPNOTSUPP.
- **D8** SCM_RIGHTS: 1..16 fds, all-or-nothing; refs move enqueue→install, released on discard; receiver
  table full → -EMFILE without consuming; rights = sender-group fd openness (single-user OS); fresh
  receiver fd numbers.
- **D9** recvmsg tails: MSG_TRUNC data truncation, MSG_CTRUNC + close-excess control truncation, NULL
  control buffer discards fds; blocking parks.
- **D10** `SYS_POLL 76` per Linux values; per-fd POLLNVAL, whole-call EBADF never; nfds ≤ 256; shared
  park/deadline machinery with select (select outputs bit-identical); probe gains a hup channel.
- **D11** epoll: not implemented, not planned; poll is the supported multiplexer (evidence §5.3).
- **D12** `SYS_MAX` 75 → 79 at the P4 gate (order-independent vs P2's 82); rows 76–79 frozen in place;
  the +2 renumber history is documented in §6.1.
- **D13** errno += EMSGSIZE/ENOTCONN; fcntl F_GETFD/F_SETFD all fds + F_GETFL/F_SETFL for unix; SO_TYPE/
  SO_ERROR(0)/SO_DOMAIN real for unix; getsockname/getpeername flagged as the next append (GLib gate);
  sys_read/sys_write error carve-out as §6.5.

**Open questions**
- **OQ1** Freeze consent: (a) rows 76–79 + `SYS_MAX` 75→79 (or top-up after P2); (b) errno additions
  EMSGSIZE 90 / ENOTCONN 107; (c) the §6.5 trap.c error carve-out in both arches; (d) the §6.6
  SOCK2TST expectation flip (L9 file — P4 lane proposes, integrator applies or delegates).
- **OQ2** Append-only naming policy for the deferred surface: recommend *no reservation now* — the
  first consumer appends at the then-current top (order: GETSOCKNAME, SHUTDOWN, BIND, CONNECT_UNIX,
  LISTEN, ACCEPT), recorded in §A.1b at that gate. Alternative (reserve 87+ immediately) rejected as
  churn against P5's pending SIGRETURN decision.
- **OQ3** Pool/budget consent: `MAX_UNIX_PAIRS 16`, 64 KiB/direction, message cap 8192, fd cap 16 —
  ~2 MiB bss typical case; confirm the numbers as gate constants (raise with P8 torture evidence,
  including the MAX_OPEN_FDS 32 question for WK).
- **OQ4** Ownership routing (P1 OQ2 / P2 OQ6 precedent): the P4 lane applies its own edits to the
  L1-owned `fs.c`/`fs.h` and the L3-owned `src/user/libc.c`, `src/user_include/libc.h` + the new
  sysroot headers; integrator resolves merges.
- **OQ5** P5 interface note: P4 ships EPIPE-without-signal; confirm P5's SIGPIPE work keeps the -EPIPE
  return and adds the signal, and that P5 execve owns FD_CLOEXEC honoring (fd_flags[] contract).

## 11. Integrator review (consent record — 2026-09-30)

Reviewed at merged base `9a939e5` (P2 S1–S3 on main); the note's grounding
claims spot-verified against the tree: the `sock2_test.c:57` AF_UNIX
assertion, `K_S_IFSOCK`, `FD_SETSIZE 256`, `MAX_OPEN_FDS 32` /
`MAX_GLOBAL_FILES 128`, the read/write legacy returns (-1 only) and the
`f1_probe_fd` select engine.  Verdict: **consented as designed, with one
amendment and one order record.**

- **OQ1(a) rows 76–79 + `SYS_MAX` — approved.**  Order record: P2's write
  landed first, so `SYS_MAX` was already 82 at the base; the P4 freeze is a
  **top-up no-op** (no `SYS_MAX` change).  Rows/args exactly as §6.2.  The
  four rows are defined in `src/include/syscall.h` now (single-writer edit
  with the P5 block, ahead of Wave 1f); the P4 gate record then only adds
  test evidence.
- **OQ1(b) errno — approved.**  `EMSGSIZE 90`, `ENOTCONN 107` (both
  verified free against `src/include/errno.h`); defined now with this
  review.
- **OQ1(c) trap.c carve-out — approved.**  `ret == -1 → -EBADF` stays for
  the legacy backends; `ret < -1` (other than the -2 restart) passes
  through, both arches; existing backends verified to return only -1/-2.
- **OQ1(d) SOCK2TST expectation flip — approved; the P4 impl lane owns the
  §6.6 edit** (file is L9-owned; the lane applies it, the integrator
  records it in §11 at the gate).
- **OQ2 naming policy — approved as recommended:** no reservation; append
  order `GETSOCKNAME` → `SHUTDOWN` → `BIND`/`CONNECT_UNIX`/`LISTEN`/
  `ACCEPT` at the consuming gate; recorded in §A.1b at that gate.
- **OQ3 pool/budget — approved as gate constants** (16 pairs; 64 KiB per
  direction; 8192-byte message cap; 16 fds per message).  Revisit with P8
  torture evidence, including the `MAX_OPEN_FDS 32` question for WK.
- **OQ4 ownership — approved** (P4 lane owns its `fs.c`/`fs.h`,
  `src/user/libc.c`, `src/user_include/libc.h` and the new sysroot
  headers).  Wave-1f coordination note: the P2 S4/S5 lane and the P5 impl
  lane also touch `fs.h`/`fs.c` (FILE_TYPE_MEMFD; exec fd sweep) — all
  additive edits; the parent reconciles merges.
- **OQ5 P5 interface — confirmed:** P4 ships `-EPIPE` without a signal; P5
  keeps the return value and adds SIGPIPE; P5 owns FD_CLOEXEC honoring at
  exec.
- **Amendment (cross-note review): `fd_flags[]` → `fd_cloexec` mask.**  D6's
  `uint8_t fd_flags[32]` and the P5 note's `uint32_t fd_cloexec` were
  independently designed for the same state; the **mask is canonical**
  (`struct process.fd_cloexec`, bit i = fd i; pre-frozen on main with this
  review).  fcntl `F_GETFD`/`F_SETFD` read/write bit 0; `dup2`/`dup` clear
  the bit on the new fd; spawn2's 0/1/2 grants clear their bits; the exec
  sweep is a bit loop.  No further semantic change.

Implementation starts in Wave 1f (`browser/l2-p4-impl`) at this review's
base (`SYS_MAX` 86 / rows 76–86 / errno / `fd_cloexec` pre-landed on main).
