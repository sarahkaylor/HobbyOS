# GNU Port Program — Candidate Survey & Roadmap

Status: living document. Started 2026-09-24.

## 1. Purpose and selection criteria

HobbyOS is growing a set of **genuinely GNU** terminal programs — real ports of
FSF sources, not lookalikes — so the shell becomes useful for text processing
and scripting. Candidate software must satisfy:

1. **License: GPL or AGPL** (GPL-2+ / GPL-3+ both fine). No proprietary or
   non-FSF-licensed sources.
2. **Terminal-only**: reads stdin/files, writes stdout/stderr. No GUI, no
   network services, no terminal-control libraries at first.
3. **Practical value on HobbyOS today**: text processing and everyday shell
   work. "Things of high value", not proof-of-concept toys.
4. **Portable scale**: not "as ambitious as emacs". A port should be
   transcribable into the HobbyOS sysroot model (custom libc, no gnulib
   autoconf machinery at build time) within a bounded engineering effort.

Every port follows the established convention (see `wc.c`, `head_gnu.c`,
`tail_gnu.c`): the original FSF source is **transcribed** into `src/`
(GPL header kept, gnulib scaffolding replaced by the sysroot + small local
helpers) and reformatted with `tools/cstyle.py` per STYLE.md. The pristine
upstream tree is vendored under `third_party/` and is used to build a
**byte-exact reference binary** for parity testing (`build_*_ref.sh`).

## 2. What exists today

### GNU textutils-2.1 ports (complete, byte-exact parity vs. reference build)

| Program | Binary | Notes |
|---|---|---|
| `wc` | `WC.BIN` | full option set incl. long options; wc_parity.sh + WCTEST.BIN |
| `head` | `HEDGNU.BIN` | head_parity.sh; legacy HEAD.BIN stays for the shell |
| `tail` | `TAILGN.BIN` | strict ref build pulls argmatch/human gnulib too |

### Legacy hand-written tools (kept; not GNU, much smaller surface)

`cat`, `ls`, `grep` (substring-only, 87 lines), `less`, `tail`/`head` (legacy),
`sort`, `uniq`, plus whole-file tools `cp rm mv touch mkdir ps free uptime kill`
and the `find`/`diff` GUI apps.

### Sysroot (src/libc) surface already provided

stdio (FILE layer, vsnprintf family byte-exact vs. glibc), string.h subset,
stdlib (strtol family, qsort/bsearch, rand, env), ctype, getopt(+long),
error.h, fcntl/open/read/write/lseek/stat/fstat/pipe/spawn2, malloc/realloc,
assert.h, signal.h stubs, mman.

## 3. Candidate inventory

Legend — Effort: S (days-scale port + tests), M, L. Deps: new libc/OS support
required beyond what exists.

### 3.1 Top-tier (selected for this program)

| Program | Source | License | LOC (core) | New deps | Value | Effort |
|---|---|---|---|---|---|---|
| **sed** | sed-4.8 | GPL-3+ | ~5.2k (`sed/*.c`) | regex.h (GNU), dfa, obstack, tempname/mkstemp/rename for -i | stream editing; scripting cornerstone | M |
| **grep** | grep-2.5.4 | GPL-2+ | ~4.0k (`src/`) + vendored regex/dfa | regex.h (GNU), dfa (shared), fnmatch, savedir-style recursion | regex search; scripting cornerstone | M |

*grep-3.11 was also evaluated (builds cleanly; ~14k lines + gnulib). Rejected
for v1 because its `-r` traversal is fts/fd-based (needs fdopendir/openat/
fchdir surface we do not have); grep-2.5.4 recurses via `savedir`
(read-dir-into-list + path recursion) which maps 1:1 onto the existing
`read_dir` syscall. Behavior for the option set that matters (-G/-E/-F, -r,
-A/-B/-C, --include/--exclude, -c/-l/-n/-v/-q/-o, --color) matches modern GNU
grep and is verified loose-mode against the host's grep during parity runs.*

### 3.2 Textutils-2.1 batch (source already vendored)

Every one of these shares the exact gnulib surface the wc/head/tail ports
proved out (getopt_long, error, xstrtoumax, closeout), so marginal cost per
tool is small. Ordered by value:

| Batch | Programs | Notes |
|---|---|---|
| 1 | `cut`, `tr`, `paste`, `fold`, `nl` | everyday text slicing/joining |
| 2 | `comm`, `tac`, `tsort`, `expand`, `unexpand`, `sum`, `cksum`, `md5sum` | set ops, checksums (md5/sha1 vendored tables) |
| 3 | `join`, `split`, `od` | joins, chunking, hex/oct dumps |

(`sha1sum` in textutils-2.1 is a wrapper; `pr`, `fmt`, `ptx`, `csplit` are
large and lower-value — deferred.)

### 3.3 Other FSF packages considered

| Program | Source | Why (not) now |
|---|---|---|
| `diff`, `cmp`, `diff3` | diffutils-2.8.1 (GPL-2+) | High value for a dev OS; needs regex (arrives with sed phase). **Planned phase G.** |
| `ed` | GNU ed 1.20 (GPL-3+) | tiny line editor, regex-based; good "editor" win without curses. **Stretch.** |
| `xargs` | findutils 4.x (GPL-3+) | needs fork/exec/waitpid (have) + getopt; small. **Stretch.** |
| `bc`/`dc` | bc 1.x (GPL-3+) | arbitrary precision; moderate. Deferred. |
| `nano` | nano-8.7 (GPL-3+) | **See 3.4.** Desired but curses-dependent. |
| `make` | GNU make 4.x | L; also needs process/FS maturity. Deferred. |
| `gawk` | gawk 5.x | L+ (locale, dynamic loading, dfa); deferred. |
| `emacs` | — | Explicitly out of scope. |

### 3.4 nano feasibility (investigated, deferred with a concrete plan)

nano-8.7 is 22.7k lines of `src/*.c` and **hard-depends on ncursesw**: it
links `-lncursesw` and uses the full curses screen model (termios raw mode,
terminfo capability strings, `wget_wch` wide input, SIGWINCH, color pairs,
soft label keys). Porting nano therefore means porting (a subset of) ncurses
plus a terminfo reader, i.e. a real terminal emulator inside the OS:

- the console window today is a *line-oriented text buffer* fed by `print()`
  with a few OSC/CSI extensions (title, menu, run-in-window). It has no
  cursor-addressing surface, no attribute model, no alt-screen, and the
  desktop key path has no Ctrl key.
- Baseline for coexistence: `EDITOR.BIN` is the GUI editor; `less` covers
  paging.

Deferred sequence (own project-sized chunk, after sed/grep land):
1. OS-side: Ctrl-key support end-to-end + an ANSI/curses-capable terminal
   surface in the console window (screen grid, cursor addressing, SGR
   attributes, raw mode push/restore, resize events).
2. libc-side: termios stub API, `ioctl(TIOCGWINSZ)`, `[[nano]]`'s POSIX
   surface (move/delete/rename, getcwd already present), and a
   curses-subset (or full ncurses port) with a built-in `xterm`/`ansi`
   fallback terminfo.
3. Then nano itself, vendored + transcribed like the other ports, with a
   scripted-injection test harness (no interactive babysitting).

## 4. This program's roadmap (each phase = one local commit)

| # | Phase | Deliverable | Gate |
|---|---|---|---|
| A | Survey + vendoring | this document; sed-4.8 + grep-2.5.4 trees under third_party/ with checksums | — |
| B | Sysroot regex | `regex.h` + regcomp/regexec/regex_internal in libc; host test **byte-exact vs. glibc** | host suite + unit tests both arches |
| C | GNU sed 4.8 | `SED.BIN` transcribed port; strict ref build; `sed_parity.sh`; `sed_test_host`; `SEDTEST.BIN` in-OS | parity + host + `make test` ARM/Intel |
| D | GNU grep 2.5.4 | `GGREP.BIN`; ref build; `grep_parity.sh`; in-OS test | same |
| E | textutils batch 1 | cut, tr, paste, fold, nl (+ parity + in-OS each, shared fixture harness) | same |
| F | Terminal/keyboard | Ctrl key end-to-end (desktop → window stdin) + shell/tests; groundwork for nano | host+GUI tests, `make test` both arches |
| G | textutils batch 2 + diffutils (`cmp`, then `diff`) | as budget allows; commit per sub-batch | same |

Shared infrastructure to build once and reuse: a `gnulib_support` set for the
transcribed helpers (quotearg, xalloc, xstrtol where the program needs them),
a per-program parity script skeleton, and an in-OS spawn-harness pattern
(`wc_test.c` is the template).

## 5. Testing strategy (per port)

1. **Strict parity (byte-exact)**: build the pristine upstream source as a
   **host reference** (`src/host/build_<pkg>_ref.sh <out>`), then race the
   host-built port against it over option matrices + fixtures, comparing
   **stdout bytes, exit codes and stderr** (`*_parity.sh`). Byte-exact is the
   acceptance bar — same bar as wc/head/tail.
2. **Loose parity vs. modern GNU**: same harness, second reference =
   the host's own GNU binary (modern version), layout-normalized comparison
   where versions legitimately differ (e.g. `sed --version` text).
3. **Host unit tests**: pure functions (option parsing, address ranges,
   pattern compile) driven directly, `make host_tests`.
4. **In-OS acceptance** (`*TEST.BIN`): spawn the real `.BIN` through
   `spawn2` + pipes with fixtures written to the FAT image, compare captured
   stdout byte-for-byte on the serial console. Runs in `KERNEL_MODE_TEST`
   (ARM default) **and** `make test_intel` for x86_64 parity.
5. Both architectures build the same sources; x86_64 gets the identical
   in-OS test list.

## 6. Source provenance (sha256)

| Tarball | URL | sha256 |
|---|---|---|
| sed-4.8.tar.gz | https://ftp.gnu.org/gnu/sed/sed-4.8.tar.gz | `53cf3e14c71f3a149f29d13a0da64120b3c1d3334fba39c4af3e520be053982a` |
| grep-2.5.4.tar.gz | https://ftp.gnu.org/gnu/grep/grep-2.5.4.tar.gz | `b091d2bf9d558dde67a4496af3f259d8779770d1b70065f27b481b6665653fac` |
| textutils-2.1.tar.gz | https://ftp.gnu.org/gnu/textutils/textutils-2.1.tar.gz | `3da5d426facef710aff4dc1ff4773604a2398c948854df0cd91b70988bded456` |
| diffutils-2.8.1.tar.gz | https://ftp.gnu.org/gnu/diffutils/diffutils-2.8.1.tar.gz | `c5001748b069224dd98bf1bb9ee877321c7de8b332c8aad5af3e2a7372d23f5a` |

(`diffutils-2.8.1` and `nano-8.7` are staged for later phases.)
