# Browser Plan — a JavaScript-capable browser for HobbyOS (v2)

Status: **v2 — reviewed: Track B (WebKit) endorsed as the primary/approved
track (maintainer review 2026-09-29, §0.2); supersedes the v1 "Dillo 3.2.0 +
FLTK 1.3.11" plan written earlier the same day.** v2 was produced after the
requirement was raised to: *"a browser with JavaScript support so that pages
like Google Search or CNN realistically work."* Planning document only — it
changes no code; it is the execution plan for the work.

Audience: the agent sessions and human maintainers who will execute this. Read
§0 and §7 before touching anything; §7.3 (frozen interfaces) is binding.

Modeled on `docs/gnu-ports.md` (survey → phases → gates → provenance → fix
log). Everything below with a file path was verified against the HobbyOS tree
and the pinned upstream sources on 2026-09-29; anything that could not be
verified is marked **verify**. If you were handed v1 material (Dillo, FLTK,
X11 v2): that track is superseded — do not start it; start from §7.

---

## 0. TL;DR and how to use this document

**The requirement (raised):** JavaScript must work well enough that Google
Search and CNN are realistic. §1 verifies what that means in 2026 and concludes
honestly: **there is no "middle" browser that meets it — it requires a real web
engine, and the only eligible one is WebKit.** (This is the same engine family
that runs those exact sites on iPhones: the risk in this program is entirely in
*bringing the engine up on a new OS*, not in web capability.)

**The plan is Track B-first; Track A is retained only as an optional
contingency:**

| Track | What | Role | Scale |
|---|---|---|---|
| **B — WebKit** (target) | WebKit **2.54.0**, WPE-flavored sources, a custom *HobbyOS* platform port (no GTK/GLib/X11; WebKit2 multi-process; Skia CPU raster; libcurl+mbedTLS networking; JSC with no JIT) | **Meets the Google/CNN bar. Endorsed as the primary/approved track** (maintainer review 2026-09-29, §0.2). Staged so usable value lands early: a `jsc` JS shell → headless page render → browser shell with JS → networked → acceptance | Years of agent-session work; gates at every stage |
| **A — NetSurf** (optional contingency — dormant by default) | NetSurf **3.11** + Duktape JS, framebuffer (no-X) frontend, our pixel-window protocol | **Not scheduled by default.** Retained as a late fallback if Track B hits a hard wall, or an early-win option only if explicitly requested. Partial JS — does not meet the Google bar alone. Reuses the same libcurl/mbedTLS/font stack | Months (if invoked) |

**What carries over from v1 unchanged** (the bulk of the *near-term* OS work):

- Syscalls `SOCKET/CONNECT_FD/SELECT/FCNTL/GETSOCKOPT/SETSOCKOPT/GETRANDOM`
  (numbers 65–71, §Appendix A.1a), TCP hardening, DHCP→DNS hand-off, entropy.
- **FPU on both arches** (still zero support — JSC and every layout engine are
  floating-point machines; this was true for Dillo and is truer for WebKit).
- libc work (sockets, resolver, time), **desktop input v2** (wheel, modifiers,
  graceful close), loader/memory growth (now much larger — see P2).
- Pins: mbedTLS 3.6.7, FreeType 2.14.3, zlib 1.3.1, libpng 1.6.44,
  libjpeg-turbo 3.1.0, DejaVu 2.37 (§2). Conventions + evidence rules (§8.4).
- The parallel-execution machinery (§7) — retargeted at the new lanes.

**What is now off the critical path:** X11 library v2, FLTK, Dillo, the Xft
mini-fontconfig shim. Neither browser track uses X11: NetSurf runs on the
framebuffer frontend, and the WebKit port integrates directly with the desktop
pixel protocol. The existing `src/user/x11/` library **stays as-is** (xcalc
etc. keep working); growing it further is optional, decoupled work — do not
schedule it here. X11 growth can resume any time as its own project.

**New workstreams Track B requires** (the honest cost of "aim higher"): OS
**threads + pthreads**, a **VM overhaul** (large-sparse address spaces, demand
paging, `MAP_SHARED`), **IPC primitives** (AF_UNIX, `SCM_RIGHTS` fd-passing,
shared memory), **process management** (fork/exec of helper processes, minimal
signals), **libc++/libc++abi**, `poll()`, **SQLite**, **ICU**, **harfbuzz**,
a **Skia** (or Cairo) port, and a **libcurl** port. §6 splits this into F/P/NS/WK
milestones, each with a gate and evidence rules.

**How to use:** Track the milestones in §6 with parallel lanes L1–L9 (§7,
including per-lane session briefs in §7.5). Frozen interfaces §7.3 and the
file-ownership map §7.4 prevent lane collisions. §8.4 evidence rules: no
"done" without executed, observed output from the real case.

### 0.1 What changed from v1 (quick diff)

| Topic | v1 (superseded) | v2 |
|---|---|---|
| Selection | Dillo 3.2.0 + FLTK 1.3.11 | **WebKit 2.54.0** (target) + **NetSurf 3.11** (interim) |
| JS | none (explicit non-goal) | **required**; full JSC in Track B, Duktape (partial) in Track A |
| X11 | grow in-repo Xlib to ~150–200 entries (biggest userland chunk) | **Not used by either track.** Library kept as-is; growth decoupled/optional |
| FLTK / Dillo / Xft | core workstreams | superseded — do not start (see §11) |
| Syscalls 65–71 | frozen & load-bearing | unchanged, still frozen (§Appendix A.1a) |
| New syscall stages | — | P-stage: threads/futex, poll, socketpair/sendmsg/recvmsg, mmap2/mprotect/munmap/madvise, memfd, signals, exec/wait (§A.1b, provisional until each gate) |
| VM/loader | 1 MiB cap → ~8 MiB, zeroing strategy | **Full overhaul**: multi-GB sparse VA per process, demand-zero pages, shared mappings, loader v2 (P2; supersedes v1 T1.6) |
| libc++ | not needed | **required** (P3) — plus keep v1's minimal C++ runtime for early smoke |
| Graphics | none (FLTK/X11 direct) | **Skia CPU raster** (per WebKit ≥2.54; fallback pin 2.52.6+Cairo — decision gate at W2) |
| Network libs | dillo's own sockets; mbedTLS | **libcurl** (port) + WebKit's curl backend + mbedTLS; NetSurf also uses libcurl fetcher |
| Acceptance | fixtures + lite pages | fixtures + **Google Search and CNN on-device** (definition in §8.3) |
| Pins | dillo/fltk/mbedtls/freetype/zlib/png/jpeg/dejavu | WebKit 2.54.0 (+2.52.6 alt) added; dillo/fltk retired to history; rest kept |

The v1 plan is superseded in full; what it got right carries forward (diff in
§0.1; the change is recorded in §11). Where v1 task IDs (T1.x/T2.x) are still
valid they are kept, marked *(v1: Tx.y)*.

### 0.2 Why two tracks (read this before arguing with the plan)

- The **NetSurf track** buys: a real browser (CSS engine, forms, images) on
  HobbyOS soon; forces the sockets/TLS/fonts/memory plumbing through a simpler
  consumer first, de-risking the same subsystems WebKit needs; and stays as a
  fallback if Track B hits a wall late. It is explicitly **not** the Google/CNN
  answer — its JS (Duktape bindings) is partial by construction (§1.4).
- The **WebKit track** is the only path that runs modern sites. It is large;
  §9 includes hard checkpoints where the program can be paused/re-scoped with
  whatever has landed (each milestone is independently valuable: jsc shell,
  headless renderer, networked browser).
- **Maintainer review (2026-09-29): Track B is the best track — it is the
  primary and approved path.** Consequence: Track A is re-scoped to *optional
  contingency* — dormant by default; invoked only on explicit request or as a
  late fallback if Track B hits a wall. The default schedule is Track B only:
  §6 NS milestones and §7 lane L7 stay documented but do not start.

---

## 1. The new requirement, verified (2026-09-29)

### 1.1 What the sites actually serve (field evidence)

Fetched today from the workstation with two user-agents (NetSurf/3.11 and a
desktop Chrome string) — raw evidence archived by the planning session:

**Google Search — effectively a JavaScript wall.** A plain `GET` of
`https://www.google.com/search?q=…` returns HTTP 200, ~92 KB, whose entire
useful payload is a redirect shell:

```html
<meta content="0;url=/httpservice/retry/enablejs?sei=…" http-equiv="refresh">
<noscript><div>Please click <a href="/httpservice/retry/enablejs?…">here</a>
if you are not redirected within a few seconds.</div></noscript>
```

The escape hatch URL is literally named **`enablejs`**; the query term never
appears in the served HTML; `gbv=1` (the old basic-HTML mode) returns the same
shell; a `support.google.com/websearch` pointer confirms the posture. Without
JavaScript, Google Search does not work. Full stop.

**CNN — server-rendered, but heavy and engine-hungry.** `cnn.com` returns a
**6,583,482-byte** page (`<title>Breaking News, Latest News and Videos | CNN`)
with real headlines in `<h2>/<h3>` markup server-side (e.g. "Six Flags says
it's shutting down its world-famous X2 roller coaster"), i.e. text a parser
could get — but the page is a modern CSS/JS application: usable rendering,
images, video, navigation need a real engine. Its JS-free sibling
**`lite.cnn.com`** (332,965 bytes, same title) is the designed escape hatch.

**Interpretation.** A no-JS browser gets: nothing from Google; a degraded,
slow CNN (or the lite edition). "Realistically work" = a real engine. That is
the requirement this plan serves.

### 1.2 The ladder — what "JavaScript support" can mean

| Tier | Browser | JS | Google Search | CNN | Port scale |
|---|---|---|---|---|---|
| 0 | Dillo 3.2 (v1 pick) | none | fails (JS wall) | content partial; modern experience no | weeks–months (v1 plan) |
| 1 | **NetSurf 3.11 + Duktape** (Track A) | partial; experimental | fails (JS wall) | partial; `lite.cnn.com` clean; cnn.com heavy | months |
| 2 | **WebKit** (Track B) | full (JSC) | **yes** — this engine renders it on iOS today | **yes** | years — this plan |
| 3 | Chromium / Gecko | full | yes | yes | excluded by requirement |

**There is no tier 1.5.** Between NetSurf-class and WebKit-class, the
open-source field offers only: Servo (Rust + SpiderMonkey — Firefox's engine
core, excluded twice), Ladybird (see §1.3 — in 2026 it is *harder* for us than
WebKit, not easier), and proprietary embeddings (Ultralight/Sciter — closed).
This is the central planning fact of v2.

### 1.3 Why WebKit (and why not the others)

**WebKit is the right engine for this OS:**
- C/C++ (C++20), no Rust, no GC-heavy managed languages in the platform layer.
- **Proven on non-mainstream systems** (§ precedent table below) — including a
  full port by a small dedicated team (Haiku) and two upstream non-Apple ports
  (WinCairo/Windows, PlayStation).
- **JSC can be built standalone first** (the `JSCOnly` port — minimal
  dependencies, static builds supported via `ENABLE_STATIC_JSC`,
  cross-building-to-a-board-and-testing-remotely is a documented upstream
  workflow) → our first milestone is "a JavaScript shell runs on HobbyOS".
- **Networking is replaceable**: WebKit has a **`libcurl`-based network
  backend** (`Source/WebKit/NetworkProcess/curl`, used by WinCairo and
  PlayStation) — we port libcurl+mbedTLS once and both tracks use it.
- **The API path is clear**: WebKit2's **C API** is glib-free (WinCairo
  precedent) — we do not port GTK, GLib, GObject or X11.
- Multi-process is a *feature* for this project: it forces exactly the OS
  maturity HobbyOS is being built for (IPC, shared memory, fd-passing,
  threads, process supervision).

**Precedents to study during execution:**

| Precedent | What to take from it |
|---|---|
| **Haiku / haikuwebkit** | The roadmap twin. 2009: first platform patches; 2010: WebPositive; 2013+: continuous work; **2024: still porting WebKit2** (GSoC). 2025: dropping 32-bit builds (compiler OOM). Lesson: multi-year commitment, WebKitLegacy-first, migrate to WebKit2 later; a small crew *can* keep a port alive. |
| **WinCairo (Windows)** | WebKit2 port whose networking is the **curl backend** and whose UI uses the **C API** — our integration model. |
| **PlayStation** | An upstream, non-desktop port carried in-tree (`Source/cmake/OptionsPlayStation.cmake`) — proof ports can be maintained upstream; stretch goal for us. |
| **WPE** | The embedded lineage of the same engine. 2.54 bundles the new "WPEPlatform" API (Wayland/DRM/headless backends) — informs how a HobbyOS backend could eventually look (but it is GLib-based: we model on it without adopting it). |
| **JSCOnly** | The standalone-JS port: `Source/cmake/OptionsJSCOnly.cmake`, `Tools/Scripts/…jsc-only`, trac wiki with a RaspberryPi cross-build-and-remote-test guide. Our WK-1 milestone. |

**Rejected — Ladybird** (checked this session against its own docs):
- `Documentation/BuildInstructionsLadybird.md`: "**Qt6.9+** development
  packages … a **Rust toolchain is also required** … **C++23** capable
  compiler"; deps via vcpkg incl. **Skia**, harfbuzz, fontconfig; the
  official UI ports are "Qt6" and "AppKit" (`Documentation/Porting.md`).
- Its engine core is **actively migrating to Rust** (2026 posts/PRs: "Ladybird
  adopts Rust"; layout/style/display-list work moved to Rust; third-party
  port analyses list "Rust is a HARD build requirement" for its workspace
  crates).
- Consequence for HobbyOS: we would first have to port **Rust's std** to a new
  bare-metal target *and* provide a Qt6-class UI stack *while* the engine
  target moves under our feet — strictly more work than WebKit, for a
  pre-alpha browser (their own target: "alpha in 2026 for Linux & macOS").
- *Revisit trigger:* if Ladybird ever ships a no-std/no-Rust-core build story
  plus a non-Qt UI port that is stable, re-evaluate (keep the pin links in
  §Appendix E).

**Also rejected:** Servo (Rust + SpiderMonkey); GTK-embedding (glib stack not
worth porting); Chromium/Blink and Gecko (explicitly out of scope); sciter /
Ultralight (closed).

### 1.4 Why NetSurf is the interim (Track A)

- A real browser: own HTML/CSS/layout engine, forms, tables, images
  (PNG/JPEG/GIF/WebP), and a **"dumb framebuffer port … no particular
  operating system or GUI toolkit"** — designed for exactly our integration
  style (a surface + input events; we back it with the desktop pixel protocol,
  not X11).
- **JavaScript via Duktape since NetSurf 3.4** — but partial: bindings are
  generated from WebIDL by `nsgenbind` and upstream publishes a list of
  *unimplemented* DOM/CSSOM methods (docs: `docs/jsbinding.md`). A 2017
  netsurf-dev thread shows the framebuffer target needed source edits to even
  enable JS (`js_initialize` → `enable_javascript = true`) and that
  `setTimeout` errored. **verify** current state at NS-4; plan conservatively.
- Version reality: stable is **3.11 (released 2023-12-28)**; 3.12 has been in
  development since (mantis: "will be in the 3.12 release"). Slow upstream —
  pin the release, keep patches tiny.
- ~15 small C libraries (libhubbub/libcss/libdom/…), all portable; the
  libcurl fetcher reuses Track B's curl port; mbedTLS already pinned.

### 1.5 Licenses (updated)

| Component | License | Note |
|---|---|---|
| WebKit (WTF/WebCore/JSC/WebKit2) | LGPL-2.1 + BSD-2 mix | static linking OK, but ship the LGPL relink/objects provision — record our compliance package at WK-7 |
| Skia | BSD-3 | fine |
| libcurl | MIT/X | fine |
| SQLite | Public domain | fine |
| ICU | Unicode license | fine |
| harfbuzz | MIT | fine |
| NetSurf | GPL-2 | app-level; fine |
| NetSurf libs | MIT-ish per lib | text per vendored tree |
| mbedTLS / FreeType / zlib / libpng / libjpeg-turbo / DejaVu / CA bundle | as v1 §1.5 | unchanged |

### 1.6 Non-goals (v2)

- **No JIT** for JSC in this program (interpreter/LLInt only): JIT needs
  executable-memory management, and signal-based machinery we will not build
  yet. Consequence: JS runs slow (especially under QEMU TCG) — accepted and
  documented. Revisit only after WK-6.
- No GPU/WebGL, no video/audio (GStreamer-class media off), no WebRTC, no
  sandboxing (single-user OS; WebKit sandboxing disabled by config), no tabs
  class of features (single window; the OS desktop tiles windows), no
  IndexedDB, no extensions/plugins.
- No X11 growth (decoupled, per §0), no GTK/GLib, no Rust toolchain.
- No parity with mainstream browsers: target = "Google and CNN honestly work,
  slowly, without crashing on the machines we have".

---

## 2. Pinned sources & provenance

Convention as v1: vendored under `third_party/` (extracted tree + tarball);
checksums recorded here at vendoring (W0 re-verifies). **New pins this
version:**

| Source | URL | sha256 / id |
|---|---|---|
| wpewebkit-2.54.0.tar.xz *(Track B base; 44.1 MiB)* | https://wpewebkit.org/releases/wpewebkit-2.54.0.tar.xz (sums: `…/wpewebkit-2.54.0.tar.xz.sums`) | `efa9bcc3cb891c2d88f50eec710d9ccee71cbdf1040420361eb98c17355eb452` |
| WebKit git tag *(same release)* | https://github.com/WebKit/WebKit tag `webkitgtk-2.54.0` | tag object `cef114f523d3bd389936cbc7ec2b13e5436d63a0` (W0: deref → commit `5220e80b97a253c60ed899361654142ab5021998`; fork base; clone at `~/webkit-hobbyos`) |
| wpewebkit-2.52.6.tar.xz *(fallback: last Cairo-era graphics; 62.5 MiB)* | https://wpewebkit.org/releases/wpewebkit-2.52.6.tar.xz | `b2bafef2751625b7fdf530f230ff0f542ff0eeba3590c3a989d931b2a55c858e` |
| NetSurf 3.11 *(Track A app)* | https://download.netsurf-browser.org/netsurf/releases/source/netsurf-3.11.tar.gz | record sha at W0 (release dir: `…/releases/source/`) |
| NetSurf libs (libhubbub, libcss, libdom, libnsfb, … ~15) + duktape | https://git.netsurf-browser.org / GitHub mirrors | pin per-lib at W0; record in this table |

Carry-over pins (unchanged, still valid):

| Source | URL | sha256 |
|---|---|---|
| mbedtls-3.6.7.tar.bz2 | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 | `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` |
| freetype-2.14.3.tar.xz | https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` |
| zlib-1.3.1.tar.gz | https://zlib.net/fossils/zlib-1.3.1.tar.gz | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` |
| libpng-1.6.44.tar.xz | https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz | `60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e` |
| libjpeg-turbo-3.1.0.tar.gz | https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz | `9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93` |
| dejavu-fonts-ttf-2.37.zip | https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip | `7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a` |
| cacert.pem (Mozilla CA extract) | https://curl.se/ca/cacert.pem | record sha at vendoring (rolling file) |

To pin at W0 (Track A/B shared library layer — versions recorded then, with
upstream-compatible choices): **libcurl** (8.x, mbedTLS backend), **ICU**
(minimum required by WebKit 2.54 — read from its CMake **at W0**), **Skia**
(commit exactly matching what WebKit 2.54 expects — **verify** at W0 from
WebKit's Skia integration docs/CMake), **harfbuzz** (WebKit min), **SQLite**
(3.4x), **libwebp** (1.x).

**W1b vendor records (2026-09-30, merged `0f194bd`)** — pinned + vendored per
AD-11 with host builds + smokes green (branches `browser/l5-fonts`,
`browser/l6-libs1`, `browser/l6-libs2`; URLs + full sums + recipes in each
`third_party/*.README.md`):
- libcurl **8.22.0**, mbedTLS backend — sha256 `f7ef3ae8…` (PGP Good: Stenberg
  key `27ED EAF2 2F3A BCEB 50DB 9A12 5CC9 08FD B71E 12C2`; live HTTPS HEAD smoke
  → 200; curl.se publishes no sha256).
- harfbuzz **14.5.0** (≥ WebKit floor 2.7.4) — sha256 `b7132e14…`; built
  `+hb-ft +hb-icu` against ICU 78.2 (host).
- SQLite **3.49.2** amalgamation (newest of the documented 3.4x series) — zip
  sha256 `921fc725…`; `sqlite3.c` sha3-256 matches the official release log;
  **3.53.4 cross-checked green** with the same recipe (bump candidate).
- libwebp **1.6.0** (was "1.x" — now exact) — sha256 `e4ab7009…` (GPG Good).
- Flags: zlib 1.3.2 released (7ASecurity audit fixes; pin stays 1.3.1 — bump
  candidate); mbedTLS 3.6.7 / FreeType 2.14.3 / DejaVu 2.37 confirmed as pinned
  (all vendored + smoke-verified).

**W1c vendor record (2026-09-30, merged `e5188de`)** — Skia, per WebKit 2.54's
own `Source/ThirdParty/skia/README.WebKit`:
- Skia @ **`588b550a4dd8af90dbe71c0554852806bd8f0b21`** — recipe vendored
  (`third_party/skia-588b550/`: `fetch.sh` + 12395-entry content manifest
  `c27786ca…` (the gate — googlesource `+archive` containers are not
  byte-reproducible) + `gen_sources.py` derived from WebKit's own file list +
  overlay CMake + smoke; 156 KB committed, 216 MiB tree gitignored).
  Provenance: content-identical vs the fork clone (12389 files) and vs the WPE
  tarball Skia subset (3059 files; `tools/compare-with-webkit.py` reproduces).
  Host build: `libSkia.a` 15,079,504 B / 595 objects; deterministic smoke
  sha256(pixels)=`eee5d809…`, fnv1a64 `0xb2e29142c06f8b86`. WK-3 finding:
  WPE's Skia `CMakeLists` omits `src/core/SkStrikeRef.cpp` (upstream `gn`
  includes it) — confirm dead-strip harmlessness or apply the one-line patch.

**W1d vendor records (2026-09-30, merged `3b83823`)** — libc++, ICU4C, GLib
trio (recipe-form per AD-11; full sums + recipes in each `third_party/*/README.md`):
- libc++ + libc++abi **21.1.8** (LLVM release matching the clang 21 toolchain)
  — `third_party/libcxx-21.1.8/`: `fetch.sh` + `container.sha256` (`4633a236…`
  artifact-container pin) + `anchors.sha256` content gate (googlesource
  containers are not byte-reproducible) + `sources.txt` + `config/` overlay +
  `build-target.sh`; hermetic target build, static archives both arches.
- ICU4C **78.3** — sha256 `3a2e7a47…` (cross-verified vs GitHub digest +
  official md5/sha512). Host static build; data = `--with-data-packaging=static`
  + `ICU_DATA_FILTER_FILE` trim (requires the `icu4c-78.3-data.zip` overlay +
  parking prebuilt `icudt78l.dat`, handled idempotently by
  `build-host-trimmed.sh`); trimmed `libicudata.a` **10.2 MB (−69 %** of the
  33.1 MB full); smoke deterministic with static data. Cross memo: `configure`
  exit 0 on `aarch64-none-elf` + `x86_64-none-elf` via the documented clang
  wrapper (lld, `-Wl,-no-pie`); next layers = mh-* stub + target libc++
  (the latter now satisfied by the libc++ record above). **Target cross-build
  DONE (Wave 1e, `84d7832`):** recipe `third_party/icu-78.3/build-target.sh`
  (+ committed `config/mh-unknown` overlay, JSC-class link probe) builds
  `obj/<arch>/icu/{libicuuc,libicui18n,libicudata}.a` for both targets, cold
  ~14 s/arch (one-time host-tools stage 18 s); nm spot-check 13/13 both
  arches; `libicudata.a` byte-identical across runs (uc/i18n hashes vary with
  DWARF/`ar` mtimes — do not gate on them); whole-archive closure leaves
  exactly **16 holes, identical both arches** (4 libc++abi RTTI —
  `__dynamic_cast` + class-info vtables, per the P3.1 no-RTTI policy; 13 libm
  transcendentals — asin atan atan2 cos expf log modf pow sin sqrt tan tanhf).
  Integrator: Makefile wiring + remediation tracked for Wave 1f; details
  `third_party/icu-78.3/cross-notes.md`.
- GLib **2.88.3** (sha256 `ab24d24e…` = GNOME sum) + pcre2 **10.49**
  (`53c156e1…`, GPG Good) + libffi **3.4.8** (`bc9842a1…`, Gentoo/Debian
  cross-checked); static host builds; smokes 46/46 + 12/12 + 8/8;
  clean-room rebuild byte-identical; cross notes (gspawn/gmodule decisions)
  in `third_party/glib-2.88.3/cross-notes.md`.

Retired pins (v1; kept here for the record only): dillo-3.2.0
(`ed685168…` tar.gz / `1066ed42…` tar.bz2), fltk-1.3.11 (`92805abc…` /
`ca2e144e…`).

Notes:
- WebKit 2.54.0 was released **2026-09-16** ("new web process compositor built
  on Skia, replacing TextureMapper" + "remove the option to use cairo for 2D
  rendering") — hence the Skia default and the 2.52.6+Cairo fallback pin.
- The planning session downloaded + hashed the carry-over tarballs; the WPE
  sums above are from the official `.sums` files (no download needed).
- **W0.4 audit correction (2026-09-29):** the *embedding C API* stays
  glib-free (§1.3), but the **WPE port build hard-requires GLib 2.70.0 and
  libsoup3 3.0.0** (`OptionsWPE.cmake:12-26`, quoted in `f0-deps-audit.md`
  §1.1).  The **curl network backend is present** in the tree
  (`Source/WebCore/platform/network/curl/`; `USE_CURL` is set by the
  PlayStation/Win ports) — moving our port onto curl is **fork-patch work**
  (L8), not a configure flag on the stock WPE port.  GLib (+deps) is added to
  the L6 host-first build list; libsoup3 drops out when/if the curl port
  patch lands.


---

## 3. What HobbyOS has today (grounding snapshot)

Verified 2026-09-29 — unchanged from v1 except where marked. **If any of this
has drifted when you start, update this section first** (commands in
Appendix D). This is the baseline the P-stage must extend.

### 3.1 X11 library — `src/user/x11/`
A real Xlib *client subset* (display/window/draw/event C files + headers +
keysym), wired to the desktop pixel-window protocol; xcalc is the reference
app (`src/user/x11/apps/xcalc/README.md`); Makefile hooks (`X11_LIB_OBJS`,
`XCALC_BIN`, `xcalc_test:`). **v2 status: kept, not extended.** (Both browser
tracks bypass X11; see §0.) The event-decoding/keysym code is nonetheless a
useful *reference* for the WebKit shell's input translation (read, don't
link).

### 3.2 Desktop pixel-window protocol — `src/user/desktop.c`, `src/user_include/graphics/window.h`
Unchanged from v1 §3.2 (pixel-mode opt-in `ESC ] X`, title `ESC ] T`, mouse
`ESC [ P/G/R`, hover `ESC [ T`, app-menu `ESC [ M`, arrows/Home/End/PgUp/PgDn/
Ins/Del/F11/F12, Shift folded into bytes, Ctrl as control bytes, F4 closes,
held-key repeat). **No Alt tracking, no modifier-state delivery, no wheel
handling** — the v1 T1.8 "input v2" work stays on the plan (F1.8) because the
browser shell and NetSurf both want wheel + modifiers + graceful close.

### 3.3 Kernel networking
As v1: TCP stack (`src/kernel/net.c`) with connect/recv/send used by `nc`;
socket fd dispatch in `fs.c`; unit tests; **no userland socket API** (libc has
no `sys/socket.h`/`netdb.h` — verified 0 matches); `SYS_CONNECT` (#18) is a
blocking whole-connection call; DHCP parses the DNS server (hand-off TBD);
QEMU slirp: host `10.0.2.2`, DNS `10.0.2.3`.

### 3.4 libc / sysroot — `src/libc/`
As v1: FILE stdio, regex, getopt, malloc, strings, env, basic time (no
`mktime`/`strftime`/`clock_gettime`), dirent, fcntl/open/read/write/lseek/stat,
pipes/spawn2, signal **stubs**, `mman` (**verify** — may be syscalls only).
Missing for the browser era: sockets/`select`, resolver, `poll`, non-blocking
fcntl, real signals, `clock_gettime`, C++ runtime, and (new) **threads**
(`pthread_*`), **libc++**, `getentropy`, `exec` family, `waitpid`, `mmap`
library surface.

### 3.5 Loader & memory
As v1: `USER_INITIAL_CLEAR_SIZE = 0x100000` (1 MiB zero+load window into a
32 MiB per-process region); `MAX_PROGRAM_SIZE == USER_INITIAL_CLEAR_SIZE`;
physical block pool 40 × 32 MiB with "Loader starved" handling. **v2 note:**
this model is for small programs; the WebKit binaries and JSC heaps do not fit
it remotely — **P2 replaces the model** (large sparse VA, demand-zero pages,
shared mappings) with F1.6 as the interim bump only where needed by Track A.

### 3.6 FPU
**Still none** on either arch (verified 0 matches ARM `CPACR|fpsimd`, x64
`fxsave|mxcsr|xmm`). Mandatory before any engine work (F1.5). *(v1: T1.5.)*

### 3.7 Build, tests, conventions
As v1: cross clang; ARM default + `make test_intel` x64; `make host_tests`
(~100 host C tests incl. `desktop_*_test.c` family, `x11_lib_test`,
`xcalc_test_host`); in-OS `*TEST.BIN` wave; QMP E2E python harnesses
(`run_xcalc_test.py` pattern — the model for new harnesses); `docs/gnu-ports.md`
conventions; `STYLE.md`/`tools/cstyle.py` for first-party only; 64 MiB FAT16
`disk.img` (**to grow — see below**); skills in `skills/`.

### 3.8 What the engine program adds beyond this (one-screen summary)

| Area | Today | P/F stage adds | Needed by |
|---|---|---|---|
| Address space | 32 MiB/process, 1 MiB load window | Multi-GB sparse VA, demand-zero anonymous pages, `MAP_SHARED`, `mprotect/munmap/madvise`, loader v2 | JSC, WebCore |
| Threads | none | kernel threads, TLS (TPIDR_EL0 / FS-base), futex-lite, `pthread_*` | JSC, WebKit, ICU |
| IPC | pipes | AF_UNIX + `socketpair`, `sendmsg/recvmsg` + `SCM_RIGHTS`, memfd/shared memory | WebKit2 processes |
| Processes | spawn2 | real `execve` (fd inheritance, `FD_CLOEXEC`), `waitpid`, minimal signals (`SIGKILL/TERM/CHLD/PIPE`) | WebKit2 process supervision |
| C++ | tiny shim | **libc++/libc++abi port** (static; exceptions off initially — WebKit compiles `-fno-exceptions` in its configs; **verify** per-component) | WebKit (all), ICU, harfbuzz, Skia |
| I/O model | select (planned, 32 fds) | `poll()` + raise select to 256 fds | WebKit event loops |
| Storage | FAT16 64 MiB | image ≥ 256 MiB (FAT16→FAT32 likely), SQLite port, cookies/localStorage files | WebKit profile |
| Graphics | framebuffer protocol | Skia CPU raster (or Cairo on fallback pin) + harfbuzz + font backend | WebKit, NetSurf |
| Network | sockets+TLS (planned) | **libcurl port** (mbedTLS backend) + WebKit curl-backend glue | both tracks |
| Text/fonts | (planned) FreeType | FreeType + **harfbuzz** shaping; WebKit font backend | WebKit |

---

## 4. Program architecture (binding decisions)

**AD-1 — Two tracks (A: NetSurf interim; B: WebKit target).** §0.2. Gates
per §6; Track A can stop after NS-5 without harming Track B.

**AD-2 — Engine = WebKit, pinned 2.54.0 (WPE-flavored sources), as a custom
"HobbyOS port" in a `HobbyOS/WebKit` fork.** No GTK/GLib/GObject, no X11, no
Wayland/DRM. Integration: our own UI process talks to the desktop via the
pixel protocol; the platform port is modeled on WinCairo (C API, curl
networking) and Haiku (small-OS reality). Fork policy in AD-11.

**AD-3 — WebKit2 multi-process architecture** (UI + WebProcess +
NetworkProcess; no GPU process build, no sandbox). Rationale: it is the
living upstream architecture (all non-Apple ports use it); WebKitLegacy
upstream now only has Apple platforms — reviving it (Haiku's path) is a
documented *alternative* if WK gates 2–3 find WebKit2 blocked, at explicitly
higher fork-maintenance cost. Decide once, at **gate WK-1.5** (after jsc
succeeds), with a written comparison in the fork README.

**AD-4 — Graphics: Skia CPU raster** (Skia is what 2.54 requires; no GPU, no
GL/EGL). **Fallback lever:** re-pin the engine to **2.52.6** (last
Cairo-compositor series, sha pinned §2) if the W2 Skia spike fails within its
budget. The spike decision is a W2 gate; record either way.

**AD-5 — No JIT (JSC interpreter + LLInt only).** §1.6. Build flags to pin at
WK-0 (`ENABLE_JIT=OFF` etc. — **verify** exact names against 2.54's
`OptionsJSCOnly.cmake`/`WebKitFeatures.cmake`).

**AD-6 — Static linking, one executable per process role** (shell UI blob;
`WebKitWebProcess` blob; `WebKitNetworkProcess` blob). No shared libraries
(dynamic linking does not exist on HobbyOS). `USE_SYSTEM_MALLOC` initially
(replaces bmalloc/libpas porting — perf acceptable); strip symbols for image.

**AD-7 — Network stack: libcurl + mbedTLS for both tracks** (NetSurf
libcurl-fetcher; WebKit curl network backend). One CA bundle file
`/CERTS/CA.PEM` (v1 AD-3 carried). Resolver = our libc `resolv.c` (F2.2).

**AD-8 — Storage: SQLite port + minimal cookie/localStorage.** FAT-backed
profile dir; IndexedDB off; cache small/off initially (network revalidation
enough). *(v1 had no storage plan beyond cookies-off; this supersedes it.)*

**AD-9 — Threads: pthreads over a small kernel thread layer with futex-lite;
single-core scheduling is acceptable** (threads are a concurrency requirement,
not an SMP requirement). TLS via TPIDR_EL0 (ARM) / FS_BASE MSR (x64).

**AD-10 — VM: sparse large address spaces + demand-zero + shared mappings**
(§3.8/P2). Program loading: read-based loading stays for small apps; large
browser binaries load via the same read path into demand-mapped regions
initially (**verify** load-time acceptable), file-backed demand paging stays a
stretch item. Budget: QEMU machines have 6–8 GiB; per-process high-water
marks get recorded (§8).

**AD-11 — Fork & patch policy.** `HobbyOS/WebKit` fork (own repo clone on the
workstation, **quarterly rebase** on upstream release tags; patch surface kept
small and grouped under a `HobbyOS/` port directory + minimal upstream-file
touches). Vendored into the OS repo as: pinned tarball + `patches/` +
`README` (per AD-6/v1 App C pattern). NetSurf/libs use the v1 patch-on-copy
scheme unchanged.

**AD-12 — Protocol/input**: v1 App A.2 stands (`ESC [ K` modifiers, wheel as
buttons 4/5, `ESC [ D` close; single-write atomicity). Consumers: NetSurf
fb backend + WebKit shell.

**AD-13 — X11 decoupling** (§0): `src/user/x11/` is frozen-content (bugfix
only); do not grow it in this program.

---

## 5. Gap analysis → work chains

| Chain | Contents | Serves |
|---|---|---|
| **F — Foundations** | v1's M0–M2 essentially unchanged (§6 F0–F2) | Track A soon; Track B base |
| **P — Platform prerequisites** | Threads, VM, libc++, IPC, processes/signals, POSIX fill-in + SQLite, toolchain/fork, torture gate (§6 P1–P8) | Track B (some serve Track A too) |
| **NS — NetSurf track** | Deps host build → libnsfb backend → curl fetcher → JS enablement → bring-up → hardening (§6 NS-1…NS-6) | Track A |
| **WK — WebKit track** | Fork/build scaffolding → JSC shell → WebProcess headless render → networking → UI shell → acceptance → hardening (§6 WK-0…WK-7) | Track B |

---

## 6. Milestones

Format as v1: **goal → tasks → gate → evidence**; checkboxes for execution
sessions; gates run by the Integrator (§7.6) on the merged tree; a lane may
not tick a gate it did not run.

### F0 — Groundwork, pins, freezes (no OS code; v1 M0 + v2 additions)

- [x] **W0.1 Vendor sources** — WPE 2.54.0: tarball sha-verified against the
      official `.sums` + this document's pin table; tarball+`.sums` committed
      (extraction gitignored per AD-11).  Fork clone `webkitgtk-2.54.0` →
      commit `5220e80b97…` (deref recorded in §2), relocated to
      `~/webkit-hobbyos` (outside the NAS-synced tree).  **Deferred
      (just-in-time):** NetSurf + its libs (Track A dormant) and the L6
      library tarballs (libcurl/ICU/harfbuzz/SQLite/libwebp) — vendor when
      the L6 host builds start.
- [x] **W0.2 Freeze ABI v2** — `SYS_MAX` 71 confirmed in tree; 65–71 freeze
      committed (`syscall.h` + net errnos + libc socket/select surface);
      FD_SETSIZE 256 amendment recorded in §A.1a; §A.1b still marked
      provisional (not frozen until its gates).
- [x] **W0.3 Budgets** — decision: **256 MiB / FAT16 / 8 KiB clusters
      pinned** (`mkfs.fat -F 16 -s 16`), **no `fat16.c` change required**
      (TotSec32 parsing already live in production); RAM is not a constraint
      (3×32 MiB blocks vs 40 x64 / 232 arm64 pool blocks).  Full memo:
      `docs/browser/f0-budgets.md`.  The Makefile flip (one line + `-s 16`)
      is deferred per the memo — land it with the first browser-sized blob.
- [x] **W0.4 Skia-vs-Cairo spike plan + deps audit** — Skia is the default
      in-tree compositor (bundled @`588b550a…`, built from source); minimum
      versions, feature-trim list and dependency-off list produced:
      `docs/browser/f0-deps-audit.md` (§4/§5 fold into Appendix B.1).
- [x] **W0.5 Toolchain check** — requirements extracted (audit §6): C++23,
      CMake ≥3.20, Ninja generator required for WPE/GTK ports, GCC ≥12.2 /
      Clang (no explicit min; accommodations below 19), Perl ≥5.10 + English/
      FindBin/JSON::PP modules, Python3, **Ruby ≥2.5**, **GPerf ≥3.0.1**
      (ENABLE_WEBCORE only), unifdef + pkg-config.  Workstation: clang
      21.1.8 OK; **local gaps: `ruby` and `gperf` absent** — install before
      the L6/WebKit host builds.
- [ ] **W0.6 Track A pins** — deferred: Track A is dormant by default (§0.2).
- [x] **W0.7 Refresh inventories** (Appendix D) — regenerated 2026-09-29:
      `SYS_MAX`/`FD_SETSIZE`/`USER_INITIAL_CLEAR_SIZE` verified as §3
      describes; desktop.c symbol grep verified pre-lane-merge; full re-pass
      after F1 integration.

**Gate F0 (status 2026-09-29): closed.**  §2 pins recorded (WPE verified;
L6/NetSurf vendoring deferred by design); §10 questions 1/2/4 closed,
3 partial (L6); trim+deps audit archived; `git status` clean of unintended
edits (five lane worktrees carry their own trees).

### F1 — OS foundations (v1 M1 carried; foundation for both tracks — first in sequence)

Goal: the OS can run a `select()`-driven, socket-using, floating-point C/C++
program of several MiB.

- [x] **F1.1 Syscalls 65–71** *(v1 T1.1)* — as v1 §Appendix A.1a; three
      commits; kernel-unit + host tests. Add the **FD_SETSIZE 256** width to
      the masks at implementation time (record amendment in A.1a).
      **Done 2026-09-30:** merged `7a4900b` — both arches; unit tests in-suite
      (ARM 48/48, x64 50/50 KVM); SOCK2TST/POLLTST/RANDTST wired; §11.
- [x] **F1.2 `select()` engine** *(v1 T1.2)* — readiness for pipes/stdin/
      sockets, ms timeout, wakeups; documented wake strategy.
      **Done:** merged `7a4900b` — scheduler park (no busy-spin); POLLTST
      blocking/timeout checks in-wave; wake-slice v1 notes carried; §11.
- [x] **F1.3 TCP hardening** *(v1 T1.3)* — non-blocking connect completion,
      `POLLOUT` semantics, retransmit; extend `net_test.c` + `SOCK2TST.BIN`.
      **Done:** merged `7a4900b` — completion via select/SO_ERROR, bounded SYN
      retransmit; SOCK2TST live HTTP GET over connect_fd in-wave; §11.
- [x] **F1.4 Entropy** *(v1 T1.4)* — `SYS_GETRANDOM` + `virtio-rng` + jitter
      fallback.
      **Done:** merged `7a4900b` — mixed-source (no virtio-rng device in tree;
      noted §11); RANDTST in-wave; §11.
- [x] **F1.5 FPU both arches** *(v1 T1.5)* — enable + context switch; userland
      loses `-mgeneral-regs-only` (ARM) / gains SSE (x64); `FPU_T.BIN` exact
      value asserts across preemption/fork.
      **Done:** merged `09b2a71` + `660b3f0` — FPU_T 12/12 in-wave (ARM);
      x64 8/11 labeled minimal-boot + unit gate (fork-stage blocked by
      pre-existing plumbing, not FP); §11.
- [ ] **F1.6 Loader/memory interim** *(scope-changed, v1 T1.6)* — raise the
      *window/cap* only as needed by NetSurf (target ≤ 32 MiB images: cap
      decision at W0.3; zeroing strategy as v1). **The real fix is P2.**
- [x] **F1.7 DHCP→DNS hand-off** *(v1 T1.7)* — **Done:** merged `7a4900b`
      (+ wiring `c7c181f`); DNSTST resolves live (`10.0.2.3` →
      `172.66.147.243`); §11.
- [x] **F1.8 Desktop input v2** *(v1 T1.8)* — Alt/modifiers `ESC [ K` (pixel
      windows only), wheel→buttons 4/5, graceful close `ESC [ D`; host tests
      + x64 QEMU input-path verification first.
      **Done:** merged `eaa0465` — host +54/+20 checks, ARM+x64 E2E green,
      13-app regression 13/13; §11.

**Gate F1:** v1's gate stands: host + kernel + in-OS suites green on both
arches (`FPU_T`, `POLLTST`, `SOCK2TST`, entropy); boot-time delta recorded.
**Gate F1 closed 2026-09-30** — suites green in-wave on the CI VM; boot-time
delta +0.55 s (ARM) / +0.55 s (x64), within the +2 s target; full record §11.

### F2 — libc & C runtime (v1 M2 carried)

- [x] **F2.1 Socket/select surface** *(v1 T2.1; FD_SETSIZE 256)* — **Done:**
      merged `7a4900b` (libc wrappers over syscalls 65–71; select engine, FD_SETSIZE 256 §A.1a).
- [x] **F2.2 Resolver** *(v1 T2.2)* — `resolv.c` + `inet_pton/ntop`; host
      codec tests + `DNSTST.BIN`. **Done:** merged `1fce4f9` (+wiring `c7c181f`);
      DNSTST resolves live in-wave (DHCP DNS 10.0.2.3 → example.com A = 172.66.147.243).
- [x] **F2.3 Time** *(v1 T2.3)* — `clock_gettime`/`gettimeofday` + calendar
      math; host tests. **Done:** merged `1fce4f9` — calendar-math host parity
      (314 checks); further time surface tracked for P3.
- [x] **F2.4 Minimal C++ runtime** *(v1 T2.4)* — new/delete, guards,
      `.init_array`, `-fno-exceptions` policy; smoke both arches. *(libc++
      proper is P3; this unblocks early C++ smoke and Track A's small needs.)*
      **Done:** merged `0f194bd` (`bd8b7e8`) — `cxxrt.cpp`/`cxxrt.h`, crt0 walks
      `.init_array`; CXXSMOKE 29/29 (ARM full wave + x64 labeled minimal-boot);
      host-parity suites 25+19; F2.5 audit folded in; see §11.
- [x] **F2.5 stdio/string audit for C++ builds** *(v1 T2.5)* — **Done:** merged
      `0f194bd` — 13 headers `extern "C"`-guarded; mangled `_assert_fail`, stdlib
      `template`, host `ho_mkdir` fixes; census 43/45 + 2 documented artifacts; §11.

**Gate F2:** host tests + `CXX_SMOKE`, `SOCK2TST`, `DNSTST` green both arches.
**Gate F2 CLOSED 2026-09-30** — ARM: full-wave green (CXX_SMOKE 29/29, zero FAIL
 tokens; SOCK2TST/POLLTST/DNSTST green); x64: unit tier green + labeled
 minimal-boot evidence (CXX_SMOKE 29/29, DNSTST resolved) under the standing
 x64 policy; host tests green incl. the new C++ parity suites. See §11.

### P1 — Threads & pthreads  *(new; Track B)*

*(Design `docs/browser/p1-threads-design.md` — integrator-reviewed 2026-09-30,
OQ1–OQ6 resolved; implemented 2026-09-30, merged `e5188de`.)*

- [x] **P1.1 Kernel threads** — thread object sharing the address space;
      create/exit/join primitives; scheduler integration (timeslice across
      threads); per-thread kernel stacks; TLS register management on switch.
      **Done:** merged `e5188de` (lane `2404db2`..`1b70868`) — PCB-slot threads
      sharing the leader AS; `process_group()` routing; THREAD_DONE reclamation;
      exit_group from any thread; `tls_base` on the context switch; §11.
- [x] **P1.2 Futex-lite** — WAIT/WAKE on user addresses (word compare + park,
      timeout) — the primitive under mutexes/conds. Keep the surface minimal,
      document semantics precisely. **Done:** merged — WAIT/WAKE + timeout
      machine in `process_check_sleeping`; process-private, proc_lock-only; §11.
- [x] **P1.3 pthreads library** (`src/libc/pthread.c`) — `pthread_create/
      join/detach/self/equal/exit`, mutex (normal+recursive), condvar,
      once, rwlock (read-mostly correctness over fairness), barriers, keys
      (`pthread_key_*`), `pthread_attr_*` subset; `sched_yield`.
      **Done:** merged — musl-model TCB + crt0/`linker.ld` TLS; §11.
- [x] **P1.4 TLS verification** — 2 threads × distinct TLS values under
      preemption; `tls_test.c` in the wave. **Done:** TLS_T 6/6 in-wave
      (device) + glibc host variant; three device-only TLS bugs found and fixed
      pre-gate; §11.
- [x] **P1.5 Test** — `THRD_T.BIN`: N threads, mutex/cond ping-pong, atomic
      ops (`__atomic_*` builtins) under contention, join correctness; runs
      both arches. **Done:** THRD_T 20/20 in-wave; kernel unit additions
      (thread_group / thread_validation / slot_reclaim / futex_machine /
      set_tls + fresh-FP assertion); §11.

**Gate P1:** `THRD_T.BIN` + kernel suite green both arches; no scheduler
regressions in existing wave. **Gate P1 CLOSED 2026-09-30** — lane gate
(host 482/0, unit-arm 53/0, unit-x64 55/0 KVM, ARM wave 0 FAIL, THRD_T 20/20,
TLS_T 6/6); boot delta ARM +0.09 s / x64 ~0; merged-tip batteries: see §11.

### P2 — VM & memory overhaul  *(new; Track B, the deepest kernel item)*
*(S1–S5 delivered and merged — S1–S3 2026-09-30 (`c1214c5`/`f0e8473`/`6cbd49e`):
frame allocator, address-space v2, demand-zero mmap family; S4/S5 on
`browser/l2-p2-s45` (`b6b85c1`), merged 2026-10-01 as `e017069` + post-merge
integration fixups (`88f680c`/`058547d`/`1aaa3b2`): memfd/MAP_SHARED/fb/
pthread-guard stacks, crt0/TLS sweep + fatal-fault exerciser, v2 exec +
routing flip, TLS/ASID/waitpid fixes. Details §11 Wave 1e + Wave 1f tail;
design `docs/browser/p2-vm-design.md`.)*

- [x] **P2.1 Address-space model v2** — per-process *sparse* VA (multi-GB
      range), region lists; unmapped-hole faults become clean segmentation
      errors (not crashes of others); document the model in `docs/`.
      **Done (S2):** per-AS arm L1+L3 / x64 PML4+PDPT roots, 4 KiB leaves,
      USER_VA_BASE `0x1_0000_0000`, sparse 32 GiB window; HOLE faults kill
      only the faulting process (prev-round leaks fixed by construction).
- [x] **P2.2 Demand-zero anonymous pages** — fault → allocate zeroed physical
      on first touch; reclaim policy for never-touched overcommit; page-pool
      growth (current 40×32 MiB pool is the floor; add growth + accounting).
      **Done (S3):** demand-zero on both arches; frame accounting = 4 KiB
      bitmap; OOM kills the faulting process (swap out of scope, design D6).
- [x] **P2.3 mmap family v2** — `mmap` (anon + `MAP_SHARED` + fd-backed),
      `munmap`, `mprotect`, `madvise` (advisory ok), `mremap` (verify need),
      guard pages; libc wrappers (F2 gap). **Anon + munmap/mprotect/madvise +
      guard/segv rules done (S3)** (rows 62 6-arg, 81, 82; `SYS_MAX` 75→82);
      `MAP_SHARED`/fd-backed/memfd done (S4): rows 80–82 through the v2
      object layer (memfd slabs, MAP_SHARED instance refcounts,
      `F_ADD_SEALS`/`F_GET_SEALS` with Linux numbers); `mremap` verified
      unnecessary (no consumers in-tree).
- [x] **P2.4 Shared memory** — `memfd`-style anonymous file fd (proposed
      `SYS_MEMFD_CREATE`, §A.1b) + `MAP_SHARED` mapping = the WebKit
      shared-memory primitive; tests: two processes write/read shared pages.
      **Done (S4):** `SYS_MEMFD_CREATE` (row 80) both arches; anonymous
      file objects (truncate via `ftruncate`) with Linux-numbered seals;
      `MAP_SHARED` instance refcounting — two-process shm covered by the
      merged vm unit tests.
- [x] **P2.5 Loader v2** — load images up to the new budgets into demand-
      mapped regions; measure load time for ~30–60 MiB blobs (record);
      keep read-path loader as the mechanism initially. **Done (S2):**
      IMAGE-only commit, 8 MiB stack + heap + mmaps demand-materialize
      (`v2 loader: PID=49 entry=0x100000000 sp=0x180000000 resident=2
      tables=3`); **S5:** v2 is the routing default (rollback lever at the
      AS_V2 selection line); large-blob timing recorded as an extrapolation
      (≈12–17 MB/s ⇒ 30 MiB ≈ 1.7–2.6 s / 60 MiB ≈ 3.4–5.2 s) — direct
      measurement is blocked by a pre-existing >1 MiB fat16 create-path
      defect (`attr=0` entries; `v2 loader: bad image size 0`).
- [x] **P2.6 Regression sweep** — all existing tests re-run; document any
      behavior changes (fix log §11); boot-time delta recorded. **Suite
      re-runs green per stage** (S2→S3 wave counts byte-identical);
      **final at `1aaa3b2`:** host 482/0, unit-arm 291/0, unit-x64 293/0,
      ARM wave 0 FAIL + `System halt`; **boot delta −3.05 s** (median
      5.93 s vs 8.98 s baseline; budget ≤ +0.5 s ✓).

**Gate P2:** memory stress suite (map/fault/free loops, shared-page test,
two-process shm test, existing suites) green both arches; boot delta within
recorded budget. **Met at `1aaa3b2`** (batteries §11).

### P3 — libc++ & C++ runtime  *(new; Track B)*

- [x] **P3.1 Port libc++ + libc++abi** — static; **no exceptions, no RTTI**
      initially (matches WebKit's own compile configs; **verify** the exact
      needs per-component later); freestanding-ish config; threading backend
      = P1 pthreads; locale C-only (ICU arrives separately).
      **Done:** merged `3b83823` — vendored libc++/libc++abi **21.1.8**
      (recipe-form) static archives both arches (arm 8.9 MB / intel 8.6 MB);
      libc++abi owns new/delete/`__cxa_*`, cxxrt.cpp trimmed to the
      type_info/`__dynamic_cast` surface; exceptions/RTTI OFF, localization
      C-only, filesystem OFF, threads ON; §11.
- [x] **P3.2 Gap-fill surfaced by libc++** — `<chrono>` clocks, `<filesystem>`
      subset (or disable), `snprintf` family completeness, wide-char basics.
      **Done:** merged — wchar/wctype/strftime/xlocale/strtod/math/stdio
      families (~4 k lines across 15 libc files + tf2 builtins); host parity
      tests A/B/C (85/3314/1511 checks, 0 failures) wired into the host gate;
      §11.
- [x] **P3.3 Tests** — host parity tests (locale-independent subset);
      in-OS `CPP_T.BIN`: iostreams smoke, `<vector>/<string>/<map>`, atomics,
      thread join through `std::thread`; sizes measured.
      **Done:** merged — shipped as `CXX_T.BIN` (name consistent with
      CXXSMOKE); 20 checks incl. vector/string/map/unordered_map/algorithm/
      memory/atomic/thread/mutex/condvar/chrono/sstream, all PASS in the ARM
      wave; sizes: arm 586,888 B / intel 551,240 B; §11.

**Gate P3:** `CPP_T.BIN` + host tests green both arches; `.bin` sizes recorded.
      **Gate P3 CLOSED 2026-09-30** — lane gate green (host `TEST EXIT: 0`;
      unit-arm 53/0; unit-x64 55/0 KVM; ARM wave 0 FAIL + `System halt`);
      x64 acceptance = build+link + unit-x64 (standing x64 baseline — the
      full x64 wave remains incomplete at tip); merged-tip batteries: **GREEN
      both machines at `9104be5`** — attempt #1 (at `3b83823`) was RED and
      surfaced three integration defects, all fixed and re-verified (bootstrap
      `944fbda`, wide-parity `1a5277d`, silent CXX_T truncation `9104be5`; §11).

### P4 — IPC primitives  *(new; Track B)*
*(Design note complete: `docs/browser/p4-ipc-design.md` — `d454566`; D1–D13
(usock pairs riding the pipe park engine, SCM_RIGHTS transfer machine,
poll-only/no-epoll, rows 76–79 + errno EMSGSIZE/ENOTCONN); OQ1–OQ5 pending
integrator review. Implementation in tree: rows 76–79, IPC_T 86 checks — see
§11.)*

- [x] **P4.1 AF_UNIX sockets** — `socketpair(AF_UNIX)` + pathless sockets;
      stream semantics; fd namespace integration. (`SYS_SOCKETPAIR` §A.1b.)
      *(In tree: rows 76–79; IPC_T runs. Checkbox lagging records — see §11.)*
- [x] **P4.2 fd-passing** — `sendmsg`/`recvmsg` with `SCM_RIGHTS` (one fd
      minimum, arrays best-effort); close-on-fork inheritance rules defined.
      *(In tree: SYS_SENDMSG/SYS_RECVMSG rows 78/79; IPC_T ✓.)*
- [x] **P4.3 `poll()`** — syscall + libc (`pollfds` incl. `POLLIN/POLLOUT/
      POLLERR/HUP`), used by WebKit event loops; select stays for old code.
      *(In tree: SYS_POLL row 76; POLLTST bounds 85–5000ms `447f02a`.)*
- [x] **P4.4 Tests** — `IPC_T.BIN`: socketpair echo, fd-pass to a child
      process, poll multiplexing 3 fds; kernel-unit coverage. *(86 IPC_T
      checks green on wave batteries; hardened against wave-pressure.)*

**Gate P4:** `IPC_T.BIN` both arches; no regressions.

### P5 — Process management & signals  *(new; Track B)*
*(Design note `docs/browser/p5-exec-signals-design.md` — `b2bf160`; D1–D13;
OQ1–OQ8 resolved (`a4d66db`). Delivered: S1/S2 storage + kill semantics
(Waves 1e–1f: `87795d5`, `018a5aa`), S4 execve completion via the P2-S45 tail
merge (`e017069`), S3 frame engine + S5 `SYS_SPAWN_EX` in Wave 3
(`2450663`).)*

- [x] **P5.1 `execve`** — replace current image with a new one (argv/envp
      pass-through; fd inheritance + `FD_CLOEXEC` honored; executable found
      via explicit path first). — **Done** (S4, via the P2-S45 tail merge
      `e017069`; v2-loader routing + env-blob fixup `1aaa3b2` — see §11).
- [x] **P5.2 `waitpid`** — exit-status propagation, zombie reaping,
      parent/child bookkeeping for multiple children. — **Done** (S2 storage
      + the unified reap delivery `reap_deliver_locked`; status layout frozen
      — see §11).
- [x] **P5.3 Minimal signals** — `sigaction` real for a small set
      (`SIGKILL`, `SIGTERM`, `SIGPIPE`, `SIGCHLD`, `SIGSEGV`/`SIGBUS` default
      kill + report), delivery on process threads (pick main-thread delivery
      initially — **verify** WebKit's needs; JIT-off reduces requirements).
      — **Done** (S1 storage `87795d5` + S3 frame engine `2450663`: trap-exit
      + resume delivery, SIGRETURN, no-nesting, SIGCHLD from the reap path;
      SIGUSR1 tenure stays D13-deferred — see §11).
- [x] **P5.4 Tests** — `PROC_T.BIN`: spawn child → socketpair talk → exec →
      exit code → reap; SIGCHLD observed; SIGPIPE kills writer cleanly.
      — **Done** (`PROC_T` + `SIG_T.BIN` 62/62 in-wave incl. spawn_ex
      fdmap/CLOEXEC; wave 17 summaries at `cbbdd70` — see §11).

**Gate P5:** `PROC_T.BIN` both arches; no regressions; process-capacity check
(≥ 6 concurrent processes documented). — **Met (ARM wave)**: `PROC_T` +
`SIG_T` green at `cbbdd70`/`447f02a`; x64 evidence = unit-x64 293/0 + no new
x64 wave signature (§11).

### P6 — POSIX fill-in & SQLite  *(new; Track B)*

- [x] **P6.1** `mkstemp`/`tmpfile`/`fsync`/`ftruncate`/`fchmod` stubs-as-
      appropriate; `flock`/`fcntl` record locks (SQLite); `getcwd`/`chdir`
      (if missing); `utime`; `sched_yield`; `sysconf` bits; `getpwuid`/
      `getgroups` stubs returning sane single-user values; `locale` C.
      *(In tree; ftruncate dup fixed `88f680c` — see §11.)*
- [x] **P6.2 Port SQLite** — pinned version; `SQLITE_THREADSAFE=1`,
      single-OS VFS over our file API; fcntl locks per P6.1; in-OS test
      (create/insert/select, WAL off initially). *(Pinned + vendored (AD-11);
      SQLTEST boots in-OS; sqlite3.h explicit Makefile rule.)*
- [x] **P6.3** Environment/aux: `environ` hygiene, `PATH`-less exec rules,
      argv[0]/program-path exposure needed by WebKit (**verify** its
      executable-path discovery; supply via spawn contract).
      *(Env blob `1aaa3b2`; argv pipeline landed with P5 exec.)*

**Gate P6:** SQLite in-OS test + host tests green both arches.

### P7 — Toolchain, fork, build system  *(new; Track B; starts at W1)*

- [x] **P7.1 Fork stand-up** — `HobbyOS/WebKit` clone at tag
      `webkitgtk-2.54.0`; add `HobbyOS/` port dir (platform files, cmake
      toolchain file, port README + rebase log); CI script = "does it
      configure" on host for sanity. *(Fork at ~/webkit-hobbyos
      `browser/l8-wk1`; WK-0 ✓; WK-2 configure GREEN `5692a0516b`.)*
- [x] **P7.2 Port scaffolding** — `WTF_OS_HOBBYOS` platform detection;
      `Platform.h`/`PlatformHobbyOS.*` (time, memory, threads conversion,
      file syscalls, StackBounds, OSAllocator); build with
      `USE_SYSTEM_MALLOC`; strip JIT options. *(toolchain-hobbyos.cmake;
      JSCOnly static C_LOOP; WK-1 jsc on-device ✓.)*
- [x] **P7.3 Build integration for HobbyOS** — cross CMake toolchain
      (`CMAKE_SYSTEM_NAME=HobbyOS`, clang, static, our sysroot + libc++);
      document the exact cmake invocation in the fork README; Wire into the
      OS repo as build rules request (Integrator applies).
      *(Toolchain `b502c65`; canonical recipe in fork README + WK2-EVIDENCE.)*
- [x] **P7.4 Feature-trim list applied** — from W0.4: media/GPU/WebRTC/WebGL/
      WebAudio/PDF/plugins off; SVG/WebP on; ICU + harfbuzz on; curl backend
      on; sandbox off; JIT off; record the full define set in the fork README.
      *(Trims applied + validated vs WebKitFeatures.cmake across WK-2; curl OFF
      at WK-2 (D-7), harfbuzz via hbcore (D-15), JIT off, media/GPU/WebGL off —
      see PORT_PLAN register.)*

**Gate P7:** fork configures + builds the "null" targets (WTF only) for both
HobbyOS arches as far as the current P-stage allows (this gate is *incremental*
— it re-runs as P1–P6 land; track it in §11).

### P8 — "POSIX torture" gate  *(new; the gate before WK leans on the OS)*

- [x] **P8.1 Suite** `TORTURE.BIN` + host parts: 64 threads × mutex/cond
      churn; socketpair fd-pass loops; mmap/fault/free storms; exec/wait
      cycles; poll on many fds; memory high-water recording. — **Done**
      (`browser/l3-p8` `c130994`..; suite reframed as 64 create/join cycles
      in bounded bursts — see §11; wave: 8/8 PASS, 0 FAIL tokens).
- [x] **P8.2 Soak** — 30-minute combined run, no panics/leaks beyond bounds;
      numbers recorded in §11. — **Done**: the ~480-round freeze was
      root-caused and fixed (`30e0089`; lane + follow-up entries in §11);
      post-fix merged-tip soak 806 rounds, violations=0, clean halt.

**Gate P8:** green both arches; §11 updated with the numbers. *This is the
"OS is ready for WebKit" evidence.*  — **Met**: P8.1 green; P8.2 numbers +
freeze fix (incl. the merged-tip 806-round soak) in §11.

### NS — NetSurf track  *(Track A — optional contingency, **dormant by default**; start only if invoked. Track B does not depend on it.)*

- [ ] **NS-1 Host build** — pristine NetSurf 3.11 + libs on the workstation;
      framebuffer target built and smoked (W0.6 continues); two-build rule
      established for every patch (host build + smoke before OS).
- [ ] **NS-2 Surface backend** — our libnsfb backend ("hobbyos") that emits
      desktop pixel-protocol messages (window create/blit/damage) + input
      translation from `ESC [ P/G/R/K`/keys into libnsfb events. (No X11.)
- [ ] **NS-3 Fetch/network** — libcurl fetcher enabled (mbedTLS; CA bundle);
      resolver via F2.2; http/https fixtures from `10.0.2.2`; cookies off v1.
- [ ] **NS-4 JS enablement** — enable Duktape in the build (record what it
      takes at 3.11 — the 2017 workaround is our baseline; **verify** current);
      JS smoke fixtures (document.title probes, simple DOM updates); **write
      down the known-unimplemented list impact** (from `jsbinding.md`).
- [ ] **NS-5 Bring-up + E2E** — `run_netsurf_test.py` in the
      `run_xcalc_test.py` mold: file:// fixtures → http fixtures → link
      navigation → scroll → graceful close; pixel assertions; both arches.
      Acceptance set: fixtures + `lite.cnn.com`-shaped page + doc'd Google
      outcome (expected: JS wall — record evidence, do not "fix").
- [ ] **NS-6 Hardening** — soak, memory, sizes, docs
      (`docs/netsurf.md` build/run/test). **Stop/continue gate:** review vs
      Track B progress; either continue polish or park (documented).

**Gate NS-5:** E2E green both arches; §8.3 Track-A acceptance lines checked.
**Gate NS-6:** soak + docs done; decision recorded.

### WK — WebKit track  *(Track B; the bar)*

- [x] **WK-0 Scaffolding (with P7.1–.2)** — fork + null build as far as
      possible; port README (goals, constraints, rebase policy); decision log
      opened in the fork. **Done (scaffold):** `hobbyos/wk0-scaffold` in
      `~/webkit-hobbyos` (`d055b25768`; 19 files, 1220 insertions, zero upstream
      edits) — PORT_STATE + PORT_PLAN + Options/PlatformHobbyOS CMake bootstrap
      + WTF stubs + WK-1 feasibility memo (H-1..H-12, OQ-1..OQ-11). Null build
      deferred to WK-1 (gated on register row 1 + cross toolchain; §11).
- [x] **WK-1 `jsc` shell on HobbyOS** *(first deliverable!)* — JSCOnly target
      for HobbyOS: platform files needed by JSC; static link; `jsc` runs:
      arithmetic/strings/regex/JSON/Date; **test262 language-subset run**
      (record pass rate; crash-free is the hard gate, pass-rate is the
      record); `jsc`-driven host-file read/write smoke. *Requires P1–P3
      minimums; sequence starts as they land.*
      **Gate WK-1:** `jsc` on both arches runs the test set; evidence = raw
      console output + test262 summary; §11 updated.
      *(ARM gate MET 2026-10-02: smoke 42/42 on-device + test262 subset
      500/500 crash-free, raw serial logs; x64 leg env-blocked by a host
      OVMF-boot freeze, not a code miss — the OS-side x64 blocker that was
      found (watchdog stack scan) is fixed at `18a62f4`. §11 batch-3.)*
- [x] **WK-1.5 Architecture decision** — WebKit2-multiprocess vs
      WebKitLegacy-in-fork vs re-pin (2.52.6): written comparison, decided,
      recorded (AD-3 default = WebKit2; deviations need the comparison to
      say why). **Gate = the decision + rationale on record.**
      *(DECIDED at CP-2, 2026-10-02 → **WebKit2 multi-process (AD-3)**.
      Summary: (1) WebKit2 = the living upstream architecture (WinCairo/WPE/
      GTK), and it matches this OS's own process model — the microkernel IS
      the process boundary (spawn2/spawn_ex, pipes+fd-passing, waitpid/reap,
      64-slot table stress-tested in Waves 1–3), jsc already runs as an in-OS
      binary, and the plan already shelves GPU process + sandbox (the two
      hardest pieces). (2) WebKitLegacy-in-fork (Haiku's path) has only Apple
      platforms upstream today — reviving is explicitly higher fork-maintenance
      cost per AD-3, and it forfeits crash isolation. (3) Re-pin 2.52.6 is NOT
      an architecture choice but AD-4's fallback lever, gated on the W2 Skia
      spike — not indicated now. Written comparison body → fork README.)*
- [x] **WK-2 Full-target build + stubs** — extend port platform code to what
      WebCore/WebKit need to *configure, compile, and link* all three process
      binaries (unimplemented platform functions may abort with a message at
      this stage); Skia-vs-Cairo spike result applied (AD-4 lever).
      **Gate WK-2: MET 2026-10-02 (fork `browser/l8-wk1`)** — WebKit static
      (AD-6), configure GREEN, WebCore+WebKit+Skia compile clean, `extern "C"`
      sysroot-header hygiene fixed; ARM binaries LINK: WebProcess 139,108,664 B
      / NetworkProcess 135,088,920 B / HobbyOS-UIProcess 58,311,528 B (+ relink
      wall-times); jsc smoke rc=0 (47,516,296 B); ci script `wk2-link-ci.sh`
      validated; x64 test262 script fixes committed (c712f63d3d). W2-R5 receipts
      in fork WK2-EVIDENCE.md. Note: WebProcess ELF exceeds the 64 MiB
      user-image cap — flatten/strip or a cap decision is WK-3 step 0.
- [x] **WK-3 Headless web process** — WebProcess standalone: create a page,
      load `file://` fixtures, run JS, paint into our surface, **dump
      PNG/pixel-hash** for comparison (no UI process yet); JS probes via
      `evaluateJavaScript`-equivalent → stdout. Includes text stack
      (FreeType+harfbuzz+our font backend), images (PNG/JPEG/WebP).
      **Gate WK-3: MET 2026-10-06 on ARM + x64** — fixture ladder renders +
      JS probes correct both arches (cross-arch FPC parity; `wk3/WK3-REPORT.json`
      unified with the wk3x64 closeout by lane H2, `a6dd6b49bd`): FIX01
      `0x759431c5`, FIX02/02D `0x5f62b9c5`, FIX03 `0xb1e75dc5`, FIX04
      `0xbc622575`, FIX05 `0xa0bf6dc5` on both arches; 7/7 green runs when the
      gate closed.
- [x] **WK-4 Networking** — NetworkProcess + libcurl backend: fetch fixtures
      over http/https from `10.0.2.2`; DNS via resolver; cookies (minimal);
      redirects; gzip (zlib).
      **Gate WK-4: GREEN on ARM (WK-4c, merged `90d3112a0c`)** — HTTP 200 /
      302→follow / HTTPS 200 / cookie round-trip (`ROUNDTRIP-42`) / CA-enforced
      negative rc=77 through the real NetworkProcess→libcurl(mbedTLS) path;
      gzip decode on-device per lane N1 (wire=gzip, decoded sha == fixture);
      direct https to real edges GREEN at the merged state (§11 W3/W4); ≥4 MiB
      single-response bodies GREEN per lane N3 (OS `7631be0`).
- [x] **WK-5 UI shell** — our browser window: desktop pixel protocol
      (create/damage present), input translation (mouse/keys/wheel), Back/
      Forward/reload/stop keys, URL entry (keyboard prompt initially — no
      toolbar claims), scroll; present from the compositor path; window
      title/close integrated (F1.8).
      **Gate WK-5: MET** — real-page sessions on QEMU navigate by link/URL
      entry, wheel scroll, graceful close, no crash; address bar + GO +
      empty-DOM fix landed (`97ac3fa07c`/`49f28a3bb5`, §11); interaction rows
      re-verified at every later wave: native 1916×982 render (R5), nav 7/7
      (R4), x64 click/hover/keyboard green (X1/X6).
- [x] **WK-6 Acceptance** — §8.3 Track-B lines: **Google Search** (load,
      type query, results render, open a result) and **CNN** (homepage
      renders, article opens, scrolls, images show), JS-heavy pages
      exercised; 5-min soak; memory/sizes recorded; performance noted
      qualitatively. Any site failures documented, not hidden.
      **Gate WK-6: MET WITH DOCUMENTED EXCEPTIONS (2026-10-06 — fs-g11 + V2c
      + V4 + `docs/RELEASE-READINESS.md` §2)** — §8.3 lines 1,3,4,5,7 DONE at
      the merged state; line 2 (CNN) = fetch/transfer PASS both legs with the
      big-doc parser-completion residual documented (owner R10/R11); line 6
      hard gates green (host 482/0, unit-arm 302/0, unit-x64 306/0 KVM; CI
      fs-w2a all tiers rc=0; fs-w5x test-arm ambient classes documented);
      Google G3/G4 = JS-required permanent expected-fail; x64 §8.3 rows =
      documented exception (owner input-stretch).
- [x] **WK-7 Hardening & stretch** — crash recovery (WebProcess crash →
      shell survives, page reloadable), high-water marks, startup time,
      license compliance package (LGPL relink provision for static WebKit),
      `docs/webkit.md`; stretch backlog: file-backed demand paging/JIT
      reconsideration/tabs/persistent cookies/GPU.

**Gate WK-7: MET WITH DOCUMENTED EXCEPTIONS (2026-10-06)** —
crash-recovery 12/12 (`wk7/evidence/wk7-run3/`) and auto-respawn 14/14
(`wk7-respawn/`) on ARM; license relink package done (H1 `5d9ad88067`,
D-16) with the written-offer text a draft and the artifact bundle to be
materialized by the Integrator; x64 crash-recovery matrix open on the x64
input stretch; `docs/webkit.md` + `docs/RELEASE-READINESS.md` done.

---

### Close-out — final-stretch.md executed end-to-end (2026-10-07)

**Final state (frozen):** OS `~/Documents/GitHub/HobbyOS` @ main `1b06ca1` (+ this record); fork `~/webkit-hobbyos`
@ `cdabe011af` (ARM WebProcess ref `779d154bad00…`, 147.5 MB; intel `51232e4b…`). All five tracks (R/N/X/V/H) and
every wave (FS-W1…W5 + the close-out run R11→V5) executed and merged; the WK-3…WK-7 boxes above stand; §11 carries
the complete, evidence-linked fix log. Plan exit condition met: WK-6/WK-7 checkboxes honest; docs done.

**What the close-out run resolved after the 2026-10-06 exceptions were recorded:**
- **CNN big-doc completion — CLOSED.** R11 font flush → R12 SVG hidden-page replay suppression → R13b coalesced
  drain (batch-kick + post-complete straggler gate): `[WIN] load-ok` fires on the 5.7 MB page (`ms=2812839`),
  `readyState=Complete` reachable, receipt quartet captured (styled frame `0xb30e689e`, census 80/80 images,
  screenshot). R14 then closed CNN **text**: the WK6 disk never staged `/DEJAVU.TTF` → empty font manager → zero
  glyphs for any family; staging fix folded into the shared runner; gate8 shows the lead h2 legible and nav-link
  rects went 0-width → text-width.
- **WM integration (user-reported) — FIXED.** R15: instant reflow catch-up (cached frame re-blit into the new tile +
  instrumented viewport apply before the slow re-render; adapted in the same second — 2.67 s), WM close exits within
  grace (`[WIN] close ok` + `exit rc=0`), address-bar font = the taskbar's 8×8 at 1:1 (pixel-identical,
  controller vision-verified).
- **Interaction validation (user-requested) — DONE.** V5: fixture pass all-green (link click; scroll both directions;
  typed address-bar entry + navigation; close 3/3); real page: link activation (`[WIN] link -> …/Main_Page` + title +
  load-ok), scroll receipts, search-form submit → `…/w/index.php?search=hobbyist`. Six limitations documented
  precisely (PageUp guest-input drop; settle-window starvation; sidebar-link overlay hit-test; slow TCG reloads;
  close-under-heavy-load; F3-channel typing) — none release-blocking; all detailed in §11.

**Frozen-tip battery `fs-w7` (2026-10-07):** host rc=0 (53 s) — unit-arm rc=0 (179 s) — unit-x64 rc=0 (109 s, KVM) —
test-arm: wave complete (`System halt.`, STRESS 120/120) with 1× SHELLTEST WATCHDOG (documented ambient family).

**Residual register (all documented; none release-blocking):** x64 wheel step + x64 §8.3 stretch rows
(input-stretch owner); CI test-arm ambient families (SHELLTEST / slot-pressure / condvar — proven variance; clean
local waves on the same kernels); teardown ATEXIT stack-walker VA=0x8 (cosmetic, post-exit); PageUp guest-input
drop; Wikipedia sidebar-link overlay hit-test; slow address-bar reloads under TCG; WK6-harness `c1-images` /
`c2-close` step quirks (driver-side, pre-existing, byte-identical across gates 5–8).
**Accepted limitations carried from plan §1.6:** TCG performance ("works but glacial" — numbers recorded, not
hidden), no JIT/GPU, no-JS posture on the T2 path.

---

## 7. Parallel execution plan (multiple agent sessions)

### 7.1 Lanes

A lane = one or more agent sessions; a session works only inside its lane's
owned paths unless the brief says otherwise.

| Lane | Scope | Owns | Depends on | Notes |
|---|---|---|---|---|
| **L1 — kernel-core** | F1 | `src/kernel/{net.c,fs.c,dhcp.c,…}`, `src/include/{syscall.h,net.h}` (F1 subset) | F0 | starts immediately |
| **L2 — kernel-platform** | P1, P2, P4, P5 | `src/kernel/arch/**`, `process.c`, `scheduler`, new VM/IPC/thread files | F0; coordination with L1 on shared files | the deep lane |
| **L3 — libc/runtime** | F2, P3, P6 | `src/libc/**`, headers | F0; P3 needs P1 (threads) | |
| **L4 — desktop** | F1.8 | `desktop.c`, `window.h`, `desktop_*_test.c` | F0 | small; finishes early |
| **L5 — fonts/images** | font stack (FreeType+harfbuzz+backend) | `third_party/freetype*`, harfbuzz vendored, font assets, backend files | F0; needs P2 for WebKit scale only later | host-first |
| **L6 — libs-3p (host-first)** | libcurl, SQLite, ICU, Skia spike, zlib/png/jpeg/webp, NetSurf libs | `third_party/**` (build scripts), `src/user/<glue>` config | F0 — **no HobbyOS build needed** | parallelization engine |
| **L7 — NetSurf (Track A)** | NS-1…NS-6 (host-side early) | `src/user/browser/netsurf/**`, `src/user/browser/tools/**` | F0 pins; on-OS parts need F1/F2 | **dormant by default (§0.2)** |
| **L8 — WebKit port (Track B)** | P7, WK-0…WK-7 | fork clone (own repo, worktrees), `src/user/browser/webkit/**` on-OS glue | P-stages landing; JSC needs P1–P3 min | split sub-lanes when ≥3 sessions: **L8a build/port**, **L8b JSC**, **L8c render/text**, **L8d network/UI** — single-writer map stays on the fork files |
| **L9 — test/QA** | suites in §8.2, fixtures, harnesses | `src/user/*_test.c`, `tests/fixtures/**`, `run_*.py` | each milestone | |
| **I — Integrator** | all gates | `Makefile`, disk recipe, test-wave lists, merges, §11 | — | single writer |

### 7.2 Waves (critical path)

- **Wave 0:** F0 (W0.1–.7) — 1–2 sessions serial-ish; L6/L7 host work may
  overlap the tail.
- **Wave 1 (max parallelism):** L1 (F1.1 first), L2 (P1 threads first — it
  unblocks the most), L3 (F2 headers/wrappers — compileable against frozen
  numbers before syscalls exist), L4 (F1.8), L5 (FreeType host build + font
  assets), L6 (NetSurf libs + libcurl + SQLite host builds; Skia spike),
  L7 (Track A host-only — dormant by default; skip unless invoked), L8 (P7.1
  fork stand-up + port scaffolding; WK-0).
  → **up to 8 concurrent sessions** in this wave (7 if Track A stays dormant).
- **Wave 2:** L1 finishes; L2 continues P2 (VM — the long pole) then P4/P5;
  L3 starts P3 when P1 minimum lands; L7 NS-3…NS-5 as F1/F2 gate (only if
  Track A is active); L8 builds what it can (WK-2 stubs); L9 grows with each
  gate; **checkpoint CP-1 after F1+F2:** Track B should have WTF compiling
  (Track A, if invoked, a rendering-in-OS target).
- **Wave 3:** L2 P5/P8; L3 P6 (SQLite); L8 WK-1 (`jsc`) as P1–P3 minimums
  land → **checkpoint CP-2 at WK-1 gate: go/no-go review (cost-so-far vs
  remaining; the architecture decision WK-1.5 is made here).**
  *(CP-2 TAKEN 2026-10-02 → **GO**, WebKit2 per AD-3 (WK-1.5 ticked above): WK-1
  gate met on ARM (jsc on-device smoke 42/42 + test262 subset 500/500,
  crash-free); the OS-side x64 blocker found (watchdog stack scan on
  never-booted cores) fixed at `18a62f4`; the remaining x64 run-block is a
  host-level OVMF-boot freeze (env drift, recorded §11 batch-3). Cost-so-far
  within plan; next go/no-go = CP-3 after WK-2…WK-3 (does a page render?).
  Full written comparison body to be recorded in the fork README by the WK-2
  lane.)*
- **Wave 4:** L2 tail; L8 WK-2 → WK-3 (headless) — **checkpoint CP-3: does a
  page render? (this is the "engine actually works" moment)**; L7 wraps (if
  active).
- **Wave 5:** L8 WK-4/WK-5 (network, UI shell); L9 E2E.
- **Wave 6:** WK-6 acceptance, WK-7 hardening; docs; final gates.

Rule (from v1): a lane never starts its in-OS tests before its dependency
milestone's gate passed on the merged tree; host-side work has no such
constraint — host builds/tests are the parallelization engine of early waves.
Track A note: L7 is dormant by default (§0.2) — if the contingency is
invoked, run its NS tasks alongside Waves 2–4 as written.

### 7.3 Frozen interfaces (binding)

| # | Interface | Owner | Change protocol |
|---|---|---|---|
| F1 | Syscall numbers + ABI (`src/include/syscall.h` + dispatch) | L1/L2 by segment (F1 range: L1; P-stage ranges: L2) | amendments recorded in §A (A.1a frozen; A.1b frozen *at each gate*), then single-writer edit |
| F2 | Desktop protocol grammar | L4 | same as v1 |
| F3 | `poll`/`select`/fd semantics | L2 (kernel) + L3 (libc) joint spec note | freeze at their gates; consumers adapt |
| F4 | Fork build defines + port surface (`HobbyOS/` dir) | L8 | fork README is the contract; L8-only edits |
| F5 | Pixel-protocol client helper API (used by NS fb backend + WK shell) | L7/L8 jointly, seam in `src/user/browser/common/` | freeze at CP-1 |
| F6 | Makefile + test-wave lists + disk recipe | **I only** | lanes propose diffs; I applies |
| F7 | E2E harness conventions (`run_*.py`) | L9 | v1 pattern; new harnesses reviewed by I |
| F8 | §11 fix-log format | I | append-mostly |

### 7.4 File ownership map (collision dodge)

- Kernel: `net.c/fs.c/dhcp.c` → L1; `arch/**`, `process.c`, scheduler, new
  VM/IPC/thread files → L2; shared header edits request from owner.
- `src/libc/**` → L3 (pthread/runtime files included).
- `desktop.c`/`window.h` → L4 until F1.8 done, then frozen.
- `third_party/**` trees pristine; per-package build/config glue → owning lane
  (L5 fonts, L6 libs); cstyle exclusions unchanged.
- WebKit fork: worktrees per sub-lane; port files single-writer per file
  (listed in fork README); rebases by I-coordination.
- `browser.md` (this file): **I** edits sections; lanes propose via reports;
  §11 append-mostly.

### 7.5 Session briefs (first actions per lane)

- **L1:** skills `hobbyos-development`, `hobbyos-kernel-constraints`; start
  F1.1(a) `SOCKET`/`CONNECT_FD` + unit test; read §A.1a first; evidence =
  raw kernel/unit logs.
- **L2:** skills `hobbyos-development`, `hobbyos-kernel-constraints`; start
  P1.1 threads (design note first — review with I), then P1.2 futex-lite;
  read any existing `process.c` notes; VM work (P2) gets its own design note
  before code; serialize `arch/` edits.
- **L3:** skills `hobbyos-development`, `hobbyos-run-tests`; start F2 headers/
  wrappers against frozen numbers; then P3 libc++ port (host-first builds via
  L6's patterns); P6 SQLite last.
- **L4:** skills `hobbyos-desktop-compositor`, `hobbyos-gui-test`; host tests
  for `K`/wheel/close first; verify x64 input path early.
- **L5:** start FreeType host build + memory-face loader; then harfbuzz;
  then the WebKit font backend contract with L8 (seam frozen at CP-1).
- **L6:** host builds: NetSurf libs → libcurl (+mbedTLS) → SQLite → ICU →
  Skia spike (timeboxed; write result to §11); produce build scripts that
  the target builds will reuse.
- **L7 (dormant by default — start only if the Track A contingency is
  invoked):** NS-1/NS-2 host-only until F1/F2 gates; keep every patch inside
  the two-build rule; fixtures land early for both tracks (shared in
  `tests/fixtures/browser/`).
- **L8:** P7.1 fork stand-up immediately (no OS deps); port scaffolding with
  `USE_SYSTEM_MALLOC` + trimmed flags; WK-1 attempt as soon as P1–P3 minimums
  are green; keep the fork's decision log current — CP-2/CP-3 read it.
- **L9:** build the fixture ladder + `run_netsurf_test.py` /
  `run_webkit_test.py` skeletons early (harness-first), following
  `run_xcalc_test.py`.
- **I:** apply Makefile/disk/test-list diffs; run gates on merged trees;
  maintain §11; enforce §7.3; own checkpoint reviews CP-1…CP-3.

### 7.6 Integration protocol (git)

As v1: commits local-only, one logical change per commit; lanes use worktree
branches (`browser/l2-threads`, `browser/l8-jsc`, …); I merges in wave order
running gates before advancing; fork repo follows the same discipline with
rebases recorded in the fork README. Lane "done" report: what changed, exact
commands, raw evidence, unchecked risks, proposed diffs (Makefile etc.).

---

## 8. Testing & acceptance strategy

### 8.1 Tiers (all existing tiers grow; none replaced)

1. **Host tests** (`make host_tests`) — grows: resolver codec, time math,
   pthreads subset (host-backed), libc++ smoke, NetSurf lib tests, fixture
   golden files.
2. **Kernel unit** — existing suite + select/socket/entropy/FPU/threads/VM/
   IPC/process cases.
3. **In-OS wave** (`*TEST.BIN`; ARM + `make test_intel`) — new: `THRD_T`,
   `CPP_T`, `IPC_T`, `PROC_T`, `TORTURE` (+ v1's `FPU_T`, `POLLTST`,
   `SOCK2TST`, `DNSTST`, font tests).
4. **QMP E2E** — `run_xcalc_test.py` (regression), `run_netsurf_test.py`,
   `run_webkit_test.py` (incl. headless dump mode), fixture ladder.
5. **Two-build rule** — every patch to vendored trees must apply + build +
   smoke on the host before the OS build (extends to NetSurf and the WebKit
   fork's HobbyOS-port files where host-buildable).
6. **Parity philosophy** (v1 carried) — byte-wise host races where references
   exist (resolver vs glibc, calendar math).
7. **New: JS evidence rule** — "JS works" claims need observed evaluation
   output (probe scripts whose values are printed/dumped), not just rendered
   screenshots (screenshots alone can't distinguish JS output from HTML).

### 8.2 New tests inventory (paths)

| Test | Kind | Where |
|---|---|---|
| jsc shell + test262 subset | in-OS binary | `jsc` on image; harness `run_webkit_test.py --jsc` |
| headless render dumps (pixel hashes / PNG) | in-OS + host compare | `WKDUMP` mode of the shell; fixtures `tests/fixtures/browser/**` |
| JS probes (title/DOM/computed values) | in-OS | fixture pages + `--probe` mode |
| threads/mutex/cond | in-OS + host | `src/user/thrd_test.c` → `THRD_T`; host pthreads subset |
| VM/mmap/shared memory | in-OS + kernel unit | `TORTURE` + kernel cases |
| IPC (socketpair, fd-pass, poll) | in-OS | `src/user/ipc_test.c` → `IPC_T` |
| process exec/wait/signals | in-OS | `src/user/proc_test.c` → `PROC_T` |
| SQLite smoke | in-OS | `src/user/sqlite_test.c` |
| NetSurf E2E | E2E | `run_netsurf_test.py` |
| WebKit E2E + acceptance | E2E | `run_webkit_test.py` |
| fixture ladder (shared) | assets | `tests/fixtures/browser/{static,js,img,tables,forms}/` |

### 8.3 Acceptance checklist

**Track A (NS-5) "done":** fixtures (file:// + http://) render; navigation +
scroll + close; JS smoke fixtures show partial-JS behavior as documented;
CNN-lite-shaped page usable; Google outcome recorded (JS wall — documented, no
"fix" attempted). Both arches.

**Track B (WK-6) "done" = the requirement:**

1. **Google Search** — load `google.com`, type a query, results render,
   open a result page; no crash; evidence: screen pixel readback + probe
   values + session log.
2. **CNN** — homepage renders (headlines + images), open an article, scroll,
   read; no crash; evidence same.
3. Fixture ladder 100% (static/js/img/tables/forms); JS probes correct.
4. Networked https + cookies round-trip; redirect handling.
5. 5-minute soak (Google/CNN browsing loop) clean; memory high-water and
   binary sizes recorded; startup time recorded.
6. Full matrix green: `host_tests` + `make test` + `make test_intel` + all
   E2E harnesses + torture suite.
7. Docs: `docs/webkit.md` (+ `docs/netsurf.md`) build/run/test; license
   compliance package present; §11 complete.

Both arches. Slow-but-working is success; broken-but-fast is not.

### 8.4 Evidence standards

As v1 (no "done" without raw output from the real case; rendering claims need
pixel readback; network claims need observed transfer) **plus**: JS claims
need observed evaluation output; large claims (test262 pass rates, memory
numbers) must cite the raw log lines. Failures get fix-log entries with
diagnosis, not just patches.

---

## 9. Risks & mitigations (with checkpoints)

| # | Risk | Mitigation |
|---|---|---|
| R1 | **Scale** — this is the largest program in the repo's history; agents burn out or drift | Staged gates accrue value (jsc → render → browser → network → acceptance); checkpoints CP-1…CP-3 (§7.2) with written go/no-go; each milestone independently useful; Haiku calibration kept in view (§1.3) |
| R2 | WebKit platform layer deeper than planned (hidden POSIX/port surface) | P8 torture gate before WK leans hard; CP-2/CP-3 review; fallback levers recorded (AD-3 alternatives) prove no dead ends |
| R3 | Skia port cost/uncertainty | Timeboxed spike at W1–W2; fallback re-pin 2.52.6+Cairo (sha pinned); decision recorded either way |
| R4 | VM overhaul destabilizes the OS | Own design note + staged delivery (anon → shared → fd-backed); full regression sweep per step; boot-time + memory deltas recorded |
| R5 | Threads/futex bugs (lockups, lost wakeups) | Futex semantics documented; torture suite at P8; host-side pthreads subset tests; kernel watchdog patterns if needed |
| R6 | libc++ integration (headers, ABI bits, no-exceptions config) | Static + trimmed config; host parity tests; expectations set that some parts get disabled rather than ported (filesystem subset etc.) |
| R7 | Upstream drift (WebKit releases move; fork ages) | Pin + quarterly rebase discipline; small patch surface; rebase log in fork README |
| R8 | Binary sizes / disk / RAM growth beyond budgets | W0.3 budgets; sizes recorded at every WK gate; strip + trim; image growth is cheap compared to code (measure before optimizing) |
| R9 | Performance (no JIT; TCG QEMU) — "works but glacial" | Explicitly accepted (§1.6); numbers recorded, not hidden; JIT/GPU re-evaluation only at WK-7 stretch; do not promise responsiveness |
| R10 | Track A expectations (partial JS misread as "done") | Docs + acceptance wording state partial-JS clearly; NS-6 stop/continue gate |
| R11 | Lane collisions in the fork and kernel shared files | §7.3/§7.4 map; worktrees; single-writer per file; I serializes the rest |
| R12 | License compliance for statically-linked WebKit (LGPL) | Compliance package task at WK-7; texts vendored; relink provision documented early (cheap to do at build time) |
| R13 | TLS/CA/time correctness on device | v1 mitigations carried (clock tests, wrong-CA negative test, CA bundle pinned) |
| R14 | Cookie/storage expectations (logins won't persist well) | Scoped: cookies minimal, IndexedDB off; documented; acceptance doesn't include logged-in flows |
| R15 | "Google/CNN depend on everything at once" (network + TLS + JS + fonts + layout) | Fixture ladder isolates subsystems first; site acceptance only after WK-5; failures debugged via headless dump mode (WK-3) |
| R16 | Scope creep from adjacent wishes (X11 growth, media, tabs) | §1.6 non-goals + AD-13; new wishes go to §11 backlog, not into lanes |

---

## 10. Open questions (close at W0)

1. **CLOSED (W0.4).** Skia is bundled in-tree — `Source/ThirdParty/skia/`
   at commit `588b550a4dd8af90dbe71c0554852806bd8f0b21` (recorded in its
   `README.WebKit`; no system dependency; built when `USE_SKIA`, ON for WPE).
   Detail: `docs/browser/f0-deps-audit.md` §2.
2. **CLOSED (W0.4).** Interpreter-only recipe (all four together, conflict-
   checked in `WebKitFeatures.cmake`): `-DPORT=JSCOnly -DENABLE_JIT=OFF
   -DENABLE_C_LOOP=ON -DENABLE_WEBASSEMBLY=OFF -DENABLE_SAMPLING_PROFILER=OFF`.
   Detail: `f0-deps-audit.md` §3.
3. **PARTIAL.** Minimum ICU is 70.1 (hard `find_package` for both WPE and
   JSCOnly). Data-trimming approach still open — resolve at the L6 ICU build.
4. **CLOSED (W0.3).** FAT16 stays sufficient at 256 MiB (8 KiB clusters,
   pinned); `fat16.c` needs NO change (the `BPB_TotSec32` path is already
   live). FAT32 deferred until images >≈512 MiB are actually needed.
   Detail: `docs/browser/f0-budgets.md`.
5. Program-load path for 30–60 MiB blobs: read-load timing acceptable? (P2.5
   measures; file-backed paging if not.)
6. Error-reporting channel for signals/aborts on-device (needed to debug
   WebKit crashes at all) — design at P5.
7. Whether the WebKit shell needs a real "chrome" (toolbar) in v1 of the port
   or keyboard-driven URL entry suffices (§6 WK-5 default: keyboard).
8. NetSurf: current framebuffer JS enablement state at 3.11 and the exact
   Duktape binding level (NS-4 evidence defines it).
9. Architecture sequencing: whichever of ARM/x64 first hits each WK gate —
   pick per-gate (both must pass by WK-6); record choice in §11.

---

## Appendix A — Frozen interface drafts

### A.1a Carry-over syscalls (F1; **frozen**, mostly v1 text)

```
SYS_SOCKET     65  (domain, type, protocol)              -> fd | -errno
SYS_CONNECT_FD 66  (fd, ip_be, port_be)                  -> 0 | -EINPROGRESS | -errno
SYS_SELECT     67  (nfds, rd_mask*, wr_mask*, ex_mask*, timeout_ms)
                                                          -> ready_count | 0 | -errno
SYS_FCNTL      68  (fd, cmd, arg)                        -> value | -errno
SYS_GETSOCKOPT 69  (fd, level, optname, val*, len*)      -> 0 | -errno   (SO_ERROR, SO_TYPE real)
SYS_SETSOCKOPT 70  (fd, level, optname, val, len)        -> 0 | -errno   (mostly no-ops, return 0)
SYS_GETRANDOM  71  (buf, len, flags)                     -> written | -errno
```
Semantics text and errno list: as v1 §A.1. **Amendment (v2):** select mask
width = **FD_SETSIZE 256** (32-bit words × 8); FD_SET API in libc matches;
update v1's "32" both here and in F1.1. `SYS_CONNECT` (#18) untouched.

### A.1b P-stage syscalls (**provisional**; each freezes at its milestone gate)

Numbers contiguous from 72; keep this table updated as gates freeze them
(owner: L2; recorded here before the header edit):

```
SYS_THREAD_CREATE 72  (entry, arg, stack, flags)        -> tid | -errno     [P1]
SYS_FUTEX         73  (uaddr, op{WAIT,WAKE}, val, timeout_ms) -> 0 | -errno [P1]
SYS_THREAD_EXIT   74  (retval)                          -> noreturn         [P1]
SYS_SET_TLS       75  (tls)                             -> 0 | -errno       [P1]
SYS_POLL          76  (fds*, nfds, timeout_ms)          -> count | -errno   [P4]
SYS_SOCKETPAIR    77  (domain, type, proto, fds[2])     -> 0 | -errno       [P4]
SYS_SENDMSG       78  (fd, msghdr*, flags)   SCM_RIGHTS: 1 fd minimum      [P4]
SYS_RECVMSG       79  (fd, msghdr*, flags)                                 [P4]
SYS_MEMFD_CREATE  80  (name*, flags)                    -> fd | -errno      [P2]
SYS_MPROTECT      81  (addr, len, prot)                 -> 0 | -errno       [P2]
SYS_MADVISE       82  (addr, len, advice)               -> 0 | -errno (adv.)[P2]
SYS_SIGACTION     83  (signum, act*, oldact*)           -> 0 | -errno       [P5]
SYS_SIGRETURN     84  (void) -> resumes (libc trampoline only)            [P5]
SYS_GETENV        85  (idx, buf, size)                  -> len | -errno     [P5]
SYS_SPAWN_EX      86  (path, argv*, envp*, fdmap*, n)   -> pid | -errno     [P5]
```

**P1 renumber (consented, §9 OQ1; applied 2026-09-30 with the P1 lane):**
THREAD_EXIT and SET_TLS were inserted at 74/75 (the pthread/TLS layer needs
them; splitting them across numbers would leave a P1-dependency hole), so
every provisional row from the old 74 up shifted **+2** (POLL 74->76 through
KILL 84->86).

**P4/P5 consent (2026-09-30, integrator reviews; formal freeze at each
milestone gate):** P4 rows 76–79 freeze as printed; the `SYS_MAX` 75→79
top-up was a **no-op** (P2's write landed first — `SYS_MAX` was 82 at
`9a939e5`).  P5's amendment (D2): `SYS_EXEC` (40) / `SYS_WAITPID` (39) /
`SYS_KILL` (16) are **extended in place** — the v1 provisional rows
`EXECVE 83` / `WAITPID 84` / `KILL 86` are **withdrawn**; 83–86 carry
`SIGACTION` / `SIGRETURN` / `GETENV` / `SPAWN_EX` as printed above;
`SYS_MAX` = **86**.  All rows are already defined in
`src/include/syscall.h` (single-writer edit ahead of Wave 1f; see
`docs/browser/p4-ipc-design.md` §11 and `p5-exec-signals-design.md` §8).
Existing `SYS_MMAP/MUNMAP` (62/63-class) get **semantic extensions** (flags
for MAP_SHARED/fd-backed) rather than new numbers — record exact ABI at P2.

### A.2 Desktop protocol additions (F2; carried unchanged from v1)

```
ESC [ K <mods> ~   (pixel-mode windows only; 1=Shift 2=Ctrl 4=Alt; stamped on KeyPress)
ESC [ D ~          (graceful close; desktop→app; lib → WM_DELETE_WINDOW)
wheel: existing mouse messages with btn=4 (up) / 5 (down)
Atomicity: one write() per desktop→app message.
```

---

## Appendix B — Inventories (seed lists)

### B.1 WebKit port surface (seed; regenerate per Appendix D)

- Build/options files to study at W0/WK-0 (present in the tree): `Source/cmake/`
  — `OptionsCommon.cmake`, `OptionsJSCOnly.cmake`, `OptionsGTK.cmake`,
  `OptionsWPE.cmake`, `OptionsPlayStation.cmake`, `WebKitFeatures.cmake`,
  `Find*.cmake` (curl backend hosts: `Source/WebKit/NetworkProcess/curl`;
  graphics deps: `FindHarfBuzz`, `FindICU`, `FindCairo` (2.52 path only);
  feature toggles from the Find list).
- Platform files to add (seed, from WPE/GTK/WinCairo precedent): WTF platform
  (memory, time, threads, filesystem, StackBounds, OSAllocator,
  RandomDevice/entropy), WebCore platform (SystemMemory, FileSystem,
  MIMETypeRegistry, Language/LocalizedStrings, logging, Pasteboard stub,
  Widget/View, Cursor stub, Screen, Sound stub = none, image decoders
  (PNG/JPEG/WebP via our libs), font backend (FreeType+harfbuzz), rendering
  backend (Skia), SharedMemory (memfd + mmap), RunLoop (poll-backed)).
- JSC needs (WK-1): WTF platform minimum + `PlatformJSCOnly`-style port bits +
  threading (P1) + libc++ (P3); static JSC build (`ENABLE_STATIC_JSC`).
- Feature-trim candidate list (decide W0.4): Media/WebRTC/WebGL/WebAudio/
  WebCrypto(external crypto—off)/PDF/Gamepad/Speech/Plugins off; SVG, WebP,
  XSLT(off), WOFF2(off), AVIF/JPEGXL(off); sandbox off; JIT off; GPU off.
- **W0.4 outcome (2026-09-29):** the concrete keep-ON (~22) / turn-OFF (~60)
  flag tables and the dependency-off list live in
  `docs/browser/f0-deps-audit.md` §4–§5 — treat them as the seed for this
  appendix (regenerate/verify per Appendix D at WK-0).

### B.2 Third-party lib layer (L6 host-first builds)

libcurl → OpenSSL off, **mbedTLS back**, zlib, no brotli(optional later) ·
SQLite (thread-safe, OS VFS) · ICU (C++17; data-trim planning) · harfbuzz ·
Skia (spike; version per W0.4) · libwebp · zlib/libpng/libjpeg-turbo
(carried) · NetSurf libs: libwapcaplet, libparserutils, libhubbub, libdom,
libcss, libnsutils, libnsbmp, libnsgif, libnsfb, libutf8proc, duktape (+
NetSurf build glue).

### B.3 NetSurf integration (Track A)

Frontend = `framebuffer` (libnsfb) with a new **hobbyos** surface/input
backend speaking the desktop protocol; curl fetcher + mbedTLS; JS = Duktape
(via NetSurf's Javascript machinery; expect partial bindings — nsgenbind;
`docs/jsbinding.md` upstream). Patches kept per two-build rule; config pins
recorded at NS-1 (NetSurf Makefile `TARGET=framebuffer` + our overrides).

### B.4 Assets

Fonts: DejaVu set (carried). CA bundle `/CERTS/CA.PEM` (carried). Fixtures:
`tests/fixtures/browser/**` (shared by tracks). Start page: minimal on-image
HTML for early boots.

---

## Appendix C — Vendoring & fork layout

```
third_party/
  wpewebkit-2.54.0/ (+ tarball + .sums)      # WebKit, pristine snapshot
  wpewebkit-2.52.6/ (+ tarball + .sums)      # fallback pin (only if lever pulled)
  netsurf-3.11/ + netsurf-libs/**            # Track A
  mbedtls-3.6.7/ freetype-2.14.3/ zlib-1.3.1/ libpng-1.6.44/
  libjpeg-turbo-3.1.0/ fonts/dejavu-2.37/    # carried
  libcurl-…/ sqlite-…/ icu-…/ harfbuzz-…/ skia-…/ libwebp-…/
src/user/browser/
  common/        # pixel-protocol client seam (F5)
  netsurf/       # Track A glue + patches/ + README
  webkit/        # Track B on-OS glue (build glue, shell app)
  tools/         # host_build.sh, fixture server, two-build scripts
../WebKit-hobbyos/       # the fork clone (worktrees per L8 sub-lane)
                         # (executed layout 2026-09-29: ~/webkit-hobbyos —
                         #  outside the NAS-synced ~/Documents/GitHub tree)
obj/third_party/<name>/  # pristine + patches applied at build time
```

Rules (v1 carried): vendored trees excluded from cstyle; patches first-party,
one concern each, purpose headers; `patches/README.md` per tree; rebase
procedure per fork README (WebKit) or per tree (others).

---

## Appendix D — Regeneration commands (run on drift suspicion)

```sh
# HobbyOS anchors
grep -n 'SYS_' src/include/syscall.h | tail -25
grep -rn 'USER_INITIAL_CLEAR_SIZE\|MAX_PROGRAM_SIZE' src/include src/kernel Makefile
grep -n 'FD_SETSIZE' src/libc/include src/include -r
grep -n 'forward_key_to_focused\|shift_keymap\|ctrl_pressed\|F4' src/user/desktop.c | head
# WebKit surface (from the pinned tree/fork)
ls Source/cmake | sort
grep -n 'ENABLE_STATIC_JSC\|WEBKIT_OPTION_DEFINE' Source/cmake/OptionsJSCOnly.cmake
ls Source/WebKit/NetworkProcess/curl
grep -rn 'webkitgtk-2.54.0' <fork>/README
# NetSurf
grep -n 'TARGET *[?:=]' netsurf/Makefile | head
ls netsurf/frontends/framebuffer netsurf/frontends/libnsfb*   # verify layout
# Site posture re-check (expect: enablejs shell / SSR CNN)
curl -s 'https://www.google.com/search?q=test' | grep -o 'enablejs[^"]*' | head -1
curl -sI https://cnn.com | grep -i content-length
curl -sI https://lite.cnn.com | grep -i content-length
```

---

## Appendix E — References

- WebKit: github.com/WebKit/WebKit (tag `webkitgtk-2.54.0`) · WPE releases
  https://wpewebkit.org/releases/ · WebKitGTK 2.54 highlights
  (webkitgtk.org, 2026-09-16: Skia compositor; cairo removed) · blog
  "Skia compositor for WPE WebKit and WebKitGTK" (Igalia) · JSCOnly trac wiki
  (+ RaspberryPi cross-build guide) · haiku/haikuwebkit (codeberg) + GSoC
  2024 "Porting WebKit2" report + Haiku forum threads · WinCairo/PlayStation
  port docs.
- Ladybird (rejected; revisit link): ladybird.org + `Documentation/Porting.md`
  + `BuildInstructionsLadybird.md` (Qt6/Rust/C++23) + Rust-migration posts.
- NetSurf: netsurf-browser.org (news: 3.11, 2023-12-28) ·
  download.netsurf-browser.org/netsurf/releases/ · `docs/jsbinding.md` ·
  netsurf-dev thread "Using JavaScript on embedded device (framebuffer)".
- Field evidence (2026-09-29): Google `/httpservice/retry/enablejs` redirect
  shell; CNN 6,583,482-byte homepage w/ SSR headlines; lite.cnn.com 332,965
  bytes. (Raw captures archived with the planning session.)
- In-repo: `docs/gnu-ports.md`, `src/user/x11/README.md` + xcalc README,
  `run_xcalc_test.py`, `posix.md`, `plan.md`, `STYLE.md`, skills `hobbyos-*`.

---

## 11. Fix log (append-only; see also per-lane reports)

- 2026-10-09 — **PERF WAVE-1 fan-out CLOSED (N1/J1/O1/M1/C1 all delivered); net1 (pool+wedge-fix) built green both arches; Leg A (x64 CNN, crash-fix validation) live; O1-continuation lands the kernel block-layer fix.** **C1** (loader wedge + cold-stall forensics): fixes on `bw-perf-net1` — `d61e68e5ad` (crash-proof wedge stop: parks a request the engine loops poll on script-allowed turns; comprehensive loader-less kick `fsKickLoaderlessPending` at parse-finish + wedge-streak with in-flight guard + 120 s grace; `fsDumpCounted` recorder) + 2 controller build fixups (`b96c380fb9` file-scope `print_console` decl — clang rejects block-scope linkage specs; `6337138cf8` `Ref<WebPage>` has no bool conversion). Key reframe: the wedge-streak "3 same-reqC" was **not** proof of a dead wedge — `[WIN-sr]` prints only after `perform()` returns, so a **cold-stalled draining batch looks static**; the cold-stall family itself traced to the **kernel block layer** (`submit_io_cmd` unbounded NVMe completion poll, IRQs off, global lock `0x7474C004`; a lost completion freezes all block I/O and starves virtio-net RX — matches PN's wire-clean/guest-silent gaps; report `c1/evidence/LOCK-0x7474C004.md`). **N1** (async pool): GREEN — commit chain `c2cce07830`→`b607920072`→`96472fea76`→`eae24e59ec`→`7c96370e00`→`b14180d16d`; local fixture gates PASS on the final commit: overlap receipt = **6 subresources submitted concurrently, 6/6 pump-delivered, 0 drops, median 128 ms**; the two post-`96472fea76` pump fixes (scheduleLoad entry + dispatchDidFinishDocumentLoad) close a real delivery-teardown gap — **net1 was cut before them, so the pumps join at integration; Leg A's binary shows the gap live (78 `[WIN-srq]`, 0 `[WIN-srp]` at T+18 min)**. **J1** (JSC tiers): builds green both arches; gate flip + `/NO-SCRIPT` opt-out verified live; **scripts-ON probe: the page script EXECUTES under native LLInt/baseline JIT (`[SCRIPT] execute inline=4323`, C_LOOP spin gone) but the page then deadlocks before completion (all CPUs idle-stuck, no load-ok)** — the predicted parser-blocking-script stall, now wave-2 (coordinate with C1's loader work); commits `ede2a642cb`, `20760e5d6a`, `37a5044c05` (+ `1f3cbb8d12` gate). **O1**: `f9518fc` (SRLCOST harness committed) + `d08a0c5` (report); clock 1:1 (TSC fix active — PX's "guest 11.7 s vs wall 47.8 s" was an anchor artifact); **launch ≈30 s = webproc image prefault (IMGPRF)**, not clock. **O1-continuation (kernel)**: `ccca3ec` on `browser/perf-os` — **bounded NVMe completion poll + recovery (never freeze), stall ticker, cid-desync hardening.** Net1 pins: intel ELF `5ca115c8…`/flat `79a0def4…`; arm ELF `5e67a765…`/flat `5e03c05c…`. Reports: `continuation/perf/wave1/` (`N1-report.json`, `J1-report.json`, `C1-report.md`, `*RUN-REQUEST.md`). **Leg A** (x64 CNN on net1-intel, proc_6d5bdf8fef6c → `c1/evidence/x64-c1a/`): success = no crash vs run-801 + kick/defer markers; load-ok bonus.
- 2026-10-09 — **CNN run-801 FINAL (both legs): identical crash in the fs-r10 loader-wedge backstop; fetch layer confirmed fixed; silent parse crawl measured; wave-2 fix list set.** M1 lane reports landed (`continuation/perf/wave1/`: `run-801-summary.md`, per-leg JSONs, `DONE-801.md`, `SHA256SUMS`). **Both arches crash deterministically** (~04:14 x64 / ~04:44:41 ARM): 3 requests created during `implicitClose` (load-event / `loading=lazy` flush) never receive a serviced loader → `requestCount` pins at 3 → readyState Interactive → `checkCompleted wedge-streak stop` → the fs-r10 recovery calls `stopAllLoaders` under `ScriptDisallowedScope` → `RELEASE_ASSERT` at `FrameLoader.cpp:2348` → process exit. (The backstop is NOT yet fixed — wave-2 needs (a) a real loader for those requests or (b) a crash-proof stop.) **Fetch layer numbers (final):** 135/135 subresources completed on both arches; reuse 97% ARM / 95.5% x64; median fetch 349 ms / 232.5 ms. **Cold fresh-TLS per new cross-host origin = 25–118 s** (x64 33.7/25.0/27.1 s; ARM 44.1/73.3 s, **118.3 s** skeleton; optimizely rc=35 on ARM at 25.7 s) — top wave-2 root-cause item (C1). **Silent parse crawls dominate wall**: ARM ~901 s + ~1,801 s; x64 ~304 s + ~646 s (tokens 200→17,000 at ~7–10 tok/s; parser pump/lock-wait starvation, P3 owner; WATCHDOG dumps benign). **T_nav not measured on either arch this run** — blocked on the wedge + crawl fixes. Next runs: 802 = pool build (N1, built both arches; x64 functional smoke green on example.com) + C1's wedge fix; then J1's JIT tiers; then budget attacks (P3).
- 2026-10-09 — **CNN run-801 x64 leg CLOSED RED — no load-ok; root cause chain found (stuck need-loader requests → wedge backstop → crash); P3 + C1 lanes opened.** Receipts: `~/hobbyos-perf-lanes/m1/evidence/x64-run801/{report.json,perf.json,driver.log,serial.log}` (boot 5.9 s, launch 17.1 s, HOME frame wall 68.2 s, nav wall 71.3 s, driver `[FAIL] no load-ok within 1800 s`; 0 memory faults). Decomposition (controller + M1 lane): fetch layer is healthy post-fix — 135/136 subresource completions, median 232 ms, reuse `nconn=0` works — but the wall is (a) a ~303 s silent parse/layout phase (x64-KVM; ~901 s on ARM — lane **P3**, `briefs/P3.md`), then (b) a page WEDGE: `[COMP] wedge-dump reqC=3 pending=129` lists loads (fonts etc.) at `needs=1 loader=0` that never receive a network loader in the single-process shell → `checkCompleted ... reqC=3` ×8 → the port's wedge-streak backstop calls `stopAllLoaders` under `ScriptDisallowedScope` → `RELEASE_ASSERT` crash (`FrameLoader.cpp:2348`) → process exit. Plus a recurring ~25–44 s cold-connection TLS stall (new connections only, both arches). Lane **C1** owns stuck-request/cold-connection forensics + a non-crashing wedge stop (`briefs/C1.md`). Ops notes: serial writers interleave/garble lines (parsers must be tolerant); `[WATCHDOG]`/`CSTK` dumps are benign (trap.c). Next x64 run (802) = pool build (N1 code, in flight) + C1 fix.
- 2026-10-09 — **Perf wave-1 v2 recovery after a second delegation reap; N1's async fetch pool landed; JSC native-LLInt/JIT builds unblocked one layer further; post-fix CNN runs 801 in flight on pinned binaries (both arches).** The delegation runtime killed wave-1 attempt #2 at ~23:27 on 2026-10-08 (children + their background processes; ~4.5 h stall until controller recovery). Salvage (controller-verified): **N1 `c2cce07830`** — async subresource fetch pool (`WKNetAsyncPool`; `scheduleLoad` no longer blocks the parser; workers fetch in quiet mode; engine-tick drain runs the exact synchronous delivery sequence on the main thread; new `[WIN-srq]/[WIN-srp]/[WIN-srd]/[WIN-srop]` markers). **J1**: `d8ccc2daba` (restore `CCallHelpers.h` include in `InlineCacheCompiler.h` — the PCH-disabled port gap; stock builds inject it via `JavaScriptCorePrefix.h:125` under `#if ENABLE(JIT)`), `0d1850d5eb` (`LOCAL_LABEL_STRING` branch for OS(HOBBYOS) in `WTF/wtf/InlineASM.h` — the native-LLInt `'expected )'` errors), `eb1de07252` (ARM64 `offlineasm` `globaladdr` for OS(HOBBYOS)). **Reap-kill corruption class found + cleaned:** the SIGKILLed ninja left 638 zero-byte outputs and 4 corrupt `bin/` executables (lld partial writes — e.g. `LLIntOffsetsExtractor` came back as non-ELF garbage) whose fresh mtimes made ninja skip rebuilding them; tell-tale = `ninja: warning: premature end of file; recovering`; cleaned, and both LLInt pipelines regenerate cleanly (J1-verified). **Operating model v2:** long jobs (builds, guest runs) are controller-owned tracked background processes; lanes keep short committed chunks + `RUN-REQUEST` hand-off (2/2 delegation batches were reaped mid-flight; a child's processes die with the child). **In flight now:** N1 build resume; J1 arm-jit + intel-jit builds (intel iterating the expected PCH tail — `SimpleJumpTable`/`StringJumpTable` missing includes); ARM CNN run 801 + x64 CNN run 801 on pinned binaries (`PINS.md`: ARM `d81af744…`/`7c3d9e09…`; x64 pinned `1d443480…`/`be00b42b…` — stale Oct-3 x64 now explicitly forbidden). **Early run-801 signals:** connection reuse works in-guest on both arches (`nconn=0 tls=0ms`; CNN fonts 63–93 ms total); ARM remains parse/fetch-bound single-core (≈3.4 k parser tokens at T+16 min; `cdn.optimizely.com` cross-host `rc=35 SSL connect error` after 25.7 s — the handshake-starvation regime, open item); x64/KVM cleared 117 `[SR-t]` fetches by T+19 min, load-ok pending. Records: `~/hobbyos-perf-lanes/{STATE.md,REAP2-RECOVERY.md,pinned/PINS.md}`.

- 2026-10-08 — **PERF WAVE-0 (3 lanes): CNN stall root-caused + fix validated + x64 leg live + JIT path mapped; rp-f @ `9a7b77ffb9` now carries the CONNECT-share fix.**
  **PN** (`bw-perf-net`): the 30–75 s/subresource stall = **no connection reuse** — `CurlShareHandle` shared COOKIE/DNS/SSL_SESSION but not `CURL_LOCK_DATA_CONNECT`, so every subresource paid a fresh TCP+TLS; in the busy windowed guest each handshake costs ~29 s wall (tcpdump: wire clean, server 15–50 ms; same binary headless: TLS 232 ms). The RTO-looking 31/63 s modals = handshake starvation (initial RTO read wrong — evidence wins). **Fix validated: 116 fetches → 115 reused, TLS 0 ms, total median 134 ms** (projection: 97-image wave ~47 min → ~30 s). Knobs `/SR-IPV4|TLS13|TLSDEF` neutral (resolver emits A only; entropy non-blocking). `-smp 2` ARM: PSCI `Failed to power-on core 1` → guest single-core (→ O1).
  **PX** (`browser/perf-x64` @ `4aafc2c`): x64 leg LIVE under KVM — closure + intel build (flat `8ce4186d…`), boot 3.3 s, HOME + example.com + wikipedia receipts, PAGE-NET byte-exact vs live curl; reusable runner `tools/run_x64_browser.sh` + `drive_x64_perf.py`. Same serialization visible (33 subresources × 3–7 s) → fix applies to x64 too.
  **PJ** (report `continuation/perf/wave0/PJ-jit-feasibility.md`): LLInt = pure config flip (both arches, runs from `.text`); **x64 baseline-JIT/DFG needs ZERO OS changes** (mmap PROT_EXEC / mprotect / MAP_FIXED guards / per-page NX all present; x86 cacheFlush no-op); ARM64 JIT blocked (`ARM64Assembler.h:4047` #error + no userland cache-flush; DFG needs signals). Page-script gate points mapped (driver:2814 + FrameLoader assert-skip + ScriptController).
  Wave-1 lanes dispatched: **N1** async fetch pool, **J1** JSC tiers (x64 LLInt/JIT + gate flip), **O1** OS (PSCI cores / serial cost / x64 clock sanity), **M1** post-fix CNN measurement + phase decomposition. All records in-repo: plan `perf-plan.md`, reports `continuation/perf/wave0/`.

- 2026-10-08 — **Perf program opened: CNN ≤ 8 s (x64-KVM) / ≤ 15 s (ARM-TCG), fully rendered — metric confirmed by maintainer (nav→load-ok+stable frame; visual completeness is the bar; JS-on a separate first-class row; boot/launch excluded).** Plan: `perf-plan.md`. Cost model from live run 700 (ARM TCG, in flight while recon ran): main doc 6.79 MB in ~9 s, then **109 serialized subresource fetches** (parser-blocking, one TCP+full TLS handshake each — no CONNECT sharing) = ≈95 % of a ~50-min CNN nav; the dominant stall is 30–75 s per media.cnn.com image (≈ 31/63 s = TCP-RTO-backoff signatures, unresolved). JSC still C_LOOP-only; x64 leg stale (no obj/intel closure, no build/intel, pre-D2/D3 cache). Wave-0 lanes dispatched: **PX** (x64 build+boot+runner, `browser/perf-x64`), **PN** (fetch forensics, `bw-perf-net`), **PJ** (JSC LLInt/JIT feasibility, read-only).
- 2026-10-08 — **D3 CLOSED: the browser render is no longer single-threaded — all four receipts green; live-URL matrix 5/5 including en.wikipedia.org.** Fork tip `e9a040c5b4` (+evidence fold-in).
  **D3 (rp-f2 lane):** the WK5 driver pump = two concurrent flows — WebKit engine on the process **MAIN** thread (a bare pthread cannot drive it: WebKit R-A's `RunLoop::isMain()`; run-126 crash evidence) + a **UI/protocol pthread** (blocking `read(0)`, ESC state machine, cached-frame reblits, `]F` acks, close detection); serial/protocol writes mutex-guarded (the kernel console write is an unlocked byte loop — unguarded dual writers tear `[WIN]` needles); event queue mutex+condvar; **close owned by the UI thread** — immediate `[WIN] close ok` + `exit rc=0` + `_exit(0)` (abandons in-flight work; correct for WM_DELETE_WINDOW, skips teardown races — 6 earlier park/join/watchdog variants failed 134–145). Structure `f9d3589f09`; final `5d2ae5c393` + receipt drive v4→v7. **Receipts (final binary `5c8064a0dfed…`, re-verified: `ninja` no-op + sha ≡ pinned):** (i) F4 during an in-flight ~90 s TALL load → `ui close-detected` → `close ok` +0.7 s → `exit rc=0` → WM **`exited within grace`** (run 147; pre-D3 baseline was close-under-load FAIL-STARVED 3/3); (ii) F10 `img-check` ACKed during post-load render with frames continuing (residual: `[WIN] wheel` never WM-delivered in these sessions — secondary evidence only); (iii) console-open re-tile → same-second cached reblit **before** the engine's viewport apply (strict during-load ordering receipted run 129; 141–147 landed just after that window's load-ok — noted honestly); (iv) run 148 rc=0, HOME `0x4719ef4d` ×4, example.com fetch 200 + persist, close within grace; **0 memory faults across all receipt serials** (only excluded run-146 SMP=4 boot crash has one). Screenshot vision-checked (browser gone post-close; note: the receipt runs' console window can show a shell-spawn ENOMEM in its own window under load — test-flow cosmetic, not in any browser serial).
  **Live-URL matrix (typed address bar, pinned `rp-d2fix2-WP-171cc6be…`):** `example.com` / `example.org` / `info.cern.ch` (http) / `neverssl.com` (http) / `en.wikipedia.org` (stretch) → all `net fetch ok=1 status=200` + `net persist` + `load-ok` + fresh-checksum page frame + `close ok`/`exit rc=0`; **bodies byte-exact vs live curl** (`25ddf2c8…` example/org; `bc11566a…` info.cern.ch — controller-verified); wiki main page legible (logo area unpainted, expected); evidence `continuation/rp/evidence/live-matrix` + adapted runner (`run_wk5_live.sh`/`drive_wk5_live.py`, adds the QEMU shift-chord key map — a lone `shift-semicolon` qcode is silently dropped).
  **In flight:** (superseded — this entry) re-vendor the fork snapshot from `e9a040c5b4` into `third_party/webkit-hobbyos/` + from-source build + typed-URL acceptance on the repo's own artifacts.
- 2026-10-08 — **re-vendor + from-source build + typed-URL acceptance ALL GREEN on the OS repo's own artifacts.** Fork `browser/rp-f` re-snapshotted `45349cb2` → `e9a040c5b4` (29 commits: D1 pthread smoke, D2 URL normalization, mtime-aware toolchain shims, tracer guard, D3 UI-thread split). Commits `d59d515` (snapshot refresh) + `0fec2c9` (arm cache) on the OS repo main.
  **Snapshot (`third_party/webkit-hobbyos/`):** 55,157 files; raw tar 865,218,560 B; xz 173,734,396 B over 2 parts (`part-00` = exactly 90,000,000 B, `split -b 90000000`). Reassembled-xz sha256 `b850179d…`, `part-00` `c77cdf82…`, `part-01` `3bb5a81f…`, raw tar `6ee50d00…`. Exclusion set reproduced byte-exact vs the old snapshot (verified programmatically: 0 in-snapshot files outside the modeled set): test suites, Apple-only trees, `HobbyOS/continuation`, `Source/ThirdParty/libwebrtc`, lane scratch (`*-BRIEF.md`, `fs-*-REPORT.json`, `logs/`, agent dirs) and every `.gitignore`/`.gitattributes` at any depth. `extract.sh --check` green; extract OK (55,148 regular + 9 symlinks); `IS_NEWER_THAN` shims + `ui close-detected`/`url-norm` markers present.
  **From-source build (`--fresh`, canonical recipe):** configure → `ninja WebProcess` 7915/7915 steps (-j64) → ELF `build/arm/WebKitBuild/bin/WebProcess` 147,531,408 B sha256 `3553f80a…`; flat `build/arm/browser.bin` 87,568,624 B sha256 `4dcdd509…`; markers verified with `grep -a` (`WK5WindowDriver`, `ui close-detected`, `url-norm`).
  **Typed-URL acceptance on the SOURCE-BUILT binary (run 150, runner adapted from the lane-proven `run_wk5_url.sh`/`drive_wk5_url.py`):** HOME `checksum=0x4719ef4d` ×4 → `home.htm` fixture loaded with **no** net fetch → typed `example.com` → `url-norm class=https in=example.com out=https://example.com` → `net fetch url=https://example.com ok=1 status=200 bytes=577` → `net persist` → `[WIN] close ok` → `[WIN] exit rc=0`; **0 memory faults; runner rc=0**. Stretch (run 301, `FRAME_TO=5400`): typed `https://en.wikipedia.org` → `net fetch ok=1 status=200 bytes=253021` → `load-ok url=https://en.wikipedia.org` → page frame `0xb69d9ea8` (≠HOME) → close rc=0, 0 memory faults; **body byte-exact vs live curl** (253,021 B; sha `436fa13e…` both guest and host). Serials + screenshots (`RPF-150-*`, `live-en.wikipedia.org-301-*`) + both logs saved in `/home/sarah/hobbyos-scratch/revendor-evidence/`.
  **Cache:** `cache/browser-arm.bin.xz` rebuilt from the new flat — sha `89d015c3…` → `3cef4d4c…`, round-trip verified (decompressed == `4dcdd509…`). **x64 cache NOT regenerated (intel track dormant): remains wf1b-era (`6706d645…`, `a90337ea…` ELF, pre-D2/D3) — flagged **STALE** in README; refresh when the intel track resumes.**
  **Residuals:** intel-side cache stale (above); wiki logo area unpainted (layout expectation, same as the d2fix2 live matrix); `[WIN] wheel` events still not WM-delivered (secondary, pre-existing); snapshot README refresh command now documents the full exclusion list + the `git archive -o` writes-into-fork pitfall.
- 2026-10-08 — **rp-f1 crash chapter CLOSED: the font-presence boot crash root-caused to a STALE build-dir shim (not the font, not D2) — fixed in-tree + toolchain hardened; D2 scheme-less-URL receipts GREEN on the fixed build; rp-w (wsurf) merged; rp-f2 D3 + live-URL lanes in flight.** Fork `browser/rp-f` @ `cb624c2f01` (toolchain fix `19dc671a04`, tracer guard `f9f0900a9f`, receipts `e4ed224c9a`, D3 brief `c4d7932c37`).
  **Crash (runs 112–117, 120): any disk with a staged `.TTF` → `memory fault VA=0x58` in FreeType `tt_face_build_cmaps` during the first load — WebProcess-only, every rp-f-dir build; wab/wbn binaries booted the same disks fine.** E-matrix excluded in order: D2 regression (E1: old binary crashes), font content (E3), source deltas (E5 smoke-reverted still crashes), the file itself (E4 presence-only triggers). **Root cause (instrumented probe + disasm):** userland `setjmp` in the generated gap vehicle `hobbyos_gaps.o` was a stale Oct-2 copy keeping the pre-WK-3 FP-saving version — `d8–d15` saves write 168 B into `jmp_buf[16]` (128 B) and zero FreeType's adjacent spilled `clazz` (`ttcmap.c` validate → `[0+0x58]`). Font bytes were byte-perfect throughout (FONTDBG fnv == host `0x9b6f9d25f3f79449`).
  **Fixes:** canonical regeneration of `hobbyos_gaps.o` in all stale arm build dirs (`clang --target=aarch64-none-elf -ffreestanding -mcpu=generic -c HobbyOS/lib/gaps.c`; sha `aee7a2eb…`, setjmp writes ≤104 B) + forced relink (**shims are NOT ninja link deps** — `rm bin/WebProcess` required); toolchain now mtime-recompiles shims (both loops) + DECISIONS.md hazard note updated; `wk5ExitTracer` frame-walk guarded (closes the R15/`[WIN] exit rc=0`-era post-exit `VA=0x8` teardown fault). **D2 receipts (runs 122/123, serials+screenshots in `continuation/rp/evidence/`):** `[WIN] font-probe rc=0x0`; typed `example.com` → `url-norm class=https` → `net fetch https://example.com ok=1 status=200 bytes=577` → `load-ok ms=101263` → styled frame `0x4719ef4d` → `close ok` → `exit rc=0`; screenshots vision-verified (HOME fully legible; real example.com text rendered). Drive baseline refreshed (`0x479f0e05`→`0x4719ef4d`; old value predates the D2 runner's own font staging). Pinned binaries: `~/hobbyos-scratch/rp-d2fix-WP-76026a15…` / `rp-d2fix2-WP-171cc6be…`. **rp-w merged (OS `249881c`):** wsurf clip-bound render-surface lib + WSRF_T wave wiring; ARM wave gate green (`System halt.` + exit 0). **In flight:** rp-f2 D3 UI-thread split (the "browser single-threaded" ask; receipts i–iv) + live-URL variety matrix (example.org / info.cern.ch / neverssl.com on `171cc6be…`).
- 2026-10-07 — **WebKit fork vendored into the OS repo + source-first build integration (user ask — the browser builds from source; prebuilts are cache only).** `third_party/webkit-hobbyos/` lands as a sha-verified split tarball (`webkit-hobbyos-rp-f-45349cb2.tar.xz.part-00/01`; fork `browser/rp-f` @ `45349cb2929b` — superset of every merged port line incl. `browser/l8-wk5` @ `cdabe011af`; base `webkitgtk-2.54.0` @ `5220e80b97` + 291 commits / 3,953 files; 58,592 files; reassembled sha `3baa2237…`) with `extract.sh` (reassemble → verify → `src/`) and **`build.sh` — the from-source path**: extract → cross-deps → canonical `wk2-link-ci.sh` configure with `-DWDK_FORK_DIR`/`-DHDYOS_SYSROOT`/`-DHOBBYOS_WK2_PREFIX` re-pointed into the repo → `ninja WebProcess` → flat `build/<arch>/browser.bin` + marker/sha verification.
  Prebuilts are demoted to an optional cache (`cache/browser-{arm,x64}.bin.xz`, sha `89d015c3…`/`6706d645…`; arm = the 2026-10-07 canonical `HobbyOS-arm-wk5` build — the binary the live rp-w receipts run — ELF `0a815545…`; x64 = wf1b merged intel build, ELF `a90337ea…`). `make disk.img` source order: `BROWSER_BIN` → `build/<arch>/browser.bin` → cache → skip; new `make browser` target drives `build.sh`. Snapshot exclusions: test suites, Apple-only trees, `HobbyOS/continuation` (~33 GB lane receipts), `Source/ThirdParty/libwebrtc` (not in the port's build graph — `ENABLE_WEB_RTC=OFF`; the upstream release tarball omits it too). Detail doc: `third_party/webkit-hobbyos/README.md`.
  **Build verification (completed same day, 17:08):** full from-source build from this snapshot on the workstation — `ninja WebProcess` 576/576 steps, 12m32s (-j12); `build/arm/WebKitBuild/bin/WebProcess` 147,524,808 B, sha256 `9623ee97…`; flat `build/arm/browser.bin` 87,564,512 B, sha256 `23493010…`; `WK5WindowDriver` marker present. Disk branch tests: fork-override and cache-fallback both ship the canonical flat `1ebac9a4…`; with **no fork build present** `make disk.img` ships the **source-built** flat (`browser: third_party/webkit-hobbyos/build/arm/browser.bin (vendored-source build) …`; disk `::/BROWSER.BIN` sha == flat sha; `cache/` untouched). `--configure-only` from the vendored tree: OK (~56 s).
- 2026-10-07 — **`rebuild_browser.sh` (cache rebuild tool) + intel cache-name fix.** `third_party/webkit-hobbyos/rebuild_browser.sh [arm|x64|both]` regenerates the optional caches from the vendored snapshot: runs `build.sh`, re-compresses `xz -T0 -6`, verifies the round-trip (decompressed sha == flat sha), rewrites `SHA256SUMS` cache lines, prints flat/cache hashes; `--from-existing` refreshes from `build/<arch>/browser.bin` without rebuilding; `-j` / `--build-deps` / `--prefix-arm` / `--prefix-x64` pass through. Arm leg exercised end-to-end; x64 leg needs the OS-side intel closure (`obj/intel/…`) first. Makefile: `BROWSER_CACHE` maps `ARCH=intel` → `cache/browser-x64.bin.xz` (previously `browser-intel.bin.xz` — a silent miss).
  **Fresh-machine hardening (user-reported, second machine):** `rebuild_browser.sh` now auto-builds missing OS-side closure pieces (`obj/<arch>/…` via the OS make targets: crt0/libc/libcxx/setjmp, then ICU) and any missing cross-deps prefix (`build.sh --build-deps`) before the WebKit build — opt-outs `--no-build-deps` / `--no-build-closure`; `-j N` forwarded to make/deps/ninja. A fresh clone's first run now does setup → deps → WebKit per arch instead of erroring out.
  **Exercising that path surfaced + fixed three real gaps:** (1) `wk2-libs-cross.sh`'s default order aborts at the D-15-blocked full harfbuzz build — `build.sh --build-deps` now runs the canonical `--only zlib,png,jpeg,webp,freetype,hbcore,sqlite,xml2` set (hbcore fills `lib/libharfbuzz.a`: 2,814,982 B / 55 objs — identical to the provably-working dev prefix artifact); (2) the ARM curl/mbedTLS half existed only as the fork's `continuation/wk4b/wk4b-libs-cross.sh` (excluded from the snapshot) — ported into the repo as `tools/cross-arm-wk2-libs.sh` (intel already had `tools/cross-intel-wk2-libs.sh`; faithful port per the arm/intel parity table, plus a link smoke — green, 2/2 backend symbols); (3) both stagings now take `WK4B_SRCROOT` from `build.sh`, so their workspaces live in-repo (`src-wk2/<arch>/src`), never `$HOME`. **Verified end-to-end into a clean prefix:** `rebuild_browser.sh arm --prefix-arm <clean>` = closure OK → wk2-libs (zlib…hbcore/sqlite/xml2) → cross-arm (mbedTLS/curl + smoke) → build.sh no-op reuse → cache round-trip, exit 0 (~3 min with the WebKit build warm; a cold machine additionally pays the deps builds + full WebKit build).
- 2026-10-07 — **V5 merged: final interaction validation on the closed tip (user item 3) — fixture pass ALL GREEN; real-page link activation + search-form submit receipted; six limitations precisely documented.** Fork `cdabe011af`.
  FIXTURE (evidence/fixture, all green): link click HOME→TALL (`[WIN] link -> file:///TALL.HTM` + load-ok); scroll PgDn ×2 (pos 40/80) + wheel down ×2 (pos 160) + wheel up (pos 120), post-paint receipts; address-bar click + type `HOME.HTM` + GO → `[WIN] url-entry` + load-ok + post-load frame (screenshot shows the typed text in the new 8×8 font); close (F4) → `ESC[D` + `[WIN] close ok` + exit rc=0 + `exited within grace` **3/3**. WIKI (run-3 + archived runs 1/2): article load-ok ~605 s; **REAL link activation via the engine's best-link path — `[WIN] open-result` → `[WIN] link -> https://en.wikipedia.org/wiki/Main_Page` → doc title `Wikipedia, the free encyclopedia` → load-ok ms=1,970,775; controller vision-verified the rendered Main Page**; scroll down (wheel; scrollY 240) and up (200) receipted; **FORM ENTRY: F3 → `hobbyist` → Enter → `[WIN] query-entry=hobbyist` + `[WIN] query nav=https://en.wikipedia.org/w/index.php?search=hobbyist`** (the page's own form action drove a real navigation). LIMITATIONS (precise, evidence-backed): (1) PageUp key never delivers (0/~13 across runs; PageDown 7+/7) — guest input path, NOT the engine (wheel-up scrolls reliably); (2) input starvation during heavy-page settle under TCG (presses dropped; retries land after settle); (3) Wikipedia sidebar links not mouse-click-activatable on this tip (exact-rect clicks echo, no `[WIN] link`; hit-test lands on an overlay) — the engine link path is the working route for real pages; (4) address-bar fixture reloads slow under TCG (72–262 s; always completes); (5) WM close starved while a heavy load/settle is in flight (settled light pages 3/3 pass); (6) in-page `<input>` clicks don't arm typing — typing uses the F3 channel, which submits via the shell fetch path.
- 2026-10-07 — **R15 merged: browser respects the tiling WM — instant reflow catch-up + prompt close + address-bar font = taskbar font (USER-REPORTED bug).** Fork `6aaa9bd395` (fix `5f616a9ffd` + receipts `58abe87ee9`/`f9e4ea87e2`/`d6f1b8b645`).
  Root cause (controller-verified in the R13 gate7 receipts): on a WM re-tile (ESC `]G` → `[WIN] geom w=956`) the `]G` handler ran `applyViewport()` → `m_page->setSize()` before anything visible — on the 5.7 MB page that blocked for minutes; the window kept showing the stale full-width render and a queued WM close starved (`grace expired, killing`). Fixes (all `Source/../hobbyos/WK5WindowDriver.cpp`): **(1) instant reflow catch-up** — the `]G` handler now FIRST re-blits the cached frame into the new content rect (`blitCachedFrame`: nearest-neighbour scale + chrome + `]F`; serial `[WIN] reblit cached view=1916x982 rect=956x982+36 at 2,70`), THEN runs the instrumented `applyViewport()` (`[WIN] viewport apply begin/end ms=`; 13–33 ms measured), then the fresh paint; `m_in.closing` is honored before the slow steps; **(2) prompt close** — device-proven: WM X-close → `[WIN] close ok` + `[WIN] exit rc=0` + `[CLOSE] win=0: exited within grace` (250 ms; previously `grace expired, killing`); **(3) font** — address bar draws the OS taskbar's 8×8 (`WK5Font8x8.h`, provenance = OS `src/user_include/graphics/font.h`) at 1:1 (was a 2× 5×7 bitmap). RECEIPTS (Run A, HOME.HTM fixture): catch-up reblit lands in the same second as the console (2.67 s); screenshot @3.7 s shows the browser adapted to the left tile (content bars re-rendered 1903→944 px; viewport apply end 13 ms); re-tile back to full on console close; font-compare images: address-bar vs taskbar glyphs pixel-identical (controller vision-verified). Wikipedia direct regression **PASS** (load-ok 563,141 ms; PAGE-NET 120,243 B byte-exact `3d83be33`). Residual (pre-existing, cosmetic): the teardown ATEXIT stack-walker faults reading VA 0x8 after `exit rc=0` — the kernel reports the exit; WM-close gate unaffected (cleanup noted for post-plan).
- 2026-10-07 — **R14 merged: CNN text-visibility root cause = the WK6 disk never staged `/DEJAVU.TTF` — FIXED + proven (gate8 headlines legible; link rects went 0-width → text-width).** Fork `ee69573fec`.
  Mechanism (settled, H2/H3 falsified): the WK6 CNN runner assembles a minimal guest disk WITHOUT the bundled font → `hobbyosCreateFontManager()` returns nullptr → `FontCacheSkia` falls back to the EMPTY `SkFontMgr_New_Custom_Empty()` (zero families) → **zero glyphs paint for any family** while images/CSS/layout still render. Gate7's disk `[MENU] count=14` had no font; wikipedia/R5/R2 disks all carry `/DEJAVU.TTF` (757,076 B) — hence legible text there. Fix (runner-only, engine untouched): stage `BUNDLED-FONT.TTF → /DEJAVU.TTF` on the WK6 disk (as `run_wk3.sh` already did). **PROOF on the merged R13 binary: fixture probe renders 5 colored text lines (~2 min); gate8 CNN re-capture: `load-ok ms=3495000`, `[MENU] 14=DEJAVU.TTF`, and the lead h2 "US issues formal request for information from Russia over potential plague fallout" renders as legible black text (controller vision-verifed); link census went `0x40` → `21x40`/`52x40` (text widths).** Wikipedia regression byte-exact green (`3d83be33`, unchanged from R13). Remaining `c1-images`/`c2-close` = pre-existing WK6 driver issues (byte-identical across R12/R13). Follow-up folded: staging line applied to the shared `continuation/wk4b/wk6/run_acceptance.sh`.
- 2026-10-06 — **R13b merged: CNN big-doc receipt CLOSED — the coalesced font drain lands the receipt quartet; the R10→R13 wedge chain ends.** Fork `7742dfac31`.
  Root cause (extending R11/R12): the ~10 min/font cost = a per-arrival FULL re-style+layout, and the DocumentFontLoader queue only holds fonts a layout has USED — discovered serially as each re-layout reaches new text. Fixes: **(A) batch-kick** — `flushPendingFontsSynchronously` now ALSO iterates `allCachedResources()` and `beginLoadIfNeeded()`s every `stillNeedsLoad()` CachedFont in one pass → all 16 pending webfonts served in a ~10 s burst (serial fetch timestamps 2293892→2303810 ms, no gaps; 16 `font-batch-kick` markers); **(B) post-complete straggler gate** — `fontsNeedUpdate` skips the full matched-properties invalidation once `loadEventFinished()`, so an arrival after completion cannot re-open a 5–12 min layout. **gate7 RECEIPT QUARTET: `[WIN] load-ok url=http://10.0.2.2:8895/ ms=2812839` (46.9 min, within the 90-min step budget) + styled frame (nonZero=7,478,542, checksum `0xb30e689e`) + census (imgs=80/80, colors=256, foreign=137,647) + screenshot (styled CNN page).** `nav-c1-relay-home` step **PASS** (TIMEOUT in gates 5/6). Phase budget: boot 0.1 / launch 0.3 / CNN nav 46.9 / c2 ~1.0 min. Wikipedia regression **PASS** (load-ok 283,447 ms; PAGE-NET `3d83be33` byte-exact to the transferred body). Pre-existing WK6 harness quirks (c1-images F10 img-check marker never driven; c2-close ESC `[D`) are byte-identical to gate5 — named, out of lane scope. **NEW FINDING (controller verification, open → R14): headline/body text not visible in the captured viewport** — the served HTML carries 21 `<h2>` headlines + nav labels; the render shows images/structure/logo/banner but I could not find page text in any crop. Text-heavy Wikipedia renders fine → page-specific; R14 probes the mechanism.
- 2026-10-07 — **R15 merged: browser respects the tiling WM — instant reflow catch-up + prompt close + address-bar font = taskbar font.** Fork `5f616a9ffd` (+ `58abe87ee9`).
  User-reported bug (controller-verified in R13 gate7): when CONSOLE opens and the WM re-tiles the browser (ESC `]G` → `[WIN] geom w=956`), the window kept showing the stale full-width render — the `]G` handler called `applyViewport()` (→`m_page->setSize()`) before anything visible, blocking for minutes on the 5.7 MB page and starving the WM close (grace expired → kill). Fixes (all in `WK5WindowDriver.cpp`): **(1) INSTANT REFLOW CATCH-UP** — the `]G` handler now FIRST re-blits the cached frame into the new content rect (`blitCachedFrame`, nearest-neighbour scale + chrome + `]F`; serial `[WIN] reblit cached view=1916x982 rect=956x982+36 at 2,70` = the old frame scaled into the new rect), THEN runs `applyViewport()` instrumented (`[WIN] viewport apply begin/end ms=`; measured 13–33 ms on the fixture), then the fresh paint; `m_in.closing` is honored before either slow step. **(2) PROMPT CLOSE** — verified on device: WM X-close → `[WIN] close ok` + `[WIN] exit rc=0` + desktop `[CLOSE] win=0: exited within grace` (250 ms grace; previously it was `grace expired, killing`). **(3) FONT** — address bar now draws the OS taskbar's 8x8 font (`WK5Font8x8.h`, provenance = OS `src/user_include/graphics/font.h`) at scale 1 (8px advance, MSB=left) instead of the old 2x 5x7 bitmap; right-align/caret math adjusted, field/GO geometry unchanged. RECEIPTS (Run A, HOME.HTM fixture): browser full-width -> CONSOLE second window -> screenshot at 3.7 s shows the browser adapted to the left tile (chrome + content; content bars re-rendered 1903px→944px; catch-up reblit in the same second as the console; viewport apply end 13 ms; render 956x982) -> console closed -> re-tile back at 1916 (re-rendered 1903px bars) -> browser X-close within grace. Font comparison images: address-bar vs taskbar glyphs pixel-identical (vision-confirmed). **Wikipedia direct regression PASS** (load-ok 563,141 ms; PAGE-NET copy-back 120,243 B byte-identical `3d83be33` to the R12 transfer). Residual (pre-existing, out of scope): after `[WIN] exit rc=0` the process teardown faults reading VA 0x8 (the ATEXIT stack-walker) — the kernel reports the process exited; the WM-close gate is unaffected.
- 2026-10-06 — **R12 merged: the "replay interleave" root-caused = SVG hidden-page construction; suppressed — ZERO replays, CNN completion chain proven open TWICE; wikipedia green (live content drift +240 B noted).** Fork `078e163b28`.
  Root cause (byte-proven): WebCore decodes every `<img src=*.svg>` by building a PRIVATE Page + main LocalFrame PER IMAGE (`SVGImage::dataChanged` → `Page::create` + `localMainFrame->init()`), feeding the SVG bytes into that empty document — on this self-driven single-process port that construction IS the stray `[FL] start-main url= subst=0` + `commitData n=0` replay (appended bytes byte-identical to the fetch: enwiki-25.svg=115,918 etc.; fresh frames, not navigations). Every SVG icon reset the completion bookkeeping (R10's streak backstop, R11's font flush), re-queued fonts, and cost a full page+frame+parse+layout per icon under TCG. Fixes: (1) `SVGImage::dataChanged` returns `Complete` without building the hidden page on HobbyOS (all consumers null-guard `m_page`; icons rasterize empty; DOM/layout/census intact); (2) driver-side completion latch (emit `[WIN] load-ok` at `readyState=Complete` observation) + load-phase heartbeat. ON-DEVICE: **12×+ `[SVG] skip-hidden-page`, ZERO replays** (vs 6 in R11's gate4); `[COMP] checkCompleted complete=1` + Complete reachable **twice** (gate5 line 785; gate6 at 53.8 min). Wikipedia direct regression **PASS** at the final state (`load-ok 263,559 ms` — faster than R11; styled frame; PAGE-NET byte-exact to the transferred body `3d83be33`, 120,243 B — the +240 B vs the morning's `bb765a43`/120,003 = LIVE content drift, not a regression). REMAINING (→ R13): pure TCG **throughput** — ~11 leftover webfonts drain one-at-a-time, each arrival forcing a full re-style+layout of the 5.7 MB page (~10 min/font) → exceeds the 90-min step budget; the wall pre-existed (R10/R11's runs ended at the same `reqC=1`/11-font dump). Fix path: coalesce the drain (single flush, no per-font re-layout) / skip post-complete re-layout for cosmetic stragglers / in-stack receipt at completion.
- 2026-10-06 — **Final pair merged: font wedge CLOSED (CNN completion chain open — `readyState=Complete` reachable on the 5.7 MB page), license residual CLOSED (bundle materialized, 83/83 self-verified).** Fork `ce4bca9b19` (R11+H1b).
  (i) **R11 (partial)** — the R10 residual (3 loader-less requests pinning requestCount=3) root-caused on-device = **FONTS**: `DocumentFontLoader::beginLoadingFontSoon` increments requestCount immediately but starts the load only from a 0 s generic-RunLoop timer this self-driven shell never pumps (gate3 dump: exactly 17 unloaded CNN font resources, `loading=1 loader=0`, no `[SR]`). Fix (HobbyOS-only; NO RunLoop pumping per R1's wedge): `Document::flushPendingFontsSynchronously()` (resume the suspended loader — stopLoading/suspend leave the queue held — then `loadPendingFonts()`, the same flush WebKit does after every style recalc), called from `Document::implicitClose` (+re-run `checkCompleted`) AND `FrameLoader::checkCompleted` before the requestCount gate. **On-device proof (gate4): font-flush fires ×2; requestCount drains 3→2→1→0; the wedged font is actually SERVED (`noto_serif-v1.woff2`, 160,248 B); `[COMP] checkCompleted complete=1` — Complete is now reachable on CNN.** Wikipedia direct regression GREEN at final state (load-ok 395,458 ms; styled frame `0x88ac7ff4`; PAGE-NET byte-exact `bb765a43`). Residual (→ R12): a pre-existing engine-side replay interleave (an empty-url main document replacing the active doc right after DOMContentLoaded — also seen by R10) prevents the single-threaded driver from observing Complete with `m_current` matching → `[WIN] load-ok` not yet physically emitted; the completion gate itself is proven open.
  (ii) **H1b (pass)** — WK-7 license materialization (D-16 Integrator action): final WebProcess link rule re-captured read-only from the merged verify build (34 inputs; token-diff vs H1's capture = closure identical), every input byte-verified (total 137,323,050 B); physical archive `HobbyOS/relink-artifacts/hobbyos-relink-arm-c6a29d50.tar.gz` (41,921,600 B; sha `c6a29d50…`; 34 inputs + 26 llvm-ar member maps / 3,226 objects + 21 license texts incl. the 5 previously-flagged tarball texts + self-verifying MANIFEST 83/83); offer text finalized (real pins/paths/shas; contact + publication URL left as explicit TODO-CLEARLY — nothing invented); closure gap-verified 34/34, no drift. `RELEASE-READINESS.md` R-8 CLOSED + §2 row 7 + G12 updated; DECISIONS D-16 addendum; wk7-license package supersede notes.
- 2026-10-06 — **W5 close-out wave: relay paint fixed (proxy query-relay), TCP RX flow control fixed (≥4 MiB identity byte-exact; sweep 0-FAIL), big-doc completion advanced (3 defects fixed, 1 documented wedge), docs + release-readiness pack landed — CP-FS3/CP-FS4 met.** Fork `061c7a8661` (R9+R10+H2b); OS `7631be0` (fs-r9 proxy/runner + fs-n3 net). Reference binary: verify build `bw-verify-w3` @ `1af2b6d3fc` (ELF sha `fa621da1…`).
  (i) **R9 (pass) — relay content-column paint**: root cause = `tools/wiki_proxy.py` DROPPED the URL query string upstream; Wikipedia's `/w/load.php` carries skin-CSS modules in the query → every relayed CSS = the 196-byte no-modules stub → no vector skin (blank column, foreign 9,887). Fix = relay the query upstream (minus `?head=N`). Receipt: relay content-column census EXACTLY matches direct (foreign 93,279 vs 93,062; blue/black counts identical); direct regression unchanged; guard tests 14/14. **Controller vision-confirmed the fixed relay renders the full article.**
  (ii) **N3 (pass) — ≥4 MiB single-response fetches**: root cause = `handle_tcp` copied every RX segment into the fixed 4 MiB per-socket ring with no overflow guard (wrap → silent corruption → open-fail). Fix = producer flow control: store only contiguous payload fitting free space, ACK only stored bytes, refuse overflow (`rx_drops`), advertise the TRUE free window (no min-2048 floor; window-0 + post-drain update-ACK), FIN only at contiguous end (ring = latency slack, not correctness bound). Receipts: 6 MiB blob + CNN 5,704,241 B identity both complete byte-exact; **sweep: SOCK2TST 35 PASS, DNSTST A-record, NETFIX 24/24, unit-arm 302/0, unit-x64 306/0, ARM wave 0-FAIL**.
  (iii) **R10 (partial) — big-doc completion (CNN 5.7 MB)**: parse COMPLETES (12,400 tokens, `end()`) but readyState stalls at three discovered gates: `<video>` → `setShouldDelayLoadEvent` pinned forever (media pipeline absent — now a no-op on HobbyOS), parser-blocking page scripts (now inert pre-prepareScript), and exactly 3 loader-less requests created at `implicitClose` whose async SubresourceLoader create never completes (shell never pumps the RunLoop; streak backstop landed but the replay-doc interleave defeats it). **Wikipedia regression GREEN at the fixed state** (load-ok 404,203 ms; styled frame `0xea5a4704`; PAGE-NET byte-exact). Residual + fix path (guarded `RunLoop::cycle()` or suppress deferred creation) → R11.
  (iv) **H2b (pass) — docs + release-readiness**: DECISIONS D-18…D-21 (relay-vs-direct truth = direct WORKS, OQ-1 settled + "direct gated-expected-fail" wording retired; timeout union 120 s/3600 s; driver nav architecture; data:-URL completion); **`docs/RELEASE-READINESS.md`** (G1–G12 → evidence paths; WK-6 checkbox status; residuals R-1…R-10 with owners; receipts index with shas); docs/webkit.md + PORT_PLAN §10 + README refreshed; §6 boxes flipped (this commit); skills drift verified resolved (all 12 in sync; the two flagged drifters already folded).
- 2026-10-06 — **W4 merged: T2 row flips at FINAL merged state (both legs load-ok + styled), 10.8-min soak, Google re-run renders real google.com — CP-FS3 closed. CNN rows fetch-proven with two new root-caused defects (→ W5).** Fork `1249bda393` (G11+H4); OS `3328495` (fs-v4+fs-h4).
  (i) **V4 (pass)**: full-skin relay load-ok 425,529 ms (styled frame `0x036bb41a`); direct https load-ok 824,991 ms (frame `0xfbd4089a`, 33 subres/30 TLS); fixture 8/8; reader + clock Δ13 s; soak 10.8 min (10 rows: 8 pass / 2 transient host-proxy resets / 0 fail, 0 FATAL); T2-17 shell-GET/DNS pass; memory probe honest (guest block pool; `frame_high` not syscall-exported). **Controller vision-verified: the direct-leg screenshot shows the fully styled article with legible text (T2's headline capability visually confirmed); the relay shot renders header/sidebar/TOC but a BLANK content column (residual → R9).**
  (ii) **G11 (partial)**: Google G1 relay+direct PASS (**real google.com UI rendered — controller vision-confirmed**; WN3-era TLS block fixed at merged state), G2 query PASS (real SERP 92,435 B), G3/G4 precisely-classified JS-required expected-fails (no silent skips); CNN fetch/transfer PASS both legs (**5,707,014 B decoded+persisted; 93/89 subresources** incl. images direct from media.cnn.com + woff2 fonts); CNN render blocked by two NEW root-caused defects: **(a) OS per-socket RX ring (`SOCKET_RX_BUF_SIZE=4 MiB`) — single-response wire bodies ≥~4 MiB abort the fetch** (A/B-proven; gzip-wire workaround; → N3), **(b) big-doc parser-completion stall** (~12,800 tokens then loop; R7's fix covers Wikipedia-size, not 5.7 MB; → R10). S2 Google-relay site soak 5/5.
  (iii) **H4 (pass)**: wb1 stale lock pruned (dir already gone); dead worktree registrations swept (fork 24→9, OS 73→13; live trees untouched); skills backfill verified + tonight-lessons (wk2-prefix-protected rule, kill-shim refresh) landed in repo sources + deployed; lane-teardown checklist documented (`docs/lane-teardown.md`).
- 2026-10-06 — **W3 merged: real-page navigation 7/7, direct-leg completion CLOSED, x64 stock-toolkit 10/10 posture — CP-FS3 met.** Fork `browser/l8-wk5` @ `1af2b6d3fc` (R7→R4→X6; conflicts: WK5WindowDriver includes union + pending-frag↔load-ok-emit order [R4×R7]; X6's interim driver-JS hunks resolved to R4's engine-side equivalents — X6's own report flagged them superseded). OS `02f6756` (wiki_proxy: R3's fixture route + R4's reader-nav test surface; fs-v2c checklist/clock-probe/perf).
  (i) **R7 (pass) — direct-leg completion**: root cause vs R1c's leads = **two `data:` URL subresources from the 213 KB vector-2022 CSS scheduled but never delivered** (the data: branch was gated on a `SubresourceLoader` downcast → fell through → Pending forever → `requestCount` never 0 → `checkCompleted` bailed). Fix = cast-independent data: branch + graceful catch-all terminal (204) + paint-and-blit BEFORE `[WIN] load-ok`. **g0-direct-fixed9: load-ok 378,387 ms; 33/33 subresources; 24/24 images; styled native frame `0x4941a85d` @ 1916×982; screenshot = fully styled article (logo/search/sidebar/tabs/text).** Bonus g1-relay-native-L2: load-ok 187,956 ms; 26/26; frame `0x58d778fd` (completes R5's crashed fullskin1).
  (ii) **R4 (pass) — real-page navigation**: three root causes — (1) `setScriptEnabled(false)` gates ALL normal-world evaluations → the true "0 link hits" cause (hit-test fine, eval readback empty) → driver JS moved to one isolated world; (2) even isolated-world eval aborted under TCG → link follow/fragment/scroll moved to engine C++ (`elementFromPoint` + `baseURL()` resolution, `scrollToFragment`, `ScrollView::scrollBy`); (3) `paintAndBlit` ignored the frame scroll offset → translate + scrolled-rect paint. **nav 7/7 @ 1916×982** (link, back, forward, fragment 0→1240, form-GET `?q=WebKit`, wheel scroll 0→160 with visible repaint) + runner reader pass + fixture 8/8 regression.
  (iii) **X6 (pass) — x64 acceptance**: rel-pointer mapping recalibrated closed-loop (origin (960,540), scales 2.3402/1.3186, click err (2,−1) px); stock-toolkit 1920×1080 coords fixed (Apps (38,1067), refocus y+12, step timeout 300 s, readback race fix); fork side = R4's isolated-world + native-viewport aim. **Stock wk6 toolkit (NO override): `session.ok=True`, 9 PASS + 1 honestly-gated (wheel — no x64 delivery path; owner = input lane); first x64 link-click nav (`WK6RES.HTM` load-ok 8,980 ms at 1 px accuracy); soak profile 9/9 iters, 0 FATAL.** Parity note: home frame family `0xde2cb675` = this binary state's native canon (0x107630ad was the 320×240 era).
  (iv) **V2c (gated) — T2 dry at merged state**: fixture **8/8 PASS** (incl. previously-gated FIX02D/FIX05), reader **1/1 PASS** (load-ok 11,418 ms, styled frame `0x91d24588`, glyph census 18,055 px), **T2-19 guest clock Δ13 s PASS**, PERF-NUMBERS (TCG-honest) delivered; full-skin/direct rows gated-with-owner-R7 → R7 landed after, V4 (W4) flips them at the final merged state.
- 2026-10-06 — **W2 results merged: L0 fully reversed (window-native legible render), images paint, x64 8/10 with byte-parity frames. CP-FS2 met (full-skin native L2 gated on R7).** Merged fork `browser/l8-wk5` @ `16b88d4fb8` (R5→R3→R1c→X5; WK5WindowDriver conflict resolved = R5 commit path + R3 census spliced, stable-state resets union; OS `734b619` adds the wiki_proxy fixture route).
  (i) **R5 (pass) — window-native render**: fixed `pref=320x240` dropped; renders at **1916×982** (`viewport render=1916x982 (scale 100%)`, `frame view=1916x982`); **glyph census 9,136 dark px + vision-confirmed legible native text**; TCG scale knob `WK5_RENDER_SCALE` (env + `::/RENDER-SCALE`; A/B 0.5 → 958×491, ~2× faster) with measured budget (~96–113 ms render + ~28–62 ms blit ≈ 150 ms/frame); post-load paint residual FIXED (checksum-convergent settle + cached-frame reblits; repair storm 32 full re-renders → 8 frames + 42 cheap reblits).
  (ii) **R3 (pass) — network images decode+paint**: 2/2 fixtures census-proven (FIX02 PNG + FIX02D data-URL; exact `0x00C800` over 710,468 px); base-href now injected for every text/html and pinned to the URL DIRECTORY; data: URLs decoded inline (async DataURLDecoder parked on the unpumped RunLoop — fixed); ImageLoader guard relaxed on HobbyOS; wiki_proxy fixture route byte-exact for non-HTML assets.
  (iii) **R1c (gated) — hung-fetch class CLOSED**: per-fetch watchdog (150 s, `/SR-DEADLINE`, graceful 504, [SR-timeout] log-once) + SVG XML-prolog mis-sniff fix + SubresourceLoader null-this guard + checkCompleted assert relax + crash-banner routing; fixed8 = **36/36 subresources delivered incl. the straggler, zero timeouts/drops/crashes, styled frame `0xd01365ff`, PAGE-NET byte-exact**. Residual: post-delivery parser-completion stall (vCPU spin, readyState unfired) → **R7** (dispatched W3).
  (iv) **X5 (gated) — x64 acceptance 8/10 on KVM**: first merged-intel build (183,243,752 B, sha `2456c9a2`); boot 10.0 s; **home-frame `0x107630ad` ×60 = ARM byte-parity; load-ok 5 = ARM; painted rect 1916×982+36 = ARM; on-device http 200/921 B; close rc=0**. Fails: link-click 0 hits + wheel (owners: R4/X6), stock-driver coords 1024×768→1920×1080 (toolkit). Prefix rebuilt from committed recipes after host-side deletion (incident logged in-controller).
- 2026-10-06 — **Stretch merge train landed: ALL lane work merged; merged-tip hard gates GREEN.** OS main @ `bef17c2` (X2 x64 liveness, X1 x64 input, V1 harness + exec bits, H2 docs/skills, R6 1920×1080 — `br_e2e` reconciled to V1 structure + R6 constants). Fork @ `cb7b80f196` (N1 gzip, R2 fonts, H2 docs/WK3, H1 license, X3+X4 x64 net + KVM probe, R1/R1b render fixes — conflicts resolved: DECISIONS.md keeps reviewed D-16 + D-17; WK5WindowDriver timeouts = 120s/3600s union). **Merged-tip verification: host 482/0 (`TEST EXIT: 0`); unit-arm 0-fail; unit-x64 (KVM) 0-fail; desktop_test at the new 1920×1080 mode green (screenshot validation passed); CI battery `fs-w2a` on hobytest: ALL FOUR TIERS rc=0 (host 53 s / unit-arm 165 s / unit-x64 108 s / test-arm 132 s — zero FAIL tokens, the first clean ARM wave).** W2 wave (R3 images / R5 window-native render / R1c hung-fetch watchdog / X5 x64 acceptance) dispatched on the merged revisions.

- 2026-10-06 — **Full-skin page through the windowed shell: fetch→parse→subresources→load-ok PROVEN, three root causes fixed, styled paint frame recorded (fork lane R1 + finisher R1b @ `bw-fs-r1`, `45bb1e9017`…`0107026f6d`).** (1) **g0 module ladder 5/5** on-device (all real en.wikipedia legs, exact shas) + integrated direct-HTTPS fetch byte-exact (`bb765a43…`, 120,003 B). (2) **Root cause A — parse wedge**: the WK5 self-driven shell never pumps the generic RunLoop, so `HTMLDocumentParser`'s time-based yield (`scheduleForResume` Timer) stalled the 120 KB parse mid-document (small docs finished inside the limit) → **ForceSynchronous single-pass parse on HobbyOS**. (3) **Root cause B — subresources**: http(s) subresource loads were scheduled to the fabricated dead `NetworkProcessConnection` and died silently at `scheduleLoad`'s null trackingParameters → **serviced in-process via WKNetFetch** (WebLoaderStrategy OS(HOBBYOS) branch), synchronous delivery, MIME from CachedResource type. (4) **Root cause C — SVG-MIME crash**: a naive `<`→`text/html` sniff handed SVG ImageResources HTML bytes → double-fetch → `WTFCrashWithInfo`; **type-first MIME resolution** fixed it (`f821a75fd1`). (5) **`<base href>` injection** pinned to the real document URL (`2b83151842`) so relative `/w/load.php` refs resolve to http(s) — the full **213,097 B Vector-2022 CSS bundle** plus site.styles, wordmark, 14 thumbnails, and icon SVGs now fetch in-process (**41 subresource receipts**); parse completes tokens=3200; **styled post-load FPC `0x60650de2`** (nonZero 300033 vs flat 300480) recorded; script execution disabled on the windowed page (JSC-under-TCG RLCONF spin; **T2 does not require JS**). (6) **Relay raw-route leg: verdict pass** — load-ok 178,701 ms, byte-exact page, 26 cleartext subresources. Residuals: on the direct leg one straggler SVG fetch can hang past its curl timeout and block `readyState=Complete` (per-fetch watchdog hardening = next render lap); the window viewport remains 320×240 (R5's domain). Evidence: `continuation/fs-r1/evidence/{g0-direct-fixed5/6/7,g1-relay-fixed7}` + README + report.

- 2026-10-06 — **Desktop display mode raised 1024×768 → 1920×1080 (OS lane R6 @ `browser/fs-r6`, `dbaec3c` + `0cc2877` + `dc9e8e5`).** One shared mode definition (`src/include/display_mode.h`: DISPLAY_WIDTH/HEIGHT/STRIDE_BYTES) now feeds both GPU paths (ARM virtio-mmio + x64 virtio-pci/BGA XRES-YRES), the userland graphics surface, the x64 multiboot graphics header, the kernel unit suite, and the host FB mock; `USER_FB_SIZE` 4 MiB→10 MiB so `SYS_MAP_FB` maps a full 8.3 MB frame. WM tiling/taskbar/wallpaper were already SCREEN_*-driven and scaled without clamp changes; the browser window is now a full-res tile (`[WIN] geom x=2 y=34 w=1916 h=1018`). Tests/tooling migrated to the new mode (host C tests; on-device editor/apps tests; `run_*.py` + capture scripts + `br_e2e` W/H). Gate PASS: **vision-verified desktop screenshot** (gradient/taskbar/Apps/clock intact, no clipping/overdraw/garbage), pixel-exact wallpaper/taskbar colours at new-mode rows, `make host_tests` green, unit-arm green (incl. updated clamp/offset suites), `run_desktop_test` green at the new mode (EDITOR_T menu→launch→File→Open flow), x64 kernel compiles. Merge note: `tools/br_e2e.py` W/H constants overlap V1's file — resolved at merge (V1 structure + R6 constants).

- 2026-10-06 — **x64 runtime net leg EXERCISED AND GREEN + toolkit KVM-probe fixed + NVMe loader re-measure (fork lane X4 @ `bw-fs-x4`, `16c9583403` + `1535667dca` + `8c322d3163` + `e626c77dba`; branch inherits X3's `c7423037ef`).** (a) **Toolkit KVM probe fixed** — the old probe (`-S` + stdio) always timed out and silently forced TCG on every x64 run; new probe boots a real q35 VM + stdio-monitor quit, `-enable-kvm` strict, `QEMU_FORCE_TCG=1`/`QEMU_ACCEL=tcg` escapes; verified live — x64 boot **58.5 s → 10.0 s**. (b) **x64 on-device https receipts**: headless KVM direct-edge B `https://en.wikipedia.org/robots.txt` status=200/28,275 B sha `48b98d80…` (**byte-identical to the ARM receipt**), A http→https 200/28,275 B, D article 200/120,003 B sha `bb765a43…`; windowed binary `[WIN] net fetch … robots.txt ok=1 status=200 bytes=28275` + `[WIN] load-ok` + `PAGE-NET.HTM` read-back sha `48b98d80…` — byte parity between the headless and windowed x64 paths. Strict CA verify (verifyhost/verifypeer) + TLS1.2 on the KVM leg. (c) **Finding**: the x64 guest clock remains mis-calibrated pre-X2-merge (~156× fast under KVM) — fork-side timeout scaling absorbed it (`1535667dca`; generous-only on ARM with its 1:1 clock); the OS-side fix is X2's (merged in this wave). (d) **NVMe loader re-measure**: v2 lazy map ~ms (no regression); 101 MB prefetch 33.3 s (KVM) / 16.9 s (TCG) vs the ~3-min pre-coalescing reference (~5× better). Residuals: LAN relay/proxy legs C/E = host-harness rc=56 (direct-edge passing outranks; not a guest defect); PS/2 address-bar typing still mangles under KVM (X1's documented input residual; TCG path used for the windowed leg); plain-text pages render unglyphed pending R2's backend (landed). **KVM probe fix flagged FOR THE TOOLKIT MERGE.**

- 2026-10-06 — **Readable text: font backend DEFAULT-ON; DejaVu letterforms verified in the windowed reader — L0 ("no letterforms") RESOLVED (fork lane R2 @ `bw-fs-r2`, `de579d93e6` + `ab7d9af3db`; disk recipe in main `48ce362`).** `HobbyOSFontManager` flipped default-on for the windowed binary (`/USE-FONT` retired; empty `/NO-FONT` = deterministic opt-out; the WK-3 gate keeps its `0x759431c5` via /NO-FONT staging in `run_wk3.sh`). Same-binary A/B via the V1 runner on `/reader/Habitat`: font ON → FPC `0x96d89031`, painted-rect census **77,478 dark px / 26,503 pure black**, **vision-confirmed readable black-on-white Wikipedia text** (title + body), load-ok 7.3 s, 0 FATAL; font OFF → FPC snaps to the historical `0xdabdfbc5`, census 0 dark px. Font staged as `::/DEJAVU.TTF` — the disk recipe now ships the vendored sha-pinned DejaVu Sans automatically (extract rule + mcopy). HarfBuzz reconciled: `libharfbuzz.a` 14.5.0 is **linked AND actively used** by the Skia text stack (HbScriptShim/ComplexTextControllerSkia) — D-15's "off" applies only to the font-MATCHING layer (single-face manager resolves every family to DejaVu Sans Book; bold/weights render in Book — legible; style fidelity deferred). Chrome address-bar glyph collisions (5×7 bitmap font) → folded into R5. Watch item: lane evidence face (`BUNDLED-FONT.TTF` sha `b4c632e3`, 759,720 B) ≠ vendored zip member (sha `7da195a7`, 757,076 B) — post-merge full-skin re-run confirms the vendored face renders.

- 2026-10-06 — **x64 PS/2 input fixed — click-launch + click-nav work on x64 (OS lane X1 @ `browser/fs-x1`, `4b611c1` + `efbc25e`).** Root cause: multi-packet PS/2 bursts overrun the i8042 output queue, so `ps2_mouse_send_packet` defers a packet to the next event and the kernel's integrated absolute position is read before motion lands (measured 380/470 units in one move) — the Apps click missed; separately E0 End/Home were never decoded, so the toolkit's keyboard menu-nav couldn't select WEBPROC. Fix (x64 half only of `src/kernel/virtio_input.c`: E0 key map incl. Home/PgUp/End/PgDn, overflow-safe decode, i8042 poll-drain in get_events; ARM path untouched). On-device receipts: staged clicks → Apps menu opens → BROWSER item click → `[LAUNCH] BROWSER.BIN` → window boot, in-window clicks delivered (`[WIN] click x=508 y=365`); **toolkit run 11: launch-browser PASS (the long-standing WF1B block), nav-home PASS, gated-http PASS, mem-probe PASS, close PASS, 0 FATAL**. Residuals: link-click hit-test (page-coordinate), wheel needs x64 IntelliMouse enable (3-byte→4-byte), back/reload = TCG timing; QEMU burst loss under KVM suggests a ≤127-unit-step driver cap (owner: next x64 lane). Note: WF1B's "EV_KEY 0x110/0x111 never reach userland" claim is **FALSIFIED** at tip (dumps show they arrive via SYS_GET_EVENTS; the real defect was position drift).

- 2026-10-06 — **gzip decode enabled + on-device gzip/big-body receipts (fork lane N1 @ `bw-fs-n1`, `5a637bd5ad` + `2c4d02971b` + `09197e0d92` + `1011359fde` + `fb930ac34b`).** curl had been built `--without-zlib`; rebuilt with zlib (old libcurl.a backed up), `CURLOPT_ACCEPT_ENCODING` set on both fetch paths (windowed libcurl bridge + WKNetFetch), in-proc transfer timeout 300→600 s. Six receipt legs, wire=gzip → on-device decoded sink, sha == fixture each time: article 73,400→472,784 B (`83499ef4…`); **big-body 181,437→1,256,646 B (`f2977cea…`)**; 64 KB 14,009→65,536 B (`37980c66…`); plus identity-encoding controls for 64 KB + 1.2 MB (same shas); `Accept-Encoding: gzip, deflate` on every guest request; `curl-config --features` now lists libz; link closure already had `-lz`. Runner line-verdicts pre-R1-merge still read 'fail' (keyed on load-ok = the render class; receipts independent and green). Follow-up: post-R1-merge render re-test of both legs (relay + scripts in-tree).

- 2026-10-06 — **x64 liveness: TSC calibration + `-smp 8` stability FIXED (OS lane X2 @ `browser/fs-x2`, `2f48639` + `d65e75a`; kernel-only).** Two stacked defects: (a) the TSC was calibrated against a timebase corrupted by the firmware-left unmasked periodic LAPIC vector-32 stream (~5–13× off on this host class, ~18× on the browser-era host), and the 8254 mode-3 counter decrements by two per clock (the 1.297M read was exactly 2× low until found) — re-calibrated against the PIT channel-0 counter. (b) The `-smp 8` desktop `LOSTWAKE → #PF` crash was the fast clock collapsing `LOSTWAKE_DEAD_OWNER_MS`; with a wall-true clock and the arm-and-confirm gate intact, the x64 idle reaper behaves like ARM's (disposal requires a ≥3.4 s real frozen-claim confirmation). Gates green: `-smp 8` idle receipts (12.5 min & 6.2 min, NO FATAL/#PF, input responsive) + unit-x64 **306/0** (KVM). RCA doc filed with the lane.

- 2026-10-06 — **OQ-2 RESOLVED: the merged WA render fix CURES the x64 white paint (fork lane X3 @ `bw-fs-x3`, `c7423037ef` + 3 driver commits; run 134).** Before (old binary): one white-canary frame, FNV-1a `0x39878dc5`. After: **66 frames, ZERO white-canary**; HOME.HTM frame `0x107630ad` ×58 = **byte-parity with the ARM canonical frame** (WA run4), painted rect `1020x670+36 at 2,70` identical to the ARM reference; content-rect pure-white fraction 49/683,400 px (vs all-white before); screenshot vision-checked (real chrome + styled page blocks, not white). Session: boot/launch/nav-home/gated-http/mem-probe/close PASS; link-click stays with lane X1 (mouse-button gap); back/reload = TCG-settle rerun item; wheel = X1 stretch. **No fork readback fix was needed — `49f28a3bb5` exonerates `WK5WindowDriver` readback/paint on x64.** Run 133 was killed mid-boot by lane X1's unscoped pkill (X1 corrected); run 134 is clean. Follow-ups: stock-toolkit x64 re-run after X1 buttons land; `run_acceptance.sh` KVM probe bug (`-S` probe always falls back to TCG) — fix for KVM-speed x64 boots.

- 2026-10-06 — **Acceptance runner unified: one canonical host-side runner + report v2 + concurrency (OS lane V1 @ `b403492` + `bf7b135` (+`a0d5aae` report); verdict gated — harness complete, 2 rows gated-with-owner).** `tools/run_browser_accept.sh` modes `fixture|reader|direct|full` (default = legacy `/wiki/Main_Page`), per-request JSONL netlog, `report.json` v2 (per-row `pass|gated|fail`, load_ok/ms/frames/checksum/screenshot/net/persist + artifact hashes), proxy `/fixture/<name>` byte-identical route, host guard tests `tools/test_wiki_proxy.py` **13/13** (no QEMU needed). Fixture leg on merged tip: **0 fail across 3 runs** (6–7 of 8 rows pass); the 2 gated rows (FIX02D data-URL img, FIX05 inline script) = the documented F6/F7 resource/settle class (**owner: the W2 image/resource lane**), never a harness defect. Reader mode 1/1 `0xdabdfbc5` == milestone checksum. **Concurrency:** `--instance`/`ACCEPT_ID` scopes sockets + proxy port-file; two live ARM QEMUs isolated side-by-side (a 7/1/0, b 6/2/0); default no-flag path byte-compatible (F-R2 intact). Follow-ups: re-run fixture after resource-settle lands (expect 8/8); **ctrl-socket timeout must derive from the go-step timeout** (N1 hit a driver wedge on 900 s legs under contention); other lanes adopt `--instance`.

- 2026-10-06 — **WK-7 license relink package DONE (fork lane H1 @ `5d9ad88067`; decision D-16, reviewed by H2).** Evidence-by-documentation pack at `HobbyOS/continuation/wk7-license/`: exact final ARM link command captured twice read-only (`link-line.txt`), SPDX cross-check of the full static closure (`spdx-crosscheck.md`; GLib/pcre2 confirmed NOT linked; D-15's "harfbuzz blocked" record superseded by the built state — full `libharfbuzz.a` 14.5.0 on the link line), artifact manifest (34 files / 137.3 MB, member maps + recipient relink recipe), written-offer DRAFT (integrator final-review placeholders). Residuals: materialize the bundle; final offer language; fork README SPDX/rebuild note.

- 2026-10-06 — **Docs & registers refreshed + WK-3 report unified (fork lane H2 @ `a6dd6b49bd` + `a5b80a30d8` + `7cb0b166d4`; OS `408cb3c` + `283e184`).** `WK3-REPORT.json` unified with the wk3x64 gate closeout — **status X64-GREEN, WK-3 GATE MET (ARM + x64)**, artifacts byte-identical + FPC parity (FIX01 `0x759431c5` etc.); WK3-EVIDENCE closeout section inserted. Fork docs: `docs/webkit.md` refresh (fixes the noted `:28` drift), README + PORT_PLAN WK-3+ rows + superseded labels, DECISIONS D-16 reviewed + **D-17 (runner concurrency)**. OS: `docs/wd-x64-white-blit.md` marked **RESOLVED** citing X3's byte-parity receipts; 4 deployed skills folded back into repo `skills/` sources (build-and-run, run-tests, graphics-acceleration, port-planning — diff-identical; deploy.sh stays one-way). Residuals: `hobbyos-desktop-compositor` + `hobbyos-kernel-constraints` also drift (flagged for a later fold); §11 drafts archived at `continuation/fs-h2/proposed-browser-md.md`.

- 2026-10-06 — **FS-W1 merged-tip CI battery (hobytest `192.168.0.20`, label `fs-w1a` + 2 ARM-wave reruns r2/r3): hard gates GREEN; ARM wave ambient-red accepted after 3 runs.** host **482 checks / 0 failed** (`TEST EXIT: 0`, 55 s); unit-arm **302/0** (172 s); unit-x64 **306/0** (KVM, 112 s); ARM wave (`make test`): 3 runs rc=1, **1 / 5 / 1 FAIL tokens — every one a documented ambient class** (SHELLTEST protocol-stall watchdog; `shell_test3: FAILED ls -l /SUB1`; NFS 3-of-60 cluster; slot-pressure forensics `no free slot` ×3 + `thread_create used=63/63` — all pre-existing on this tip), with 18 suite summaries + STRESS 120/120 + `System halt` present each run; x64 wave NOT run (known-incomplete at tip by design — **unit-x64 is the hard x64 gate**); desktop + accept subset: desktop PASS, V1 fixture leg 0-fail (see runner entry above). Receipts: `~/ci/logs/` on hobytest; verdicts three-way (rc / marker / FAIL-count).

- 2026-10-05 — **Address bar + GO button; Wikipedia loads on-device (user ask).**
  The windowed browser now has a persistent address input field + a GO
  button (WK5 driver chrome, bitmap font): click focus → type → Enter/GO →
  navigate.  WA also root-caused the merged empty-DOM regression: the
  self-pumped poll loop dropped the RunLoop pump, so substitute-data bytes
  never reached the HTML parser; DocumentLoader now delivers them
  synchronously under OS(HOBBYOS).  WB found the OTHER half: the guest v1
  TCP stack never ACKed and advertised a fixed 2048-B window → any receive
  > ~2 KiB stalled (direct-edge TLS 'SSL connect error' + proxied-bulk
  'Failure when receiving data from the peer'); fixed on OS main (per-segment
  ACK + real window + window-update after drain, `477c5c4`), unit-arm 302/0.
  WB proved 5/5 real en.wikipedia fetches on-device (robots.txt direct + via
  relay + https-proxy, and the 120 KB wiki/Hobbyist_operating_system page).
  WD corrected an earlier controller error: the x64 'identical checksum'
  was FNV-1a of an ALL-WHITE frame — the OS display path is byte-perfect
  (chroma probe); the fork-side paint black hole is WK5WindowDriver's
  Skia readback (real page hash 0x759431c5).  FINAL integrated run (merged
  fork + TCP-patched OS): address bar → GO on the reader-mode Wikipedia
  URL (`/reader/Hobbyist_operating_system` on `tools/wiki_proxy.py` —
  real REST extract) → `load-ok` + painted frame `0xdabdfbc5` with page
  structure on screen; session.ok=True.  Known limits: text glyphs are
  shell-font blobs; DIRECT https to https://en.wikipedia.org still hits the
  mbedTLS<->edge interop (documented; the proxied/reader path is the
  deliverable).

- 2026-10-05 — **Browser in the Apps menu + 1 GiB startup disk (user ask).**
  `make disk.img` is now 1 GiB FAT16 (auto cluster; driver BPB-driven, no
  kernel change) and ships the flat windowed WebProcess as `::/BROWSER.BIN`
  (BROWSER_BIN per-arch default; objcopy -O binary'ed to the v2 loader
  contract — the raw ELF was rejected: `v2 loader: bad image size` over the
  128 MiB cap; the flat is 82/96 MiB).  desktop.c pins BROWSER.BIN first in
  the Apps menu and adds **F1 = open Apps menu preselected on BROWSER**
  (Enter launches) — a keyboard path to the menu, needed on x64 where the
  kernel's PS/2 mouse buttons are never delivered (WF1B).  Also fixed an
  arch-stale kernel ELF trap: ./hobbyos.elf is shared by both ARCHs, so
  building intel then arm produced an arm disk with an intel kernel
  (`limine: invalid kernel image`); the kernel rule now relinks on ARCH
  change (FORCE_ARCH via .EXTRA_PREREQS).  VERIFIED on-device both arches:
  ARM click-launch and F1-launch render HOME.HTM (checksum 0x39878dc5, the
  probe-green page); x64 F1 → menu (BROWSER preselected) → Enter launches
  the 100 MB windowed browser with a byte-identical frame checksum.  NEW
  FINDING (x64 windowed display): the x64 frame blit paints the window
  WHITE while the render is correct — first x64 windowed render surfaced an
  x64-specific display bug in the fork's WK5 blit/scanout path (open).

- 2026-10-05 — **Wave-5 close-outs (fork tip `befd84e00`, OS main has all).**
  ARM 16 GiB FIXED (WE1): root cause = Limine loads the flat kernel near
  TOP of RAM (image_pc = RAM_top - ~94 MiB), so at 12/16 GiB the image sat
  OUTSIDE the old 0..9 GiB identity map and faulted the instant MMU turned
  on. Map extended 0..18 GiB (mmu.c L2 tables 9..17); full ARM wave 120/120
  @16 GiB and @8 GiB control, unit 302/0 both; Makefile ARM back to
  `-m 16384M` — the user's RAM×2 now fully valid both arches.  Intel
  build reproducible (WE2): tools/cross-intel-wk2-libs.sh stages
  mbedTLS 3.6.7 + libcurl 8.22.0 into the intel prefix (same layout as
  ARM); fresh ARCH=intel configure finds CURL and NetworkProcess links
  (180.7 MB).  WN3 site-live: G2 text-input + F9/F10 scripted ops landed
  fork-side; REAL google/CNN attempted — guest TLS genuinely reaches
  google's wire (142.251.155.119:443, pcaps) but the mbedTLS<->edge
  handshake interop fails; host-proxied https handshake is GREEN (real
  google 200/88486B upstream) with a guest bulk-recv class left; 0 FATAL
  across all five site runs.  CRITICAL finding: the merged-tree windowed
  WebProcess renders an EMPTY DOM even for fixtures (load/policy/start-main
  all execute; doc title=|0|0|0|0) while the pre-WK-4b 141 MB ELF passes
  dryfix+link-click in the same env —=> merged-build render regression is
  the top fix target for the G/C rows.  WF1B x64 toolkit: --arch intel
  wired end-to-end (OVMF/q35 runner, rel-pointer mode calibrated
  pixel-exact, fresh 182 MB intel windowed WebProcess via WE2's staging);
  the x64 acceptance gate is BLOCKED on an OS-side x64 PS/2 mouse-button
  gap (EV_KEY 0x110/0x111 never reaches userland; wheel is ARM-only per
  browser.md) — OS input lane scheduled; retest command documented in the
  toolkit.  Also surfaced: x64 windowed desktop crashes at -smp 8 (kernel
  LOSTWAKE->#PF), worked around with -smp 1; scheduled for a kernel lane.

- 2026-10-05 — **WK-4 GATE GREEN on ARM — Path B end-to-end (fork merged at
  `232f694b74`).** WN2 passed the WK-4c runtime-networking gate: on-device
  HTTP (status 200) + redirect (302→follow) + HTTPS (200, plus CA-ENFORCED
  negative test: bogus CA → curl rc=77) + cookie round-trip (ROUNDTRIP-42)
  through the real NetworkProcess→libcurl(mbedTLS) path. Root causes fixed:
  the spawned NetProcess has no stdio fds so `socket()` stole fd 0/2 and
  console writes vanished into the socket (curl aborted already-ESTABLISHED
  handshakes) → reserve fds 0–2 at NetworkProcessMainCurl entry;
  BSD `send()`/`recv()` routed over AF_UNIX-only `sendmsg`/`recvmsg`
  (-EOPNOTSUPP) → routed to `write()`/`read()`; curl was configured against
  another lane's include path (reconfigured for wn). FDMA other caveats:
  mbedTLS post-handshake `verify_result`=0xffffffff on TLS-1.3-era ⇒ CA
  enforcement proven via the negative test; redirect `chain='none'`
  (effective-URL evidence gates). WF1: the x64 WK-3 "wedge" was LOST
  EVIDENCE, not a defect — the current intel WebProcess redirects gate
  markers to `/WK4DBG.TXT` (UART-DMA workaround) and the old runner only
  tallied serial; with dual-channel tally the CURRENT binary is green
  (JWK3 ALL-DONE + FPC 0x759431c5, runs 26-28; WX's "wedged" runs produced
  byte-identical pixels). WF2: WK-7 auto-respawn GREEN — WEBWATCH.BIN
  supervisor (waitpid + bounded relaunch, max 2 per 180 s burst, graceful
  close never respawns): kill -11 → respawn + window + page reload with zero
  human action; third crash → give-up stop-loud; desktop never faults. WF3:
  full 5-min §8.3 site-soak ok=True (300 s, 16 iters, 64 wheel, 16/16 exact
  URLs), checklist truth-swept (every row evidence-mapped or gated+owner),
  http site-live dry proves the transfer (byte-identical FETCHED.HTM over
  Path A; the WK-4 probe-timing fail is documented non-gating). Left for
  wave-4→5: live Google/CNN runs (text-input wiring for Google queries),
  x64 toolkit leg, ARM 16 GiB memory map, intel curl staging.

- 2026-10-05 — **Browser wave 2 + OS x64 RCA (merged).** OS main:
  `SYS_GETARGV` lacked the `process_user_ok` pre-commit its sibling GETENV
  has → with lazy demand-loaded webproc the argv page could fault the KERNEL
  (early x64 #PF dump, vector 14, RIP 0x70020EB6; the handler's FATAL dump
  printed before its L9 recover — non-fatal but loud); fixed in `2b75311`
  (x64+arm trap.c), verified on-device 4/4→0/4. WX verdicts: the x64 wave
  final-STRESS wedge is a PRE-EXISTING latent x64 class (freeze-tree control
  wedges identically on this box; kernel process/loader byte-identical;
  `.lbss`-enlarged bins manifest it earlier) — unit-x64 remains the hard
  gate; the current-intel-WebProcess early halt is FORK-side (gate-era
  binary completes ALL-DONE with cross-arch FPC parity 0x759431c5 on the
  same OS) → x64 WK-3 fix lane dispatched. Fork (`94e72db74e`,
  `e79b385ff6`): WI fixed the port input bugs — URL bleed was the driver's
  `m_urlBuf` never tail-cleared (wire tap proved zero phantom bytes: OS key
  re-delivery exonerated), wheel-up crash was the tick loop running through
  the fragile sysroot RunLoop-timer/condvar path (replaced with a self-pumped
  poll loop) — gate 16/16, soak ok=True (7 iters, 28 wheels, 0 mangled,
  0 exits). WH proved WK-7 crash recovery (real SIGSEGV via kill -11 →
  desktop survives, page reloadable, no reboot; no auto-respawn yet — WK-7
  respawn lane dispatched), measured startup (6.6s desktop / 14.8s first
  frame / 1.5s warm respawn) + high water, finalized docs/webkit.md, drafted
  the WK-7 evidence matrix. WK-4c (runtime networking) in flight.

- 2026-10-05 — **Browser wave 1 (WK-4 Path B build-out) MERGED in the fork
  (`659a8d6cc5`, 4 lanes, zero conflicts).** (a) **WK-4b backend**: mbedTLS
  3.6.7 + libcurl 8.22.0 (static, ARM) cross-built into the wk2 prefix;
  `USE_CURL` wired (find_package + Curl.cmake + MbedTLS::MbedTLS); real
  mbedTLS TLS glue (MbedTLSHelper, CurlSSLVerifier/CurlContext
  OS(HOBBYOS) branches, PAL CryptoDigest over mbedtls_md — replaces the WK-2
  abort stub); NetworkProcess LINKS with the real backend (146,192,872 B,
  curl + 250 mbedtls syms). Link-gate only — runtime is WK-4c (next wave);
  no CA bundle yet; `CURLINFO_SSL_VERIFYRESULT` always 0 on mbedTLS
  (use `mbedtls_ssl_get_verify_result`). (b) **NetworkProcess bootstrap**:
  real spawn_ex fd-handover + AuxiliaryProcess init handshake + IPC
  round-trips, e on-device ARM (`pid=4 … ping#1/ping#2 reply=pong`, alive
  13 s, rc=0). ROOT-CAUSED kernel boundary: OS caps `msg_controllen` at
  `K_IPC_CTRL_MAX=512` but upstream WebKit asks `CMSG_SPACE(254×4)=1032` →
  every `recvmsg` EINVAL → vacuous teardowns; fork-side fix:
  `attachmentMaxAmount 254→120` + a vacuous-completion diagnostic. (c) WK-5
  E2E on the GX-merged OS tip: soft headless rc=0 + framebuffer checksum
  `0xe0982225` (runs 5/7/8) and GL run 8 rc=0 with X-capture parity (same-
  step MAD 0.68–0.80, byte-identical samples); first-frame ≈23 s soft/GL.
  x64 found split: gate-era intel WebProcess completes WK-3 ALL-DONE (cross-
  arch FPC parity 0x759431c5 = ARM), CURRENT intel build wedges (WK-4
  network-smoke region, all-CPU SYS_FUTEX) — plus a new merged-tip early x64
  #PF (vector 14, RIP 0x70020EB6) → scheduled x64 fix lane. (d) WK-6
  acceptance TOOLKIT: parameterized driver + executable checklist + soak
  harness + hung-page watchdog + license inventory; dry-run **ACCEPTED**
  (session.ok=True, 10 steps 9/0/1-gated, 11 screenshots, guest mem/proc
  artifacts, close step passes); settle() race fixed (offline 13/13). Soak
  documented as limitation: wheel-up exits WEBPROC + repeated-URL key bleed
  → scheduled input-fix lane. Next wave: WK-4c runtime networking, x64
  wedge, input fixes, WK-7 recovery.

- 2026-10-05 — **GX graphics program COMPLETE + merged (`9fce7ec`; full record
  in `docs/graphics-accel.md` §7).** Both arches present through accelerated
  paths (ARM virtio-gpu IRQ-completed rect flush; x64 modern virtio-pci + BGA
  rects), every launcher carries `QEMU_GPU` gl/soft/auto + `make gpu-check`,
  desktop damage rects drive `flush_fb_rects` end to end, and the x64 desktop
  boot blocker (`.lbss` flat-image truncation, `4cf6d44`) is fixed.  Test
  throughput: VM RAM (x64 6→12 GiB, ARM 8→16 GiB — ARM fixed on the
  arm-16gib-map lane: identity map extended 0..18 GiB) + `tools/run_parallel.sh`
  (3×8 =
  24 cores).  Browser
  resume next: WK-4 Path B (NetworkProcess + libcurl) → WK-5 perf sanity on
  the accelerated present path → WK-6 acceptance; apply G3's fork-runner GPU
  patch at dispatch; note the x64 main-wave wedge open item (unit-x64 is the
  x64 gate).

- 2026-10-04 — **GX graphics-acceleration program opened (user-directed:
  "high performance, accelerated graphics throughout the system" + "each
  QEMU launcher ... graphics acceleration (even if it needs to be
  emulated)" + both arches; browser support).** Plan + fix log:
  `docs/graphics-accel.md`. Freeze landed: `SYS_FLUSH_FB_RECTS` (89),
  libc `flush_fb_rect(s)`, kernel `virtio_gpu_flush_rects()` (full-flush
  fallback), x64 driver split (`virtio_gpu_x64.c`), `GFXBENCH.BIN` wired.
  De-risk: virtio-gpu-gl-device + `-display gtk,gl=on` renders the
  existing 2D guest (capture archived); headless tiers keep plain device +
  `-display none`. Lanes G1–G4 in flight.

- 2026-10-04 — **Test-harness /tmp hygiene (user-directed; closes the /tmp inode incident): every run script now cleans its own per-run artifacts at exit.** Fork-side runners — WK-3 ARM/x64, WK-4, WK-5, test262 chunk, JSC smokes — remove the assembled `disk-*-<run>.img` (128 MiB each, the main accumulator), fixed-path `/tmp/px-*`/fixture/fontdbg/stable temps, the WK-4 servedir + server log, test262/JSC OVMF copies, and the WK-5 QMP socket; success or failure; **`KEEP_DISK=1` retains the disk** (used by timed `wk3_timed2.sh` re-boots). Scratch trees and fork-dir evidence are never touched. Host-side: `build_tu21_tail_ref.sh` was the last `src/host` script leaking a mktemp dir per run — now trapped like its siblings. Verified: 8/8 `bash -n`; every cleanup function exercised in both modes; live **WK-5 122→removed / 123→kept** and **WK-3 124→removed** — all green; tail-ref rebuild leaves zero dirs; ~1.3 GB of pre-existing per-run disks + 29 stale outputs pruned. Fixed-name /tmp paths that are documented evidence (repo-root Python E2E screenshots/serials; C-test fixtures) are bounded per run — deliberately left. Commits: HobbyOS `333e811`; fork `0730edbc08` (the wk4 runner edit rides in that repo's untracked working tree).

- 2026-10-04 — **Browser first frame 55 s → 18 s — background prefetch task + eager first page MERGED (`a399abe`; lane `browser/l9-prefetch` @ `de56024`).** User-directed loader strategy ("I love that the loader is using demand paging… map the pages but proactively load only the first executable page; schedule the process and let it go; a background kernel thread loads the remainder while the process executes — fault/prefetch synchronization accepted as the challenge; the thread is done once the image is fully loaded; queue disk reads so at least a few pages load at a time"). **Loader:** `v2_map_image` eagerly loads the entry page at `USER_IMG_BASE` (marker now `first=in elap=0–1ms`; `resident=1 tables=3` at load vs `resident=0 tables=1` on the demand-only baseline). **Prefetch task:** images ≥ 2 MiB (`V2_PREFETCH_MIN_BYTES`) autostart a runtime kernel task (`process_create_kernel_nowait`) that walks the image in file order, loading up to 128 pages per `fat16` call (`V2_PREFETCH_BATCH_PAGES` — 512 KiB, exactly one 1024-sector device cap), coalesced per contiguous cluster run into a reusable scratch run; refreshes `cpu_heartbeat_ms` per batch/page; `kernel_exit()`s when fully resident (`stop=1`), on AS epoch mismatch (`img_epoch`, bumped at `vm_as_create`/teardown — pid/AS slot-reuse safety), or on image identity change. Exec path (AS rebuilt in-slot) autostarts identically. **Synchronization:** both paths map-if-absent under `vm_lock`; first installer wins, late-arriver drops its frame — races benign by construction, worst case one duplicate 4 KiB read (recorded in `skipped`). **Gated + best-effort:** the `MODE=test` wave spawns zero tasks; eager-page/spawn failures leave the demand path intact. **Evidence:** WK-3 86 MB WebProcess (`-smp 8`) — `resident=1` at load, then `[IMGPRF] done: pages=21061 mapped=20873 skipped=188 batches=165 stop=0 ms=5472` (86 MB in 165 batched reads while the process ran); rendered-frame checksum byte-identical to base on both trees (`0x759431c5` / fnv1a64 `0x9287e527c8ce4f83`); **WK-3 timed wall-clock QEMU-start → first completed frame: 18 s vs 55 s base** (same harness/disk/flags/machine); WK-5 acceptance full green in all four runs (2 base, 2 changed; `exit rc=0`, all converging to final-frame checksum `0xe0982225`), changed loads earlier throughout (first post-boot load ~2.2 s vs ~7.1 s; cumulative tall.htm 8.9 s vs 13.9 s in the quiet pair; desktop-flow prefetch 21067 pages / 165 batches / 5.3–5.8 s); tiers: unit-arm 299/0 (+2 new vm tests), unit-x64 302/0 (KVM), host 482/0 server + local full suite 0 failed (15/15 tool-parity suites, 1,466 checks + 1,367 parity cases, `TEST EXIT: 0`). **Residuals:** `[IDLESTUCK]` armed-candidate dumps under prefetch load (21 vs 5 on the demand-only run — pre-existing class, none confirm/dispose, both runs green); wave FAIL-token flakiness (slot-pressure/serial class) reproduces on pristine main with identical forensics — not lane-attributable; x64 runtime browser load not re-run (unit-x64 green; same posture as l9-pagein).

- 2026-10-04 — **IDLESTUCK diagnostic gated on real anomalies** (user: "don't get too much signal in logging if there isn't something more real going on"). The dump fired purely on an idle-round counter (≥500, then every ~1000), so a healthy 8-core desktop with only a couple of live processes spammed 347+ full table dumps per run — noise that feeds the documented serial-pressure/drop flakiness. Now the dump (process.c `start_scheduler` idle path) fires **only when this reaper pass actually found something real**: a RUNNING slot with no claiming CPU (the reclaim branch) or a stale-dead-owner candidate (the arm branch). A healthy-but-idle core shows `slot ... st=3 ... claims: cX=pid` (alive, claimed) and prints nothing. The monotone 1000-round rate-limiter still throttles a genuinely wedged system to one dump per window so a stuck suite still names itself. Verified: cstyle clean; unit-arm 297/0, unit-x64 300/0 (KVM); host suite result logged at `/tmp/idlestuck-hosttests.log` (run after this change — see that log for the pass/fail tail).

- 2026-10-04 — **Post-SMP-fix test-server battery (`post-smpfix-20261003`, hobytest @ `9dd046c`): fully GREEN where gating + 256 MiB disk geometry regression surface verified.** Sync verified (`rev-parse` = `9dd046c`). **Critical geometry item CONFIRMED:** disks built in both unit workdirs = exactly 256 MiB, fsck.fat 8192 B/cluster (16 sect/cluster), 524,288 sectors; `fat16_test_suite` (8 tests incl. lfn + large_create_write) passed in **both** unit-arm and unit-x64 on the new geometry — **no regression** from bpb_sectors_per_cluster 8→16; test-arm `[FATBIG] ALL PASSED` (2 MiB round-trip) on the new disk. Tiers: host 482 checks 0 failed (`ALL APPS SUITE TESTS PASSED`/`TEST EXIT: 0`); unit-arm 297/0; unit-x64 300/0 (KVM); **test-arm PASSED** (`System halt from CPU 6.`, 0 FAIL, STRESS 120/120, 21 suite summaries, browser skip line correct since `BROWSER_BIN` absent on the server); test-x64 known-incomplete unchanged (1800-s bound, reached STRESS 110/120 then late-IDLESTUCK storm — 85,512 IDLESTUCK lines; NOT a lane failure; 2 FAIL tokens both inside the incomplete wave, incl. a FATBIG tail-checksum `got=0x800 want=0x0` that does NOT reproduce on ARM with the same disk — kept as a watch item only).

- 2026-10-04 — **Browser load: 90 s → ~2 s — coalesced reads + demand-paging MERGED (`c450cff`; lane `browser/l9-pagein` @ `c3c6c81`+`911b71f`).** User-driven (load-time never explained by ~1 MB/s). **Part 1 (`c3c6c81`):** the v2 loader read the image one 512-byte sector per virtio-blk device request (`fat16_read` per 4 KiB frame) — ~168,000 TCG round-trips for the 86 MB WebProcess. Switched to `fat16_read_direct` (coalesced multi-sector requests, up to ~1 MiB): on-device 86 MB load **90 s → 2.3 s**. **Part 2 (`911b71f`):** demand-page the v2 IMAGE region — `v2_map_image` records the image's FAT identity (start cluster + size) on the addr_space and returns, so the loader syscall costs **0–1 ms**; each page materializes on first touch through EL0 faults (ARM EC=0x20/0x24, x64 #PF) and the kernel-mode touch path, via new positional `fat16_read_direct_pos` (serialized under fat_lock, no shared cursor → safe under concurrent multi-CPU faults); I/O never runs under vm_lock; only touched pages occupy frames (also a memory win for 86 MB + 256-byte bmalloc). ARM/x64 kernel-mode #PF now demand-materializes a legal not-yet-faulted user page and retries. **Evidence:** WK-5 at `-smp 8` ×2 runs: loader 0–1 ms, full acceptance (link/back/fwd/reload/URL/wheel/close `exit rc=0`), framebuffer checksum `0xe0982225` **identical to the eager path** (byte parity proven), zero kernel FATAL, zero LOSTWAKE-dispose; unit-arm 297/0, unit-x64 300/0 (KVM), host 3161 PASS/0. Residuals: first-touch page-in storm serialized per page (~seconds wall-to-runnable, still ~40× faster than before); x64 browser load not re-run (same vm code; unit-x64 green); scalar-only image source → nothing to close at teardown, inherited by v2 fork via `vm_as_clone_into`.

- 2026-10-03 — **SMP liveness fix MERGED (`80f404a`; lane `browser/l9-smpfix` @ `3b14758`) — the browser runs at 4-8 cores, `-smp 1` is dead.** User-driven: the WK-5 browser had been landed with `-smp 1` as a workaround; the standing rule is the OS works at 4-8 cores always. **Repro (ARM run 11, `-smp 8`):** `[LOSTWAKE] disposing pid=2 (stale dead-owner claim, poisoned resume)` → EL1 FATAL (`EC 0x21`, CPU 1) → WebProcess (pid 3) stuck RUNNING forever, 2,795× IDLESTUCK, zero window markers. **Root cause: CONFIRMED** — `cpu_heartbeat_ms` refreshed only on claim (process.c:1221) and idle (process.c:2596); the v2 loader core's long IRQ-off stretches during the 86 MB image read aged its claim stale, so the idle reaper DISPOSEd a genuinely-running process (the x64 lane's parallel defect: ~18×-fast TSC collapsed `LOSTWAKE_DEAD_OWNER_MS` to ~110 real-ms). **Fix (5 files, +152/−27):** refresh the heartbeat at ARM timer-IRQ + syscall entry (x64 parity), per sector in `virtio_blk_read_sector`/`virtio_blk_do_op`, per page in `load_image_v2`, and an **arm-and-confirm LOSTWAKE gate** (a single stale-heartbeat pass no longer disposes; two consecutive stale passes are required — a host-starved-but-alive core exits the stale window within one pass). **Verified at `-smp 8` (the new `QEMU_SMP ?= 8` default):** WK-5 run 18 = clean PASS (0 dispose / 0 FATAL / all 16 acceptance markers incl. `[WIN] exit rc=0` / 3 QMP screenshots); unit-arm 297/0; unit-x64 300/0; host 3161 PASS 0 FAIL; ARM wave clean (0 FAIL tokens, `System halt from CPU 6`, STRESS 120/120 SUCCESS, rc=0). Residuals: single-false-dispose possible only across TWO consecutive starved passes (>~2.8 s), never observed; 20 s console-silence watchdog remains the freeze backstop. Also on this head: **menu integration (`56067aa`)** — browser now *actually* on the standard disk as `browser.bin`, pinned at the top of the Apps menu, label reads exactly **"browser"** (case-insensitive `.BIN` strip), disk flipped 64→256 MiB FAT16 `-s 16` (W0.3-pinned; 131 MiB free after the 82 MiB browser), `QEMU_SMP ?= 8` default, `BROWSER_BIN` variable (skips gracefully when the fork binary is absent). Host menu test 13/13 PASS incl. `browser.bin -> "browser"`.

- 2026-10-03 — **Post-netfix test-server battery (`post-netfix-20261003`, hobytest @ `0d9bd99`): GREEN where gating.** host PASSED (`ALL APPS SUITE TESTS PASSED`/`TEST EXIT: 0`); unit-arm **297/0** (was 296 — the +1 = new `test_net_ether_padding`); unit-x64 **300/0** (was 299); test-arm **0 FAIL + `System halt.` + 21× ALL TESTS PASSED** on a fresh full-rebuild run (`post-netfix-20261003-testarm3`); test-x64 unchanged-known-incomplete (1800-s bound, all-idle IDLESTUCK stall identical signature to baseline, furthest MD5SUM_T/TACTEST/CMPTEST). Notes: two earlier incremental test-arm runs each showed 1–2 transient SHELLTEST protocol-stall FAIL tokens at different points — did NOT reproduce on the clean rebuild; classified as the lane's documented Bug #3 serial flake, not a netfix regression (netfix diff = virtio-net TX/RX only; shell/serial path untouched). `NETFIX.BIN` confirmed inert in the wave (disk mcopy only, never booted). If shell protocol stalls recur on future CLEAN runs, that is a real serial-drop signal to investigate.

- 2026-10-03 — **OS kernel lane `browser/l8-netfix`: all three WK-4 kernel net bugs resolved + on-device evidence (fork head below; lane report + harness in `continuation/l8-netfix/`).**
  - **Bug #1 — virtio-net TX shares a DMA source with the UART console (socket send transmits the last console line → host server 400):** root cause is structural in the ARM MMIO driver: `virtio_net_send` pointed the TX payload descriptor DIRECTLY at the caller's buffer (`send_tcp_segment`'s kernel-stack frame). The device DMA-reads that memory asynchronously, so anything the kernel stack or console path later writes there shows up on the wire (exactly the observed `WK4 NET connect=ok … → 400`). The x86/PCI path already copies into driver-owned slots; ARM did not. **Fix:** ARM TX now owns a per-slot header+payload pool (8 slots = the 16-descriptor queue, mirrors the x86 design), copies the payload in before submit, and never references caller memory; a bounded in-flight cap prevents reusing a slot while still queued. On-device: `NETFIX.BIN` (repro app, `src/user/netfix_test.c`) does 24 socket sends under console floods against a host echo server that echoes back the first bytes it received — **`NETFIX TX-OK` 24/24, byte-exact** (`run-f2-serial.log`; `run-pc1.pcap` TX frames clean). The exact WK-4 webproc-workload race was not re-triggered on this QEMU/TCG even under floods, so this lands by construction (removing the shared-memory hazard class) + the byte-exact repro as regression.
  - **Bug #2 — TCP RX prepends 6 stray 0x00 before the HTTP response:** root-caused with a QEMU `filter-dump` pcap + per-segment driver trace: the stream is CLEAN on the wire (one 163-byte response segment), but **every header-only segment (ACK/FIN/SYN-ACK) is padded by the producer to the 60-byte IEEE Ethernet minimum with zeroes**, and the stack fed the padded frame length into the TCP parse (`data_offset(20) < len(26)` → the 6 pad NULs were appended to the socket ring before the real payload — exactly the `NET resp-head hex: 00 00 00 00 00 00 48 54 …` evidence). **Fix:** `net_rx_packet` derives the L4 length from the IPv4 `total_len` field and drops a datagram whose total_len is smaller than the IPv4 header or larger than the delivered frame. On-device before/after: `NETFIX RX total=175` + `hex: 00…00 4E 45…` (8/8 FAIL) → `NETFIX RX total=163` + `hex: 4E 45 54 46…` (**24/24 RX-OK**, `run-m2` vs `run-f2`). Unit regression `test_net_ether_padding` (padded ACK contributes 0 bytes; data segment byte-exact; malformed total_len dropped) lands in the unit tier (unit-arm 297/0, unit-x64 300/0).
  - **Bug #3 — driver-serial console drops marker lines:** DOCUMENTED, not reproduced — the OS console path (`uart_putc` loop, per-char `uart_lock` irqsave, blocking TXFF wait) cannot drop characters by construction; under heavy serial load (console floods + 24 socket trials) the lane measured **720/720 flood lines + every TX/RX verdict present** (`run-f2-serial.log` inventory). The WK-3-era drops coincided with ~20× denser serial throughput (117 K lines / 300 s, run 53) plus host-side stdio capture, which is why the WK-3/WK-4 lanes made the guest-disk trace the authoritative channel. No kernel change; residual risk noted (per-char lock churn in `console_write_user`, and host serial capture loss under extreme density).
  - **Battery (committed tree):** host 482/0 (`TEST EXIT: 0`); unit-arm 297/0 (incl. new padding test); unit-x64 300/0 (KVM); ARM `make test` wave — run 1: `System halt from CPU 7.` with only the two KNOWN transient shell-suite FAIL tokens (`shell_test: FAILED clear validation`, `SHELLTEST WATCHDOG: protocol stalled 25 s` — the documented boot-wave read-lag/stall flake class from l6-glib, unrelated to the net paths); run 2 (same tree, quieter box): **0 FAIL tokens, 39 `ALL PASSED` summaries, `System halt.`** — green. cstyle clean; no merge to main (branch `browser/l8-netfix`).

- 2026-10-03 — **WK-3 GATE MET — ARM + x64, all six fixtures, cross-arch FPC parity (fork `browser/l8-wk3x64` @ `92ba80f85d`; lane report below).**
  - **x64 leg GREEN:** built WebProcess (177,022,512 B ELF / 99,720,440 B flat, fits the 128 MiB cap), NetworkProcess (170,605,600 B), HobbyOS-UIProcess (65,207,576 B) for `HobbyOS-intel` (own build dir; cross-built the WK-2 dep stack for x64, fixing three recipe/toolchain x64 link issues en route). Ran the headless driver on the x64 guest under KVM (`-smp 8`, OVMF, NVMe, Limine multiboot1): **7/7 GREEN on-device runs across all six fixtures; FPC checksums bit-identical to the ARM ladder** — FIX01 `0x759431c5`, FIX02/02D `0x5f62b9c5`, FIX03 `0xb1e75dc5`, FIX04 `0xbc622575`, FIX05 `0xa0bf6dc5` (run 30 full trace quoted in the lane report: BOOT/IPC/PAGE/JS 3-3/FPC/STABLE 306 s/ALL-DONE/`System halt from CPU 2.`). `aggregate_wk3_intel.py`: runs=7, green=7, GATE PASS. **`WK3-REPORT.json` regenerated + gate closeout appended to `WK3-EVIDENCE.md`; verdict: WK-3 GATE MET (ARM + x64).**
  - **Two real x64 OS blockers surfaced (fixed scratch-only, OS-owner carry-forwards):** (1) **x64 TSC/timer_get_ms calibration defect** — TSC rate 35,726 ticks/ms makes the guest clock ~18× fast, collapsing `LOSTWAKE_DEAD_OWNER_MS=2000` to ~110 real-ms so the idle lost-wake reaper DISPOSED the live WebProcess mid-load (pid1 teardown + Vector-14 + process_create hang); the gate needed a scratch-only x64 neuter of the lost-wake pass + syscall-entry heartbeat refresh. OS-owner: audit TSC calibration, re-enable the reaper. (2) **x64 NVMe loader throughput** — single-sector `fat16_read` ~50 IOPS under this QEMU KVM (~3 min real / 100 MB load; ARM uses fast virtio-blk; virtio-blk-pci init FAILS on x64). OS-owner: batch loader reads and/or fix x64 virtio-blk init. Also: x64 STABLE window is guest-clock based (300 guest-s ≈ 17 real-s — same TSC consequence; ARM ran true 301 s).
  - Lane hygiene note: the sibling branch switch to `browser/l8-wk5` split 2 lane commits — the lane cherry-picked them back (`028d4e57ca`, `399a5938ce`); controller may drop the stray copies from `browser/l8-wk5`.

- 2026-10-03 — **WK-5 UI shell DONE — a real browser window works on the HobbyOS desktop (fork `browser/l8-wk5` @ `6ddf3cbf` + `4dd1193a`; lane report below; verified by controller via QMP screenshot).**
  - **What works (on-device ARM QEMU, MODE=desktop, `-smp 1`):** `WK5WindowDriver.{h,cpp}` reuses the WK-3 in-process WebPage→Skia→320x240 pipeline but runs the WebProcess as a **desktop-spawned userland app** (Apps menu → WEBPROC.BIN), speaking the OS pixel-window protocol (`ESC ] X/T/P/F`, WM→app geometry/keys/mouse/close on fd 0). WebProcessMainHobbyOS routes `--wk5-driver` or any `argc<3` (desktop spawn) to it. **Controller-verified pixel readback** (QMP screenshot `WK5-4-01-window.png` — I loaded it myself): window titled **"HobbyOS-Browser - HOME"** on the desktop with the WebKit-rendered page inside (green `#probe` div = fixture content), taskbar Apps + browser shortcut + clock.
  - **Evidence (raw serial):** `[WIN] created pref=320x240` / `[WIN] init webprocess=ok` / `[WIN] geom x=2 y=34 w=1020 h=706` / `[WIN] load-ok url=HOME.HTM ms=25` / `[WIN] frame view=320x240 nonZero=307200 checksum=0x39878dc5` / `[WIN] painted rect=1020x706 at 2,34`; **input**: `[WIN] click x=509 y=617 → link → FIX02.HTM mass=1753`, `wheel btn=5 dir=down ×2`, `url-prompt → tall.htm + Enter`; **nav**: `f7 back → HOME.HTM (2830ms)`, `f8 forward → FIX02.HTM (3463ms)`, `f5 reload`; **close**: `ESC [ D → [WIN] close ok / exit rc=0 / exited within grace`, **zero kernel FATAL**. Screenshots WK5-{3,4}-0{1,2,3}-*.png + `run_wk5.sh`/`run_wk5_drive.py` (QMP) + fixtures HOME.HTM/TALL.HTM committed.
  - **Residual / risks:** QEMU must run `-smp 1` (desktop-spawning the 84 MB WebProcess under SMP hits the known LOSTWAKE/EL1 kernel wedge — OS-owner); parent-IPC socketpair logs a cosmetic EINVAL/didClose (no-op under OS(HOBBYOS)); URL entry loads FAT16-root fixtures (network is WK-4's domain — which now has Path A on-device HTTP in `browser/l8-wk4`). Lane committed explicit paths only; sibling WK-3/WK-4 uncommitted work left untouched.

- 2026-10-03 — **WK-3 text stack FIXED — real glyphs render on-device (fork `browser/l8-wk3text` @ `c75f30e86d`; lane report below).**
  - **Root cause (the deep FT-in-WebProcess fault, finally):** a **40-byte stack overflow in the fork's own `HobbyOS/lib/gaps.c` aarch64 `setjmp`** — it saved callee-saved GPRs x19–x30+sp **AND FP/SIMD d8–d15 = 168 B** into the sysroot's `jmp_buf[16]` (**128 B**). FreeType's `tt_face_build_cmaps` (ttcmap.c) keeps its validator `jmp_buf` (first member) on the stack and its matched `clazz` at `sp+0xb0` = jmp_buf+0x98..0xa7; setjmp's `stp d14,d15,[x0,#152]` zeroed `clazz` between the format match and `clazz->validate` → `ldr x8,[x8,#0x58]` with x8=0 → **VA=0x58** on the FIRST `FT_New_Memory_Face`, WebProcess-only. FT's cmap data was always correct; its stack was clobbered by the fork's own setjmp. The isolated probe linked the OS's GPR-only `setjmp.o` (104 B) and never overflowed. (Independently cross-validated by the x64 lane, which landed the same fix with an x86_64 carve-out.)
  - **Fix (`9253ed1a7f`):** `gaps.c` setjmp/longjmp save/restore ONLY callee-saved GPRs (104 B, offsets 0..0x60) — byte-for-byte the OS `kernel/arch/arm/setjmp.s` ABI, fits the 128 B jmp_buf, no FP context (matches port convention; jsc/WTF/FT all exercise the path fine). Verified in the linked binary (setjmp writes x0..0x67 only, 0 `stp d8` instructions). Harness: `run_wk3.sh` gains `WP_BUILD_DIR=<subdir>` + `USE_FONT=1` (drops empty `/USE-FONT`, flips `HobbyOSFontManager` on; default stays off → gate FPC unchanged).
  - **Pixel evidence (on-device, ARM SMP=1):** with `/USE-FONT`: FPC **`0x75249b56`** / fnv1a64 `0x9fa40cc629b9ecd8`, **165 dark glyph pixels** in the POK div (ascii map decodes literal P/O/K), `FT_New_Memory_Face rc=0` (was unreachable 0xFFFFFFFF in runs 75–83), both buffers FNV `0xe986ae9e73ba5c0b` intact. No-font baseline: FPC `0x759431c5` (bit-identical to runs 63–66/84/99), 0 dark px. Deterministic across two independently-built binaries (runs 111/113/115 → font-on hash; 112/114 → gate hash). Run 115 log clean (no `memory fault`), JS 3/3, STABLE 301 s, ALL-DONE.
  - Residual: full ladder (FIX02–05) not yet re-run with font-on (expected FPC shifts if the integrator flips the default); font stays `/USE-FONT`-opt-in pending that; x64 on-device text evidence is the x64 lane's to land. Optional OS hardening (not blocking): `kernel/arch/arm/setjmp.s` also skips d8–d15 (documented pre-existing ABI — no FP context across setjmp on this OS).

- 2026-10-03 — **WK-4 networking: Path A DONE — real on-device HTTP fetch + DNS (fork `browser/l8-wk4` @ `3e7502b257`; lane report below).**
  - **What works:** the headless `--wk3-driver` WebProcess opens a genuine in-guest TCP socket (OS F1 surface: `socket`/`connect_fd`/`read`/`write`), GETs `http://10.0.2.2:8000/FETCHED.HTM` from a workstation `python http.server` (fixture **never** on the guest disk), loads the served bytes through the SubstituteData pipeline, and renders with a **deterministic** FPC `0x95f309c5` / fnv1a64 `0xf6e053ec15dc6583` across runs 3/4/9/10 (+ PIXEL raw re-hash). Guest-persisted copy byte-identical to the served file. **DNS live from the guest**: `localhost → 127.0.0.1`, `example.com → 104.20.23.154 / 172.66.147.243` (rcode=0, UDP → 10.0.2.3). Raw markers in the commit's evidence (run-WK4-10.log: `WK4 NET-GET status="HTTP/1.0 200 OK" header=182B body=325B total=523B` → JS probe `document.title=WK4-SERVED` pass → FPC).
  - **Three OS kernel bugs surfaced (fork-safe workarounds in the driver; OS-lane carry-forwards):**
    1. **virtio-net TX shares a DMA source with the UART console** — a socket send while the console prints transmits the last console line instead of the payload (host server sees `WK4 NET connect=ok …` → HTTP/1.0 400). Driver workaround: divert console output into a guest-disk `/WK4DBG.TXT` trace. Kernel: net TX needs its own TX buffer/descriptor.
    2. **TCP RX prepends 6 stray 0x00 bytes** before the HTTP response on some boots (runs 2–4). Driver parses anchored at the first `HTTP/`. Kernel recv should not emit leading NULs.
    3. **Driver-serial console drops arbitrary marker lines** (known WK-3 PAGE/LOAD drops + WK4 socket/connect lines) — the byte-accurate evidence channel on this port is the guest-disk trace, not the serial.
  - **Path B (NetworkProcess + libcurl + second OS process) not landed** — precisely scoped: (1) target build libcurl 8.22.0 + mbedTLS 3.6.7 into the prefix + `find_package(CURL)` in OptionsHobbyOS.cmake (currently `SET_AND_EXPOSE_TO_BUILD(USE_CURL ON)` with a "re-enable with WK-4 find_package work" note); (2) include `Platform/Curl.cmake` + `WebCore/platform/Curl.cmake` (deliberately excluded today); (3) swap OpenSSL glue in CurlSSLVerifier/CurlContext for mbedTLS (PORT_PLAN D-7) or build curl TLS-less for http-only; (4) the real NetworkProcess bootstrap — spawn NetworkProcessMainHobbyOS as a peer, hand over the socketpair via SCM_RIGHTS, AuxiliaryProcess init handshake — the main fork surface, multi-day. Path A already exercises the same OS sockets + renderer; Path B is additive.

- 2026-10-03 — **WK-3 probe-robustness lane: FULL FIXTURE LADDER GREEN on ARM (runs 94–100, fork `browser/l8-wk1` @ `962c70999c`).** Both remaining crash classes root-caused and fixed driver-side, all six fixtures GREEN on-device with durable pixel evidence (PIXEL-WK3-{95..100}.raw, 307,200 B each):
  - **FIX01** gate: JS 3/3, FPC `0x759431c5` (bit-identical to pre-fix run 84) — run 99.
  - **FIX02 / FIX02D** (img): JS 4/4, FPC `0x5f62b9c5` — runs 96/95. **Root cause:** `runProbes()` called `executeScriptInWorldIgnoringException()`, which returns an EMPTY JSC::JSValue (all-zero bits) whenever the probe expression throws — the unguarded probe `(document.getElementById('img').querySelector('img').complete ? ...)` threw because the `<img>` resource never completes in the sandbox (no network process) — and runProbes then unconditionally converted that empty value to WTF::String, hitting the JSC fast path `ldrb w8,[x23,#5]` on a NULL JSValue (VA=0x5, PC 0x1000000F20). **Fix:** runProbes null-tolerance (empty value → clean probe FAIL `<exception>`, guarded before the toWTFString fast path) + fully null-guarded img probe (missing node → 'no-img'; `.complete` only read on verified-present node). Stable sandbox verdict documented as `img-pending` (expected updated; if a loader/decode path is ever added, flip expected back to img-complete — noted in source).
  - **FIX04** (form): JS 4/4, FPC `0xbc622575` — run 97. **Root cause (not a kernel gap — important for the OS lane):** ELR `0x100384C708` was a deliberate user-mode `brk #0x1` (EC=0x3C = BRK) inside libc++'s WEAK `operator new(size_t, const std::nothrow_t&)`. With exceptions disabled, libc++ hardens: since the OS libc/cxxrt strong-overrides throwing `new(size_t)` but NOT nothrow-new, libc++'s weak nothrow stub traps on the first nothrow allocation (FIX04's form probe path made that call). **Fix:** fork-side `NothrowNewShim.cpp` provides STRONG nothrow new/new[] overrides (tail-branch to the throwing new) — exactly what the hardening assertion demands. Residual: aligned-nothrow new not overridden but currently unreferenced; OS may optionally map EC=0x3C to a known-crash diagnostic path, but no kernel change was required for the fix.
  - **FIX03** (table): JS 5/5, FPC `0xb1e75dc5` (bit-identical to pre-fix run 88) — run 98. **FIX05** (DOM): JS 5/5, FPC `0xa0bf6dc5` (bit-identical to pre-fix run 90) — run 100.
  - Regression proof: FIX01/03/05 FPC **byte-identical** before vs after the probe fixes — the robustness change had zero pixel impact on already-green fixtures. Lantern: still-open — x64 WK-3 leg, the deeper FT-in-WebProcess fault (gated, § above), real image decode, and `aggregate_wk3.py` keying the LOAD marker on FIX01's URL (ladder runs read NOT-GREEN — verdicts must come from the WK3-EVIDENCE ladder tables).

- 2026-10-03 — **WK-3 fork lane closeout: gate GREEN deterministic + two font root-causes isolated (fork `browser/l8-wk1` @ `487145a41e`, + hand-off `890a5b5db1`).**
  - **Gate evidence (run 84, ARM SMP=1, committed tree):** `WK3 BOOT/IPC/PAGE ok`, JS probes **3/3** (`1+2→3`, `probe.textContent→POK`, `body-ok`), layout complete, FPC `checksum=0x759431c5` / `fnv1a64=0x9287e527c8ce4f83` (the historic deterministic hash — **byte-identical across runs 63–66 and 84**), STABLE 301 s, `HARNESS_EXIT=0`, and a durable **`PIXEL-WK3-84.raw` (307,200 B)** exported for host comparison — the WK-3 pixel-evidence artifact. Determinism preserved by gating the font backend behind a guest-disk `/USE-FONT` flag (default off → WK-2 empty FontMgr).
  - **Root cause #1 — driver's own bug, FIXED (`d04608f7a2`):** the new `HobbyOSFontManager::face()` built typefaces via `makeFromData()` whose virtual dispatch recursed into `onMakeFromData` → infinite recursion → 8 MiB stack exhausted (the deterministic runs 70–71 stack-top VA `0x17FF7FFFE0` = main-stack top − 0x20). Fixed by creating directly via `SkTypeface_FreeType::MakeFromStream`.
  - **Root cause #2 — deeper OS/WebProcess-environment FreeType fault, ISOLATED not fixed (`487145a41e`; OS-lane carry-forward):** the *first* `FT_New_Memory_Face` in the WebProcess deterministically faults `VA=0x58` (NULL cmap class in `tt_face_build_cmaps`, PC region `0x100375D…`) while the **identical** `libfreetype.a` + the same font bytes parse cleanly in an isolated probe — even under a webproc-mimic memory profile. Forensically proven intact in the *running* process via a kernel EL0 register+stack+user-memory dump (the scratch kernel's WK3DIAG): font bytes **byte-perfect** (FNV `0xe986ae9e73ba5c0b` exact on both the static-BSS and a fresh heap-copy buffer), `tt_cmap_classes[]`+records intact, stream base/length correct, setjmp/longjmp implementations identical. Fault signature: FT derives a wild cmap table pointer (buffer − 0x140) — i.e. a read of a garbage offset field inside FT's sfnt/cmap machinery that only fires inside the WebProcess's environment. Engineering decision taken: gate (fix safe), keep all font code committed and opt-in, hand the deeper fault to the OS lane with full documentation (commit body + `/FONTDBG.TXT` protocol + probe harnesses in `/tmp/wk3-lane/probe/`).
  - **Fixture selection hand-off (`890a5b5db1`):** the webproc kernel hardcodes `--wk3-driver` and cannot pass further argv, so the harness now drops `/FIXTURE.TXT` on the guest disk (via `WK3_FIXTURE=...` env → `mcopy`) and the driver picks the fixture from it; WebCore-layer `stderr` prints are unreliable on-device, so diagnostics ride the guest-disk blob (`/FONTDBG.TXT`).
  - **Fixture ladder:** FIX02/FIX02D/FIX03/FIX04/FIX05 = runs 85–90 (run 85 = lane's FIX02 trial: JS 3/3 + FPC `0x759431c5`); the cap-hit lane left the ladder running and it was killed with it — controller relaunched (`ladder2`), which finished clean, then controller reran the three failures (runs 91–93) to confirm determinism. **Ladder verdict (all on the same committed tree, evidence committed `89afd13d39`):** FIX01 GREEN-deterministic (5 runs), **FIX03 tables GREEN** (JS 5/5, FPC `0xb1e75dc5`, PIXEL-WK3-88.raw), **FIX05 DOM GREEN** (JS 5/5, FPC `0xa0bf6dc5`, PIXEL-WK3-90.raw); **FIX02/FIX02D CRASH** — reproducible null+5 read at PC `0x1000000F20` in `DriverState::runProbes()` (unguarded `.querySelector('img').complete` on an absent img node; image never loads without the network process); **FIX04 CRASH** — reproducible EC=0x3C unhandled at ELR `0x100384C708` (form probe path, distinct root cause). Both crash classes are single-root-cause driver-side probe robustness bugs, captured for a follow-up lane (null-guard the probes / make runProbes DOM-node tolerant); `aggregate_wk3.py` flags ladder runs NOT-GREEN only because it keys the LOAD marker on FIX01's URL — the per-fixture verdicts above are authoritative (and the aggregator's `rg 89afd13d39` WK3-EVIDENCE.md ladder table records them).

- 2026-10-03 — **Post-fat16 test-server battery (`post-fat16-20261003`, hobytest @ `cbe1909`): GREEN where gating, incl. FATBIG_T; unit-runner budget raised.** host PASSED (`ALL APPS SUITE TESTS PASSED`/`TEST EXIT: 0`); test-arm PASSED (`System halt from CPU 3.`, 0 FAIL tokens) with **`[FATBIG] ALL PASSED`** (2 MiB write/reopen/tail-read/checksum, line 16318); test-x64 unchanged-known-incomplete (late-IDLESTUCK all-idle stall, rc=124 @1800 s, furthest ~UNEXPAND_T.BIN). unit-arm/unit-x64: **genuinely green** (`Tests run: 296/299, failed 0` — the expected +2), but the runner's hardcoded `TIMEOUT=20` verdict poll reds them because the new >1 MiB fat16 unit tests take ~60–90 s wall (~65 s guest in `test_fat16_large_create_write`; measured via watchdog timestamps; reproduced on an idle box, not contention). **Controller fix (`a0b675a`):** `run_unit_tests.sh` + `run_unit_tests_intel.sh` default verdict budget 20 s → **300 s**, env-overridable via `UNIT_TESTS_TIMEOUT`. (The 30-s "deadlock rule" intent — a hang emits no PASS/FAIL marker and still times out at the budget — is preserved; the budget is now honest about the fat16 suite's duration.) Re-confirmation: both suites verified ungated on the VM with a `/tmp` runner copy (`TIMEOUT=300`, since removed; VM source untouched).
  - Note: `b74abfe`→`cbe1909` test-arm FAIL tokens = 0 (no NFS fails this run); host's single FAIL token = the pre-existing line-442 note, byte-identical to the prior green log.

- 2026-10-03 — **fat16 >1 MiB create/write defect ROOT-CAUSED + FIXED, merged (`3c781c5`; lane `browser/l7-fat16`).**
  - **Root cause (verified empirically, honest correction of the §P2.5 note):** the documented "file_size reads back 0 / `v2 loader: bad image size 0`" symptom does **not** reproduce at `f35afa2` (kernel create + 2 MiB write via 8.3 AND LFN paths, close, reopen — all read size+tail back correctly; that class was already closed by prior entry-durability/LFN-run-placements). The **live** defect is `fat16_write`'s **O(n²)** FAT-chain re-walk from `start_cluster` per chunk (+ `fat16_truncate_to` zero-filling in 64 B chunks): a 2 MiB kernel write walks ~2e6 FAT entries and blew the unit tier's 20 s budget (`Tests timed out!` inside `test_fat16_large_create_write`), and the wave program never finished its write after ~8 min under TCG — i.e. a 30 MiB blob was effectively unwritable from inside the OS, which is why P2.5 resorted to extrapolation.
  - **Fix (`0920b27`):** walk the chain once to the cursor's cluster, carry walk state forward through the call, advance to the next cluster (alloc+linking) only at true cluster-boundary crossings (4-sector/2 KiB clusters — the first O(n) attempt skipped sectors and was caught by the new tail-checksum test); `fat16_truncate_to` zero-fills in 4 KiB chunks.
  - **Regression tests (`86ba2f7`):** kernel `test_fat16_large_create_write` (8.3) + `test_fat16_large_lfn_create_write` (LFN); wave `FATBIG_T.BIN` (full syscall stack: open(O_CREAT|O_TRUNC) → write → close → reopen → lseek(SEEK_END) → tail read + checksum), wired via `TU21_TEST_RULE` + disk.img mcopy + `test_wave_loader` entry.
  - **Battery:** host 482/0 (`TEST EXIT: 0`); unit-arm **296/0**; unit-x64 **299/0** (KVM); ARM wave `System halt from CPU 4.` + `[FATBIG] ALL PASSED` + 18 summaries (only FAIL tokens = the 2 environmental NFS checks, proven identical on pristine main). cstyle clean. Merged `3c781c5` (auto-resolved, no conflicts with the OS-drafts work).
  - Residual (out of scope): `alloc_cluster()`'s linear free-cluster scan is the next cost for multi-MB writes on a populated disk; the 64 MiB volume has only ~22 MiB free so 30–60 MiB blobs still cannot fit — direct big-blob timing needs a larger disk image.

- 2026-10-03 — **Controller resume: OS draft patches 0003–0008 merged to main (`e348563`) + three lanes in flight.**
  - **OS-side drafts landed (`e348563`, fast-forward from `f35afa2`; lane `browser/l7-os-drafts`)** — six commits, the WK-3 carve-out set: 0003 webproc mode (`b067fac`), 0004 `USER_IMG_SIZE` 64→128 MiB (`2a4aba5a`), 0005 recvmsg `MSG_NOSIGNAL` parity (`4536ee26`), 0006 loader span-vs-heap guard (`b7a6dad`), 0007 recoverable EL1/user-space kernel faults (`95051535`), 0008 schedule()-claim heartbeat refresh (`e348563`). 0003/0004 applied verbatim from the fork's patches dir; 0005–0008 **reconstructed** (the previous session's copies were lost with the pruned scratch; each re-derived from this §11 record + the code). Lane battery: host `TEST EXIT: 0`; unit-arm 294/0; unit-x64 297/0 (KVM); ARM wave `System halt from CPU 5.` + 18 summaries + **0 kernel faults**; the only FAIL tokens = 2 NFS checks **proven byte-identical on the unpatched base `f35afa2`** (live host NFS export absent at 10.0.2.2 — environmental, not a regression). 0007's guard: only EL1 EC 0x21/0x25 aborts (ARM) / #PF vector-14 (x64) with faulting address ≥ USER_VA_BASE **and** a current process become process kills; kernel-address faults stay FATAL+spin. Post-merge VM battery (label `post-osdrafts-20261003`) run on hobytest.
  - **WK-3 fork lane (in flight, `browser/l8-wk1` @ `033002f2b8`)** — paint blocker **resolved**: on-device runs 46–64 show `FPC nonblank=1`; deterministic across runs 63–66 (checksum `0x759431c5`, fnv1a64 `0x9287e527c8ce4f83`, full 320×240 box, all JS probes pass, STABLE+ALL-DONE clean). Lane continuing on the text stack (new `HobbyOSFontManager.cpp` Skia font backend), images, fixture ladder.
  - **fat16 >1 MiB create-path lane (in flight, `browser/l7-fat16`)** — repro test `test_fat16_large_create_write` added; unit tier green 296/0 with the fix; 30 MB v2 loader load verified clean (`image mapped, bytes=30167040`); root-cause work continues.
  - Context: the previous session's `/tmp/wk3-os-scratch` + `~/.hermes/cache/scratch/{ostear,ovmf}` were pruned (24-h idle policy), taking the original 0005–0008 patch files with them — hence the reconstruction above. `MODE=bigload` repro scaffolding remains in-tree (inert unless selected).

- 2026-10-02 — **OS-side lane: 3 root causes, 3 draft patches (0006/0007/0008).**
  (a) EL1 "double-fault" on process teardown = the WK-3 fork's unclamped
  exit-diag stack walk read one VA past the 8 MiB main-stack region → EL1 data
  abort EC=0x25 (translation-level-1), unrecoverable in sync_handler_c → FATAL
  spin; EC=0 FATALs were `halt()` `hlt` raised as EC=0 without `-semihosting`.
  Proof: instruction-level disassembly of the fork's own ELF (ELR
  0xBA69AFEC). Patch 0007 = recoverable EL1 fault → clean process kill.
  (b) ARM owner-liveness heartbeat only refreshed on idle → an idle reaper
  DISPOSES a still-running single process after a >2 s user-mode stretch
  (caught mid-malloc-ladder on-device) — likely the run-22 IDLESTUCK wedge
  class. Patch 0008 = refresh `cpu_heartbeat_ms` on every claim in
  `schedule()`.
  (c) Loader image gate was only `fsize <= USER_IMG_SIZE` with no span-vs-heap
  check (matters once 0004 raises the cap to 128 MiB). Patch 0006 adds
  `USER_IMG_BASE + img_len > USER_HEAP_BASE_V2 → reject` in `v2_map_image`.
  Budget verdict: NO limit raise needed (in-OS ladder 1…256 MiB malloc +
  256 MiB mmap + 192 MiB brk all green on-device). All three patches
  `git apply --check` clean vs the OS repo; evidence + repros under
  ~/.hermes/cache/scratch/ostear/. OS repo untouched (drafts for controller
  apply after battery).

- 2026-10-02 — **WK-2 GATE MET — three ARM process binaries LINK.** All three
  targets link RC=0 (fork `browser/l8-wk1`, finalization lane deleg_de1d1eaf):
  WebProcess 139,108,664 B / NetworkProcess 135,088,920 B / HobbyOS-UIProcess
  58,311,528 B; jsc smoke rc=0; CI script `wk2-link-ci.sh` validated;
  `extern "C"` sysroot-header hygiene fixed fork-side (mangled C++ refs to
  shm_open/mkdir/setjmp were the last link cluster; gaps strong/weak variants
  were symptom treatment); Skia-vs-Cairo spike resolved in favour of Skia (all
  three binaries are Skia-backed — Cairo falls back only under AD-4, not
  indicated); W2-R5 receipts appended to the fork WK2-EVIDENCE.md. Note for
  WK-3: WebProcess ELF (139 MB) exceeds the 64 MiB user-image cap — step 0 is
  measuring the flattened raw size and either stripping or raising the cap
  (OS change, patch-for-controller-apply).

- 2026-10-02 — **WK-1 GATE COMPLETE ON BOTH ARCHES — ARM + x64 test262
  500/500.** The x64 leg (previously blocked by the OVMF freeze, itself a stale
  AArch64-ELF disk — see below) is now fully green: chunks 2–13 ran 40/40×11 +
  20/20 first-pass on KVM with the root-caused recipe (forced x86-64 relink,
  `-smp 8`); joined with the OVMF lane's `c1-x64.log` = **x64 500/500 PASS,
  0 FAIL, 0 SKIP (100%)**, categories all clean (arithmetic 110 / strings 76 /
  regex 34 / JSON 23 / Date 68 / core 189). Combined with ARM (500/500 at
  `e3bc45f`) the on-device test262 subset is green on BOTH arches. Evidence:
  ~/.hermes/cache/scratch/ovmf/test262-x64/ (raw serial `x64/chunk-{1..13}.log`
  with T262 DONE + ALL-DONE markers). No OS/kernel defect found. Script fixes
  landed in the fork (`c712f63d3d`).

- 2026-10-02 — **x64 OVMF freeze ROOT-CAUSED + FIXED** (parallel lane
  deleg_1a266585). The `BdsDxe: starting Boot0002` hang was NEVER a host/
  qemu/firmware regression: the x64 disks shipped an **AArch64 kernel ELF** —
  the OS Makefile uses ONE shared `hobbyos.elf` target across arches
  (`TARGET=hobbyos.elf`, `OBJ_DIR=obj/$(ARCH)`) and a leftover ARM `MODE=jsc`
  build (03:39) made `make ARCH=intel` a no-op, so the ARM ELF (Machine=AArch64,
  entry 0x40080000) was silently shipped as the x64 kernel; x86 Limine failed to
  load it and spun in a `hlt`/IF=0 loop. Secondary: `-smp 4` fails AP-bringup
  (TCG hang, KVM stack-dump) but `-smp 8` boots (matches the green unit-x64
  tier). Fix = force the intel relink (`rm -f hobbyos.elf` outside the repo +
  `make ARCH=intel`) + `-smp 8`. **Verified: jsc x64 smoke PASS (smp8 KVM/TCG/
  pkg OVMF) + test262 C1 x64 40/40 PASS/0 FAIL/0 SKIP `T262 ALL-DONE`** — first
  green x64 chunk since the freeze; chunks 2–13 should pass with the same script
  fixes. Two fork-script diffs await controller apply (scratch/ovmf/diffs/) once
  the WK-2 fork lane is clear; optional OS Makefile hardening (arch-keyed
  target) recommended. Evidence: ~/.hermes/cache/scratch/ovmf/.

- 2026-10-02 — **Wave 4 progress — WK-2: WebCore compile gate CLEAN, process
  layer in flight** (fork `browser/l8-wk1`). After CP-2 GO: WebKit2 ARM configure
  GREEN (fresh-cache recipe, all -D on CLI — `5692a0516b`); Skia + ALL of
  WebCore now compile against the bare-metal sysroot (fork shims in PORT_PLAN
  register rows 23–36; notable root causes fixed: `SK_BUILD_FOR_MAC`
  misdetection → `SK_BUILD_FOR_UNIX` pin, sysroot gaps semaphore/malloc/unistd/
  netinet + weak nextafterf/pread/pwrite fillers, a `/*`-in-a-comment breaking
  4+ TUs, framework-header platform set, NetworkStorageSession 2.54 cookie API,
  CryptoKey token containers, AX 2.54 drift). WK-1.5 written comparison BODY
  confirmed in the fork README (`a50fa78ebf`, WebKit2 per AD-3). CI script
  `HobbyOS/scripts/wk2-link-ci.sh` committed. Gate residual: Source/WebKit
  process layer ~18 TUs + the three-binary link (IPC unix ConnectionUnix,
  AuxiliaryProcessMain, WebMemoryPressureHandler, …) — continuation lane in
  flight. WK-2 remains OPEN until three binaries link + sizes/times recorded.

- 2026-10-02 — **CP-2 CHECKPOINT TAKEN — GO** (Wave 3 close; WK-1 gate met on
  ARM; WK-1.5 architecture decision made). Recorded in §7.2 + the WK-1 / WK-1.5
  items above: WebKit2 multi-process (AD-3 default) adopted; written comparison
  body to be recorded in the fork README by the WK-2 lane. Batteries at the
  merge tip `18a62f4` (watchdog fix + batch-3 record): LOCAL host 0-fail,
  unit-arm 294/0, unit-x64 297/0 (+watchdog regression), wave 0 FAIL + halt +
  18 summaries; VM `w7-*`: host ✓, unit-arm ✓, unit-x64 ✓, test-arm rc=0,
  0 FAIL + halt + 21 summaries + TORTURE 8/8
  (`w7-test-arm_20261002-135216_test-arm.log`) — **both machines green at the
  tip**. Next: Wave 4 (L2 tail already landed — P4/P6 present in-tree; L8
  WK-2 → WK-3 headless; CP-3 = does a page render?).

- 2026-10-02 — **Wave L8 batch-3 — WK-1 GATE MET (ARM): test262 500/500 on-device;
  x64 watchdog stack-scan fault fixed** (deleg_8b6b8883; main tip `18a62f4`).
  - **test262 language-subset — ARM 500/500 PASS, crash-free, on-device** (fork
    lane, `browser/l8-wk1` commits `3bad9d7d39`/`6b5ef6fd9f`/`e4eb6748c6`):
    deterministic 500-file subset of official test262 (pin
    tc39/test262@7a096c205fd422ecba49a407d5ac4d1b3f842296; source-pinned map +
    selection at fork `HobbyOS/continuation/test262/`), covers arithmetic 110 /
    strings 76 / regex 34 / JSON 23 / Date 68 / language core 189; 23
    negative-syntax, 8 onlyStrict. Run through the OS MODE=jsc single-program
    path in 13 chunk boots; 500/500 PASS, 0 FAIL, 0 SKIP; per-file verdict
    stream + `T262 CHUNK DONE` markers; crash-free end to end. Harness lessons
    recorded (assert.sameValue NaN semantics; jsc global-var DontDelete — each
    file isolated in its own function scope; parse-negatives via `load()`).
    x64 leg NOT RUN: the fork's x64 OVMF boot froze at `BdsDxe` under TCG/KVM
    this session (the M4-R1-proven smoke command also hangs — host/env drift,
    raw log `test262/intel/chunk-C1.log`); x64 remains env-blocked.
  - **x64 KVM v2-load "page fault" (Vector 14 @ 0x7002E0E0) — ROOT-CAUSED +
    FIXED, NOT a loader/MMU bug** (lane `l8-x64loadfix`, `32f999c`/`18a62f4`):
    the x64 watchdog's caller-chain dump scanned each core's kernel stack from
    `top = cpu_locals[c].kernel_stack` with NO validity guard — a core that
    never powered on (`-smp 4`, cores 4–7) leaves `kernel_stack == 0`, so the
    scan dereferenced (0 − 8) → Vector 14 CR2 0xFFFFFFFFFFFFFFF8 inside the
    watchdog. The 30 MB v2 load only mattered as the thing keeping the console
    silent >20 s so the watchdog fired mid-load; TCG loads finish before the
    threshold (no MMU difference). Reproduced 1:1 on KVM with a synthetic
    30 MB BIG.BIN (MODE=bigload; same FATAL shape as the M4 log). Fix:
    `watchdog_stack_scan_ok()` (trap.h) rejects tops outside
    [0x70000000, 0x75000000); both unguarded scans skip such cores; regression
    unit test x86_64-scoped. After-fix KVM: 30 MB load completes + halt, 828
    "uninitialized kstack, skip" guards, lane gate OK (units both arches incl.
    KVM 297/0, ARM wave 0 FAIL), cstyle clean. Repro scaffolding (MODE=bigload)
    committed, inert unless selected. Garage note: a kernel halt-path EL1 fault
    fires loudly after every clean exit in MODE=jsc (verdicts unaffected) —
    OS team carry-forward item.
  - **Open at this tip**: x64 test262 leg env-blocked (OVMF boot freeze — host
    level; was green at M4-R1); the fat16 >1 MiB `attr=0` defect; the WK-1.5
    architecture decision + CP-2 go/no-go (both imminent; see §7.2).

- 2026-10-02 — **Wave L8 batch-2 — CLASS B RESOLVED + wave pressure hardened +
  WK-1 `jsc` ON-DEVICE; batteries GREEN both machines at `5ae0362`** —
  three lanes merged (`0668f54` = `browser/l8-idlefix` + `browser/l8-wave-harden`,
  then `5ae0362` = WK-1 single-program boot mode).
  - **Class B IDLESTUCK lost-owner soak freeze — ROOT-CAUSED + FIXED** (lane
    `l8-idlefix`, commits `5eb9309`/`a10fdf9`/`f58e52d`, doc
    `docs/browser/l8-idlestuck-class-RESOLVED-l8-idlefix.md`): a TORTURE
    exec-child slot left RUNNING-but-claimed because its claiming CPU wedges
    permanently IRQ-off inside the claim→resume window (heartbeat frozen —
    c3 1554 ms vs ~4 ms others; `claims: c3=1`; identical signature in the
    historical freeze@337/186 logs). The old reaper only reclaimed RUNNING
    slots with **no** claiming CPU, so the stale claim pinned the slot
    forever and the soak froze with no halt. First fix candidate (re-READY
    the slot) **disproven empirically**: re-picking the wedged child
    cascades and wedges all 8 cores (the poison rides the child's AS).
    Landed recovery: the reaper detects a RUNNING slot whose every claimer
    heartbeat is ≥2000 ms stale **with the table otherwise drained**,
    releases the zombie claims, and `group_teardown`s the slot (code 97) —
    the parked WAIT_CHILD parent reaps a rejected exec and TORTURE reforks
    fresh; rounds continue, violations=0. Evidence: repro freeze@392 (197
    IDLESTUCK); live-recovery soak — wedge@~199, exactly one `[LOSTWAKE]
    disposing`, resumed to 800 rounds + halt; clean soak **807 rounds,
    IDLESTUCK=0, violations=0**; unit regression
    `test_lostwake_stale_claim_reclaim` both arches; LANE GATE OK. Residual
    risk (documented): each wedge permanently burns one core (a 28-min soak
    loses ~1); the clean-run receipt is luck-dependent (wedge fires ~2/3 of
    runs — the fix's value is the live recovery).
  - **Wave-pressure flake class — CLOSED at the tip by test-side hardening**
    (lane `l8-wave-harden`, 8 commits; the merged-tip wave that prompted it:
    **12 FAIL tokens** — shell×3, IPC_T×4, MMTEST, NFS×2; different suites
    per run, every run carrying exactly the 6 rate-limited `no free
    slot/process` forensics): extended the 4cddb2a skip-with-note pattern to
    SIGTEST/POLLTST/FPU_T/IPC_T/MMTEST/shell_*/SQLTEST fork/spawn-dependent
    checks (retry 3×2 s then skip-with-note on sustained pressure; precise
    stderr-pipe fork-failure discriminator; bounded reads + watchdog for
    starved-shell stalls), moved **WK1C_T to the END of the churn group**
    (launch-hungry-last rule), densified pressure classification across the
    whole check window, and fixed PROCTEST's WNOHANG probe to sample the
    table around the probe. TEMP SPAWNPR probes committed then cleanly
    reverted. Result: merged-tip wave = **0 FAIL, 18 summaries** (local) —
    the 12-token / hang tail-freeze samples (hooktails correlated to class B,
    now kernel-fixed) are gone.
  - **WK-1 `jsc` — ON-DEVICE (M3 ARM + M4 x64)** (fork lane, `browser/l8-wk1`
    head `9b1bfc83c9`): jsc relinked against the merged real libc sysroot
    (0 undefined; raw 26.9 MB ARM / 30.2 MB x64 flat, both < 64 MiB
    USER_IMG_SIZE); **ARM QEMU smoke 42/42 PASS crash-free + `System halt`**
    + **x64 TCG smoke 42/42** (raw serial logs + image-budget math in
    `HobbyOS/continuation/` + `WK1-M3-EVIDENCE.md` ledger). Remaining before
    the formal WK-1 gate: the **test262 language-subset run** (gate's second
    evidence item) and an OS defect found by M4 — an **x64 KVM page fault at
    RIP 0x7002E0E0 during v2 image load of a 30 MB image** (before any jsc
    code; TCG-green accepted for M4's own gate, kernel fault is a follow-up).
  - **OS mode landed** (`5ae0362`): `console_write_user()` (length-bounded,
    full-range `vm_range_ok`, byte-wise `uart_putc`), `sys_write_console`
    honoring the length on both arches, **fd-1/2→console fallback** for
    boot-wave programs with no console fd (jsc's stdout; `print_console`
    prefix dropped — verified benign across the wave), and
    `KERNEL_MODE_JSC`/`MODE=jsc`/`jsc_run`. cstyle clean; MODE=jsc kernel
    build smoke green.
  - **Batteries at `5ae0362`** — LOCAL: host 0-fail; unit-arm **294/0**;
    unit-x64 **296/0** (both +2 = the lostwake regression); wave **0 FAIL +
    `System halt` + 18 summaries**. VM (`w6-*`): host ✓ rc=0;
    unit-arm ✓ 294; unit-x64 ✓ 296; test-arm rc=0, **0 FAIL tokens, halt,
    21 summaries, TORTURE 8/8** (`w6-test-arm_20261002-120055_test-arm.log`).
    Both machines green at the merged tip.
  - **Open at this tip (carry-forward)**: test262 subset harness + x64 KVM
    v2-load fault (above); the fat16 >1 MiB `attr=0` create-path defect
    (pre-existing; blocks direct blob-timing); the WK-1.5 architecture
    decision, which CP-2 (at the WK-1 gate) makes.

- 2026-10-01 — **x64 full-wave re-characterization at the Wave 1f tail tip (`b122e0c`) —
  6-copy KVM battery: one class (all-idle stall), no resets, deeper reach than baseline;
  `unit-x64` green** — lane `browser/l3-x64char`; template `make ARCH=intel MODE=test
  hobbyos.elf disk.img` (no run), then 6 fresh copies seeded from it (rsync source +
  `cp -a obj/ disk.img hobbyos.elf`; per-copy OVMF pflash and vendor-marker mtimes
  normalized so each copy boots QEMU-only), each run bounded `timeout 900 make
  ARCH=intel MODE=test run QEMU_ARGS='-enable-kvm -display none'`; leftovers killed via
  `fuser -k <copy>/disk.img`. Logs `/tmp/x64char_20261001-1214/wave{1..6}.log` (+ `.meta`,
  `analysis.json`, `build_template.log`).
  - **Outcome class — 6/6 identical**: 1 boot per run (**0 silent resets / 0 guest
    re-entries**), **0/6 reached `System halt.`**, **0 LOCKFOREVER / 0 WATCHDOG / 0
    LOSTWAKE**; **[IDLESTUCK] storm 36.6k–37.3k dumps per run** (all cores in the idle
    loop, rounds ~450k, guest t≈740 s at the bound; the storm's table dump shows the wave's
    slots held at `st=4` EXITED, unreaped). This maps to the *late-IDLESTUCK / all-idle
    stall* family; the silent-reset class (wave4's 1167 boots) did not reproduce in this
    batch.
  - **Furthest reached: `SQLTEST.BIN`** (the loader's last `Loading program …` in all six;
    the five tail programs `EDITOR_T`/`APPS_T`/`PONG_T`/`FILEDIAL`/`DESKTOP` never load) —
    past `STRESS.BIN` (`[STRESS TEST] SUCCESS`), i.e. deeper than the F1-era "late
    IDLESTUCK at STRESS.BIN" note. En route per run: `[ERRNO] ALL TESTS PASSED` + 4×
    `ALL TESTS PASSED SUCCESSFULLY!`.
  - **FAIL tokens: exactly 1 plain per run — `MMTEST FAIL 11`** (the S4
    memfd+`MAP_SHARED`-across-fork step), consistent 6/6; 0 `FAILED`/`FAILURES`.
    **New-at-tip signal:** ~40 `in=HOLE -> killed` v2 fault kills per run (SUBPRB…SQLTEST);
    the two historical x64 wave logs (l2-fpu-x64 `wave1`/`wave4`, pre-S4/S5) carry 0 such
    events. Recorded as wave evidence only — the x64 full wave remains incomplete and
    `unit-x64` stays the hard gate.
  - **`unit-x64` at `b122e0c` (KVM, fresh disk)**: `run_unit_tests_intel.sh` rc=0 —
    `Tests run: 293 / Tests failed: 0`, `UNIT TESTS PASSED` (`unit_x64.log`).

- 2026-10-01 — **l2-p5-tail S3+S5 lane: GREEN (lane gate OK)** — `browser/l2-p5-tail`
  at `25d1fa6` (base `b122e0c` = post-wave-1f-merge fixups; 3 commits: `9e25062`,
  `a652227`, `25d1fa6`).  Closes the two deferrals the wave-1f record left open
  (S3 frame engine + S5 SPAWN_EX, route-B boundary):
  - **S3 (D8 frame engine, rows 83–84)**: frame construction on the leader's user
    stack (16-byte aligned below SP; full regs + FP/SIMD via the new
    `arch_fpu_save_to`/`arch_fpu_restore_from` per-arch helpers; minimal siginfo:
    signo + SIGCHLD CLD payload); restorer mechanism (libc
    `__ho_sigreturn_trampoline`, already landed with S1; kernel stores
    `sa_restorer`, libc auto-fills); SIGRETURN full restore + pending re-check +
    `-EINVAL` on a stale call; delivery at the trap-exit boundary (syscall return,
    incl. the yield path, and timer-preempt resume) and at the schedule() resume
    boundary (timer resume + parked-slice restarts: select/poll/sleep deliver,
    rewound pipe/futex/`WAIT_*` parks defer to their own trap exit); no-nesting
    via `sig_in_handler`; D8.8 stack-bounds/no-trampoline -> force default;
    SIGCHLD generation from the unified reap path (D5.4) incl. the D8.3 early
    wake of `BLOCKED`+`wake_ms>0` slice parks.  Deliverable set stays D6/D13
    (SIGUSR1 recorded-never-delivered — asserted by the test).
  - **S5 (D4 / row 86 SPAWN_EX)**: loader copies the caller's fd table via
    `fs_reopen` per slot, applies the fdmap pairs as dup2 (dst CLOEXEC cleared;
    whole map validated first -> `-EBADF` atomic), then the CLOEXEC sweep;
    envp blob (NULL = empty, D3.1), argv stored as given; all-default
    dispositions; spawn2 worker machinery reused in both arch dispatchers
    (`WAIT_SPAWN` + `spawn_retval` + retry/release-on-worker-failure); returns
    pid | `-errno` (ENOENT/ENOEXEC/EFAULT/EBADF/EAGAIN/ENOMEM); libc `spawn_ex()`
    wrapper.  A zero-size image entry (fat16_open creates on miss) now reports
    `-ENOENT`; the spawn2 wrapper keeps its historical `-1`.
  - **Evidence** (all at `25d1fa6` unless noted): host `ALL APPS SUITE TESTS
    PASSED` / `TEST EXIT: 0` (482 checks; `/tmp/l2p5_host.log`, `/tmp/lane_gate_l2-p5-tail_host.log`);
    unit-arm **291/0** (`/tmp/l2p5_unit_arm.log`); unit-x64 **293/0** KVM
    (`/tmp/l2p5_unit_x64.log`); ARM wave `timeout 1500 make test`
    `/tmp/l2p5_wave_arm2.log`: `System halt from CPU 3.`, **0 FAIL / 0 FAILED
    tokens**, suite-summary set = the wave-1f baseline + `[SIGTEST] ALL TESTS
    PASSED SUCCESSFULLY!` (62/62 checks — SIG_T.BIN: delivery/no-nesting/pending
    re-check, stale-sigreturn -EINVAL, SIGCHLD at the waitpid resume and inside a
    poll() slice park with the park restarting and completing, D13 SIGUSR1, and
    the spawn_ex fd copy/fdmap(dst 20)/CLOEXEC sweep/argv/envp + EBADF/ENOENT
    paths); first-wave run `/tmp/l2p5_wave_arm.log` documented the pre-fix reds
    (test-side fd-probe shadowing, fixed in `25d1fa6`).
  - Lane-gate: run on this worktree (host+unit tiers); cstyle clean on all
    touched files.  Fidelity note: the FP/SIMD save-restore is exercised
    functionally (handler FP math across SIGRETURN) but not register-window
    exact; `SIGUSR1` tenure remains D13's deferred item.
  - x64 wave attempted (KVM, bounded): reached the documented pre-existing
    late-IDLESTUCK signature right after STRESS.BIN/FPU_T completed (~60+
    suites in, zero exception/panic markers; `/tmp/l2p5_wave_x64.log`) —
    SIG_T.BIN not reached on x64.  Per the F1 convention the x64 evidence is
    unit-x64 green + no NEW wave signature; the x64 delivery paths were still
    exercised (fork-heavy suites raise SIGCHLD pending -> the DFL path runs
    at their trap exits/resumes with no incidents).

- 2026-10-01 — **P8 lane (`browser/l3-p8`): `TORTURE.BIN` "POSIX torture" suite
  green in the wave; bounded soak run + a reproducible kernel freeze documented
  for a follow-up lane.** Base `b122e0c`; commits `c130994` onward.
  - **P8.1 (suite + wiring)** — `src/user/torture_test.c` (+ host part
    `torture_test_host`, same TU): 64 thread create/join cycles × mutex/cond
    churn (**reframed as bounded bursts of 8 concurrent quick / 16 soak** — a
    64-wide grab held all 63 PCB slots and wedged wave smoke #1; sticky-
    broadcast turnstile; famine stop-fast after 2 pressured bursts);
    socketpair fd-pass loops (16/32 iters, in-process + fork round,
    `SCM_RIGHTS`); mmap/fault/free storms (demand-zero, `PROT_NONE` zap +
    re-arm, `MADV_DONTNEED`, fault-kill w/ SIGSEGV status); exec/wait cycles
    (8/12 × fork+exec `/TORTURE.BIN child`, exit codes verified); poll on many
    fds (24 fds incl. pipes/socketpair); memory high-water via `sysinfo`.
    Launch rejects are notes, never FAILs (smoke #2: 90 transient `no free
    process slots`, still green). Wire: `main.c` wave (after `SQLTEST.BIN`) +
    `disk.img` + Makefile (`MODE=soak`, `soak` target, `TORTURE_TEST_HOST`).
  - **Wave receipt (smoke #2, `MODE=test`)**: 8/8 TORTURE checks PASS; 0
    `FAIL`/`FAILED` tokens; `System halt` present; `counts: thr_created=64
    thr_rejected=0 fdpass_bad=0 exec_ok=8 exec_rejected=0 suite_ms=26489`.
  - **P8.2 (soak, bounded checks)**: `timeout 2100 make soak`, in-program cap
    28 min default (20-min cap via TEMP arg for the bounded checks). Best run:
    **482 rounds completed** (thr_created=30,848 = 482×64; exec_ok=5,784 =
    482×12; thr_rejected=0; fdpass_bad=0; exec_rejected=0; violations=0;
    min_free_kb=7,372,800 (7.03 GiB); high-water delta 0 KiB; 0 FAIL tokens)
    before the freeze below. No watchdog/stuck prints at any point.
  - **Freeze finding (reproduced 3/3 runs that reached ~480 rounds; kernel
    side, fresh P2 machinery)** — an exec-cycle fork freezes inside
    `vm_as_clone_into` (v2 AS clone). Last markers (clean + instrumented):
    `process_create` tail (`lock released. pid=1 block_idx=-1`) → `[VMF]
    enter/created` → `[VMC] start nr=3 res=641 tbl=12` → IMAGE region walks +
    maps 17 pages → `[VMC] region done i=0 pages=17` → **no further output;
    all cores spinning (host qemu ~460% CPU); unrecoverable ≥13 min.** Soak1
    froze mid-round 488 (children 5,848+), soak3 (clean build) mid-round 483
    (children 5,789+), gated-triage run (prints only from fork ≥5500) froze at
    round 487 — i.e. after ~31k thread cycles + ~5.8k execs of single-TORTURE
    soak. QEMU monitor showed all 8 CPUs parked at the same PC≈0x23a6ab860
    (top-of-RAM band; inconclusive read). Lock order is documented
    `proc_lock → vm_lock → frame_lock`; no inversion found by inspection in
    the healthy path. Freeze path is **merge-fresh** (v2 clone, region
    structures, AS pool — P2-S45 lineage `c1214c5`/`f0e8473`/`e017069`), not
    long-standing code; plausibly surfaced because P8 is its first sustained
    multi-thousand-cycle churn. **Follow-up lane start point: `vm_as_clone_into`
    region i=1 (main stack, after region i=0's 17 pages); dump region contents
    + frame/lock state on entry/exit; repro = `timeout 2100 make soak`
    (freeze expected ~round 480).** Evidence: `/tmp/p8_soak1.log`,
    `/tmp/p8_soak3_evidence.log`, `/tmp/p8_soak4.log` (gated triage capture),
    `/tmp/p8_soak2_evidence.log` (wave-overlap slot-famine variant); coredump
    `core.qemu-system-aar…2341011…zst` captures the frozen guest. TEMP
    diagnostics isolated in commits `a22dbda`/`0e3e166`, content reverted
    before the final commit.
  - **Wave-overlap variant (soak #2, triage build)**: full-table famine —
    `thread_create: no free slot (used=63/63 done=0)` ×2 suites +
    `process_create: no free process slots! zombies=3` ≈60k lines over ~12 min
    while wave spawn-heavy suites retried; TORTURE's churn slow-fail was a
    contributor → fixed (`eeb3692`: stop-fast under sustained pressure).
  - **Host part**: `torture_test_host` green 64/64 checks, ~180 ms (incl. the
    host-only mem-highwater monotonic check; device prints the 8 suite
    checks).

- 2026-10-01 — **Wave 3 — batteries: GREEN both machines at `cbbdd70`
  (+`447f02a`)** — three lanes merged: `070989e` (`browser/l3-x64char`, the x64
  re-characterization entry above), `2450663` (`browser/l2-p5-tail`: **P5
  S3/S5** — D8 signal-delivery frame engine (rows 83–84) + **SPAWN_EX** (row
  86); lane entry above), `cbbdd70` (`browser/l3-p8`: **P8** — `TORTURE.BIN` +
  `MODE=soak`; lane entry above); plus flake fix `447f02a` (below).
  - **Local battery at `cbbdd70`** (main worktree): host 0-fail (`TEST EXIT:
    0`); unit-arm **291/0**; unit-x64 **293/0** (KVM); ARM wave `System halt`,
    **0 FAIL / 0 FAILED tokens**, **17 suite summaries** = 15 baseline +
    `SIG_T` (62/62) + `TORTURE` (all-PASS; thr_created=64/thr_rejected=0/
    exec_ok=8; suite_ms=12731) — `/tmp/w1fc_*.log`, `/tmp/w3_battery_driver.log`.
  - **VM battery at `cbbdd70`** (`w3-*` tiers): host rc=0 (56 s), unit-arm
    rc=0 (148 s), unit-x64 rc=0 (140 s), test-arm rc=1 = **the POLLTST
    park-floor flake** (2 FAIL lines: the `timeout took >= 95ms …` check +
    `POLLTST FAILED: 1`; the elapsed value itself was console-spliced out of
    the log). Same class as the `db2e3f2` IPC_T fix, POLLTST edition — fixed
    in **`447f02a`** (bounds widened to `>= 85` / `< 5000` with the tick-
    quantization + caller-preemption rationale inline; discriminating
    properties kept, check name updated). **Re-verified both machines**:
    local wave at `447f02a` — 0 FAIL + halt + patched check PASS
    (`/tmp/w3b_wave_local.log`); VM `w3b-test-arm` **rc=0 — 0 FAIL tokens**,
    halt, `SIG_T` 62/0, `TORTURE` 8/8, patched check PASS
    (`w3b-test-arm_20261001-205407_test-arm.log`).
  - **P8.2 soak receipts**: 482 rounds, 0 violations, thr_created=30848,
    exec_ok=5784, fdpass_bad=0, exec_rejected=0, min_free_kb=7372800, mem
    high-water delta 0 — **then a reproducible kernel freeze at ~round
    483–488 inside `vm_as_clone_into`** (region i=1/MAIN-STACK; ~460% host
    CPU; 3/3 runs; no watchdog). The full 28-min clean pass is NOT yet
    achieved; handed to follow-up lane `browser/l2-clonefix` (in flight;
    findings `/tmp/p8_FINDINGS.md`, QEMU coredump kept). The wave-overlap
    slot-famine variant + TORTURE stop-fast (`eeb3692`) are in the P8 lane
    entry above.
  - §6: P5.1–P5.4 ticked (S1/S2 Waves 1e–1f; S4 via `e017069`; S3/S5 here);
    P8.1/P8.2 ticked with the soak caveat; x64 wave = the re-characterization
    entry above (6-copy KVM battery: all-idle stall 6/6, SQLTEST reach,
    MMTEST FAIL 11 6/6, unit-x64 293/0).

- 2026-10-01 — **Wave 3 follow-up — the P8 soak freeze: root-caused, fixed,
  merged** (`browser/l2-clonefix` → `30e0089`; reproduced 3/3 at ~round
  483–488 / fork seq ~5905). **Root cause (coredump-proven)**: the frame pool
  `[0x70000000,0x240000000)` never reserved the kernel image
  (`[phys(_start), phys(__stack_top)) = [0x23A680000, 0x23F2DE000)`); the
  forward allocation hint crosses into the image after ~1,877,632 frames of
  loader/fork churn, and `frame_alloc_zeroed` (reached from
  `vm_as_clone_into` → `vm_arch_map`'s L3 allocation) zeroed live kernel
  frames. The first image page holds `_start` + the exception vector table:
  once wiped, every exception (next timer tick onward, any CPU) jumped into
  zeroed memory — a silent, lock-free, unrecoverable spin (~460 % host CPU,
  zero LOCKDIAG — matches all observations). Coredump forensics: exactly
  nine set bits inside the image range in the frame bitmap = the child's last
  nine frames `0x23a680000..0x23a688000`; the frozen allocation
  `0x23a688000` is the page containing `frame_alloc_zeroed`'s own code (the
  CPU was zeroing the page it executed from). The clone's region i=1 is the
  1 GiB HEAP (region i=0/IMAGE had completed).
  **Fix**: `frame_reserve_range(lo, hi)` (`frame.c`/`frame.h`) reserves whole
  32 MiB blocks (preserving the legacy block layer's free/claimed partition),
  called from `frame_init` for `[phys(_start), phys(__stack_top) + 64 KiB)`
  on both arches; the boot line now reports `[FRAME] … reserved=24576`
  (96 MiB, 0.24 % of the pool). **Regression test**:
  `frame_test_image_reserved` drives the frontier from the pool bottom to the
  reserved region with 2048-frame contig runs (the exact soak churn pattern),
  asserting no allocation lands inside the image, the edge is reached, and
  the next allocation wraps ("a wall, not a hole"); green both arches.
  **Receipts**: pre-fix 3/3 freeze ≈ round 485; post-fix lane run **831
  rounds, violations=0**, clean halt; merged-tip local battery: host 0-fail,
  unit-arm **292/0**, unit-x64 **294/0** (each +1 = the new regression test),
  wave 0 FAIL + halt + 17 summaries + `TORTURE` 8/8; merged-tip soak **806
  rounds, violations=0** (thr_created=51584, exec_ok=9672, fdpass_bad=0,
  exec_rejected=0), `System halt from CPU 7.`; VM battery (merged tip, `w4`/`w4b`)
  fully green: host rc=0, unit-arm rc=0 (292/0), unit-x64 rc=0 (294/0,
  incl. the imgrsv regression), test-arm rc=0 (0 FAIL + halt, TORTURE
  8/8, SIGTEST 62/0). Lane gate: host /
  unit-arm / unit-x64 / wave all PASS pre-merge. TEMP triage stripped
  (`1028fb1` reverts `c85f971`; zero TEMP symbols).
  **Separate open issue (risk-noted)**: sustained process-slot starvation —
  soak attempts can still wedge at low rounds under the `no free process
  slots` flood (3/4 pre-fix attempts; merged-tip attempt #1 wedged at
  rounds=1, attempt #2 clean at 806). Fix direction: bound the loader's
  create-retry and/or reclaim dead-parent zombies.

- 2026-10-01 — **Wave 1f (part 1) — batteries: GREEN both machines at `a7b7330`** —
  first merged-tip attempt, no defects found beyond the pre-battery fixup below.
  **P2-S45 is deliberately NOT in this merge** (its S5 routing flip still shows
  run-to-run race variance; round-3 continuation active — it lands as this wave's
  tail with its own battery).
  - **Local `w1f` battery**: host all suites 0 failed + `TEST EXIT: 0` (single FAIL
    token = a corpus skip-note); unit-arm **286/0**; unit-x64 **288/0** (KVM); ARM
    wave `System halt from CPU 6.`, **0 FAIL / 0 FAILED tokens**, **13/13 suite
    summaries** = 10 baseline + the three new suites: `MATH_T` (60+ `MATH_*: PASS`
    incl. INF/NaN specials + its own summary), `RTTI_T` (16/16 distinct names),
    `IPC_T` (full suite); MMTEST 7/7 + `MMTEST PASS` (v2 loader live); DNSTST live
    (`example.com A = 104.20.23.154`).
  - **VM `w1f1-*`** at the same tip (runner v3, fresh dir per tier): host rc=0
    (`w1f1-host_20261001-110744_host.log`; `ALL APPS SUITE TESTS PASSED`,
    `TEST EXIT: 0`); unit-arm rc=0 (`w1f1-unit-arm_20261001-110849`; **286/0**);
    unit-x64 rc=0 (`w1f1-unit-x64_20261001-111002`; **288/0**); test-arm rc=0
    (`w1f1-test-arm_20261001-111108`; `System halt from CPU 4.`, **0 FAIL**, 13/13
    summaries, MATH_T/RTTI_T/IPC_T/MMTEST/DNSTST all present). Counts byte-match
    the local receipts.
  - **Pre-battery fixup (`a7b7330`)**: the P4 merge's conflict resolutions left 10
    line-joins (a lost trailing newline glued the following line onto the resolved
    line: Makefile ×4 — incl. the `disk.img` recipe having absorbed its `dd`/`mkfs`
    lines and the `host_tests` deps line its first recipe lines — arm/x64 `trap.c`
    ×2 each, `fs.c` ×2, `main.c` ×1; plus an fs.h duplicate-macro dedup).  Caught
    by a scan-before-batteries pass; all sites split back; cstyle 5/5; `make -n
    disk.img` / `make -n host_tests` parse checks pass.
  - **Method notes:** splice-tolerant counting needed again — `IPC_T`'s 70 checks
    reconcile as 58 strict regex matches + 12 names containing `: ` (the `[^:]+`
    class regex cannot match them) + **12 failure-only setup guards** verified in
    source (82 sites − 12 guards = 70; guard names are absent on green by design).
    `MATH_T`'s verdict rides a splice; its `ALL TESTS PASSED SUCCESSFULLY!` sits
    directly before its process exit.

- 2026-10-01 — **Wave 1f (part 1) landed: rtti + libm + P5 + P4 merged at
  `a7b7330`** (base `25d1c86`; merge order rtti → libm → p5 → p4; conflicts:
  Makefile (unions), arm/x64 `trap.c` (errno-carve-out unified to one branch;
  dispatch chain unions), `fs.c` (`file_fcntl` = the P4 implementation — both
  lanes' suites only pin valid-fd mask semantics; dup/dup2 CLOEXEC comment unions),
  `main.c` (wave order CXX_T → RTTI_T → IPC_T → MMTEST)).
  - **Wave-1f prep (same line)**: `a4d66db` — P4 §11 + P5 §8 integrator-review
    consent records (OQs approved; fd_cloexec unification; OQ5 EPIPE w/o signal),
    §A.1b amendment rows 83–86 (`SIGACTION`/`SIGRETURN`/`GETENV`/`SPAWN_EX`;
    v1 provisionals withdrawn); `25d1c86` — ABI freeze (rows 76–86, `SYS_MAX` 86,
    errno `EMSGSIZE 90`/`ENOTCONN 107`, canonical `fd_cloexec`).
  - **l3-rtti `089d49c`, `5d66c3e`**: 4 libc++abi RTTI holes closed
    (`__dynamic_cast` + class-info vtables; vendor class-list `-frtti` subset);
    ICU link probe: the 4 gone, none introduced; host `rtti_test` 17/17; wave
    `RTTI_T` 16/16 (up/down/cross/vbase + typeid).
  - **l3-libm `8de2c9e`, `66eb787`, `5743d33`**: 12 libm transcendentals
    (sin/cos/tan/asin/atan/atan2/log/pow/sqrt/modf/expf/tanhf — the probe's true
    hole list, "no 13th"); ICU link probe **16 undefined → 0** (4 RTTI + 12 libm,
    jointly with rtti); host parity vs glibc 1–3 ulp within budget; new
    `math_test_suite` (EL1, both arches) + `MATH_T.BIN` in the wave.
  - **l2-p5 `87795d5`, `018a5aa`, `9a0e27c`**: SIGACTION/SIGRETURN/GETENV rows
    83–85 + kill/SIGPIPE; unified reap delivery (closes the confirmed
    waitpid-wake gap; `vm_kwrite` cross-context status path); 3-arg execve +
    FD_CLOEXEC sweep + fcntl bits; `process_test_suite` added.  S3 (frame
    engine) + S5 (SPAWN_EX) remain deferred at the route-B boundary (Wave 1g).
  - **l2-p4 `435a0aa`…`2bf6dcc`** (13 commits incl. continuation): AF_UNIX usock
    pairs on the pipe park engine, poll/socketpair/sendmsg/recvmsg rows 76–79,
    SCM_RIGHTS cross-process fd passing, SOCK2TST flip.  Tail defects root-caused:
    LP64 cmsg header layout (`6b0309c`; fixed 11 of the last 12 failing checks),
    `sys_poll` demanding **8-byte alignment** of a natural-4 `struct pollfd` array
    (`2bf6dcc`; glibc accepts any alignment) — after which IPC_T is all-pass; the
    x64 panic in the lane was a 64 KiB test stack array vs 64 KiB kernel stacks
    (fixed by the lane; two consecutive clean gates).
  - **ICU 16→0 closure proven at the merged tip** (`w1f` re-run, both arches):
    the committed link probe reports `link OK (libicui18n.a libicuuc.a libicudata.a
    libcxx.a libc.a)` (arm + intel; build logs + `obj/<arch>/icu/MANIFEST.txt`),
    and the whole-archive upper bound (`nm --undefined-only` over the three ICU
    archives minus everything defined by {archives ∪ `libcxx.a` ∪ `libc.a}`) is
    **0 unresolved symbols on both arches** — was exactly 16.
  - **P2-S45 (carve-out, not merged)**: S4 committed on `browser/l2-p2-s45`; the
    S5 v2-routing flip runs red with variance (1–7 FAILs across identical
    kernels): TP=0 TLS/errno tiny-VA faults, `set_tls` garbage tiny args, torn
    save/resume register frames, fat16 sector-0 write corruption (prime suspect
    `alloc_entry_in_dir`).  Round-3 continuation active (WIP already committed;
    fix-forward).  Its record appends here when green; Wave 1f then closes
    formally and Wave 1g proceeds.

- 2026-10-01 — **Wave 1f tail: P2 S4/S5 — MERGED AND VERIFIED; Wave 1f formally
  CLOSED** — `browser/l2-p2-s45` (`b6b85c1`) merged as **`e017069`** (parents
  `9d691b7` + `b6b85c1`) plus three post-merge integration fixups found by the
  merged-tip batteries: **`88f680c`** (user-space `ftruncate` duplicate — S4
  `mman.c` vs P6.1 `src/user/libc.c`; kept the universally-linked one),
  **`058547d`** (ICU intel cross-build needs `-mcmodel=large`: the S5 flip's
  USER_IMG_BASE breaks its configure link tests (`unknown endianness`) and the
  archive link otherwise), **`1aaa3b2`** (**v2 loader env-blob inheritance**:
  the S5 flip routes all wave/spawn loads through `load_and_run_program_v2`,
  which predated P6.3 and copied only pid/cwd/fds — PROCTEST `spawn2 child
  inherits env blob` failed on both machines at the merged tip; fixed and
  wave-verified).
  - Reconciliation: 7 conflicted files / 20 hunks — P5 reap machinery unified
    (`reap_deliver_locked` kept; the branch's `proc_write_user_u32` retired —
    no remaining users); argv = marshal→blob→install with the branch's v2
    pointer validation folded in; `file_ftruncate` = FAT16+memfd union; the
    v2 exec path now applies env + signal-state + FD_CLOEXEC + argv like v1
    (integration gap fixed during the merge); duplicate-artifact sweep removed
    both-sides additions (`vm_arch_leaf_phys` ×2 arches, `ftruncate`
    defs/decls, dead S4 dispatch arms, the global `sys_ftruncate` wrapper).
  - Lane evidence: round-3 waves 10+11 consecutive green on the TEMP-free tree
    (78.3 s / 62.3 s; 19/19 suites; 0 FAIL); post-wave sector 0 `eb 3c 90`;
    boot delta −3.05 s (median 5.93 s vs 8.98 s baseline; budget ≤ +0.5 s);
    LANE GATE OK.
  - Merged-tip batteries at `1aaa3b2` (local + CI VM): LOCAL host 482/0
    (`TEST EXIT: 0`), unit-arm **291/0**, unit-x64 **293/0** (KVM), ARM wave
    **0 FAIL + `System halt` + 15 summaries** (`/tmp/w1fc_*.log`). VM: host
    rc=0 (`w1fcg`/`w1fch`), unit-arm rc=0 (both); unit-x64 rc=0 (`w1fch`;
    `w1fcg` missed it — the sync had skipped `git checkout -- third_party/`,
    leaving the pre-fix ICU script); test-arm rc=0 at `w1fcg` **and** green
    rerun `w1fch2` (0 FAIL tokens; `w1fch` itself took one documented
    slot-pressure transient — below).
  - Documented/known: **thread-slot transients in CI waves** — sustained
    63/63 exhaustion amid the SHTEST3 fork-burst outlasts the shim's ~2 s
    EAGAIN retry, failing launch-hungry checks (observed: CXX_T
    `condvar_wait_notify` at `w1gf`; THRD_T `futex-wake` at `w1fch` with
    `no free slot (used=63/63 done=0)` forensics; ~2/3 recent VM waves, 0
    local); kernel rejects correctly — recommended follow-up: make those two
    checks tolerate a failed launch. Also: the fat16 >1 MiB create-path
    `attr=0` defect (pre-existing, out of scope) blocks direct blob-timing
    (extrapolation 12–17 MB/s recorded, P2.5).
  - **Wave 1f formally closed** — part-1 (`a7b7330`…`1b517f7`) + tail
    (`e017069` + fixups `88f680c`/`058547d`/`1aaa3b2`); §6 P2.1–P2.6 ticked;
    Wave 1g carries on (P5 S3/S5 + P8 next).


- 2026-09-30 — **Wave 1e — batteries: GREEN both machines at `9a939e5`** —
  first merged-tip attempt, no defects found (contrast Wave 1d's RED #1).
  - **Local `w1g`**: host 482/0 + `TEST EXIT: 0` (wide parity PASSED); unit-arm
    66/0; unit-x64 68/0 (KVM); ARM wave `System halt.`, **0 FAIL tokens**,
    **10/10 suite summaries** — MMTEST full acceptance (7 checks + `MMTEST
    PASS`) with the **v2 loader live** (`entry=0x100000000 sp=0x180000000
    resident=2 tables=3`), CXX_T 20/20 distinct names, THRD_T 20/20 + TLS_T
    6/6 (per-name check), DNSTST live (`example.com A = 104.20.23.154`); 3
    `no free slot` pressure events absorbed by the P1 fix.
  - **VM `w1g-*`** at the same tip: host rc=0 wall=50 s
    (`w1g-host_20261001-001101_host.log`; wide parity PASSED, `TEST EXIT: 0`);
    unit-arm rc=0 wall=66 s (66/0; `w1g-unit-arm_20261001-001156`); unit-x64
    rc=0 wall=57 s (68/0 KVM; `w1g-unit-x64_20261001-001307`); test-arm rc=0
    wall=103 s
    (`w1g-test-arm_20261001-001410`; `System halt from CPU 5.`, **0 FAIL**,
    10/10 suites, MMTEST `PASS`, v2 loader live, DNSTST live; 1 absorbed
    pressure event).
  - **Method note:** the strict `name : PASS` adjacency regex **undercounts**
    under `[CONSOLE]` multiplexer splices — THRD_T read 18/20 and TLS_T 5/6 by
    regex while per-name substring search found all names in both logs; count
    by per-name substring against the known list and cross-check the suite's
    `ALL TESTS PASSED SUCCESSFULLY` line (run-tests skill updated).
  - Wave 1e is closed with this entry: all four lanes merged (`9a939e5`) and
    the merged tip carries verified green batteries on both machines.

- 2026-09-30 — **Wave 1e landed: P2 S1–S3 (VM v2) + P4/P5 design notes + ICU
  target build — merged at `9a939e5`** (base `07b6c00`; p4 fast-forward,
  p5/icu/p2 merges clean; 31 files, +5702/−270; cstyle 20/20 on changed src;
  no conflicts).
  - **L2 / P2 implementation** — `c1214c5` S1 (4 KiB frame allocator bitmap +
    block layer on the bitmap), `f0e8473` S2 (address-space v2: per-AS roots,
    4 KiB leaves, loader v2 IMAGE-only commit, `MMTEST.BIN`), `6cbd49e` S3
    (demand-zero faults; HOLE/PROT/OOM classification kills only the faulting
    process with a signal-shaped waitpid status (SIGSEGV=11); mmap-family v2 —
    row 62 6-arg, row 80 MEMFD_CREATE number frozen, `SYS_MAX` 75→82; brk
    range-switched; kernel copyin/copyout paths demand-materialize via
    `vm_touch`; v1 fallbacks explicit — v1 mprotect returns 0, v1 madvise
    no-op, fd-backed mmap `-ENOTSUP`).  Stage gates: host 482/0; unit-arm
    66/0 (7 vm tests); unit-x64 68/0 (KVM); ARM wave `MMTEST PASS` + v2-loader
    boot (`entry=0x100000000 sp=0x180000000 resident=2 tables=3`), 0 FAIL
    tokens, suite-count histogram byte-identical to S2; `lane-gate` OK;
    cstyle 11/11.  Deliberately deferred by design §8.4/§8.5: memfd +
    MAP_SHARED + fb slot + pthread-guard stacks (S4); crt0/TLS rework,
    fatal-fault exerciser, boot-delta recording (S5).
  - **L2 / P4 design** — `d454566`: `docs/browser/p4-ipc-design.md` (562
    lines; D1–D13, OQ1–OQ5 pending integrator review) — usock pairs on the
    pipe park engine, SCM_RIGHTS transfer machine, poll-only (epoll decided
    out on pinned-consumer evidence), rows 76–79 + errno EMSGSIZE/ENOTCONN.
  - **L6 / ICU target** — `0e9f19b`…`84d7832` (7 commits): `build-target.sh`
    + committed `config/mh-unknown` overlay + JSC-class link probe +
    cross-notes; both arches green, cold-reproducible; whole-archive closure
    = exactly 16 holes (§2).
  - **L2 / P5 design** — `b2bf160`: `docs/browser/p5-exec-signals-design.md`
    (653 lines; D1–D13, OQ1–OQ8 pending integrator review) — in-place execve
    + `SYS_SPAWN_EX`, unified reap delivery (closes the confirmed
    waitpid-wake gap), main-thread SIGCHLD frame engine, rows 83–86 supersede
    the §A.1b provisionals.
  - Merged-tip batteries (local + VM) launched at `9a939e5`: **GREEN** — see
    the verdict entry above.

- 2026-09-30 — **Wave 1d — batteries: GREEN both machines** at `9104be5`
  (first merged-tip attempt at `3b83823` was RED; the failures were three real
  integration defects, all fixed and re-verified).
  - **Local `w1f`**: host 482/0 + `TEST EXIT: 0` (scanf/wprintf + num + **wide**
    parity PASSED); unit-arm PASSED; unit-x64 PASSED (KVM); wave `System halt`,
    **0 FAIL tokens**, all 10 suite markers, CXX_T full suite (thread-region
    check names all present; DNSTST live).
  - **VM `w1f-*`** at the same tip: host rc=0 53 s (wide parity PASSED, 6 clean
    suites); unit-arm rc=0 65 s; unit-x64 rc=0 58 s (KVM); test-arm rc=0 110 s —
    `System halt`, **0 FAIL**, all 10 suite markers, CXX_T full.
  - **Battery #1 (RED at `3b83823`) → three defects:**
    1. *Pristine-tree bootstrap* (`944fbda`): the libc++ builtins rules required
       the fetched vendor tree as make prerequisites — on trees without the
       extraction (the VM; fresh clones) make rejected the implicit rule
       ("No rule to make target 'obj/<arch>/builtins/<x>.o'").  Fix: guard-recipe
       source pattern (a recipe-less pattern is rejected during implicit-rule
       search), `.fetched-ok` invalidation before re-fetching (fetch.sh
       early-exits on marker+anchors, so one missing file would never return),
       trailing `test -f`, `.SECONDARY` against intermediate deletion; `cxx_t.o`
       gated on the marker instead of an extracted path.  Pristine-tree builds
       verified from genuinely empty states, both arches.
    2. *Wide-parity test flake* (`1a5277d`): `wcstold` byte-compared 16-byte
       long doubles incl. 6 padding bytes of stack garbage (flaky run-to-run;
       red on the VM, green locally, same clang/glibc); now `ld_equal` by
       class+value.  `newlocale("")` resolved through the host environment on
       glibc — the test now pins `LC_ALL=C` (its stated intent).
    3. *CXX_T silent truncation* (`9104be5`) — the deepest one: the wave can
       momentarily hold **every** PCB slot (forensic: `used=63/63 done=0`);
       `SYS_THREAD_CREATE` then returns `-EAGAIN`, and libc++ `std::thread`
       (`-fno-exceptions`) **aborts the process** instead of retrying like C
       callers — CXX_T died mid-suite in ~half of the runs on BOTH machines
       with **zero FAIL tokens** (only the distinct-check-name count exposed it:
       9/20 vs 20/20, all-suite `ALL TESTS PASSED SUCCESSFULLY` count 9 vs 10).
       Diagnosed with marker-instrumented copies + a kernel EAGAIN print in a
       disposable workdir (deaths localized to the first `std::thread`
       creations; 6/13 pre-fix runs affected).  Fix: bounded ≈2 s EAGAIN retry
       inside `pthread_create` (yields first, then 10 ms sleeps; sustained
       exhaustion still surfaces EAGAIN); the kernel no-slot path gains a
       rate-limited `[KERNEL] thread_create: no free slot (used=N/63 done=M)
       for pid=P` forensic.  Verified: 12 consecutive wave runs (8 local, 4 VM)
       all complete, and both `w1f` battery waves absorbed live pressure events
       and still finished with every suite.  P1 §6 amended.
  - Wave 1d is closed with this entry: all four lanes merged (`c3be6c6`,
    `f1cf1dc`, `0985f28`, libc++ ladder via `3b83823`) and the merged tip
    carries a verified green battery on both machines.

- 2026-09-30 — **Wave 1d landed: P2 design note + P3 libc++ + ICU + GLib —
  merged at `3b83823`** (base `77b872e`; icu fast-forward; glib and libcxx
  merges each resolved as a `.gitignore` union).
  - **l2-p2-design `0089560`** (docs; 514 lines): `p2-vm-design.md` —
    decision-complete (D1–D12, OQ1–OQ8); integrator review §12 appended
    (`c3c693e`): rows 80–82 freeze at the P2 gate; 6-arg `SYS_MMAP` (row 62)
    approved; COW defer + PROT_NONE zap+free divergence accepted; shootdown
    IPI 0x82 approved; L3 diffs by the lane; minimal memfd seals; sysinfo(2)
    append approved. Findings en route: **EFER.NXE is not enabled today**
    (needed for PROT_NONE/NX — enable in P2); x64 r9 already in the trap
    frame (no asm change for 6-arg mmap); ARM kernel runs relocated
    (0x23A680000) — user-VA separation becomes structural at 64 GiB.
  - **l3-libcxx** (tip `66100a8`, 7 commits `0cb92f3`..`66100a8`; 47 files,
    +7434/−29): libc++/libc++abi 21.1.8 static both arches; `CXX_T.BIN`
    acceptance (20 checks) in the ARM wave; libc gap-fills (~4 k lines:
    wchar/wctype/strftime/xlocale/strtod/math/stdio + tf2 builtins) with
    host parity tests A/B/C (85/3314/1511 checks, 0 failures) wired into the
    host gate; intel hermetic include path; host-only fixes (string.c errno,
    locale.h `include_next`). Lane gate `LANE GATE OK` (host `TEST EXIT: 0`,
    unit-arm 53/0, unit-x64 55/0 KVM, wave 0 FAIL + `System halt`). The five
    predecessor wave FAILs were root-caused: three CXX_T test-expectation
    bugs (fixed in `cxx_t.cpp`), the `CXX_T FAILURES` summary line, and one
    **transient `shell_test3` read-lag under boot-wave memory pressure**
    (unreproduced in two clean runs — watch item, noted as a risk).
  - **l6-icu `c3be6c6`** + **l6-glib `479b509`**: host-first vendor+build
    records — see §2 W1d; smokes re-verified first-hand by the integrator.
  - Merged-tip batteries (local + VM `w1d-*`) against `3b83823`; verdicts
    in the next entry.

- 2026-09-30 — **Wave 1c batteries GREEN on `e5188de` (both machines); Gate P1 stands closed.**
  Local (workstation): host `TEST EXIT: 0` (482 checks / 0 failed; CXX suites
  included), unit-arm 53/53, unit-x64 55/55 (KVM), ARM wave rc=0 — 0 FAIL,
  `System halt`, THRD_T (20 checks, all PASS) + TLS_T 6/6, 9× "ALL TESTS
  PASSED SUCCESSFULLY!", CXXSMOKE ran in-wave. VM (`w1c-*` tiers): host 57 s
  rc=0 (0 failed), unit-arm 51 s 53/53, unit-x64 45 s 55/55 (KVM), test-arm
  94 s rc=0 — 0 FAIL, `System halt.`, 9× verdicts, THRD_T (all checks PASS) +
  TLS_T 6/6 in-wave, TACTEST PASS, DNSTST live (`10.0.2.3` → `example.com A =
  172.66.147.243`), CXXSMOKE ran. THRD_T covers: create/join/retval, distinct
  stacks, mutex counter + recursive, cond turnstile, barrier rounds, rwlock
  exclusion, once, keys + dtor-at-exit, detach, thread exit retval, concurrent
  malloc, fresh-thread FP state, slot recycle, futex timeout/eagain/wake/bounds,
  exit-group-from-any-thread.

- 2026-09-30 — **Wave 1c landed: P1 (threads/futex/pthreads) + Skia spike + WK-0 scaffold +
  refs-vendor — merged at `e5188de`** (base `00fce5d`; all three HobbyOS-repo merges clean,
  Skia a fast-forward).
  - **l2-p1 `e5188de`** (7 commits `2404db2`..`1b70868`; 24 files, +3293/−135): the full P1 —
    PCB-slot threads sharing the leader address space (process.c +666); process_group()
    routing through fs.c/vfs.c/fat16.c/program_loader.c; THREAD_DONE reclamation; exit_group
    from any thread; `tls_base` on save/restore_context; futex-lite (WAIT/WAKE + timeout
    machine, proc_lock-only); syscalls 72–75 on both arches + libc wrappers; libpthread
    (musl TCB) + crt0/`linker.ld` TLS; THRD_T.BIN / TLS_T.BIN wired into the wave; kernel
    unit additions; glibc host variant. Three device-only bugs found by the wave and fixed
    pre-gate: absolute-linker-symbol TLS read (data abort at crt0 for every program);
    main-thread TLS storage mutating the `.tdata` template (thread-isolation failure);
    and a **latent pre-existing fat16 LFN bug** exposed by the +2 disk entries
    (`alloc_lfn_run` placing runs past the 0x00 terminator → invisible files; intel unit
    tier red) — fixed. Lane gate: host 482/0, unit-arm 53/0, unit-x64 55/0 KVM, ARM wave
    0 FAIL with THRD_T 20/20 + TLS_T 6/6. Boot delta (Gate F1 method): ARM 8.89 → 8.98 s
    (+0.09 s), x64 ~0 (1.309 s both). cstyle clean (24 files). §A.1b rows 72–75 + the
    consented +2 renumber recorded by the lane in `9bf91a0`.
  - **l6-skia `c0c6a3d`** (Skia host spike): pin + recipe + host build + deterministic
    smoke — see the §2 W1c vendor record; GN/depot_tools path probed to a documented
    boundary (memo); the two-build-rule reuse recipe for the target leg is in the lane's
    note (`docs/browser/l6-skia-host-spike.md`).
  - **WK-0 `d055b25768`** (in `~/webkit-hobbyos`, branch `hobbyos/wk0-scaffold`; 4 commits,
    19 files, 1220 insertions, zero upstream edits): PORT_STATE (shallow single-commit
    clone, no private remote yet), PORT_PLAN (every seam + upstream-edit register rows
    1–7 + dependency ledger), CMake bootstrap (OptionsHobbyOS + PlatformHobbyOS glue +
    WTF stubs), WK-1 feasibility memo (H-1..H-12, OQ-1..OQ-11). Nothing configure-tested
    by design; §6 WK-0 ticked with the null-build caveat.
  - Merged-tip batteries (local + VM `w1c-*`): **GREEN** — see the next entry.

- 2026-09-30 — **F1 carried item #3 CLOSED: the diffutils cmp ref-build story is
  vendored in-repo.** `third_party/diffutils-2.8.1.tar.gz` (780,086 B; sha256
  `c5001748…d23f5a` — matches the `docs/gnu-ports.md` §6 pin) + `.sha256` + AD-11
  README committed; the extraction stays gitignored (build area
  `obj/third_party/refs/`, already covered by the `obj/` ignore). `src/host/build_diffutils_cmp_ref.sh`
  resolves sources (a) legacy `third_party/_staging/diffutils-2.8.1/` when present,
  else (b) the sha256-verified vendored tarball extracted once into
  `obj/third_party/refs/diffutils-2.8.1/`; every failure path names the fix. Ref build
  inputs unchanged: CFLAGS/FILES untouched, the extraction is `diff -r`-identical to
  `_staging`, and the pre-change script vs the new one on the same legacy path build a
  byte-identical ref (`9ab2a624…ce42`); tarball-path vs legacy-path refs are behaviorally
  identical (ref-vs-ref 55/55 byte-exact — binary deltas are only the embedded
  `xstrtol.c` assert() path + derived build-id, a checkout-location artifact predating
  this change). Proof: pristine worktree (no `_staging`) `make host_tests` — ref built
  from the vendored tarball, 482 checks / 0 failed, `ALL APPS SUITE TESTS PASSED` /
  `TEST EXIT: 0`; `_staging`-seeded worktree still prefers the legacy path and is green
  likewise. The workstation lane-gate's `_staging` seeding is retained as
  belt-and-braces.

- 2026-09-30 — **Wave 1b gate batteries GREEN on both machines (`0f194bd`).**
  VM (runner v3, fresh dir per tier): host rc=0 (all suites `0 failed`, incl. the new
  `CXXRT TEST PASSED` + `CXX HEADERS TEST PASSED`; `TEST EXIT: 0`), unit-arm rc=0
  (`UNIT TESTS PASSED`, 0 FAIL lines), unit-x64 rc=0 KVM (`UNIT TESTS PASSED`, 0 FAIL),
  test-arm rc=0 86s (`System halt.`, 0 FAIL lines; in-wave CXX_SMOKE PASS lines incl.
  global-object-constructed + cxa_finalize-runs-dtors; DNSTST live: DHCP 10.0.2.3 →
  example.com A = 172.66.147.243). Workstation mirror: host suites 0 failed + both C++
  suites PASSED, both unit tiers PASSED, ARM full wave 0 FAIL (`System halt from CPU 2.`).
  (Batteries ran on the code tip; the docs-only delta `411f7a0` is the record itself.)

- 2026-09-30 — **Wave 1b landed: F2.4/F2.5 + L5/L6 vendoring + P1 design note — all
  five 1b branches merged at `0f194bd` (base `9660552`).**
  - **l3-cxxrt `bd8b7e8`** (F2.4/F2.5): minimal C++ runtime — `src/libc/src/cxxrt.cpp` +
    `src/libc/include/cxxrt.h` (over-aligned new/delete over malloc, `__cxa_guard_*`,
    `__cxa_atexit` LIFO 64-slot + `__cxa_finalize`, `__cxa_pure_virtual`, weak
    `__dso_handle`; `-fno-exceptions`/`-fno-rtti` enforced by #error). `.init_array`
    (SORT_BY_INIT_PRIORITY) collected in linker.ld, walked by crt0 before main; loader
    unchanged. **CXXSMOKE.BIN** wired to Makefile/disk/wave: 29 checks incl. exact
    static-ctor order `E0AB`, 16-byte new alignment, virtual dispatch, guards, atexit —
    ARM full wave 29/29 zero FAIL; x64 labeled minimal-boot 29/29 (same evidence policy
    as FPU_T; the x64 full wave remains the known FORKTEST-stall baseline). F2.5 audit:
    13 headers gained `extern "C"` guards; fixed mangled `_assert_fail` (assert.h),
    stdlib `template` param, host `ho_mkdir` conflict; host-parity `cxxrt_test.cpp`
    (25 checks) + `cxx_headers_test.cpp` (19) in host_tests; cstyle clean.
  - **l5-fonts `eb5088b`** (L5): FreeType 2.14.3 + HarfBuzz 14.5.0 (`+hb-ft +hb-icu`,
    ICU 78.2 host) + DejaVu 2.37 vendored per AD-11; memory-face render smoke
    (`FT-TOTAL renders=12 hash=4e88e851d05bd62e`) and shaping smoke (OT/FT glyph-id
    parity `HB-MATCH`) — byte-identical across 4 executions incl. a cold rebuild;
    recipes under `src/user/browser/fonts/`.
  - **l6-libs1 `9dfd203`** (L6a): zlib 1.3.1 / libpng 1.6.44 / libjpeg-turbo 3.1.0 /
    libwebp 1.6.0 vendored + host-built + smoked (jpeg + webp tarballs GPG-Good);
    `--clean` double-run checksum stability; committed gate script.
  - **l6-libs2 `e34045f`** (L6b): mbedTLS 3.6.7 / libcurl 8.22.0 (mbedTLS backend;
    live HTTPS HEAD 200) / SQLite 3.49.2 (3.53.4 cross-checked green; PRAGMA options
    recorded incl. `THREADSAFE=1`) vendored + host-built + smoked.
  - **l2-p1-design `a383f4c`** (L2): `docs/browser/p1-threads-design.md` —
    decision-complete P1.1/P1.2 design (threads as PCB slots sharing the leader AS;
    syscalls 72–75; `tls_base` joined to the context switch; futex-lite semantics +
    bounds; pthread mapping; THRD_T/TLS_T plan); **integrator review §9 resolves
    OQ1–OQ6** (OQ1: +2 renumber consented pre-first-ship; OQ2: implementation lane
    applies routing edits directly; OQ3–OQ6 accepted with bounds).
  - Integration notes: `.gitignore` union conflict (l6a vs l6b blocks) resolved by
    hand; worktree `_staging` gap root-caused and fixed in-env (lane-gate seeds from
    main; vendoring the ref-build story remains queued). Gate batteries (local + VM
    `w1b-*`) are running against `0f194bd`.

- 2026-09-30 — **F1 CLOSED: all five lanes merged and gated on both arches.**
  Merge order + commits: l4 `eaa0465` (F1.8 input v2: K modifier stamps, wheel
  btn 4/5, 250 ms graceful close; ARM+x64 QMP E2E green, 13-app regression
  13/13), l3 `1fce4f9` (F2.2 resolver + F2.3 clocks; host 482/0), l2-fpu-arm
  `09b2a71` (CPACR_EL1.FPEN per core, FPSIMD save/restore; 4/4 waves green
  with FPU_T 12/12; torture sensitivity proven by deliberate negative control),
  l2-fpu-x64 `660b3f0` (CR0/CR4, FXSAVE64/FXRSTOR64, SysV stack-ABI fix
  `fda9536`; FPU_T 8/11 in labeled minimal-boot — completion blocked by a
  pre-existing fork/pipe wait-reap wedge, not FP), l1 `7a4900b` (syscalls
  65–71 both arches, select() engine, TCP hardening incl. non-blocking
  connect completion, entropy, DHCP→DNS; x64 4th-arg `regs[9]` trap fix
  `a2a0e64`). Parent integration fixes: `63add42` — connect_fd `port_be` is
  literal network order (fs.c boundary ntohs()es; resolv.c was host-order —
  now `be16(RESOLV_PORT)`; libc.h documents wire-order ip/port); `c7c181f` —
  DNSTST wired (Makefile bin/rule/disk/mcopy + main.c wave entry) and resolv
  read-wait bounded via `select(3000)` (removes the documented
  unbounded-block hazard on a lost UDP reply).
  **Official gates (CI VM, runner v3, fresh dir per tier):** host rc=0
  (482 checks / 0 failed, 47 s); unit-arm rc=0 (48/48, 50 s); unit-x64 rc=0
  (50/50 KVM, 45 s); test-arm rc=0 (86 s — completion marker + 0 FAIL tokens;
  in-wave: FPU_T 12/12 incl. fork-isolation/syscall-boundary; SOCK2TST live
  HTTP GET over connect_fd; POLLTST blocking-select wake; RANDTST; DNSTST
  live: DHCP DNS `10.0.2.3` → `example.com A = 172.66.147.243`).
  Workstation battery mirrors it (host 482/0, ARM 48/48, x64 50/50).
  Pre-merge: all five lane gates green (host/unit-arm/unit-x64 each);
  ARM wave baseline green (85 s). **Gate F1 boot-time delta** (unit-tier
  wall, warm dirs, touch-refreshed — same method as the baseline): ARM
  7.78 → 8.33 s, x64 4.75 → 5.30 s (both +0.55 s, within the +2 s target;
  includes +4 added unit tests per arch and 5 new bins on the disk image).
  x64 full wave remains incomplete at tip (pre-existing silent-reset class,
  unchanged — lane waves map to baseline classes, no new class: l2-x64 wave4
  1167 boots, 0 LOCKFOREVER / 0 IDLESTUCK); `unit-x64` green stands as the
  hard x64 gate.
  **Carried (non-gate) items:** exec() FP-context reset + exec-path SP align
  (lane-proposed `program_loader.c` diffs); x64 PS/2 IntelliMouse wheel +
  E0-extended keys (wheel currently ARM-only); `third_party/_staging` seeding
  for host_tests in pristine worktrees (**closed 2026-09-30** — ref story vendored;
  lane-gate seeding kept as belt-and-braces); select() v1 wake-slice notes
  (10 ms bound); SYS_GETRANDOM is not a CSPRNG and the SYN-loss path is
  untested (no loss injection); FP-disabled trap path unexercised (FPEN is
  set before any FP use).

- 2026-09-29 — **W0.3 + W0.4 audits landed (archived under `docs/browser/`).**
  W0.3 (`f0-budgets.md`): the 256 MiB image grows safely as **FAT16 with
  8 KiB clusters pinned** (`mkfs.fat -F 16 -s 16`); **no `fat16.c` change** —
  `BPB_TotSec32` parsing is already live (the shipping 64 MiB image has
  `TotSec16 == 0` and boots today), all layout math stays in 32-bit range
  with ≥51 clusters of margin; FAT32 deferred until >≈512 MiB; tooling
  verified with real mtools runs + fsck (a 6 MiB file copied in/out of a
  256 MiB image); RAM envelope fine.  W0.4 (`f0-deps-audit.md`): full
  min-version table (ICU 70.1, HarfBuzz 2.7.4+ICU, GLib 2.70.0, FreeType
  2.9.0, libsoup3 3.0.0, Epoxy 1.5.4, libgcrypt 1.7.0, xkbcommon 0.4.0,
  libxml2 2.9.13, libtasn1; libjpeg/png/webp/sqlite/zlib versionless for the
  WPE port), Skia bundled @`588b550a…`, the JSC interpreter-only recipe
  (§10.2 closed), keep-ON/turn-OFF feature tables (Appendix B.1 seed) and the
  dependency-off list.  Key plan correction recorded in §2: the WPE port
  **hard-requires GLib + libsoup3**; the curl backend exists but is
  fork-port work (PlayStation/Win precedent).  Also: Makefile RAM comments
  corrected (said 3GB/2GB; flags are 6144M/8192M); §10 questions 1/2/4
  closed, 3 partial.

- 2026-09-29 — **CI hygiene round 2 (found while rehearsing re-runs).** Two
  failure classes surfaced and were fixed engine-side, not by re-running:
  (a) *stateful disk*: kernel unit tests write to disk.img (fat16
  create/move), so a second tier run in the same tree false-failed
  `fat16_rename("/MOVESUB", "/MOVEDIR2/MOVESUB")` at fat16_test.c:126; the
  unit scripts now rebuild the image via a new `make fresh_disk` target
  (touch-driven — the image recipe re-runs dd+mkfs, no file deletions) —
  verified by two consecutive local runs, 44/44 both.  (b) *completed wave
  ≠ suites passed*: `System halt.` can coexist with a failed suite; a green
  ARM wave carries **zero** uppercase `FAIL` tokens (checked against the b2
  baseline), so `hobbyos-ci.sh` v3 fails any test wave with FAIL tokens and
  the workstation lane-gate does the same.  Also recorded for the F1 gate:
  x64 KVM wave baseline = 3/3 fresh runs did NOT reach `System halt.` (three
  classes: late IDLESTUCK at STRESS.BIN with ~60 suites passed; mid-boot
  IDLESTUCK; early LOCKFOREVER storm) — so the x64 F1 gate is
  **unit-x64 green + no NEW wave signature**, not wave completion, until
  the pre-existing x64 instability is addressed on its own schedule.
- 2026-09-29 — **F0 execution start: CI hardened, ABI frozen, vendoring
  verified, lanes launched.** (a) CI harness defects fixed before any gate
  use: the unit scripts started the 20 s pass/fail clock at build time
  (false reds on cold copies) and the x64 tier exited 0 on
  `UNIT TESTS FAILED` (false green); `run_unit_tests*.sh` are now
  build-then-run two-phase and `hobbyos-ci.sh` is marker-gated and runs
  every tier in a fresh copy (the shared-workdir race killed an ARM link
  in the first baseline). (b) First green baseline on the CI VM at this
  tree: host, unit-arm (44 checks), unit-x64 (46) — plus test-arm earlier;
  x64 wave characterization in flight. (c) ABI frozen per §7.3: syscall.h
  65–71 (socket/connect_fd/select/fcntl/getsockopt/setsockopt/getrandom,
  `SYS_MAX` 71) + net errnos (Linux values) + the libc socket/select
  surface (`FD_SETSIZE` 256) with stub impls via `errno_ret` — device-side
  only, the shared header host-guarded. F1.7 field appended to
  `sys_netinfo` (`dns`, network byte order; pre-extension-size callers
  unaffected). (d) Vendoring: `wpewebkit-2.54.0.tar.xz` sha256
  `efa9bcc3cb891c2d88f50eec710d9ccee71cbdf1040420361eb98c17355eb452`
  matches the official `.sums` (verified manually — the `.sums` is
  prose-formatted, `sha256sum -c` cannot consume it) and this document's
  pin table; tarball + `.sums` committed, extraction gitignored per AD-11.
  Fork clone: tag obj `cef114f5…` dereferences to commit `5220e80b97…`
  (matches §2); the clone was relocated out of `~/Documents/GitHub` (the
  NAS sync is a 30-min two-way `rsync --delete` and would ship ~8 GB per
  tick) to `~/webkit-hobbyos` — Appendix C note updated. (e) F1.5
  test-first: `src/user/fpu_test.c` (`FPU_T.BIN`) committed *unwired* — it
  cannot compile until each arch's user-flag drop lands, so the FPU lanes
  wire it as part of their change. (f) fat16 stale-size defect fixed
  properly: `UNEXPAND_T_BIN_SIZE` is now generated by the Makefile from
  the built on-image binary instead of a hardcoded 13072 (arch- and
  toolchain-dependent). (g) Five lanes launched in worktrees
  `~/hobbyos-lanes/{l1-net,l2-fpu-arm,l2-fpu-x64,l3-libc,l4-desktop}`
  (branches `browser/*`), all against the frozen interfaces above.
- 2026-09-29 — **Maintainer review of v2: Track B endorsed as the best /
  primary track.** Consequences: Track A re-scoped to optional contingency
  (dormant by default — §0.2, §6 NS, §7 lanes/waves/briefs); status line,
  Track table and wave notes updated accordingly. No other changes; no code.
- 2026-09-29 — **browser.md v2 (this document).** Requirement raised to
  "JavaScript so Google Search / CNN realistically work"; v1 Dillo+FLTK plan
  superseded before execution. Verified field evidence: Google serves a
  JS-redirect shell (`/httpservice/retry/enablejs`) with no results without
  JS; CNN serves 6.5 MB server-rendered HTML; lite.cnn.com exists. Engine
  survey: WebKit selected (precedents Haiku/WinCairo/PlayStation/WPE; JSCOnly
  first-light; curl backend; C API glib-free); Ladybird rejected (Rust hard
  requirement + Qt6 UI + Skia + pre-alpha — checked against its own docs
  today); Servo/GTK-embedding/Chromium/Gecko out. Two-track plan (A NetSurf
  interim / B WebKit target) with F/P/NS/WK milestones; pins: WPE 2.54.0
  +2.52.6 checksums recorded from official `.sums`; carry-over pins kept.
  No code changed by the planning session.
- 2026-09-29 — browser.md created (v1 planning session). Survey done; Dillo
  3.2.0 + FLTK 1.3.11 selected; sources pinned with sha256 (§2 of v1); no
  code changed. (Superseded by the v2 entry above; retained for history.)
