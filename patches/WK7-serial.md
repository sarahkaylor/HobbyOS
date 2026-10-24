# WK-7 — Serial console line splice: kernel `[ALIVE-SET]` watchdog cutting `[WIN]` lines in half

**Patch:** `HobbyOS/patches/WK7-serial.patch` (2 hunks, 2 repositories)
**Type:** kernel + browser-driver console locking fix (mechanism (i): missing shared console lock).
**Status:** validated read-only with `git apply --check` in both repos.
**Carry note (repos are under concurrent parallel-agent edit):** while producing this patch, sibling agents briefly staged/removed uncommitted `fs-r16` census changes (revealed as `char line[2048]` + a `[WIN] doc title` CString-lifetime fix) in the same `WK5WindowDriver.cpp`, and one transient write leaked foreign `fs-r16` hunks into an intermediate copy of this patch. The delivered patch was therefore regenerated deterministically from the **committed HEAD blobs** (`git show HEAD:...`), validated against the **index** (`git apply --cached --check`, index == HEAD and stable under worktree churn) — both files rc=0 — and confined strictly to the `wk5emit`/`console_write_user` console-lock path. It does **not** touch the sibling's `fs-r16` territory (link-census JS, `line[2048]`, doc-title CString): that is a *separate* stale-buffer (ii)-class bug at the `[WIN] doc title` call site, distinct from this task's `[ALIVE-SET]` splice, and should be kept if the sibling's changes return.
**Verification of the patch's own logic:** also re-ran `git apply --check` (worktree) for both hunks — rc=0 at check time; if the worktree has since moved, re-apply to the index/HEAD baseline instead.

---

## 1. The two writers

| Writer | Repo / file | Emitter | Console path | Lock held |
|---|---|---|---|---|
| Kernel scheduler watchdog `[ALIVE-SET]` | `HobbyOS/src/kernel/process.c` | header `uart_puts("[ALIVE-SET] no READY work ...\n")` **L1706-1709**; per-slot lines via `db_s/db_i/db_emit` **L1719-1746** (buffer `struct db_buf`, **L76-92**) | `uart_puts` → `HobbyOS/src/kernel/arch/x64/uart.c` **L191-197** | **`uart_lock` held once for the whole string** (`spinlock_acquire_irqsave` … `release_irqrestore`) |
| Browser driver `[WIN]` / `[IMG…]` census lines | `webkit-hobbyos/HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp` | `wk5emit` **L388-399** (formats into stack `char line[512]` **L390**, then two `print_console` calls **L396-397**, guarded by `s_wk5EmitMu` **L385**) | `print_console` → `HobbyOS/src/user/libc.c` **L241-246** → `SYS_WRITE_CONSOLE` → `HobbyOS/src/kernel/arch/x64/trap.c` `sys_write_console` **L142-148** → `console_write_user` `HobbyOS/src/kernel/fs.c` **L1464-1474** | **`uart_lock` re-acquired and released PER BYTE** (loop calls `uart_putc`, uart.c **L181-185**) — no whole-call lock |

Both sides share one physical serial device (COM1) and one kernel spinlock (`uart_lock`, `src/kernel/arch/x64/uart.c` **L8**), but the **browser's write is byte-granular** while the kernel's is whole-line. The driver's own `s_wk5EmitMu` (a user pthread mutex) serializes the driver's two threads against each other only; the kernel ALIVE-SET writer never takes it.

## 2. The real race mechanism — with proof: motherboard (i) kernel splice, not (ii) stale-buffer

**Mechanism (i), proven.** `console_write_user` (fs.c L1464-1474) emits the user's buffer with `for (i…) uart_putc(s[i])`. Each `uart_putc` acquires and releases `uart_lock`. So between every pair of bytes of a `[WIN]` line, `uart_lock` is free. The kernel scheduler's ALIVE-SET path takes `uart_lock` once for the whole line (`uart_puts`, uart.c L191-197) and can therefore start **between any two bytes** of the browser's in-flight line, emit its complete line, and let the browser resume — exactly the observed splice:

```
[WIN] reblit cached [ALIVE-SET] no READY work, yet the scheduler counts live work (the predicate…)\r\n
[WIN] tick> n=47[ALIVE-SET] no READY work, yet the scheduler counts live work…\r\n
```

**Hard proof from the logs (asymmetry is decisive):**

| Log | mid-line `[ALIVE-SET]` splices (inside non-ALIVE lines) | `[ALIVE-SET]` lines with mid-line foreign tags |
|---|---|---|
| `wklane-agentc-s3/serial.log` | **229** (215 no-prefix/continued lines, 9 `[DOM]`, 5 `[WIN]`) | **0** of 1441 |
| `wklane-probe-https/serial.log` (Google links line incl.) | **195** (181 no-prefix, 8 `[WIN]`, 5 `[DOM]`, 1 `[SCRIPT]`) | **0** of 3080 |

- **424** spliced lines in total, every one **ALIVE-SET bytes inside another line — never the reverse.** That one direction is only possible if the kernel's ALIVE-SET write is atomic (whole-string `uart_puts` under `uart_lock`) and the browser's is not (per-byte `uart_putc`). If the kernel write were not atomic, foreign bytes would appear inside ALIVE-SET lines too — 0 of 4,521 do.
- **Mechanism (ii) stale-buffer reuse is contradicted by inspection:** `wk5emit` formats into `char line[512]` — a **stack local, private to each call** (WK5WindowDriver.cpp **L390**). There is no shared/static/`malloc`'d buffer and no `realloc` anywhere in the path, so another thread cannot corrupt it. The only shared state is `s_wk5EmitMu`, which is correctly held across the two `print_console` calls (L395-398). A driver-internal race would corrupt the driver's own bytes (WIN text inside WIN lines or garbage), but the spliced payload is always the kernel's `[ALIVE-SET]` text — only the kernel prints it. => Kernel preemption at a per-byte boundary. This also falsifies the stale comment in the driver (L377-378) claiming *"The guest kernel's console write is a byte-at-a-time uart loop with no per-write lock"* — the kernel's `uart_puts` is whole-string locked; it is the **user console path** (`console_write_user`) that is byte-at-a-time.
- Bonus fact confirming the same: `wk5emit` calls `print_console(line)` **then** `print_console("\n")` as two syscalls, so the newline itself is a third-party insertion point — the splice sits between the last text char and the CRLF.
- Distinct-but-adjacent: the sibling's `fs-r16` investigation found a **separate** (ii)-class stale-buffer bug at the `[WIN] doc title=%s` call site (temporary `CString::utf8().data()` passed through varargs dies before `%s` reads it — a *different* corruption of census *content*, not the `[ALIVE-SET]` splice addressed here).

## 3. The fix + lock ordering

Smallest possible change: make a *line* own `uart_lock` for one whole emission on **both** sides. Two tiny hunks:

**Hunk A — kernel `HobbyOS/src/kernel/fs.c`, `console_write_user` (L1465-1484):** acquire `uart_lock` **once** for the whole user write and emit through the lock-free body `uart_putc_raw` (uart.c L94-96) inside the held region, mirroring `uart_puts`/`print_int`. Every `print_console()` call in the browser thus becomes one atomic `[WIN]` region — the kernel ALIVE-SET `uart_puts` can no longer land between its bytes.

```c
extern spinlock_t uart_lock;
extern void uart_putc_raw(char c);
...
uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
for (int i = 0; i < size; i++)
    uart_putc_raw(s[i]);
spinlock_release_irqrestore(&uart_lock, flags);
```

**Hunk B — driver `WK5WindowDriver.cpp`, `wk5emit` (L394-411):** emit **text + `'\n'` in one `print_console()` call** (append `'\n'` to the same stack buffer, guarded against double-newline), so a logical `[WIN]` line is one `SYS_WRITE_CONSOLE` = one `uart_lock` region. The separate `print_console("\n")` call is removed. The stale comment block (L375-388) is rewritten with the real mechanism + lock ordering.

**Lock ordering / no deadlock:**
- Only nested order in the system is `s_wk5EmitMu` (user pthread mutex) → `uart_lock` (kernel spinlock taken inside the SYS_WRITE_CONSOLE syscall). The kernel **never acquires** `s_wk5EmitMu` — it is a user-space mutex invisible to the kernel — so there is no cycle: `uart_lock` holders (kernel ALIVE-SET, browser console_write_user) never wait on anything a console writer holds, and always release in bounded time (existing TX-empty spin, ~µs at 38400 baud, unchanged).
- The stall/deadlock watchdog never blocks: it prints through **lock-free** `uart_puts_raw` (uart.c **L170-175**) which bypasses `uart_lock`, so halts and [LOCKFOREVER] dumps still reach the wire even while a console line owns the lock.
- No busy-wait added, no spare lock invented, HARD RULEs respected: watchdog **not** disabled (ALIVE-SET still prints), no spin beyond the pre-existing bounded per-char TX-empty wait.

## 4. Verification plan

`git apply --check` already passed read-only for both hunks in their repos; apply order:
```
git -C ~/Documents/GitHub/HobbyOS     apply --check patches/WK7-serial.patch   # fs.c hunk
git -C ~/Documents/GitHub/webkit-hobbyos apply --check <patch>                 # driver hunk
```
Then (build + run is outside this read-only task):
1. Boot wave with the browser, run a **Wikipedia load** (the `wklane-probe-https` scenario), capture `-serial file:serial.log`.
2. **Zero mid-line splices** — must be `0` (was 5/8):
   ```
   python3 - <<'EOF'
   import sys
   bad=0
   for line in open("serial.log","rb"):
       ls=line.lstrip()
       if b"[ALIVE-SET]" in line and not ls.startswith((b"[ALIVE-SET]",b"  [ALIVE-SET]")):
           bad+=1
   print("mid-line splices:", bad)   # expect 0
   EOF
   ```
   Equivalently `grep -cP '^(?!\[ALIVE-SET\]|  \[ALIVE-SET\])[^\r\n]*\[ALIVE-SET\]' serial.log` → `0`.
3. **Watchdog untouched:** `[ALIVE-SET]` lines and per-slot `  [ALIVE-SET] slot=…` lines must still appear in the log, with **0 corrupted ALIVE-SET lines** (unchanged, since kernel lines were already atomic).
4. **Line-granular interleave (allowed residual):** whole `[ALIVE-SET]` blocks may still print *between* two different `[WIN]` lines (kernel releases `uart_lock` between ALIVE-SET lines). That is readable and not the bug.

## Patch contents

- `HobbyOS/src/kernel/fs.c` — `console_write_user`: whole-write `uart_lock` (per-byte `uart_putc` → held `uart_putc_raw`); comment updated.
- `webkit-hobbyos/HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp` — `wk5emit`: single `print_console(line+"\n")` call; stale kernel-console comment corrected + lock-ordering documented.
