# GB-059-V — verdict: RED (clause 1 compat half unmeasurable on any arch; clauses 2–4 pass)

Gate revision: `origin/main` @ **b4e4524**, fresh detached worktree
`/home/sarah/Documents/GitHub/HobbyOS/.worktrees/verify-gb059v` (clause 4).
Host: farm-3 VM, 32 cores, QEMU system emulation. Date: 2026-10-27T08:2x–08:4xZ.
Verifier: DEEP Verifier session `wgames-verify-gb059v`. I did not implement this card.

The card was promoted `Backlog,blocked` → `Ready` on 2026-10-27T08:11Z by the Coordinator, on the
stated ground that *"compat half unmeasurable" is not true any more* because GB-066 landed an
in-guest dispatched static-PIE probe. **Measured: that ground does not hold for this card.** A probe
is not a title, there is no compat title on the shipped image, and the rig cannot launch one on the
only architecture where a compat ELF can load. Details and quotes below.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | Both titles launch **by name** from the parsed `[MENU]` dump and produce a live frame | **RED** — native half measured PASS (frame present, not animating); compat half unmeasurable: 0 ELF on the image, hand-staged ELF wiped by `make run`, and 0 `[LAUNCH]` on intel where compat ELFs can alone load |
| 2 | The four hand-rolled runners are deleted (or each survivor named) | **PASS** — `git ls-files` match count 0 |
| 3 | No magic launcher slot index anywhere in the rig or its callers | **PASS with residual** — rig clean; caller `run_evidence_xnibbler.sh:21` still passes a stale `--slot 18` (actual slot 20), overridden by the rig |
| 4 | The run is done from a worktree, not the main checkout | **PASS** — detached worktree of `origin/main` |

## Clause 2 — no hand-rolled runners

```
$ git ls-files | grep -E 'run_(doom|stardust|xnibbler|xcrypt)_test\.py'
$ echo $?
1
```
No output, exit 1: none of the four survive. (Note: unrelated `run_*_test.py` files for desktop
apps still exist at the repo root; they are not the four T5 harnesses this clause names.)

## Clause 3 — no magic slot index in the rig

```
$ grep -nE 'slot\s*=\s*[0-9]+|index\s*=\s*[0-9]+' tools/evidence_rig.py
$ echo $?
1
```
Residual, recorded rather than passed silently: the clause says "the rig **or its callers**", and one
caller still carries a literal index:

```
$ git grep -n -- '--slot'
run_evidence_xnibbler.sh:21:  --game xnibbler --bin XNIBBLER.BIN --slot 18 --arch "$ARCH" \
```
`18` is *stale* on this tree (XNIBBLER.BIN is at **20**). It cannot cause a false launch — the rig
resolves by name and only warns (`resolve_launch_slot()`, `tools/evidence_rig.py:153-167`) — but the
literal should be deleted, and the rig's own test covers the override
(`tools/evidence_rig_test.py:112`).

## Clause 1, native half — launches by name, frame is the game's own

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch arm --run-seconds 10
[rig] booting arm QEMU, capturing to evidence/XNIBBLER/20261027T082708Z
[rig] XNIBBLER.BIN slot: found by name = 20 (launching 20, by name)
[rig] evidence bundle: evidence/XNIBBLER/20261027T082708Z
[rig] report: XNIBBLER-EVIDENCE-REPORT.md
real    0m27.809s
```
serial (`frames-arm/serial.log`):
```
[MENU] count=128
[MENU] 20=XNIBBLER.BIN
[LAUNCH] XNIBBLER.BIN
[WAVE-TRY] XNIBBLER.BIN
[ARRIVED] XNIBBLER.BIN rc>0
[APP] XNIBBLER started
```
`frames-arm/gameplay.png` (viewed): a window titled `xnibbler` with the play field, `SCORE 0`, a green
3-block snake and an orange food square — the game's own rendering, not a stale desktop
(97.74% of pixels differ from the desktop control frame; `cmp_frames.py`).

**"Live" is the weak word here, and I measured it rather than assuming it.** Sampling the launched
game four times over 9 s (`04-liveness-native-arm-keys.log`, `analyze_frames.py`):

```
frame0 vs frame1: client_changed=     0px bbox=None   taskbar_changed=   31px bbox=(1881, 1063, 1894, 1069)
frame0 vs frame2: client_changed=     0px bbox=None   taskbar_changed=   28px bbox=(1881, 1063, 1894, 1068)
frame0 vs frame3: client_changed=     0px bbox=None   taskbar_changed=   28px bbox=(1881, 1063, 1894, 1068)
```
Zero pixels change in the client area — only the taskbar clock. Injected `ret,spc` did not start the
game (keystrokes do not reach a game window: GB-088). **A single rendered frame is what exists, not an
animating one.** My first, naive liveness test (distinct screendump digests) was *invalid and the
control proved it*: with nothing launched at all, the bare desktop also gave 3/3 distinct digests
(`03-control-static-arm.log`), because the clock ticks. That is why the measure above is masked and
localised; the digest test is reported here as a discarded instrument, not as evidence.

## Clause 1, compat half — unmeasurable, three independent grounds

**(a) The shipped image carries zero ELF titles.**
```
$ mdir -i disk.img ::/ | grep -iE 'elf|pie|sdl'
SELFHOST  BIN     53352 ...        # .BIN, native — not an ELF-compat title
[MENU] count=128                   # from serial.log; 0 ELF-compat entries among the 128
```

**(b) A hand-staged compat ELF is wiped, because the rig boots via `make run`, which regenerates
`disk.img`.** Measured twice:
```
STAGED=1                                     # mdir ::/GAMES shows SDLAPP.ELF after mcopy
[rig] booting intel QEMU, capturing to evidence/SDLAPP/20261027T083705Z
[rig] WARNING: SDLAPP.ELF not in start-menu dump
[rig] FATAL: SDLAPP.ELF not in start-menu dump
mdir: File "::/GAMES" not found              # after the boot: the image was rebuilt
```
The rig has no staging hook equivalent to the tier's `T3_STAGE` (`tools/t3_tier.sh:180`).

**(c) Even with a title staged, the rig cannot launch it where a compat ELF can load.**
`src/kernel/elf_image.c:101` rejects any image that is not EM_X86_64, so ELF-compat titles are
x86-64-only. On intel the rig finds the name and then never launches:

```
$ python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel --run-seconds 6
[rig] XNIBBLER.BIN slot: found by name = 20 (launching 20, by name)
$ grep -c '\[LAUNCH\]' evidence/XNIBBLER/20261027T083908Z/serial.log
0
```
Cause, from the QEMU lines the rig inherits: `Makefile:275` (arm) ends with
`-device virtio-keyboard-device -device virtio-tablet-device`; `Makefile:257` (intel) has neither, so
the rig's only launch gesture — `q.click(38, H-26+13)` in `launch_from_menu()` — has no pointer to
click with. Same rig, same game: **arm launches, intel never does.**

## What the Coordinator's promotion claim gets right, and what it misses

Right: a static-PIE compat binary is now dispatched and runs in-guest (GB-066), and the launcher does
scan `/GAMES` for ELF (`src/user/desktop.c:915`, `src/user/games_scan.c`). Missed: neither makes *this
rig* capable of running a compat **title** — the image ships none, the rig cannot stage one, and on
x86-64 (where one could load) the rig cannot launch anything at all.

## Not counted (walls other cards own)

- Keystrokes not reaching a game window (GB-088) — affects "live" only, not launch-by-name.
- `[GPUDIAG] gpu wait > 5 ms: ack=79 used=80` appears in the arm serial; not a GB-059 clause.

## Reproduce

```
git worktree add .worktrees/gb059v --detach b4e4524 && cd .worktrees/gb059v
ln -s $PWD_MAIN/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging   # + libcxx/icu src
export PATH=/usr/sbin:$PATH && mkdir -p obj
make disk.img ARCH=arm -j32 && python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch arm
make disk.img ARCH=intel -j32 && python3 tools/evidence_rig.py --game XNIBBLER --bin XNIBBLER.BIN --arch intel
python3 evidence/gb-059-v/probe_liveness.py --bin XNIBBLER.BIN --arch arm --label static-control --no-launch
```