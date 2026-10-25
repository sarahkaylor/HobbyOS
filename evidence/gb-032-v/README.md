# GB-032-V — gate evidence (VERDICT: RED, clause 1 PASS / 2, 3, 4 FAIL)

Ticket: GB-032-V (#108) — Verify GB-032: the static-musl SDL test app draws and echoes input
Verifies: GB-032 (#33), shipped on `main` @ `9db1388` (HEAD of this worktree; the 07:09Z RED on this
card was measured at `b874d54`, so this is a gate at a newer revision, not a re-report)
Verifier: farm Verifier, DEEP (DeepSeek v4.1 Flash, dgx1), host `swarm-farm-2`, own worktree
Run date: 2026-10-25 ~08:30–08:40Z. Independence: I authored neither GB-032 nor `sdl_test_app.c`.

Artifact under test: `tools/sdl/sdl_test_app.c` sha256 `6623dcf52b2c1ed4…`, unchanged since `ee1a1d2`
(2026-10-20); built by `make obj/sdl_test_app_pie`.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | app's arrival line in the intel guest console + process in the wave's exited set | **PASS** (measured) |
| 2 | draws: surface blitted with a whole-frame margin vs the desktop, or the app's own framebuffer PASS line | **FAIL** — never reaches `main()`; and by construction it draws into its own `.bss`, not the framebuffer |
| 3 | scripted QMP `input-send-event` produces the echo line in the app's own output | **FAIL** — never reaches `main()`; the echo lines are literals (host control below) |
| 4 | quits clean, exit line visible | **FAIL** — fatal glibc TLS allocation, then a user memory fault, killed |

## Clause 1 PASS — dispatch works (GB-072's wall stays gone)

Staged with `T3_STAGE` (app copied to `::SDLAPP.BIN`, `::WAVE.CFG` naming only it), intel tier:

```
[WAVE-MANIFEST] SDLAPP.BIN
[WAVE-TRY] SDLAPP.BIN
Loading program for scheduler (v2 AS): SDLAPP.BIN
[ARRIVED] SDLAPP.BIN rc>0
Fatal glibc error: Cannot allocate TLS block
[KERNEL] pid=2 (SDLAPP.BIN) memory fault VA=0x0000000000000CF8 PC=0x000000100000A997 rw=w in=PROT region=NONE ... -> killed
[KERNEL] Process 2: SDLAPP.BIN exited unexpectedly or gracefully.
[EXITPC] pid=2 last_user_pc=0x100000AA44
[HALT-CD] cpu=1 free=63 alloc=0 exited=1 running=0 blocked=0 other=0
System halt from CPU 1.
```
Tier line: `T3_intel: 119 console lines, 0 FAIL lines, 1 user memory fault records across 1 processes,
1 programs exited, halted=1` (the tier exits 1 only on its completeness floor — a one-member manifest
must trip it; that is not this card's criterion).

## Clauses 2–4 FAIL — the artifact dies before `main()`, and could not satisfy 2–3 anyway

The card's static-musl premise is false in the shipped artifact:

```
$ file obj/sdl_test_app_pie
ELF 64-bit LSB pie executable, x86-64, ... static-pie linked, for GNU/Linux 3.2.0, not stripped
$ strings obj/sdl_test_app_pie | grep -cE "GNU C Library|GLIBC_PRIVATE|Fatal glibc error"   -> 8
$ strings obj/sdl_test_app_pie | grep -ciE "musl"                                           -> 0
```
`--target=x86_64-linux-musl` links the host glibc, and the guest dies on the first thing that needs a
TCB: `Fatal glibc error: Cannot allocate TLS block` at VA `0xCF8` (variant-II TCB / `%fs`). The musl
profile is PR #169 (`ticket/31-musl`), unmerged; it is not this card's artifact.

Host control — the same binary on Linux with **no input source at all**:
```
$ ./obj/sdl_test_app_pie < /dev/null
sdl test app: drew 64x48 window, blitted 8x8 tile @(10,10)
sdl test app echo: KEY 1e=1
sdl test app echo: ABS 00=512
rc=0
```
Identical text to what a QMP-driven run would show, because it is literal: `main()` builds
`struct hb_input_event key = {0, 0, HB_EV_KEY, 30, 1}` and `abs = {0, 0, HB_EV_ABS, 0, 512}` and prints
them (`tools/sdl/sdl_test_app.c:96-102`). Nothing reads a device:
`grep -nE "open|mmap|ioctl|read\(|/dev/" tools/sdl/sdl_test_app.c` → no match. The "window" is
`static uint32_t fb[64*48]` in its own `.bss` (`:81`) — a framebuffer assertion cannot come from it.

So clauses 2 and 3 are unmeasurable *and* unsatisfiable against this artifact: even if the glibc TLS
path were fixed, a keypress could not change its output.

## To reach GREEN (unchanged from the 07:09Z RED, now confirmed at a newer HEAD)

1. Ship a genuine static-musl static-PIE (pin a musl sysroot / `musl-gcc`, and **assert** it —
   `strings | grep -q musl` — in the build or the host test). PR #169 does this for GB-031.
2. Make the app device-backed — open `/dev/fb0`, blit into the mapped surface, read
   `/dev/input/event*` and print the echo when the event actually arrives — or amend the card, because
   clauses 2–3 are untestable by construction against this demo.
3. Then the glibc-TLS gap at VA `0xCF8` matters; with a musl artifact it may not be on the path at all.
