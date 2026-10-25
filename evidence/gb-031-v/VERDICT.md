# GB-031-V (round 2) — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-031-V (#166) · verifies GB-031 (#32) · ship = PR #169 head `25bb902` (branch `ticket/31-musl`)
Base `origin/main` @ `8f95204`; the branch is main + 1 commit, so the gate tree **is** `25bb902`
Run window 2026-10-25T08:22Z–08:38Z · worktree `hermes-9209a3a5` (swarm-farm-1)

Tier demanded: **T1 host cross-build** (no guest). T3/T4/T5 not named, not run.

## VERDICT: RED

Clauses 1–3 as written pass, and the musl fix is real. The card's **subject** — "static-musl SDL2
build profile: fbcon + evdev drivers, no X11" — fails on **both** driver halves, and the evdev half is
a **regression introduced by this fix**. A GREEN here would certify an SDL profile that can neither
read input nor draw to `/dev/fb0`, which is exactly what M2 would then cite.

| Clause | Result |
|---|---|
| 1 cross-dest byte-identical | **PASS** — `libSDL2.a e09876b4…`, `sdl2_static_test 6674931d…`, dests A and B |
| 2 ELF shape + loader acceptance | **PASS** — `EXEC`, `X86-64`, `statically linked`; ELF-image host test `93 checks, 0 failed` on the real artifact |
| 3 profile recorded (tarball, flags, hashes) | **PASS** — README pins `2.30.10`, archive SHA `f59adf36…`, both post-fix hashes |
| **Subject: evdev** | **FAIL — regressed**: `/* #undef SDL_INPUT_LINUXEV */` in the musl build vs `#define SDL_INPUT_LINUXEV 1` in the pre-fix build |
| **Subject: fbcon** | **FAIL — not achievable at this pin**: SDL 2.30.10 ships no fbcon driver (no `src/video/SDL_fbcon*`, no configure option) |

### The failing check (evdev), quoted

    ### musl tree (/tmp/g-musl-a, the shipped profile)
    $ grep -nE "SDL_VIDEO_DRIVER_FBCON|SDL_INPUT_LINUXEV" include/SDL_config.h
    318:/* #undef SDL_INPUT_LINUXEV */

    ### PRE-FIX tree (/tmp/g-glibc-a, origin/main's clang profile, same pinned tarball)
    $ grep -nE "SDL_VIDEO_DRIVER_FBCON|SDL_INPUT_LINUXEV" include/SDL_config.h
    318:#define SDL_INPUT_LINUXEV 1

    ### cause, from the musl build's own configure log
    checking for linux/input.h... no

`musl-gcc`'s default search path is `/usr/include/x86_64-linux-musl` + gcc's own dir; the kernel UAPI
headers (`/usr/include/linux`, `/usr/include/x86_64-linux-gnu/asm`) are not on it, so SDL configures
the evdev driver out. Round 1 measured the opposite (evdev in, fbcon out); the toolchain change has
silently dropped the input half.

**Verified remedy (I ran it, configure only):** adding the UAPI dirs after musl's own headers makes
configure find the header and define the driver:

    $ CC=musl-gcc CFLAGS="-O2 -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu" ./configure …
    checking for linux/input.h... yes
    $ grep -n SDL_INPUT_LINUXEV include/SDL_config.h
    318:#define SDL_INPUT_LINUXEV 1

`-I/usr/include` is **not** the fix — it drags glibc's `features.h` in and dies on `bits/wordsize.h`;
`-idirafter` is what keeps musl's headers winning.

### fbcon is a card problem, not a worker problem

    $ ls src/video/ | grep -i fb        -> (no fbcon driver source)
    $ grep -i fbcon configure           -> (no such configure option)

SDL 2.30.10 has no fbcon video driver. The clause "fbcon" in this card's title cannot be met by a flag;
it needs a DoD amendment (decision-class) — e.g. "video via the HobbyOS `/dev/fb0` path owned by
GB-032/GB-033, profile asserts evdev + no X11" — or a different SDL pin. Round 1's RED said the same and
the card was re-minted without the amendment.

### Proof the gate can fail (the musl fix itself is genuine)

    ### G3 CONTROL — the PRE-FIX profile (clang --target=x86_64-linux-musl), built from origin/main's script
    build-rc=0
    --- glibc __libc_start_main_impl count (expect >0): 1
    --- musl libc_start_main_stage2 count: 0
    --- musl strings: 0
    --- 5,283,664 bytes
    ### the shipped musl profile
    --- glibc __libc_start_main_impl count: 0
    --- musl libc_start_main_stage2 count:  1
    --- musl strings: 2
    --- 1,633,xxx bytes, 153 archive members, app runs rc=0

So PR #169 does what it says on the libc axis, and the artifact is reproducible; it just buys musl by
losing evdev.

### Instrument notes (not verdict cells)

1. The card's clause-1 command still does not exist: `tools/gate/evidence/gb-031/crossdest_verify.sh`
   → ENOENT. The real script is `evidence/gb-031/crossdest_musl.sh` (what I ran). Round 1 flagged this
   and it is unchanged, so a verifier following the card literally gets no gate.
2. `crossdest_musl.sh` prints `IDENTICAL`/`DIFFERS` but **does not exit non-zero on DIFFERS** (its exit
   status is the trailing `file` call). I graded the hashes themselves, not the script's status.
3. `-idirafter` diagnosis above is read-only experimentation in `/tmp`; nothing in the deliverable was
   modified by me.
