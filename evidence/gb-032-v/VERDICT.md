# GB-032-V — verdict: RED (ground: GB-072, exactly as this card instructs)

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-21.
Tree: `origin/main` @ `b1e0827`, clean worktree; `disk.img` built here; intel wave run here.

The card's own rule: *"If the binary is not dispatched, the verdict is RED with GB-072 named as the
ground and the card notes which of 1–4 could not be measured."* That is what was measured.

## The measurement: staged, present on the image, never dispatched

```
$ make disk.img
make disk.img EXIT=0

$ T3_STAGE='mcopy -o -i "$ROOT/disk.img" obj/intel/t4_probe.elf ::T4PROBE.BIN' bash tools/t3_tier.sh intel
=== T3 (intel): stage ===
T3_intel: 924 console lines, 4 FAIL lines, 60 user memory fault records across 52 processes,
          66 programs exited, halted=1
T3_intel: FAIL — 4 FAIL line(s) …        # the known graphics rows (GB-065), not this ticket
t3 intel EXIT=1

$ mdir -i disk.img ::/ | grep -i t4probe
T4PROBE  BIN     23904 2026-10-21   5:55      # the file IS on the image that booted

$ grep -ac T4PROBE obj/t3_intel.log
0                                           # … and the guest never mentions it
```

66 programs exited in that wave; the staged binary is not one of them and leaves no arrival line. This
is GB-072's wall reproduced first-hand on the sha named, not borrowed from another session.

## Why the SDL app specifically cannot be measured

```
$ git ls-files | grep -i sdl
tools/sdl/README.md
tools/sdl/build-sdl2-static-musl.sh
tools/sdl/sdl2_static_test.c
$ grep -n "SDL" src/kernel/main.c
(no match — no SDL entry in the boot wave)
```

What is on `main` is GB-031's build recipe and a host-side static-musl test source; there is no HobbyOS
SDL app binary in the tree or on the image, and the wave has no entry for one. So even with GB-072 fixed,
this card needs the app staged *and* named in the dispatch set before criteria 1–4 can be touched.

## DoD items — all four unmeasurable

| # | criterion | status |
|---|---|---|
| 1 | arrival line + in the exited set | **not measurable** — binary staged on the image, never dispatched (GB-072) |
| 2 | draws (frame differs from desktop, whole-frame sample) | **not measurable** — no process, no frame |
| 3 | echoes a scripted QMP key in its own output | **not measurable** |
| 4 | quits clean, exit line visible | **not measurable** |

No host-mock substitute was used anywhere above (`src/host/t4_compat_probe.c` was not consulted for the
guest leg), as the card requires.

## What closes it

1. GB-072: a staged program must be dispatched (manifest or explicit wave entry) — this is the wall.
2. Get the SDL static-musl app built into the tree/image and into the dispatch set.
3. Then re-mint this card: criteria 2–4 all need a running process, and only criterion 2 even has a
   partial path today (the app's own framebuffer PASS line, which no in-tree app prints yet).
