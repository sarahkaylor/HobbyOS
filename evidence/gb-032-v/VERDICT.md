# GB-032-V — VERDICT: **RED**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-25 ~07:10Z.
Verifies GB-032 (#33), ship = the SDL test app merged to `main` at `e2ca063` (PR #82).
Gate revision: `origin/main` **`b874d54`** (`git rev-parse HEAD` in the run worktree), intel guest,
QEMU-KVM (the tier printed its mode). I did not author GB-032 (`wgfx` did) — independent gate.

## One line

Dispatch is no longer the problem — GB-072 landed and the staged app **is** dispatched, mapped and
entered — but it dies before `main()` in `Fatal glibc error: Cannot allocate TLS block`, and the
shipped artifact is a **glibc** static-PIE that never touches `/dev/fb0` or any input device, so the
card's "draws + echoes input" clauses cannot be met by it even after the loader is fixed.

## Clause table (the four clauses the card names)

| # | Clause | Result |
|---|---|---|
| 1 | app's arrival line in the intel guest console **and** the process in the wave's exited set | **PASS** — measured |
| 2 | it **draws**: surface blitted, whole-frame margin vs desktop, or the app's own framebuffer PASS line on serial | **FAIL** — never reached `main()`; and by construction it draws into a local array, never the framebuffer |
| 3 | it **echoes input**: a scripted QMP key/pointer event produces the echo line in the app's own output | **FAIL** — never reached `main()`; and the echo lines are unconditional literals, produced with no input at all (host control below), so no event can drive them |
| 4 | it **quits clean** and the exit line is visible | **FAIL** — fatal TLS allocation error, then a user memory fault, process killed |

## Evidence — dispatch works (clause 1)

Staged exactly as the card asks — `T3_STAGE` copies the built app onto the boot image and a
`WAVE.CFG` names **only** it, so the wave is that manifest (GB-072):

```
$ T3_STAGE='mcopy -o -i "$ROOT/disk.img" "$ROOT/obj/sdl_test_app_pie" ::SDLAPP.BIN &&
             mcopy -o -i "$ROOT/disk.img" .../WAVE_SDLAPP.CFG ::WAVE.CFG && mdir ... | grep -i sdlapp'
             bash tools/t3_tier.sh intel
```

Guest console (`evidence/gb-032-v/01-t3-intel-sdlapp.log`):

```
[WAVE-MANIFEST] SDLAPP.BIN
0[WAVE-TRY] SDLAPP.BIN

Loading program for scheduler (v2 AS): SDLAPP.BIN
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=2 block_idx=-1
/SDLAPP.BIN
v2 loader: PID=2 entry=0x1000007760 sp=0x1800000000 resident=215 tables=6
[ARRIVED] SDLAPP.BIN rc>0
Fatal glibc error: Cannot allocate TLS block
[KERNEL] pid=2 (SDLAPP.BIN) memory fault VA=0x0000000000000CF8 PC=0x000000100000A997 rw=w  in=PROT region=NONE cr3frame=0x0000000020C06000 asroot=0x0000000020C06000 sys=231 ret=0x000000000000002D rbx=0x00000000000003C0 rdi=0x00000000000003C8 rsi=0x00000000000003C0 rbp=0x00000017FFFFFEF0 rbp0=0x00000017FFFFFF40 ret=0x000000100000A66B -> killed
[KERNEL] Process 2: SDLAPP.BIN exited unexpectedly or gracefully.
[EXITPC] pid=2 last_user_pc=0x100000AA44
[HALT-CD] cpu=1 free=63 alloc=0 exited=1 running=0 blocked=0 other=0
System halt from CPU 1.
```

So: **[WAVE-MANIFEST] fires, the v2 loader maps the ELF at entry `0x1000007760`, `[ARRIVED]` prints,
and the process appears in the exited census (`exited=1`)**. The 2026-10-21 RED's ground (GB-072's
wall: staged, never dispatched) is **gone** — this is a different failure, further along.

Tier line for that run (the tier itself is red for wave-size, expected with a one-member manifest):

```
T3_intel: 119 console lines, 0 FAIL lines, 1 user memory fault records across 1 processes, 1 programs exited, halted=1
T3_intel: FAIL — only 1 programs exited (floor 55): too little work for a complete wave, so this cannot be a pass
T3_intel EXIT=1
```

## Evidence — the app is a **glibc** static-PIE, not the static-musl build the card assumes

`make obj/sdl_test_app_pie` runs `clang --target=x86_64-linux-musl -fPIE -static-pie`, but on this
host that clang resolves the **host glibc** crt/lib paths, so the artifact is glibc:

```
$ clang -### --target=x86_64-linux-musl -fPIE -static-pie -Isrc/include tools/sdl/sdl_test_app.c -o /tmp/probe_pie
"-L/lib/x86_64-linux-gnu"
"-L/usr/lib/gcc/x86_64-linux-gnu/15"
"/lib/x86_64-linux-gnu/rcrt1.o"
"/usr/lib/gcc/x86_64-linux-gnu/15/crtbeginS.o"
```
(`evidence/gb-032-v/02-clang-link-line.log`; no `musl` path and no musl sysroot appears in the link line)

```
$ strings -a obj/sdl_test_app_pie | grep -iE "Fatal glibc|GLIBC_PRIVATE" | sort -u | head
Fatal glibc error: %s:%s (%s): assertion failed: %s
Fatal glibc error: Cannot allocate TLS block
Fatal glibc error: Internal __libc_message error. Too many arguments.
GLIBC_PRIVATE
```

`Fatal glibc error:` is glibc's own prefix; the guest's dying line is glibc's `Cannot allocate TLS
block` at VA `0xCF8` (variant-II TCB/`%fs` allocation), i.e. a **glibc static-PIE** reaching a TLS
path the compat layer does not provide. The card's dependency (GB-031, the static-musl SDL profile)
is still an open PR (#161) — this is the same class of gap, measured in the guest.

## Evidence — clauses 2 and 3 cannot be met by this artifact even if it ran

The app has **no device access at all** — `grep -nE "open|mmap|ioctl|read\(|/dev/" tools/sdl/sdl_test_app.c`
returns nothing. It draws into a local array and echoes **literal** events:

```
81:  static uint32_t fb[64 * 48];           /* demo window, maps the same API */
98:  struct hb_input_event key = {0, 0, HB_EV_KEY, 30, 1};   /* KEY_A down */
99:  struct hb_input_event abs = {0, 0, HB_EV_ABS, 0, 512};  /* ABS_X pos */
100:  sdl_echo_input(&key, line, sizeof line);
102:  sdl_echo_input(&abs, line, sizeof line);
```

Measured control, no input source connected (`stdin=/dev/null`, nothing driving an event device):

```
$ ./obj/sdl_test_app_pie < /dev/null
sdl test app: drew 64x48 window, blitted 8x8 tile @(10,10)
sdl test app echo: KEY 1e=1
sdl test app echo: ABS 00=512
rc=0
```
(`evidence/gb-032-v/04-host-echo-not-input-driven.log`)

The echo lines are therefore **constants**, not an echo: a QMP `input-send-event` cannot change them.
Clause 3 as written (event -> app's own echo line) is unmeasurable against this artifact, and
clause 2's "frame differs from the desktop" is unmeasurable because the app never writes the
framebuffer — it writes `static uint32_t fb[64*48]` in its own `.bss`.

## Control — the image and the harness are sound

Same image, same tier, **no** manifest (default literal wave), same revision:

```
T3_intel: 41045 console lines, 290 FAIL lines, 25 user memory fault records across 20 processes, 86 programs exited, halted=1
T3_intel: FAIL — 1 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
T3_intel: FAIL — 290 FAIL line(s); first 20:
T3_intel EXIT=1
```
(`evidence/gb-032-v/03-t3-intel-default-wave.log`)

86 programs run — so the boot, the loader and the serial capture work, and `SDLAPP.BIN`'s death is
its own. **Not counted against this card:** that run's 290 FAIL lines / 25 user memory faults /
fatal kernel fault are `main`'s known intel T3 state (GB-057/GB-058/GB-095 family), measured here
only as the harness control.

## One instrument contradiction worth naming (not a verdict cell)

The in-tree `MODE=test` hook still prints its **own** claim that the compat exec leg is closed:

```
[GB-032] SDL test-app exec: SKIP (GB-013 compat exec leg not open; app + static-PIE ELF props proven on host by sdl_test_app_test_host)
```

while, forty lines later on the same boot, the wave manifest dispatched the same binary and it
reached user mode. The hook's guard is now stale — it would report "skipped with a reason" for an
exec leg that demonstrably works. That line should be removed or made to reflect the real state, or
a reader will conclude the opposite of what the serial shows.

## What has to change before a GREEN is possible

1. **Build the artifact as what the card says**: a real static-musl static-PIE (pin a musl sysroot /
   use `musl-gcc`, and assert it — `strings | grep -q musl` — in the build or the host test, the way
   GB-031's profile does). Today `--target=x86_64-linux-musl` silently links glibc.
2. **Make it device-backed**, or change the card: open `/dev/fb0`, blit to the mapped surface, read
   `/dev/input/event*` and print the echo *when the event arrives*. Otherwise clauses 2 and 3 are
   untestable by construction, no matter how green the loader gets.
3. Then the loader's glibc-TLS gap (`Cannot allocate TLS block` at VA `0xCF8`) only matters if the
   app stays glibc — with a musl artifact it may not be on the path at all. Fix (1) first.

## Reproduce

```
git worktree add /tmp/gb032v b874d54 && cd /tmp/gb032v && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=intel MODE=test hobbyos.elf disk.img
make obj/sdl_test_app_pie                       # glibc static-PIE on this host (see log 02)
printf 'SDLAPP.BIN\n' > /tmp/WAVE_SDLAPP.CFG
T3_LOG=$PWD/evidence/gb-032-v/01-t3-intel-sdlapp.log T3_TIMEOUT=900 \
T3_STAGE='mcopy -o -i "$ROOT/disk.img" "$ROOT/obj/sdl_test_app_pie" ::SDLAPP.BIN &&
          mcopy -o -i "$ROOT/disk.img" /tmp/WAVE_SDLAPP.CFG ::WAVE.CFG' \
  bash tools/t3_tier.sh intel
```
