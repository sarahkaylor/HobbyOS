# Lane L4 — Build-tools audit: GNU make, mtools, dosfstools + sh matrix + Makefile.hobbyos sketch

**You are lane L4 of the HobbyOS self-hosting program.** Your COMPLETE instructions are
this file. Read `~/hobbyos-selfhost/selfhost.md` §0, §3, §4 (W-D/W-E), §5 (AD-7/AD-8),
§7 — this brief is its L4 slice and binds you.

## Identity & tree

- **Worktree:** `/home/sarah/hobbyos-selfhost-lanes/l4` (branch `selfhost/l4-buildtools-audit`,
  based on `main` of `/home/sarah/hobbyos-selfhost`).
- **NEVER touch the workstation repo** `/home/sarah/Documents/GitHub/HobbyOS` (read-only
  measurements are allowed and encouraged — e.g. its built `hobbyos.elf` size). **No pushes.**
- Read-only for HobbyOS sources; your deliverables are docs + downloaded tarballs + a
  script sketch. Skills: `hobbyos-development` (sysroot facts), `docs/gnu-ports.md`
  (house port style — read it).

## Mission

Produce `docs/selfhost/buildtools-audit.md`: **pins + portability audits + port work
breakdowns** for the three external tools the in-OS build needs (GNU make; mtools
`mcopy`/`mmd`; dosfstools `mkfs.fat`), the **sh capability matrix** for running make
recipes, and the **`Makefile.hobbyos` sketch**. These feed the wave-2 port lanes; nothing
here ships to the OS yet.

## Deliverable 1 — pins (do this first; commit early)

- Select current stable versions and record for each: upstream URL, exact tarball,
  sha256 (compute locally from the download), tag→commit (`git ls-remote` against the
  canonical repo; for GNU projects use `https://git.savannah.gnu.org/git/<name>.git`,
  for dosfstools `https://github.com/dosfstools/dosfstools.git`), release date, license.
  Candidates to check (verify latest at lane time, don't trust these guesses):
  - GNU make — `https://ftp.gnu.org/gnu/make/` (4.4.1 was stable for a long stretch;
    check for newer).
  - mtools — `https://ftp.gnu.org/gnu/mtools/` (4.0.4x).
  - dosfstools — `https://github.com/dosfstools/dosfstools/releases` (4.2).
- Download each tarball into `third_party/_staging/` (gitignored; already sized for this)
  and `sha256sum` it. Record everything in a `## Pins` table.
- If a download fails after retries, record the exact failure and move on (mark VERIFY).

## Deliverable 2 — portability audits (per tool)

For each tool: extract in `_staging`, then enumerate its OS-facing surface by grep:
`open/read/write/lseek/stat/fstat/lstat/mmap/ioctl/termios/getopt/getpwuid/getgrgid/
getcwd/glob/realpath/system/popen/fork/exec/pipe/waitpid/kill/signal/fcntl(F_SETLK)/
utime/ftruncate/fsync/readdir/opendir/fnmatch/locale/setlocale/time/clock_gettime/
isdigit..., malloc/realloc/strdup (libc-level — fine)`.

For each call: **FULL / PARTIAL / MISSING** against the sysroot (`src/libc/include/**`,
`src/libc/src/**`, `src/user_include/libc.h`) with anchors. Then the port work breakdown
per tool, house style (`docs/gnu-ports.md`: "transcribed into src/ (GPL header kept,
gnulib scaffolding replaced by the sysroot + small local helpers)"; autotools is NOT used
in-tree — the port is a transcription job):
- `make`: list the files that make up the port (main.c, read.c, job.c, function.c,
  expand.c, variable.c, dir.c, commands.c, file.c, misc.c, output.c, remake.c, rule.c,
  implicit.c, default.c, signame.c, strcache.c, … per its src/); the gnulib/portability
  layer it will need (it has its own, mostly replaceable); **exact sysroot gaps** with sizes.
  Note make's shell usage: it runs recipes via `$(SHELL) -c` — our SHELL will be
  `/TOOLS/SH.BIN`; also note the jobserver (pipe+fds) for `-j` — **recommend `-j1` v1**
  (TCG is serial-bound anyway) and record what `-j` would need later.
- `mtools` (mcopy+mmd subset): file list for just those two programs + shared libs
  (`mcopy.c`, `mmd.c` links `msdos.c`/`init.c`/`plain_io.c`/`file*.c`/`sysdos.c`...);
  the subset decisions (dosfstools-style: keep VFAT LFN writing! the staging needs it —
  mcopy integration is the acceptance's copy-in path too); ioctl/raw-device paths can be
  stubbed OUT if unused for image-file operation — flag every stub needed.
- `dosfstools mkfs.fat`: it is small (`src/mkfs.fat.c`); list required syscalls (it wants
  O_EXCL, lseek SEEK_HOLE/SEEK_DATA probing — check 4.2!), the BPB fields it writes for
  `-F 16` (our kernel's parser is BPB-data-driven, fat16.c headers), and the boot-sector
  compatibility note with Limine/UEFI (the EFI stub is chainloaded by OVMF from
  `/EFI/BOOT/BOOTAA64.EFI`; a standard mkfs.fat layout is what builds today — the port
  must reproduce byte-compatible-enough structure; record what "enough" means: BPB fields
  + FATs + root dir; NOT byte-identical to host mkfs.fat).

## Deliverable 3 — sh capability matrix

1. Enumerate the shell constructs the host Makefile's **recipes actually use** — grep
   the recipe lines (`grep -nP '^\t' Makefile`), miss nothing: `mkdir -p`, `[ -f ... ]`,
   `&&`/`||`/`;`, `>`/`>>`/`2>`/`2>&1`, `$(MAKE)` recursion, `echo`, variable assignment
   + expansion in recipes, `if ... then ... fi`, `cp/rm/mv/ls`, `command -v`,
   `src=""; if [ -f "$(X)" ]; then ...`, `du`, `sed`-free text munging, etc. Also the
   `$(shell ...)` uses (makefile expansion — those need a shell too, but the in-OS
   Makefile.hobbyos will AVOID `$(shell)`; record which exist so the sketch can drop them).
2. Cross-check against `src/user/sh.c` (3369 lines): read its header comment + syntax
   sections (`sh.c:13-15` lists features; find the parser). Build the matrix:
   `construct | used-by-makefile | sh.c status (supported/partial/absent) | evidence
   (sh.c anchor) | fix estimate (S/M/L)`. Where uncertain, propose the exact host test
   (`src/host/ shell_test*.c` exist — read them) that would settle it.
3. Output: the matrix + an itemized sh-gap fix list (these become M3 tasks).

## Deliverable 4 — `Makefile.hobbyos` sketch + recommendations

- Sketch (as a fenced block or `docs/selfhost/Makefile.hobbyos.sketch` — the REAL file
  lands in wave 2): frozen targets `check`, `user`, `kernel`, `disk`, `all` (F3); variables
  (`CC=/TOOLS/CLANG.BIN`, `LD=/TOOLS/LLD.BIN`, `OBJCOPY=/TOOLS/OBJCOPY.BIN`,
  `AR=/TOOLS/LLVMAR.BIN`, `SHELL=/TOOLS/SH.BIN`, flags verbatim from Makefile:183/:189/
  :215/:226 plus `-isystem /TOOLS/RESDIR/include` per AD-5); kept source sets (kernel:
  all 71 C/asm; user: acceptance set AD-9); dropped groups (browser, ICU/Skia/glib,
  python tiers, E2E, screenshots, intel branches v1); recipe rules: **shell-conservative**
  (one command per line where possible, no `$(shell)`, no process substitution); stage
  markers `[SELFHOST] phase=<name> rc=<n>` echoed by the heavy steps (the acceptance
  parser greps them — define the exact marker vocabulary here: `phase=user-start/…`);
  disk recipe: `MKFSFAT -F 16 OUT.IMG` + `MMD` dirs + `MCOPY` files (no `dd`); note the
  `dd` replacement analysis (host `dd` zero-fills; in-OS: OUT.IMG ≤256 MiB, offer
  `ftruncate`-only vs zero-fill options with tradeoffs — recommend one).
- Recommendations section: make vs hbuild (default: make; reasoning + activation gate),
  one-command-per-line recipe policy, serial `-j1` policy, and the exact wave-2 lane
  lineup you'd propose (3 lanes: make-port, mtools-port, mkfs-port).

## Constraints (hard)

- Downloads strictly into `third_party/_staging/`; scratch under your worktree or
  `~/.hermes/cache/scratch/`. No QEMU. No repo builds. Do not edit `src/**`, `Makefile`,
  `selfhost.md`, `.gitignore`, other lanes' paths.
- Commit cadence ≈ every 30-45 min; subjects `selfhost: l4: <what>`; stop committed if
  the budget nears. Every pin claim = a real sha256 you computed; otherwise mark VERIFY.

## Report (write + validate before finishing)

`docs/selfhost/lanes/l4-report.json` (commit; `python3 -m json.tool`):

```json
{"lane": "l4", "status": "complete|partial|blocked",
 "commits": ["sha"], "artifacts": ["path"], "evidence": ["path"],
 "pins": [{"tool": "make", "version": "?", "sha256": "?"}],
 "blockers": ["..."], "next_steps": ["wave-2 lane order"],
 "notes": "sh gaps count; Makefile.hobbyos open questions"}
```
