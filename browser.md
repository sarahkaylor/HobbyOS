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
integrator review. Implementation pending.)*

- [ ] **P4.1 AF_UNIX sockets** — `socketpair(AF_UNIX)` + pathless sockets;
      stream semantics; fd namespace integration. (`SYS_SOCKETPAIR` §A.1b.)
- [ ] **P4.2 fd-passing** — `sendmsg`/`recvmsg` with `SCM_RIGHTS` (one fd
      minimum, arrays best-effort); close-on-fork inheritance rules defined.
- [ ] **P4.3 `poll()`** — syscall + libc (`pollfds` incl. `POLLIN/POLLOUT/
      POLLERR/HUP`), used by WebKit event loops; select stays for old code.
- [ ] **P4.4 Tests** — `IPC_T.BIN`: socketpair echo, fd-pass to a child
      process, poll multiplexing 3 fds; kernel-unit coverage.

**Gate P4:** `IPC_T.BIN` both arches; no regressions.

### P5 — Process management & signals  *(new; Track B)*
*(Design note complete: `docs/browser/p5-exec-signals-design.md` — `b2bf160`;
D1–D13 (in-place execve + `SYS_SPAWN_EX`, unified reap delivery closing the
waitpid-wake gap, main-thread SIGCHLD frame engine, rows 83–86 supersede the
§A.1b provisionals); OQ1–OQ8 pending integrator review. Implementation
pending.)*

- [ ] **P5.1 `execve`** — replace current image with a new one (argv/envp
      pass-through; fd inheritance + `FD_CLOEXEC` honored; executable found
      via explicit path first).
- [ ] **P5.2 `waitpid`** — exit-status propagation, zombie reaping,
      parent/child bookkeeping for multiple children.
- [ ] **P5.3 Minimal signals** — `sigaction` real for a small set
      (`SIGKILL`, `SIGTERM`, `SIGPIPE`, `SIGCHLD`, `SIGSEGV`/`SIGBUS` default
      kill + report), delivery on process threads (pick main-thread delivery
      initially — **verify** WebKit's needs; JIT-off reduces requirements).
- [ ] **P5.4 Tests** — `PROC_T.BIN`: spawn child → socketpair talk → exec →
      exit code → reap; SIGCHLD observed; SIGPIPE kills writer cleanly.

**Gate P5:** `PROC_T.BIN` both arches; no regressions; process-capacity check
(≥ 6 concurrent processes documented).

### P6 — POSIX fill-in & SQLite  *(new; Track B)*

- [ ] **P6.1** `mkstemp`/`tmpfile`/`fsync`/`ftruncate`/`fchmod` stubs-as-
      appropriate; `flock`/`fcntl` record locks (SQLite); `getcwd`/`chdir`
      (if missing); `utime`; `sched_yield`; `sysconf` bits; `getpwuid`/
      `getgroups` stubs returning sane single-user values; `locale` C.
- [ ] **P6.2 Port SQLite** — pinned version; `SQLITE_THREADSAFE=1`,
      single-OS VFS over our file API; fcntl locks per P6.1; in-OS test
      (create/insert/select, WAL off initially).
- [ ] **P6.3** Environment/aux: `environ` hygiene, `PATH`-less exec rules,
      argv[0]/program-path exposure needed by WebKit (**verify** its
      executable-path discovery; supply via spawn contract).

**Gate P6:** SQLite in-OS test + host tests green both arches.

### P7 — Toolchain, fork, build system  *(new; Track B; starts at W1)*

- [ ] **P7.1 Fork stand-up** — `HobbyOS/WebKit` clone at tag
      `webkitgtk-2.54.0`; add `HobbyOS/` port dir (platform files, cmake
      toolchain file, port README + rebase log); CI script = "does it
      configure" on host for sanity.
- [ ] **P7.2 Port scaffolding** — `WTF_OS_HOBBYOS` platform detection;
      `Platform.h`/`PlatformHobbyOS.*` (time, memory, threads conversion,
      file syscalls, StackBounds, OSAllocator); build with
      `USE_SYSTEM_MALLOC`; strip JIT options.
- [ ] **P7.3 Build integration for HobbyOS** — cross CMake toolchain
      (`CMAKE_SYSTEM_NAME=HobbyOS`, clang, static, our sysroot + libc++);
      document the exact cmake invocation in the fork README; Wire into the
      OS repo as build rules request (Integrator applies).
- [ ] **P7.4 Feature-trim list applied** — from W0.4: media/GPU/WebRTC/WebGL/
      WebAudio/PDF/plugins off; SVG/WebP on; ICU + harfbuzz on; curl backend
      on; sandbox off; JIT off; record the full define set in the fork README.

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
      numbers recorded in §11. — **Bounded run recorded in §11; blocked by a
      reproducible kernel freeze at ~480 rounds (documented, follow-up lane).**

**Gate P8:** green both arches; §11 updated with the numbers. *This is the
"OS is ready for WebKit" evidence.*  — **P8.1 green; P8.2 numbers + freeze
finding in §11.**

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
- [ ] **WK-1 `jsc` shell on HobbyOS** *(first deliverable!)* — JSCOnly target
      for HobbyOS: platform files needed by JSC; static link; `jsc` runs:
      arithmetic/strings/regex/JSON/Date; **test262 language-subset run**
      (record pass rate; crash-free is the hard gate, pass-rate is the
      record); `jsc`-driven host-file read/write smoke. *Requires P1–P3
      minimums; sequence starts as they land.*
      **Gate WK-1:** `jsc` on both arches runs the test set; evidence = raw
      console output + test262 summary; §11 updated.
- [ ] **WK-1.5 Architecture decision** — WebKit2-multiprocess vs
      WebKitLegacy-in-fork vs re-pin (2.52.6): written comparison, decided,
      recorded (AD-3 default = WebKit2; deviations need the comparison to
      say why). **Gate = the decision + rationale on record.**
- [ ] **WK-2 Full-target build + stubs** — extend port platform code to what
      WebCore/WebKit need to *configure, compile, and link* all three process
      binaries (unimplemented platform functions may abort with a message at
      this stage); Skia-vs-Cairo spike result applied (AD-4 lever).
      **Gate WK-2:** three binaries build for both arches (size + time
      recorded); "configure→link" CI script green.
- [ ] **WK-3 Headless web process** — WebProcess standalone: create a page,
      load `file://` fixtures, run JS, paint into our surface, **dump
      PNG/pixel-hash** for comparison (no UI process yet); JS probes via
      `evaluateJavaScript`-equivalent → stdout. Includes text stack
      (FreeType+harfbuzz+our font backend), images (PNG/JPEG/WebP).
      **Gate WK-3:** fixture ladder renders + JS probes correct on both
      arches; evidence = pixel hashes + probe output files.
- [ ] **WK-4 Networking** — NetworkProcess + libcurl backend: fetch fixtures
      over http/https from `10.0.2.2`; DNS via resolver; cookies (minimal);
      redirects; gzip? (*zlib available* — verify what curl backend uses).
      **Gate WK-4:** https fixture + redirect + cookie round-trip green both
      arches; transfer evidence captured.
- [ ] **WK-5 UI shell** — our browser window: desktop pixel protocol
      (create/damage present), input translation (mouse/keys/wheel), Back/
      Forward/reload/stop keys, URL entry (keyboard prompt initially — no
      toolbar claims), scroll; present from the compositor path; window
      title/close integrated (F1.8).
      **Gate WK-5:** human-run session on QEMU: navigate fixtures by link
      clicks + URL entry; wheel scroll; graceful close; no crash.
- [ ] **WK-6 Acceptance** — §8.3 Track-B lines: **Google Search** (load,
      type query, results render, open a result) and **CNN** (homepage
      renders, article opens, scrolls, images show), JS-heavy pages
      exercised; 5-min soak; memory/sizes recorded; performance noted
      qualitatively. Any site failures documented, not hidden.
      **Gate WK-6:** acceptance checklist complete on both arches (or
      documented exceptions with evidence).
- [ ] **WK-7 Hardening & stretch** — crash recovery (WebProcess crash →
      shell survives, page reloadable), high-water marks, startup time,
      license compliance package (LGPL relink provision for static WebKit),
      `docs/webkit.md`; stretch backlog: file-backed demand paging/JIT
      reconsideration/tabs/persistent cookies/GPU.

**Gate WK-7:** full matrix green; docs done; §8.3 fully checked; evidence
archived.

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
