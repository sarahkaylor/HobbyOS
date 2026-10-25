# GB-031-V — gate evidence (VERDICT: RED, clause 1 FAIL / clause 2 shape PASS but not musl / clause 3 FAIL)

Ticket: GB-031-V (#166) — Verify static-musl SDL2 build profile (fbcon + evdev, no X11)
Verifies: GB-031 (#32). Card instruction: "Head for the gate: `ticket/31-repro-fix` (PR #161) if it
is merged, else `origin/main` as it stands — state which in the verdict."
**PR #161 is not merged** (no `-ffile-prefix-map` in main's script; `git log origin/main -- tools/sdl/`
tip is `ee1a1d2`), so this gate ran on **`origin/main` @ `96ba7d2`** as it stands.
Verifier: farm Verifier, DEEP (DeepSeek v4.1 Flash, dgx1), host `swarm-farm-2`, own worktree.
Run 2026-10-25 ~13:55–14:10Z. Independence: I authored neither GB-031, PR #161 nor PR #169.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | two clean builds at **different dest dirs** produce byte-identical artifacts | **FAIL** — both differ |
| 2 | ELF shape `Type: EXEC` (non-PIE), statically linked, loader accepts | shape **PASS** + loader **PASS** (93 checks) — but the artifact is **glibc, not musl** |
| 3 | `tools/sdl/README.md` documents tarball, flags and the **post-fix** citable hashes | **FAIL** — no post-fix hash anywhere; it defers dest-independence |

## Clause 1 FAIL — the exact RED ground, reproduced

Two clean builds of the shipped script, `bash tools/sdl/build-sdl2-static-musl.sh <dest>`:

```
dest A = /tmp/gb031-a            build rc=0, 28 s
dest B = /tmp/gb031-b-other-dest build rc=0, 30 s

-- libSDL2.a
ab62f66e1197f3a4e5bd63497c2547551bdbf8334c43dcafa3ca92f2ef36efb7  /tmp/gb031-a/stage/SDL2-2.30.10/build/.libs/libSDL2.a
73e671209c260a2790ac5aa998d7a2d45bf801e8f0fc56cda657218ee46f794c  /tmp/gb031-b-other-dest/stage/SDL2-2.30.10/build/.libs/libSDL2.a

-- sdl2_static_test
4dee2e8f1249edd85895fa3c0a67fcb54a7c213531190780b3124848dd918942  /tmp/gb031-a/sdl2_static_test
458cc25826a1ca6394948345ce5350138cb6e6311ac91448676b4e3f09d3a77e  /tmp/gb031-b-other-dest/sdl2_static_test
```

Cause, measured rather than assumed — the archive carries the build directory in DWARF:

```
strings .../libSDL2.a | grep -c "/tmp/gb031-a"            -> 534
strings .../libSDL2.a | grep -c "/tmp/gb031-b-other-dest" -> 534
```

## The control: one change flips clause 1, so the gate can pass and the RED is precisely located

`prefixmap_control.py` copies the shipped script to `/tmp` and adds only
`-ffile-prefix-map=$(pwd)=. -fdebug-prefix-map=$(pwd)=.` to the configure `CC` and the app link (the
change PR #161/#169 carry). Same two-dest test, tree untouched:

```
f10186fe0499b29cf75caacb0f150b5d58f38db3383313345fc3d6e49aea977c  /tmp/gb031-c/.../libSDL2.a
f10186fe0499b29cf75caacb0f150b5d58f38db3383313345fc3d6e49aea977c  /tmp/gb031-d-other/.../libSDL2.a
f6a75204fafb001f1dcb3173939a28bd8cf6d6a3f8ac40ae92848939a272d07a  /tmp/gb031-c/sdl2_static_test
f6a75204fafb001f1dcb3173939a28bd8cf6d6a3f8ac40ae92848939a272d07a  /tmp/gb031-d-other/sdl2_static_test

strings ... | grep -c "/tmp/gb031-c"       -> 0
strings ... | grep -c "/tmp/gb031-d-other" -> 0
```

So: the profile is reproducible **within** a dest (README's run1/run2 pair is real) and **not** across
dests, and the missing prefix maps are the whole of the difference. (These control hashes are not the
card's expected `82e72c09`/`c9f6c351`: that pair came from PR #161's exact flag spelling; the property
under test is identity, not a specific digest.)

## Clause 2 — shape PASS, loader PASS, but it is not musl

```
$ readelf -h /tmp/gb031-a/sdl2_static_test | grep -E "Type|Machine"
  Type:    EXEC (Executable file)
  Machine: Advanced Micro Devices X86-64
$ file /tmp/gb031-a/sdl2_static_test
ELF 64-bit LSB executable, x86-64, ... statically linked, ... with debug_info, not stripped

$ ELF_REAL_BINARY=/tmp/gb031-a/sdl2_static_test ./elf_image_test_host
  === 93 checks, 0 failed ===
  ALL ELF IMAGE HOST TESTS PASSED          EXIT=0

$ strings /tmp/gb031-a/sdl2_static_test | grep -cE "GNU C Library|GLIBC_PRIVATE|Fatal glibc error"  -> 11
$ strings /tmp/gb031-a/sdl2_static_test | grep -ciE "musl"                                          -> 0
```

The loader half of clause 2 is genuinely met. The **musl** half of the card's premise is not: on this
host `clang --target=x86_64-linux-musl` links the host glibc (no musl sysroot wired into the clang
target), which is the ground the 07:35Z RED named and which is still true on `main`. `musl-gcc` exists
on this host (`/usr/bin/musl-gcc`) — the script just does not use it; that is PR #169.

## Clause 3 FAIL — the README does not record the post-fix hashes

`tools/sdl/README.md` pins the tarball and its SHA (`release-2.30.10`, `f59adf36…661196aa`) and the
flag set, so the first half of the clause holds. The citable hashes do not: it lists
`54827f3b…` twice (run 1 / run 2, same canonical path) and then says outright

```
paths and hash differently. The gate (clean tree, stable profile dir) is met;
fully dest-independent hashing is a strip/`-fdebug-prefix-map` follow-on,
```

i.e. the README documents the *same-dest* claim and defers the cross-dest one. `grep -c` for any of the
post-fix digests (`82e72c09`, `c9f6c351`, `e09876b4`, `6674931d`, and this run's
`ab62f66e`/`4dee2e8f`/`f10186fe`/`f6a75204`) → **0**.

## Verdict

**RED.** Clause 1 fails on `main` exactly as the previous RED found; clause 3 fails independently; clause
2's shape/loader half passes and its musl half does not. GREEN needs, on `main`:
1. the prefix-map pair in the shipped script (PR #161 or #169) — the control above shows it is sufficient
   for clause 1;
2. the musl-gcc profile (PR #169) plus an assertion that the artifact is musl
   (`strings | grep -q musl`), so the card title is true of the artifact;
3. a README that records the **cross-dest** hashes actually produced by the shipped script.
