# GB-031-V — gate run manifest

- Verifier: DEEP, worktree `hermes-b14bd40c`, host swarm-farm-2
- Date: 2026-10-22 (UTC)
- Revision run: **69f3893** = `origin/main` (worktree switched to a branch at `origin/main` before running; `git rev-parse HEAD` = 69f38936dda2b2a4c336a29f96a9ec119730fc44)

## Step 1 — the named gate does not exist on the verified sha

```
$ bash evidence/gb-031/repro.sh
bash: evidence/gb-031/repro.sh: No such file or directory
exit=127

$ git cat-file -e origin/main:evidence/gb-031/repro.sh
fatal: path 'evidence/gb-031/repro.sh' does not exist in 'origin/main'
exit=128

# where a8ff1ac (the commit the ticket cites) actually lives:
$ git branch -a --contains a8ff1ac
  remotes/origin/hermes/hermes-5e4d816e
```

`a8ff1ac` ("GB-031: make the static-musl SDL2 profile reproducible across dest dirs") is **not an ancestor of
`origin/main`**. Its evidence dir and `repro.sh` exist only on that branch. `make host_loader_test` — step 3's
target — likewise does not exist on main; the loader test is `elf_image_test_host`.

## Step 2 — the substance, measured with the shipped build script

Two clean builds of the shipped `tools/sdl/build-sdl2-static-musl.sh`, different dest dirs:

```
$ sha256sum /tmp/sdl2-A/sdl2_static_test /tmp/sdl2-B/sdl2_static_test
fc967f0d678c18cf38043ffa95a9bc751a541c4c2867f9ac39ac39f1d09a7bd4  /tmp/sdl2-A/sdl2_static_test
57777c9f9b8cb7cbaf533e9605f1b55c281686b9550d0347b9c19acbbf5e1707  /tmp/sdl2-B/sdl2_static_test

$ sha256sum …/libSDL2.a
cde68aa1d88c4dc90ac652c635b166d713d12924671eb189868e10418a9585c0  /tmp/sdl2-A/stage/SDL2-2.30.10/build/.libs/libSDL2.a
e75b9047a87b1de4af71a0a7656fef3c413d5f55e0c36b4b9d8b53f1489e835e  /tmp/sdl2-B/stage/SDL2-2.30.10/build/.libs/libSDL2.a

$ cmp -l A B | wc -l
238            (first difference at byte 753; same size, 5283016)
```

Both artifacts differ. The comparison is not vacuous — it fired on two builds of the same script with the same
pinned inputs.

## Step 3 — the ELF contract and loader acceptance (these PASS)

```
$ readelf -h /tmp/sdl2-A/sdl2_static_test | grep -E 'Type|Machine'
  Type:    EXEC (Executable file)
  Machine: Advanced Micro Devices X86-64

$ ELF_REAL_BINARY=/tmp/sdl2-A/sdl2_static_test ./elf_image_test_host
  PASS real binary /tmp/sdl2-A/sdl2_static_test -> ok (entry 0x401ac0, 4 segs)
  PASS real binary plan: 4 region(s), span 0x26b000, first off 0x0 fl 0x4
=== 93 checks, 0 failed ===
```

(Unset, the real-binary case SKIPs: `SKIP real-binary parse (set ELF_REAL_BINARY=…)`. The ticket's "84
checks" is also stale — the shipped test reports 93 with the real-binary case enabled, 91 without.)

## DoD scorecard

| clause | result |
|---|---|
| 1 two clean builds byte-identical (both artifacts) | **FAIL** — `sdl2_static_test` and `libSDL2.a` both differ across dest dirs |
| 2 static non-PIE EXEC, X86-64, loader accepts | **PASS** — `EXEC`/`X86-64`, `real binary -> ok (entry 0x401ac0, 4 segs)`, 93 checks 0 failed |
| 3 the gate fails when a hash diverges | **FIRES** — but on the shipped script there is no two-build comparison at all; the comparison that exists (`a8ff1ac`'s `repro.sh`) is not on main |

## Verdict

**RED** — DoD 1 fails on `origin/main`. The shape the loader needs (DoD 2) is present and proven; the
reproducibility promise is not: the fix that names exactly this defect ("reproducible across dest dirs") is
stranded on `origin/hermes/hermes-5e4d816e`.
