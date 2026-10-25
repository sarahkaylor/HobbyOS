# GB-031-V — VERDICT: **RED**

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-25 07:2xZ.
Verifies GB-031 (#32) / ship PR #161, head `5e22da3` ("GB-031: make the static-musl SDL2 profile
reproducible across dest dirs"). Host-side gate (cross-build, no guest), exactly as the card names.
I did not author GB-031 or PR #161 — independent gate.

## One line

The **reproducibility** half is real and it reproduces exactly here — two clean builds at different
dest dirs are byte-identical, matching the committed hashes — but the artifact is **not a static-musl
SDL2**, and it has **no fbcon video driver**: `clang --target=x86_64-linux-musl` on this host silently
links the host **glibc**, and the archive carries only the dummy/offscreen video drivers. The card's
named deliverable is therefore not what the profile produces.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | two clean builds at **different** dest dirs produce byte-identical artifacts (the 2026-10-22 RED ground) | **PASS** — measured, hashes match the committed evidence |
| 2 | `readelf` Type EXEC non-PIE / X86-64, `file` statically linked, host ELF-image validator on the real artifact | **PASS** — 93 checks, 0 failed |
| 3 | `tools/sdl/README.md` documents the pinned tarball, the flag set, the post-fix citable hashes | **PASS** |
| — | **the card's named artifact**: "static-musl SDL2 build profile (**fbcon** + evdev, no X11)" | **FAIL** — the artifact is glibc-static; fbcon is not compiled in |

## Clause 1 — cross-dest reproducibility: PASS (quoted)

```
$ bash evidence/gb-031/crossdest_verify.sh          # rev 5e22da3
########## clean build at /tmp/sdl-a ##########
########## clean build at /tmp/sdl-b ##########
########## cross-dest comparison ##########
libSDL2.a: IDENTICAL across different dests (82e72c095e0f4ee773dfcc5313284f2a8e726faf3d6f543596b65f2ac0428d53)
sdl2_static_test: IDENTICAL across different dests (c9f6c35126ebf9b533cffabd120c0d4a354340314212e559b837ac763f4a77ab)
rc=0
```
(`evidence/gb-031-v/01-crossdest.log`) — 173 objects built per dest, 2.7 MB app, 2.2 MB archive, built
a minute apart in `/tmp/sdl-a` and `/tmp/sdl-b`; the hashes are the ones the PR records
(`82e72c09…`, `c9f6c351…`). The 2026-10-22 RED's ground is genuinely gone.

## Clause 2 — ELF shape + loader acceptance: PASS (quoted)

```
  Type:                              EXEC (Executable file)
  Machine:                           Advanced Micro Devices X86-64
/tmp/sdl-a/sdl2_static_test: ELF 64-bit LSB executable, x86-64, version 1 (GNU/Linux), statically linked, BuildID[sha1]=..., for GNU/Linux 3.2.0, not stripped
```
```
$ ELF_REAL_BINARY=/tmp/sdl-a/sdl2_static_test ./elf_image_test_host
=== 93 checks, 0 failed ===
ALL ELF IMAGE HOST TESTS PASSED
```

## Clause 3 — profile recorded: PASS

`tools/sdl/README.md` pins SDL2 `release-2.30.10` with archive SHA256 `f59adf36…661196aa`, records the
configure flag set and both post-fix hashes, and states the cross-dest caveat and its fix.

## The failing cell — this is not a static-musl artifact

Zero musl strings; glibc's own symbols and strings instead:

```
$ strings -a /tmp/sdl-a/sdl2_static_test | grep -icE "musl"
0
$ strings -a /tmp/sdl-a/sdl2_static_test | grep -E "GNU C Library|glibc|GLIBC_2\.[0-9]+" | sort -u | head
 glibc: .bss
 glibc: getrandom
 glibc: malloc
 glibc: malloc arena
 glibc: pthread stack: %u
$ nm /tmp/sdl-a/sdl2_static_test | grep -E "__libc_start_main|_dl_relocate_static_pie"
00000000004e28e0 T __libc_start_main
00000000004e28e0 T __libc_start_main_impl
0000000000401af0 T _dl_relocate_static_pie
```
(`evidence/gb-031-v/02-libc-identity.log`) — `__libc_start_main_impl` and `_dl_relocate_static_pie`
are glibc static-PIE machinery; musl has neither.

**Why: the flag does not do what it says on this host.** `clang -###` for the very command the build
script runs resolves the host's glibc crt and library paths:

```
$ clang -### --target=x86_64-linux-musl -static -no-pie ...
"-L/lib/x86_64-linux-gnu"
"-L/usr/lib/gcc/x86_64-linux-gnu/15"
"/lib/x86_64-linux-gnu/crt1.o"
"/lib/x86_64-linux-gnu/crti.o"
"/usr/lib/gcc/x86_64-linux-gnu/15/crtbeginT.o"
```
(`evidence/gb-031-v/03-profile-claims.log`) — and the build log's linker is `x86_64-linux-gnu-ld.bfd`
with the glibc warning `Using 'dlopen' in statically linked applications requires at runtime the
shared libraries from the glibc version used for linking`, which a genuine musl static link does not
emit.

**Discrimination control (so this is not a code-reading argument).** Same one-line C file, two real
builds, same checks (`evidence/gb-031-v/04-musl-vs-clang-control.log`):

```
$ musl-gcc -static -o /tmp/hello_musl hello_min.c        # genuine musl
musl-string-count: 2
0000000000401440 T __libc_start_main                     # no _impl
/tmp/hello_musl:  ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked, with debug_info

$ clang --target=x86_64-linux-musl -static -o /tmp/hello_clang hello_min.c
musl-string-count: 0
0000000000404140 T __libc_start_main_impl                # glibc
/tmp/hello_clang: ELF 64-bit LSB executable, x86-64, version 1 (GNU/Linux), statically linked, BuildID[sha1]=..., for GNU/Linux 3.2.0
```
The profile's artifact matches the **clang control** (glibc), not the **musl-gcc control**. So the
`--target=x86_64-linux-musl` invocation is decorative on this VM; `musl-gcc` (already installed, and
already used by the repo's own T4 probe target) is the build that yields musl.

## Second miss — the profile has no fbcon video driver

```
$ grep -nE "SDL_VIDEO_DRIVER_(FBCON|DUMMY|OFFSCREEN|X11) 1|SDL_INPUT_LINUXEV 1" /tmp/sdl-a/stage/SDL2-2.30.10/include/SDL_config.h
318:#define SDL_INPUT_LINUXEV 1
378:#define SDL_VIDEO_DRIVER_DUMMY 1
394:#define SDL_VIDEO_DRIVER_OFFSCREEN 1
```
(`evidence/gb-031-v/03-profile-claims.log`) — **evdev is in** (the card's input half is met), **fbcon
is not**: `SDL_VIDEO_DRIVER_FBCON` is undefined, so the built profile can render to `dummy`/`offscreen`
only. The script's own comment defers fbcon ("the later HobbyOS integration, tracked separately in
GB-031's remaining half"), which is honest in the script and contradicted by the ticket title and the
README's "enabling the Linux evdev input driver and the dummy/offscreen video drivers" framing.

## Why this is RED and not a GREEN-with-notes

GREEN here moves GB-031 to Merge Gate, from which M2 cites this profile as *the* SDL. Twenty minutes
before this gate, on GB-032-V, the SDL app built by `make obj/sdl_test_app_pie` — same
`clang --target=x86_64-linux-musl` pattern, same silent glibc fallback (measured there: same host
`rcrt1.o` paths) — was dispatched into the intel guest and died at
`Fatal glibc error: Cannot allocate TLS block` (VA `0xCF8`). A profile certified as "static-musl" that
actually ships glibc-static is the direct cause of that guest failure, so a GREEN would certify the
defect that the other verify card has just measured in the guest.

## Also found (card hygiene, not a verdict cell)

The gate command the card names does not exist at the path it names:
```
$ ls tools/gate/evidence
ls: cannot access 'tools/gate/evidence': No such file or directory
```
The script is at `evidence/gb-031/crossdest_verify.sh` (that is what I ran, rev `5e22da3`). Either the
card's path or the script's location should move; as written, a verifier following the card literally
gets "No such file or directory" and no gate.

## What has to change before a GREEN is possible

1. Build with a real musl toolchain — `musl-gcc -static` (proven above to produce musl here) or a
   pinned musl sysroot passed as `--sysroot=` — and **assert the property in the build**:
   `strings libSDL2.a | grep -q musl` / absence of `__libc_start_main_impl`. Reproducibility is
   worthless if it reproduces the wrong libc.
2. Re-record the hashes after the toolchain change (they will change; the current ones are the glibc
   profile's).
3. Either enable `SDL_VIDEO_DRIVER_FBCON` in the configure set, or amend the card so "fbcon" is not a
   claim this profile carries — the current title promises a driver the build cannot produce.

## Reproduce

```
git worktree add /tmp/gb031v 5e22da3 && cd /tmp/gb031v
make elf_image_test_host
bash evidence/gb-031/crossdest_verify.sh                 # clause 1 + 2, ~2 min at -j32
strings -a /tmp/sdl-a/sdl2_static_test | grep -ic musl    # 0  <-- the RED
nm /tmp/sdl-a/sdl2_static_test | grep __libc_start_main_impl
grep -nE "SDL_VIDEO_DRIVER_FBCON|SDL_INPUT_LINUXEV" /tmp/sdl-a/stage/SDL2-2.30.10/include/SDL_config.h
```
