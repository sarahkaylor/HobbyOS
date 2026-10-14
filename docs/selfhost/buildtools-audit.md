# Build-tools audit: GNU make / mtools / dosfstools pins + portability audits + sh matrix

Lane L4 of the HobbyOS self-hosting program (selfhost.md W-D/W-E, AD-7/AD-8).
Scope: pins + per-tool portability audits vs the HobbyOS sysroot, the sh
capability matrix for running make recipes in-OS, and the `Makefile.hobbyos`
sketch + recommendations. Nothing here ships to the OS; these feed the wave-2
port lanes (make-port, mtools-port, mkfs-port) and M3 (build driver).

House style: `docs/gnu-ports.md` — ports are transcription jobs into `src/`
(GPL header kept, gnulib/autotools scaffolding replaced by the sysroot +
small local helpers); autotools is NOT used in-tree. All three tools are
GPL-3+ and compatible with that policy.

---

## 1. Pins

Verified 2026-10-14 at lane time (facts drift — the lane brief's guesses were
re-checked, not trusted). All tarballs downloaded to `third_party/_staging/`
(gitignored) and hashed locally; `_staging` also holds the extracted trees
used for the audits below.

| Tool | Version | Upstream URL (tarball) | sha256 (computed locally) | tag→commit | Release date | License |
|---|---|---|---|---|---|---|
| GNU make | **4.4.1** (latest stable; 4.4.2 does not exist) | https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz | `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3` | tag `4.4.1` → `6dd575465372c7d8c22d9b35b977c1c6c63a0b6b` (`git ls-remote https://git.savannah.gnu.org/git/make.git`) | 2023-02-26 (NEWS + ftp listing) | GPL-3+ (COPYING v3) |
| mtools (mcopy/mmd subset) | **4.0.49** (latest stable) | https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz | `10cd1111da87bf2400a380c1639a6cba8bfb937a24f9c51f5f88d393ae5f6f76` | **N/A — upstream VCS is SVN, not git** (see note) | 2025-06-14 (GNU announcement, info-mtools 2025-06) | GPL-3+ (COPYING v3) |
| dosfstools (mkfs.fat) | **4.2** (latest stable) | https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz | `64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527` | tag `v4.2` → `bba5a12447991cc1be9c33855c6c68e89b9ab29c` (`git ls-remote https://github.com/dosfstools/dosfstools.git`); GitHub release v4.2 published 2021-01-31 | 2021-01-31 | GPL-3+ (COPYING v3) |

Cross-verification of the hashes:

- make 4.4.1: computed sha256 matches the long-published value; tarball size
  2,348,200 B. `.sig` present on ftp.gnu.org (GPG, not verified here).
- mtools 4.0.49: computed sha1 `b9f90c51a91cab19b5c470daff6f18cfb5740473`
  matches the SHA-1 published in the GNU release announcement
  (info-mtools / info-gnu, 2025-06-14). `.sums` file is not published on
  ftp.gnu.org for this tarball.
- dosfstools 4.2: the GitHub release asset accompanies the signed
  `dosfstools-4.2.tar.gz.sig`; project CONFIRMS GPL-3+ in README.

**mtools tag→commit note.** The sandbox-canonical `git ls-remote
https://git.savannah.gnu.org/git/mtools.git` returns "repository not found":
mtools is one of the last GNU packages whose upstream VCS is **SVN**
(savannah `svn://svn.savannah.gnu.org/mtools/`, browsable at
`https://svn.savannah.gnu.org/viewvc/mtools/` — the viewvc instance was
503 at lane time). There is no upstream git repo, so there is no git
tag→commit to record. Options recorded for the port lane:

1. Integrity gate = the tarball + its sha256 (+ published SHA-1), which is
   exactly the GNU-release provenance we actually need.
2. If a git history is wanted for the port work, the Debian packaging
   mirror `https://salsa.debian.org/debian/mtools.git` tracks upstream
   (tags there are `debian/*` packaging tags, not upstream); treat it as a
   convenience reference only, never as provenance.

## 2. Port surface summary (per tool) — see §3–§5 for the full audits

| Tool | LOC (core sources) | VCS | Port model | Key syscall surface | verdict |
|---|---|---|---|---|---|
| make 4.4.1 | ~36 src/*.c (≈28k C incl. VMS/Amiga/guile we drop) | git (savannah) | transcription of ~22 core files + own getopt; replace gnulib-ish local portability layer | fork/exec/waitpid, pipe, signals, fcntl F_SETLK (lockf), glob, getcwd, isatty/termios, time | **PORTABLE with the known gap list in §3**; recommend `-j1` v1 |
| mtools 4.0.49 | ~60 C files (whole tree); subset = mcopy + mmd + shared libs | SVN (no git) | transcription of a ~12-file subset; VFAT LFN writing is REQUIRED (staging copy-in path) | open/read/write/lseek, stat/fstat, readdir/opendir (local dirs), termios (raw floppies — stub), ioctl(BLKGETSIZE64) — stub for image files | **PARTIAL by design** — subset + stubs enumerated in §4 |
| dosfstools 4.2 | src/mkfs.fat.c + boot.c (+ common/) — small | git (github) | transcription of ~4 sources; no ioctl needed if we implement the O_EXCL/SEEK_HOLE probe fallback | open(O_EXCL), lseek (SEEK_HOLE/SEEK_DATA probe — conditional), write, ftruncate/fsync, time | **EASIEST of the three** — §5 |
