# HobbyOS Gaming Program — Plan (living document)

Status: **IN EXECUTION (started 2026-10-17, Coordinator on `swarm-coordinator` .233).**
M0 board/fleet automation landed (GB-002/003/004); M1 ABI-core is the critical path.
Owner: Sarah (architect), assisted by the HobbyOS agent swarm.
Ticketing: **Forgejo v17 live at http://192.168.0.234:3000**, repo `sarah/gaming-program`;
`GAMING-BOARD` board scaffolding = tickets GB-002/GB-003.
Swarm: 5 VMs on Proxmox `c2` (192.168.0.19) — inventory in `swarm-fleet.md`, credentials in the
scratch file (see §11.1). This document is the single source of truth. The **Coordinator** keeps
it in sync with the board; agents never fork it. If a fact here drifts from the repo, update §3
first, then the board.

---

## 0. Executive summary

**Vision (success criteria):** launch real games "like Dyson Sphere Program, RimWorld, and
Total Annihilation" from HobbyOS, running under QEMU (no real-GPU path this program).

**Program spine (what makes that vision reachable):** give HobbyOS **Linux ELF binary load &
run capability** — a Linux x86_64 syscall-ABI compatibility layer plus an ELF loader — so
real Linux binaries can execute inside HobbyOS. Alongside it, a **native game catalog** (ports
on the HobbyOS ABI) guarantees playable results this month no matter how far the compat layer
gets.

**Honest scope contract (read §1 before anything else):** the three named games *as shipped*
cannot run in HobbyOS this month — and two of them need a Mono/CLR and a Vulkan driver,
respectively, which are not one-month builds for any swarm. The plan therefore stages them as
**vision-level epics on a long arc**, and defines this program's real acceptance criteria as a
**weight-class ladder** that makes measured progress toward exactly those games. By the end of
week 4 we expect: **a playable game delivered through each of the two tracks** (native port +
unmodified Linux static-ELF binary), plus a game launcher, plus full multiplan testing.

**Autonomy model:** one **Coordinator** agent (the only agent allowed to call the deep model)
runs the board, the plan, and a **hourly/daily re-plan loop** that invokes **DeepSeek v4.1
Flash** to re-assess and mint new tickets. Every other agent runs almost entirely on the **much
faster DeepSeek V4 Flash** workhorse.

---

## 1. Success criteria and the honest scope contract

### 1.1 What the three target games actually are

| Game | Engine / runtime | Ships as | What "run it" requires | Verdict for this program |
|---|---|---|---|---|
| **RimWorld** | C# on **Mono** (.NET), OpenTK/OpenGL | x86_64 Linux bundle w/ bundled Mono | A **CLR (Mono) port** + OpenGL + near-complete Linux userspace | **Not this month.** Track as Program-2 epic (`MONO`). |
| **Dyson Sphere Program** | **Unity** (IL2CPP) | x86_64 Linux ELF, Vulkan/GL | **Vulkan driver** + full glibc/dynamic-loader + huge proprietary binary | **Not this month.** Track as Program-2/3 epic (`VULKAN`). |
| **Total Annihilation** | Cavedog C++ (Windows PE) / **Spring RTS** (open-source engine) | Windows binary, or Spring (Linux ELF, SDL2+OpenGL2) | Spring-class C++/OpenGL2 engine under compat | **Not this month in full form.** Most tractable of the three; Program-2 epic (`RTS`). |

No swarm, no amount of parallelism, changes that these require a CLR, a Vulkan/driver stack, or
a Spring-class engine **plus** the full Linux userspace they drag in. Any proposal that says
"we'll run the DSP Linux binary this month" is not engineering. What we *can* do is build the
layer those games all need — **Linux ELF run capability** — and prove it on a ladder of
heavier and heavier real binaries.

### 1.2 The weight-class ladder (this is the real success metric)

Every step is a **ticket with a Definition of Done** and a differential test (same binary on a
real Linux box vs. HobbyOS):

| Class | Binary example | Enables | Target |
|---|---|---|---|
| W0 | static-musl **`-static-pie`** `hello`, `cat`, shell builtins | ABI skeleton proven | **Week 1 gate (M1)** |
| W1 | static-musl GNU coreutils / BusyBox | Real-world syscall coverage | Week 2 |
| W2 | static-musl **ncurses** app (`ninvaders`, a roguelike) | termios, ioctl, TTY | Week 2 |
| W3 | static-musl **SDL2** app (fbcon/fbdev path) | fbdev+evdev+threads+futex+TLS | **Week 3 gate (M2/M3)** |
| W4 | **static-musl Doom** (doomgeneric) — *playable* | Full 2D game under ELF-compat | **Week 4 (S1)** |
| W5 | dynamic glibc binaries / windowed SDL over X11-ish | glibc dynamic loader, X protocol | Program-2 |
| W6 | Mono (RimWorld) / Vulkan (DSP) / Spring RTS | CLR + driver stacks | Program-2/3 (epics) |

### 1.3 This program's acceptance criteria (S0/S1)

- **S0 — Native track (definite):** ≥5 open-source games launch and play from the HobbyOS
  desktop under QEMU on **both** `ARCH=arm` and `ARCH=intel` — including a full-screen 3D-ish
  title (DOOM-class raycaster) and a text roguelike — with persistent input, windowed/launcher
  integration, and a scripted **playthrough test** (QMP input + screendump assertions).
- **S1 — ELF-compat track (the moonshot):** HobbyOS boots **unmodified x86-64 Linux static-ELF
  binaries** through the compat layer, culminating in a **playable static-musl SDL game
  (class W4)** on `ARCH=intel`, differential-tested for syscall/errno parity.
- **S2 — Launcher + evidence:** a desktop **game launcher app** that starts both native `.BIN`
  games and `ELF-compat` games; per-game evidence (serial log, screenshots, replay script);
  RimWorld/DSP/TA feasibility re-derivation landed as tickets on the Program-2 arc.

---

## 2. Architecture

### 2.1 Two tracks, one OS

```
HobbyOS (QEMU: -M virt aarch64 / -M q35 x86_64)
├─ Native track (HobbyOS ABI)  → desktop, xlib, gfx libs, native .BIN games     [S0]
└─ ELF-compat track (Linux ABI, ARCH=intel)                                     [S1]
   ├─ ELF loader (ET_EXEC/ET_DYN-static) in the kernel loader path
   └─ Linux x86_64 syscall translation → HobbyOS kernel (Linuxulator-style)
        + compat device nodes: /dev/fb0 (framebuffer), /dev/input/event* (evdev),
          /dev/urandom, /dev/null; TLS via arch_prctl/fs; real signal delivery
```

- The ELF-compat track is **x86_64-only** (Linux binaries are x86_64; musl cross targets
  `x86_64-linux-musl`). It does **not** touch the ARM primary target — ARM stays the home of
  the native catalog and must never regress (two-arch parity rule, §6).
- **Corpus flavour (GB-013 decision, 2026-10-17 — `docs/elf-loader.md`):** the compat corpus is
  built **`-static-pie`** (`ET_DYN`, no `PT_INTERP`), not fixed-address `ET_EXEC`. A musl `ET_EXEC`
  is linked at 0x400000, which HobbyOS cannot map: `vm_region_insert()` rejects any region outside
  the 64 GiB user window, and low VAs are the shared kernel identity map in every address space
  (x64 0-8 GiB). Static-PIE self-relocates, so the loader may place it in the window. `ET_EXEC`
  still parses but is refused at load time with a clear reason.
- "Required support" for ELF run = the kernel ABI surface below, not a userspace emulator.

### 2.2 The compat layer (what gets built)

1. **ELF static loader.** Parse **static-PIE `ET_DYN`** (`ET_EXEC` accepted-but-refused, see §2.1),
   map each PT_LOAD with its real `p_flags` protections, zero p->bss, set the entry to
   `base + e_entry`. Loader lives alongside the existing flat-binary loader
   (`src/kernel/program_loader.c`) and dispatches on magic bytes; design + integration points in
   `docs/elf-loader.md`. `PT_INTERP` (dynamic) is a Program-2 ticket — static-PIE builds don't need it.
2. **Syscall translation table.** Linux x86_64 syscall numbers and HobbyOS's internal ABI are
   different. Implement a per-syscall shim (Linux args → HobbyOS call → Linux negative-errno —
   errno *numbers* must be Linux's, see §2.3). Core set first (W0/W1), then ioctl/futex/etc. as
   classes demand. Dispatch stays a table so the existing per-arch parity rule holds.
3. **ABI ground truth (the contract).** `gaming.md` §2.3 + `docs/linux-abi.md` define: syscall
   numbers, errno values, and **struct layouts** (`struct stat`, `sigaction`, `ucontext`,
   `epoll_event`, `termios`) exactly matching Linux x86_64. musl is compiled by a normal
   `x86_64-linux-musl` cross — musl itself never runs on HobbyOS; HobbyOS must satisfy what
   musl-generated code calls. This is the highest-risk correctness surface → differential tests.
4. **Kernel prerequisites** (proven on the selfhost fork; must be merged/gated first, ticket
   GB-012): `brk`, anon `mmap`/`munmap`, full `fork`/`execve`/`waitpid` + exit status, threads
   via `clone`+`futex`, **TLS** (`arch_prctl(ARCH_SET_FS)` on x86_64 → swap `%fs` base),
   **real signal delivery** (frame + `sigreturn`), `poll`/`select`, `gettimeofday`/`nanosleep`,
   `uname` (report a Linux-ish name for SDL/games), minimal `/proc/self` and `/sys` stubs.
5. **Compat device nodes** for the game experience, all backed by QEMU virtio:
   - `/dev/fb0` — mmap-backed HobbyOS framebuffer (+ `FBIOGET_VSCREENINFO` ioctls) so SDL's
     `fbcon` driver works untouched.
   - `/dev/input/event*` — evdev ABI fed from virtio-input, so SDL's evdev driver works.
   - `/dev/urandom`, `/dev/null`, `/dev/dsp` (audio if the virtio-sound epic lands).
   - These are added in the compat shim, not the native ABI.
6. **SDL2 static-musl** built against `x86_64-linux-musl` with the fbcon+evdev drivers — this is
   the dependency that makes "SDL game under compat" real.

### 2.3 Ground rules that never regress

- `-mgeneral-regs-only` kernel rule, no unaligned access, byte-wise parsing, Normal-NC DMA — the
  existing kernel constraints continue to apply to all new code.
- **Two-arch parity:** every native change must stay green on ARM **and** intel
  (`make host_tests`, `./run_unit_tests.sh`, `./run_unit_tests_intel.sh`, `make test`,
  `make test_intel`). The ELF-compat work lives behind existing arch guards and adds its own
  intel-only test tier (differential).
- **Tests are part of every ticket** (see §6) and every DoD names its test.

---

## 3. Environment & hardware

### 3.1 Inference servers — model-tier policy (how we use the fast model as much as possible)

| Tier | Model | Endpoint | Used by | Policy |
|---|---|---|---|---|
| **FLASH (workhorse)** | DeepSeek **V4 Flash** | `http://llm.localdomain:8000/v1` (vLLM, RTX 6000 pair box) | **every** worker / reviewer / tester agent | **Default for ~all tickets.** Target >90% of fleet tokens. |
| **DEEP (planner)** | DeepSeek **v4.1 Flash** (8× DGX Spark) | `http://dgx1.localdomain:8000/v1` (vLLM; alias `dgx-main`) | **Coordinator + farm Verifiers** | Planning, re-planning, ABI/architecture design, blocked-root-cause after 1 FLASH retry, milestone reviews, **and verification/gate verdicts (`verify` tickets)**. |

Measures behind us (Sept/Oct 2026 community benchmarks for exactly this class of hardware):
~156–241 tok/s aggregate on the 8-Spark v4.1 box; ~47 tok/s single-stream per RTX 6000 card;
cold prefill ~840–2,100 tok/s depending on context. Consequences coded into the plan:

- **Agent count ≈ 8–12 full-speed agents** (fleet ceiling ~250–335 tok/s sustained). Configure
  10–14 agents; expect ~8 actively generating at any time. More agents ≠ faster; deeper/batched
  tickets get more throughput, not more agents.
- **Long-lived sessions, never churn.** Restarting a 300K-context agent costs minutes of
  prefill. Agents stay on their subsystem; new work = new ticket, not new session.
- **Deterministic work stays in tools** (grep/sed/test/CI), never in model tokens.
- **Verification is DEEP-class work.** Every `verify`-labeled gate ticket (T3 QEMU intel+arm, merge-gate,
  device-tier) is run by a farm **Verifier** session on **DeepSeek v4.1 Flash (`dgx1`)** via
  `--provider dgx -m deepseek-v4.1-flash --reasoning high` — never on the FLASH workhorse. A wrong verdict
  is the costliest mistake a swarm can make, so gate judgment gets the stronger model; gates stay
  scripted/deterministic, so DEEP stays ~10% of tokens even with verification parallelized.
- Endpoints recorded and verified 2026-10-17 (GB-004), see §11.3 for the probe evidence. Mapping the
  tiers to the built infra: **FLASH = DeepSeek V4 on the RTX-6000 pair box** (workhorse),
  **DEEP = DeepSeek v4.1 on the 8× DGX Spark fleet** (planner/coordinator only).
- **Measured 2026-10-17 (single stream, GB-004, `max_tokens=200`, temp 0):** FLASH
  `llm.localdomain:8000` **57.7 tok/s** (196 completion tokens); DEEP `dgx1.localdomain:8000`
  **40.5 tok/s** (119 tokens). Both serve `max_model_len: 1048576`. These are *single-stream*
  numbers on a shared box — §3.1's 94–150 tok/s is an aggregate/community figure, so cap active
  workers from measurement, not from the table.
- **Model budget (validated 2026-10-17):** split lands ~90% FLASH / ~10% DEEP tokens; DEEP duty
  ~3% (it is mostly parked — that is intended). The binding constraint is FLASH throughput:
  the RTX pair measures ~94–150 tok/s, which feeds only **~3–5 agents** at full clip (the
  8–11 named in §5 is optimistic). Before scaling agents past ~4–6, benchmark both endpoints
  at GB-004 and cap concurrency at what FLASH sustains; keep DEEP reserved for planning + verification verdicts.

### 3.2 QEMU focus (scope boundary)

All game execution this program is under QEMU. No real-GPU, no Proxmox-GPU/RDMA passthrough —
that workstream (`proxmox-gpu`) is explicitly **out of scope** here. Targets:

| Target | Invocation | Used for |
|---|---|---|
| `ARCH=arm` | `qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4` | Native catalog, primary target |
| `ARCH=intel` | `qemu-system-x86_64 -M q35 -smp 8` | ELF-compat track, native ports |
| Nested KVM | enable if the Proxmox host CPU permits (x86_64 farm VMs) | Speeds up intel boot/test several× |

### 3.3 Proxmox 192.168.0.19 — the built fleet (Oct 2026)

Five VMs created on Proxmox `c2`; disks on `c2flash` (NAS 192.168.0.56); **no VM has a
physical GPU**. All guests Ubuntu 26.04.1; dev VMs run the Kubuntu desktop (like VM 125).

| VMID | Name | IP | vCPU/RAM/Disk | Role |
|---|---|---|---|---|
| 140 | `swarm-farm-1` | 192.168.0.230 | 32 / 64 GiB / 128 G | QEMU build/test farm + worker agents |
| 141 | `swarm-farm-2` | 192.168.0.231 | 32 / 64 GiB / 128 G | same |
| 142 | `swarm-farm-3` | 192.168.0.232 | 32 / 64 GiB / 128 G | same |
| 143 | `swarm-coordinator` | 192.168.0.233 | 32 / 64 GiB / 128 G | coordinator Hermes (DEEP), re-plan cron |
| 144 | `swarm-ticketing` | 192.168.0.234 | 8 / 16 GiB / 64 G | Forgejo (git + issues + projects) |

Every dev VM already has: clang/lld/llvm, `qemu-system-arm`+`qemu-system-x86`, `qemu-efi-aarch64`
+`ovmf` firmware symlinked to the Makefile's `~/.local/share/...` paths, `mkfs.fat`/`mtools`,
python3, ccache/sccache, **Hermes installed** (`sarah:~/.hermes`), **HobbyOS repo cloned incl. the
WebKit port** (`third_party/webkit-hobbyos`), and `qemu-guest-agent` enabled. **LLVM 21.1.8
dist-staging is built and cached on farm-1** — rsync `third_party/llvm-21.1.8/dist-staging` to the
other VMs to skip that one-time build.

Notes: agents SSH passwordlessly between VMs; shared home/artifacts go via NAS (`/mnt/fast` on
ubuntu62 or the c2flash share). **Inference boxes and the DGX Sparks are off-limits for build
load** — never run QEMU tests on them (measured TTFT is concurrency-sensitive). Full inventory +
known-good verification commands: `swarm-fleet.md`.

---

## 4. Ticketing system

**Choice: Forgejo v17 on `swarm-ticketing` (192.168.0.234:3000 — built Oct 2026).** Rationale:
self-hosted, free, OSS, full REST API + webhooks (agents live off the API), git + issues + kanban
boards in one, milestones and labels, light enough for a VM.

### 4.1 Structure

- **Project:** `GAMING-BOARD` (kanban).
- Lanes: `📋 Backlog` → `🟦 Ready` → `🟨 In Progress` → `🟧 In Review` → `🚦 Merge Gate` → `✅ Done`; plus `🧊 Icebox` (Program-2 vision epics).
- Labels: `infra`, `abi`, `loader`, `libc`, `elf-compat`, `graphics`, `input`, `audio`, `native-port`, `game`, `tests`, `perf`, `docs`, `decision` (needs Sarah's sign-off), `deep` (DEEP-model required), `blocked`, `debug` (DEEP root-cause / design-freeze work claimed by a farm Verifier only when no `verify` ticket is Ready — §5.1 debug reserve), `parity-ar` (ARM), `parity-x64`.
- Milestones: `M0·infra` `M1·abi-core` `M2·devices-sdl` `M3·games` `M4·acceptance`.
- Ticket skeleton: ID, workstream, weight S/M/L, depends-on, DoD + test, evidence link.
- Bot user: `swarm-bot` (API token) — the Coordinator and workers move/annotate tickets through it. Webhook on the board posts to the Coordinator.

**LANE LIMITATION (GB-002, found 2026-10-17).** The live Forgejo is
**v17.0.0+gitea-1.22.0** and its REST API has **no `/projects` endpoint**
(`/api/v1/repos/sarah/gaming-program/projects` → `404 page not found`), so the
`GAMING-BOARD` kanban project cannot be created or driven over the API that every
agent works through. **The lanes are therefore labels** whose names are the lane
names above (one lane label per issue, colours matching the lane), stacked on the
**native milestones** M0–M4; agents move tickets with
`tools/board/lane-move.sh GB-nnn "<lane>"` instead of dragging cards. When a Forgejo
build with `/projects` lands, `tools/board/board.py` grows a project backend and the
label fallback retires. Weight is carried as a `wt-S|wt-M|wt-L` label, the ticket
skeleton as an issue header block (see `tools/board/README.md`).

### 4.2 Ticket workflow (agents own it end-to-end)

1. Worker claims a `Ready` ticket → sets `In Progress` → creates git branch `ticket/<id>-<slug>` (worktree mode, one per agent).
2. Worker codes **and tests** (§6); runs the local gate; opens a merge request against `main`.
3. Reviewer agent reviews (diff + DoD + tests) → `In Review`.
4. **Verification is its own ticket** (`verify` + `deep` labels). When an implementation lands `In Review`
   with evidence but no gate proof, the Coordinator mints `GB-nnn-V` (ticket skeleton per §4.1; DoD = the
   exact gate command(s) + pass criteria per §6 + two-arch parity). Minting happens at every re-plan for the
   `In Review` backlog, so gates run **in parallel** with new production tickets instead of queueing behind
   a single verifier.
5. A farm **Verifier** (DEEP session, §5.1) claims `GB-nnn-V` → `In Progress` → runs the gate on its own VM →
   posts evidence + a **RED/GREEN** verdict on both the verification and the implementation ticket.
   GREEN → the implementation ticket to `Merge Gate`; RED → back to `In Progress` with the failing output
   quoted verbatim. **This propagation is machine-checked:** `python3 tools/board/check_verdicts.py`
   (and `--self-test`, which proves the check can fail) compares every posted verdict against its
   parent's current lane and exits non-zero on a mismatch. It exists because the propagation was
   missed twice in one loop — GB-010 and GB-070 both sat `In Review` with a RED already posted
   against them, reading as "awaiting review" when a gate had been run and failed.
6. Merge-gate CI (all test tiers + differential + both arches) must be green → `Merge Gate`.
7. Coordinator merges (or escalates `decision` to Sarah) → closes ticket **with evidence link** → lane `Done`.

- **Verifier honesty is law.** A Verifier never verifies its own just-finished implementation without the
  Coordinator's sign-off (independence); no GREEN ever lands without a gate that can **provably fail**
  (GB-066/GB-070 discipline) — a skipped, self-served, or "looks green" gate is not a verdict.

### 4.3 The re-plan loop (autonomy — this is how the swarm steers itself)

Triggers (whichever first): **every 24h** (cron on `swarm-coordinator`), every milestone gate, `>3 tickets blocked`, `>30% red on latest gate`, or acceptance evidence landed.

The loop:
1. Coordinator snapshots: board state, open MRs, gate results, evidence dir tree, `gaming.md`.
2. Coordinator runs a **`deep` `PLAN` ticket** on **DeepSeek v4.1 Flash** only: re-assess plan vs reality, re-scope acceptance if reality demands, **mint new tickets** on the board — including a `verify`+`deep` **verification ticket `GB-nnn-V` for every `In Review` ticket that lacks a gate verdict** — re-prioritize lanes, update `gaming.md` if facts changed.
3. Coordinator posts the re-plan summary as a comment on the `PLAN` ticket and closes it.
4. New tickets are `Ready` for workers by the next loop. The swarm therefore never waits for a human to discover what to do next — **autonomy is key**.

---

## 5. Swarm operating model

### 5.1 Roles

| Role | Count | Model | Where | Responsibility |
|---|---|---|---|---|
| **Coordinator** | 1 | DEEP | `swarm-coordinator` (.233) | plan + board + re-plan loop + merges + escalation; primary DEEP consumer |
| **Worker** | 3–8 | FLASH | `swarm-farm-1..3` | claim tickets, branch, code+tests, MR — one long-lived session per subsystem |
| **Verifier** | 3 (one per farm) | DEEP | `swarm-farm-1..3` | claim `verify`+`deep` tickets, run §6 gates on own VM, post RED/GREEN verdicts; gate-running is its own role so gates parallelize. **Debug reserve:** when no `verify`+`deep` Ready ticket exists, a farm Verifier may claim a Ready `debug`+`deep` ticket (root-cause / design-freeze work that is DEEP-class but not a gate, e.g. kernel-wall debugging, build-profile freezes) and work it — this keeps DEEP wall work from serializing on the Coordinator alone |
| **Reviewer** | 1 (dedicated cross-lane loop `wrev`, FLASH, farm-3) | FLASH | farm-3 + any farm | contract/diff/DoD review of In-Review + In-Progress-with-PR cards across ALL workstreams; posts APPROVE/BOUNCE/FLAG verdicts; **mints missing `GB-nnn-V` gate tickets (§4.2 step 4) for gate-ready In-Review cards so farm Verifiers gate in parallel**; flags `decision`. Review verdicts are NOT gate verdicts — Verifiers own RED/GREEN |
| **Tester** | 1 | FLASH | `swarm-farm-1..3` | playthrough + stress harnesses, flaky triage |

### 5.2 Agent mechanics (existing Hermes capabilities; no custom build)

- One **profile per role** (`swarm-coordinator`, `swarm-worker`), each with a distinct
  `HERMES_HOME`; workers use `hermes -w` (worktree mode) so parallel agents never collide on
  files. Sessions under `tmux` on their VM; transcripts/evidence land on the shared NFS store so
  the Coordinator/planner can read them.
- **Verifier session** = a second headless session on each farm running at the DEEP provider
  (`--provider dgx -m deepseek-v4.1-flash --reasoning high`), same `~/.hermes`, its own worktree,
  kept alive by a `swarm-verify@<agent>` systemd unit (clone of `swarm-driver@` carrying the DEEP
  provider flags) — see §11.4/§11.5.
- Re-plan cadence via `hermes cronjob` (daily) on `swarm-coordinator`.
- `gaming.md`, `docs/linux-abi.md`, and this board are the only cross-agent interface points —
  no agent improvises ABI/architecture (that path is `decision`+user or the `deep` planner).

### 5.3 Autonomy guardrails (freedom within rails)

- Merge only through the gate; evidence-before-close; `decision` labels pause for Sarah.
- Real machines: **inference servers and DGX Sparks are never touched** (no repurposing, no
  test load). All execution is QEMU on the farm VMs.
- The 30-second deadlock rule from `hobbyos-run-tests` stays law: hang = defect, investigate,
  don't re-run.
- The two-arch parity rule stays law — no feature lands grey on either arch.

---

## 6. Testing strategy (three native tiers + two compat tiers)

Reuse the proven discipline; add the two new tiers for the compat track:

| Tier | What | How | Catches |
|---|---|---|---|
| T1 Host golden | pure userland logic on macOS, seconds | `make host_tests` (+ new per-game/function tests) | logic, refactor regressions |
| T2 Kernel unit | in-EL1, pre-scheduler | `./run_unit_tests.sh`, `./run_unit_tests_intel.sh` | kernel subsystems |
| T3 Integration | userland syscalls/SMP/IPC under booted kernel | `make test`, `make test_intel`; parallel `tools/run_parallel.sh`/`run_waves.sh` | ABI, scheduling, drivers |
| **T4 Differential (new)** | same static-musl binary on the Linux build VM **and** on HobbyOS-intel; compare stdout/exit/errno | `run_differential.sh` (harness from GB-007/GB-015) | syscall/errno/struct-layout parity — the compat layer's core risk |
| **T5 Playthrough (new)** | scripted QMP input + screendump assertions per game, long-duration cycles | extends `run_desktop_test.py` / `test_input.py` patterns | "launches and plays" at the acceptance level |

Every ticket's DoD names its tier(s). New game/port logic first appears as a T1 host test; the
compat layer's numbers are T4-locked before any T3/T5 work depends on them.

---

## 7. Timeline & milestones (4 weeks)

| Milestone | Target | Gate (all must be green + evidence) |
|---|---|---|
| **M0 — Infra (d1–3)** | VMs up, Forgejo live, Hermes fleet configured, branching+CI wired, baseline green | GB-001→GB-012 done; baseline suites green on `main` — **as of 2026-10-20 this clause is measurably FALSE, not merely unmet: the intel T3 tier exits RED (4 graphics FAIL lines) and `make t4_compat_parity`, a `host_tests` prerequisite, prints `PASS` having compared 0 rows whenever `musl-gcc` is absent (GB-007). "Baseline green" cannot be claimed while a skip can print a pass**. **2026-10-23: the gate is OPEN with three named exceptions (PLAN-011) — sync met, baseline captured but red on both arches (ARM 4 FAIL lines: GB-058/GB-081; intel 84: GB-057, every count a lower bound because 3.8% of console lines are corrupt — GB-082), GB-005 is a script not CI (no `.github/workflows`), and GB-001's NAS clause is split to DECISION-003. The instrument defect is gone: before GB-080 the same `GATE_CI=1` reported 7/7 tiers FAIL, four of them for measurement reasons** |
| **M1 — ABI core (wk1)** | static-musl W0 binary boots on `ARCH=intel`, differential parity | GB-013→GB-024; T4 green for W0 corpus — **T4 means leg A vs a *HobbyOS guest*, not vs host `ho_*` mocks** (2026-10-19: the host-mock leg is a proxy that has never compared HobbyOS against Linux; the device leg is GB-066 and it does not exist yet). **2026-10-23 status:** the corpus *runs* on both arches (GB-013/GB-072 Done; `::WAVE.CFG` dispatch verified both arches, ARM W0 dispatch→run→output→exit→halt) and the four `-V` gates that were held behind GB-072/073/076 now have their instrument — but the **default** wave still ends with 3 processes parked in `__stack_chk_fail` and 5 execute-at-stack faults (**GB-077**), and the device leg's row-name check has a mode-dependent verdict (**GB-079**), so "T4 green for the W0 corpus" is **not yet claimable** |
| **M2 — Devices + SDL (wk2)** | `/dev/fb0`, evdev, TLS, threads/futex under compat; static-musl **SDL app** draws+inputs | GB-025→GB-033; T4+T3 green |
| **M3 — Games (wk3)** | native catalog ≥5 playable; launcher; **static-musl Doom (W4) playable under compat** | GB-034→GB-042 majority with T1/T3/T5 evidence |
| **M4 — Acceptance (wk4)** | S0+S1 evidence pack; long-duration playthroughs pass; re-plan → Program-2 backlog | GB-043→GB-055; both arches; evidence archive on NAS |

---

## 8. Seed backlog (first wave of tickets)

~55 tickets, `GB-001+`. Weights S/M/L; `[DEEP]` = v4.1-only (all others are FLASH). The
re-plan loop (GB-048) will grow/re-prioritize this continuously — this is the seed, not the
final list.

### Milestone M0 — Infrastructure (GB-001 … GB-012)

| ID | Ticket | Wt | Depends | DoD / test |
|---|---|---|---|---|
| GB-001 | Verify built fleet (§3.3: `swarm-farm-1..3` .230-.232, `swarm-coordinator` .233, `swarm-ticketing` .234): SSH trust + passwordless sudo, shared store (NAS `/mnt/fast` or c2flash) mounted, host_tests green | M | — | all VMs answer ssh; shared store mounted; `make host_tests` PASSES on ≥2 VMs |
| GB-002 | Forgejo already deployed (§11.2, `swarm-ticketing` .234:3000, repo `sarah/gaming-program`, admin `sarah`, `swarm-bot` token) — finish: `GAMING-BOARD` project, labels, milestones, repo mirrors | M | GB-001 | board visible; bot token works via API |
| GB-003 | Board automation: webhook board→Coordinator; `swarm-bot` lane-move + comment scripts | S | GB-002 | moving a ticket posts to coordinator |
| GB-004 | Hermes fleet: profile per role, endpoints (`FLASH` default, `DEEP` coordinator-only), tmux supervisors, session-store on NFS | M | GB-001 | every worker `GET /models` shows FLASH; coordinator shows DEEP |
| GB-005 | Git workflow + merge-gate CI: branch-per-ticket, MR template, gate = T1+T2+T3 both arches + parity | M | GB-001/002 | a dummy branch fails→passes the gate |
| GB-006 | Shared build cache: sccache/ccache + the prebuilt clang 21.1.8 cross toolchain; seed from farm-1 (LLVM dist-staging already built there → rsync `third_party/llvm-21.1.8/dist-staging` to farms 2/3 + coordinator) | M | GB-001 | cold vs cached build time measured |
| GB-007 | Differential harness skeleton: builder image (x86_64-musl cross on Linux) + run-on-HobbyOS-intel + stdout/exit/errno compare | M | GB-005 | `hello` diverges/lie detector works |
| GB-008 | `gaming.md`↔board sync script (tickets must match plan §1/§7) | S | GB-002 | lint passes |
| GB-009 | Baseline capture: record current green state of all suites on `main`; CI badge | S | — | baseline archived |
| GB-010 | **[DEEP]** Write `docs/linux-abi.md` v1: syscall numbers, errno values, struct layouts (stat/sigaction/ucontext/termios/epoll), TLS ABI, uname string | L | — | doc + first T4 assertions against it |
| GB-011 | **[DEEP]** Re-derive RimWorld / DSP / TA feasibility: per-game 20K-word assessment → staging plan → `MONO`/`VULKAN`/`RTS` icebox epics | M | GB-010 | three epics on board with staged tickets |
| GB-012 | Merge selfhost kernel core (threads/futex, vm v2+memfd+MAP_SHARED, fork/exec/signals) to `main` behind arch guards + tests | L | GB-010 | T2/T3 both arches green post-merge |
| GB-064 | **Self-registering host test suite.** Every ticket PR edits the *same* `host_tests:` prerequisite line and recipe body (PRs #25–#31 all collided on it on the night of 2026-10-17/18; #27's branch even *replaced* #29's two recipe lines), so N open PRs guarantee N−1 conflicts and every merge is a hand-resolved line. Discover `src/host/*_test.c` (and the python/parity tests) by `$(wildcard …)`/`$(patsubst …)` instead of enumerating them | M | — | adding a host test needs no edit to a shared Makefile line; two branches that each add a test merge with **no** conflict; `make host_tests` still builds *and executes* every existing test (verified by grepping the run's own PASS lines, never by the build alone) |
| GB-074 | **`tools/board/claim.sh` is read-then-move, so two agents claim one ticket — measured twice in two hours, both times paying for two full DEEP gates.** GB-005-V was claimed at 00:44:27Z and 00:44:28Z; GB-010-V at 01:12:25Z and 01:12:27Z. In both cases two Verifier sessions on different farms ran the *complete* gate (a full intel T3 wave each) and posted independent, agreeing verdicts — the duplication cost no correctness and ~2 gate-hours of DEEP capacity, on a lane that is the program's scarcest resource. The claim step reads the lane, then moves it, so a second session passes the check in the gap. Deliverable: the lane transition **is** the lock — move first, and treat "already `In Progress` by another claimant" as a lost race that exits non-zero having run nothing | S | — | two `claim.sh` processes started simultaneously on one ticket: exactly one exits 0 and proceeds, the other exits non-zero naming the winner and runs no gate; a test reproduces the double claim on the old script and passes on the new one; `board.py move` prints the observed transition it acted on |
| GB-080 | **The T1 baseline's red was four-fifths the measuring apparatus: the suite graded by substring, the gate died on `PATH`, and one host test's own fixture wrote half a frame.** Measured 2026-10-23 on `main` @ `4fa985d`, `GATE_CI=1 ./tools/gate/merge_gate.sh`: all seven tiers FAIL, and **four of those reds are instrument defects, not code.** (1) `tools/t5/run_t5_tests.py` decided a test's verdict with `ok = "PASSED" in output` — so `claim_race_test.py` ("`CLAIM_RACE_TEST: PASS — …`") and `acceptance_gate_test.py` ("`ACCEPTANCE_GATE_TEST: PASS — …`") were graded FAIL while both exit 0, and a script that crashed *after* printing `ALL PASSED` would have been graded PASS. The verdict now **is** the exit status, with a stated-verdict requirement so a silent exit 0 is not a pass, and a 5-case `--self-test` runs inside every suite run (`lies.py`: prints `ALL PASSED`, exits 1 → must be FAIL). (2) **T2 failed on both arches with zero code involvement:** `mkfs.fat: No such file or directory` — the tool is at `/usr/sbin`, a background shell's `PATH` omits it, and the tier therefore "FAILed" after the build had succeeded (§11.2b #5). `merge_gate.sh` now prepends `/usr/sbin:/sbin` and, for any missing tool, exits **2 (ENVIRONMENT FAILURE)** naming it instead of reporting a red verdict. (3) **`GATE_CI=1` made the gate un-passable by construction:** `STYLE_check` is red on 6 pre-existing files and `STYLE_test` fails 2 of 86 (`src/user/grep/dfa.c` is not idempotent; one file changes its token stream when formatted) — both defects in `tools/cstyle.py`, and neither something a PR can fix. The merge gate now asks the question a merge gate asks — *did this change add a violation?* — checking only C sources changed (`src/*.c`, `src/*.h`) vs `GATE_STYLE_BASE` (default `origin/main`) and reporting cstyle's own suite instead of gating on it (`GATE_STYLE_STRICT=1` restores tree-wide). Proven both ways: 0 changed files → PASS, changed misformatted file → FAIL. (4) `tools/t5/acceptance_test.py` wrote **8 pixels into a 4×4 (16-pixel) frame** (`… * 4`), so its "a frame with distinct pixels IS live" check failed against a *correct* `ppm_live` and reddened T1 — fixed to `* 8`, with a truncated-frame case added so the fixture cannot silently regress. Also minted from this run: **`tools/cstyle.py` is not idempotent on `grep/dfa.c`** — a real formatter defect that will fight every future PR (own card) | M | GB-005, GB-064 | `make host_tests` green with the grader's own `--self-test` in-suite; `GATE_CI=1` runs to a verdict (not an environment abort) on a provisioned box; a clean tree PASSes the style tier and a misformatted changed file FAILs it; `acceptance_test.py` passes against the unmodified `ppm_live`; a script that exits non-zero while printing `PASSED` is graded FAIL by the suite |
| GB-081 | **ARM T3's CMPTEST fails two checks on current `main` — a fresh red in a program that passed at the 2026-10-17 baseline.** Measured 2026-10-23 18:01Z by the Coordinator's `GATE_CI=1` gate run on `origin/main` @ `4fa985d` (`obj/t3_arm.log`, 31,990 lines, `halted=1`, 85 programs exited, **4 FAIL lines**): `[CMPTEST] FAIL /EXPAND.BIN-b quotes differing bytes` / `stdout missing "is 154"; got ""` and `[CMPTEST] FAIL -s prints nothing` / `exit status 2 != 1`, then `[CMPTEST] FAILURES: 2`. Both failing checks are **content/exit-status disagreements about the program's own output**, while the other 18 CMPTEST checks pass (`-l`, `-l -b`, EOF cases, `--help`, `-i`, `-n`, stdin) — so this is not a broken build. Two live hypotheses, and they are not the same bug: (a) `cmp`'s quoting path emits nothing where GNU prints `is 154`, i.e. a real content defect in the quoting logic; (b) the test's own stdout capture under an 8-CPU console lost the text. **Distinguish them before fixing:** run the same `CMPTEST.BIN` on the ARM tier with `-smp 1`, and run `cmp_test` at T1 (host) where there is no SMP console at all — a host reproduction means (a), a host pass with a uniprocessor guest pass means (b). Separately note the console is still interleaving *across writers* mid-line (`[ARRIVED] [CMPTEST] PASS SED.BINv2 loader: PID=15 …`), which is what makes (b) plausible and is worth naming as its own defect if confirmed | M | — | `T3_arm` on `main` prints **0 FAIL lines** and CMPTEST completes; the cause is named with the host-vs-guest and 1-CPU-vs-n-CPU comparison quoted in the ticket; a deliberately-broken `cmp` quoting path (or deliberately-lost capture) reproduces the failure so the fix's test can fail; `make host_tests` runs the same assertions |

### Milestone M1 — ABI core / ELF loader (GB-013 … GB-024)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-013 | **[DEEP design, FLASH exec]** ELF static loader (static-PIE `ET_DYN`; `ET_EXEC` parsed-but-refused) in loader path; coexist w/ flat loader; magic dispatch | L | GB-012 | static-PIE binary maps+boot, entry set right (`docs/elf-loader.md`) |
| GB-014 | Linux x86_64 syscall translation table: core set (open/read/write/close/lseek/stat/fstat/brk/mmap/munmap/fork/execve/waitpit/exit_group/writev… ) | L | GB-013, GB-010 | T4 W0 corpus pass |
| GB-015 | errno + struct-layout parity locks (T4 assertion suite from GB-010) | M | GB-014 | T4 fails on any mismatch |
| GB-016 | `uname` shim reporting Linux-ish identity (SDL/games sniff it) | S | GB-014 | T4 matches |
| GB-017 | `/dev/null`, `/dev/urandom`, `getrandom` shims | S | GB-014 | T4 |
| GB-018 | `poll`/`select`/`pselect6` under compat | M | GB-014 | T4 socket+pipe cases |
| GB-019 | `ioctl` subset: `TIOCGWINSZ`, `F_GETFL/SETFL`, `TCGETS` | M | GB-014 | T4 |
| GB-020 | `getdents64` + `/proc/self` minimal stubs | M | GB-014 | T4 |
| GB-021 | Threads under compat: `clone` → HobbyOS threads; `futex` wait/wake parity | L | GB-012, GB-014 | T4 multithread test |
| GB-022 | TLS ABI: `arch_prctl(ARCH_SET_FS)` (x86) / `TPIDR_EL0` (arm) on context switch | L | GB-021 | T4 `errno` per-thread + `__thread` vars |
| GB-023 | Real signal delivery: frame build, `sigreturn`, `rt_sigprocmask` | L | GB-021 | T4 signal test |
| GB-024 | **M1 gate:** W0 corpus (hello, cat, sh, coreutils-static) boots on intel + writes expected output, T4 green | M | GB-013..23 | gate evidence |
| GB-057 | **ARCH=intel T3 prints 222 failing checks (sed/grep/cmp/tsort) yet exits 0** — the intel integration tier is not a gate (minted from the GB-009 baseline run) | L | — | `make test_intel` with 0 FAIL lines + a deliberately-broken check fails the tier; `make test` (ARM) stays green. **Split 2026-10-19:** **a** (no halt at end of wave, 7/8) is **fixed** — `8995009`, waker-liveness predicate + `[ORPHAN-WAIT]`, two consecutive tiers `halt=2 orphan=1` (`evidence/gb-057/t3intel_probe56_wave_halts_orphan_named.log`); **b** (premature halt, 1/8, probe54) and **c** (LAPIC fault, ≤1/7) remain **open**, and b is the one that can still make a green tier a lie — `T3_MIN_LINES` caught it once, which is the only reason it was not read as a pass |
| GB-058 | **ARCH=arm T3 carries one real failure the exit-status-only tier called green: `SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST`** (`evidence/gb-009-baseline/t3_integration.log:5329`, surfaced by `tools/t3_tier.sh police`) | M | GB-056 | the stall is explained and fixed (the watchdog process-table dump is the evidence); `make test` (policed, ARM) green. **2026-10-19 — do this before writing any code:** GB-063's whole hypothesis was that this stall and the intel never-halt were *one* predicate defect, and that predicate changed on `main` (`8995009`, `4cefd7d`). Re-run the policed ARM tier on current `main` first; if it is green the ticket closes as a consequence, if it still stalls the dump now names the live set and the work is scoped by that |
| GB-060 | **x64 context-switch durability: a `proc_lock` critical section must not be able to strand the lock.** Measured on GB-057: a CPU that stops inside the scheduler's IRQs-off section holds `proc_lock` forever and every other CPU waits behind it — 9 `[HOLDER] cpu=2 sched_stage=5 picked=45` reports, exactly one claimant, no lock cycle, no exception (`evidence/gb-057/t3intel_probe3_*`). Whatever the immediate cause, the scheduler's critical sections need an owner + a failure path that releases (or a watchdog-bounded break) instead of a silent machine-wide freeze. **Update 2026-10-18: two shapes, and they must be separated.** The *stopped* holder is fixed and proven — `spinlock_t.owner`, `spinlock_release_if_held_by_me()` on both fatal halt paths, and a halt-keyed break that frees a dead owner's lock and re-acquires it normally (T1: `src/host/locksteal_test.c`, 14 checks, running in-suite); it was observed working in the wild (probe16) and declining correctly in probe19. The *wedged-alive* holder is measured and **open**: probe19 showed a halted CPU release `proc_lock`, a live CPU re-acquire it and then loop while holding it, with 46 `LOCKFOREVER` and no `[STOLEN]` — the break declined because the owner is running, not stopped (`evidence/gb-057/t3intel_probe19_steal_declined_live_owner.log`). Remaining work = a break keyed on **per-CPU progress heartbeat** (not elapsed time, which would free a live-but-slow holder's lock), with the threshold stated and printed on any break. **Second shape landed 2026-10-18 (`24a7f8a`):** `lock_stallbreak_ok()` breaks a lock whose recorded owner has stopped taking timer interrupts for `LOSTWAKE_DEAD_OWNER_MS` — the lost-owner reaper's own constant, reused so the two cannot drift — freeing it and re-contending exactly like the halt case, announced as `[STALLBREAK]` with the stale age; **T1 proves the boundary exactly** (`locksteal_test_host`, 27 checks: one ms inside the grace period protected, at the boundary broken, fresh heartbeat never broken, halt rule wins immediately). Wild (T3) run in flight as probe20 | M | GB-057 | **both** shapes recover: a deliberately-stopped holder *and* a deliberately-wedged holder do not freeze the machine, provable in T1/T3; the holder is named and the lock recovered in each |
| GB-061 | **x64 resume-path exception: `general_interrupt_handler -> enter_user_space -> restore_user_sp_helper` faults (CPU 6, pid 42)** — resolved stack from `evidence/gb-057/t3intel_probe2_exception_not_wedge.log`; a fault in the interrupt-driven return-to-user path is a defect with or without the GB-057 wedge, and it has a reproducible register/stack dump to work from | M | — | the stack is explained and the path survives a stress run: no exception dump in a clean T3 intel run |
| GB-063 | **Scheduler liveness accounting: a finished wave must reach `System halt`.** `schedule()`'s halt/`any_alive` branch is never reached because a slot parked in a state that is neither READY nor FREE/EXITED/ALLOCATED/THREAD_DONE is counted *alive*, so the machine idles forever instead of halting — measured all-idle-no-halt in `evidence/gb-057/t3intel_probe17_false_pass_loader_gp.log` (every CPU in `safe_wfi`, one slot parked), and consistent with GB-058's `SHELLTEST WATCHDOG: protocol stalled 25 s` (a test waiting on a process that is never scheduled again, machine otherwise healthy). **Update 2026-10-18 (Coordinator):** the *measurement* half is landed — when no READY work exists and the predicate still counts live work, the scheduler prints every live slot (`slot/pid/state/parent/claimed_by/name`), once per idle stretch, annotated in place for the two unwakeable shapes (RUNNING with no CPU on it = stale claim; a parked WAIT_SPAWN/WAIT_CHILD with no parent). probe20 is why: 30,179 console lines, `proc_lock` free, no strand, no kernel fault, and still no `System halt`. The predicate itself is **deliberately untouched** until that report has been read — `BLOCKED` on a timer is legitimately alive and a wrong predicate halts healthy waves, which is strictly worse than a wave that fails to halt. Second increment = the **owner-side bounded wait**: give a parked `WAIT_SPAWN` caller a deadline and resume it with `-EAGAIN` past it, which covers worker death, worker starvation and slot reuse with one rule and never touches another context's state. Ruled out by reading, so nobody repeats it: both spawn paths *do* release the caller when no worker slot is free, and both workers *do* unpark on completion — so the reachable leak is a worker that dies before unparking (the loader `#GP`, GB-013). Also ruled out by measurement: a scheduler-side forced reap (my own attempt) is wrong, because the live spawn path re-parks the slot underneath it — the same slot "reaped" 11 times in a row | M | — | the counter's state set is enumerated; a parked/idle slot is not counted live (or is reaped); a T3 run on the affected arch prints `System halt` where it previously idled forever, and each state it used to count is named. **CLOSED 2026-10-19 — both halves landed on `main`:** the counting half is `8995009` (a `BLOCKED` slot is alive only while it has a possible waker; 60 s budget; `[ORPHAN-WAIT]` names slot/pid/image/parent and states the slot is **not** reaped) and the owner-side half is `4cefd7d` (bounded parked `WAIT_SPAWN`, `-EAGAIN` past it, set at all four park sites on both arches). Proof of the DoD sentence itself: two consecutive intel tiers reach `System halt` with the orphan named (`evidence/gb-057/t3intel_probe56_wave_halts_orphan_named.log`, tail `[ORPHAN-WAIT] slot=13 … / System halt`), and the states it used to count are `BLOCKED`-on-a-dead-waker and `WAIT_SPAWN`-with-a-dead-worker |
| GB-062 | **intel FAT big-file test is inconsistent across runs: `[FATBIG] FAIL tail checksum clean (got=0x0000000000000800 want=0x0000000000000000)` in one run and `[FATBIG] FAIL short` in another, while it passes in two other intel runs of the same night and passed on ARM** — evidence `evidence/gb-057/t3intel_probe16_gb060_release_fired.log` and `t3intel_probe14_idle_stuck_no_halt.log` versus `t3intel_probe15_waitspawn_blocker.log` and `t3intel_probe17_false_pass_loader_gp.log`. Two different sub-checks failing in different runs is the signature of a timing/ordering defect, not a wrong constant: count it as flaky-on-intel (2 of 4) rather than deterministically broken | M | — | FATBIG passes on intel in repeated consecutive T3 intel runs, and the cause is named with evidence. **DoD amended 2026-10-18:** the original second clause ("the ordering the FAT write path depends on is named") assumed a storage-ordering cause; the verified cause is a signed-`char` comparison in `fatbig_test.c:95` (half the tail has bit 7 set, sign-extended against an int), reproduced on a host three ways and confirmed by consecutive intel runs, with the wrong NVMe conclusion corrected in `docs/fatbig-intel-delta.md` |
| GB-065 | **Device-tier blocker (2026-10-18): the only FAIL lines left in a complete intel T3 wave** — **`graphics_draw_line`'s pixel assertions fail on intel: GRAPHICS.BIN reports `FAIL: line startpoint`, `line midpoint`, `line endpoint`** — surfaced when GB-057's step bound let the graphics test complete for the first time; before the bound the process hung *inside* this function, so these assertions were never reached. **Attribution closed 2026-10-18 (worker `wgfx`, verified by me):** not the bound (the failing diagonal `(10,600)→(110,700)` needs 201 iterations and uses 101, so the bound cannot fire), not the line maths (32/32 host tests, 217 checks, exact in-OS call reproduced on a coherent buffer, `c654013`). Root cause = **`mmu_map_user_framebuffer()` mapped two 2 MB huge pages (4 MB) for an 8 MB 1920×1080×4 framebuffer**, so every pixel at y ≥ 546 was unmapped; the diagonal lives at y = 600..700 and the rect check at y = 100, which is exactly the observed asymmetry. **The fix is written and NOT on `main`** (`src/kernel/arch/x64/mmu.c`, map `ceil(fb_bytes/2 MB) = 4` pages; commit `3a86967` exists only on `ticket/65-line-assert` because PR #45 was merged at `c654013`, *before* this commit existed) | S | — | The three line assertions pass on intel with GRAPHICS.BIN completing. **Next action, stated so it is not re-derived:** rebase the single `3a86967` hunk onto `main`, build intel, rerun `tools/t3_tier.sh intel`, and attach the run where `FAIL=` is empty — that is the first intel tier that can be cited as green |
| GB-066 | **T4's device leg does not exist — the M1 gate has never been measured against HobbyOS.** `src/host/t4_parity.sh` calls leg A (static-musl, real Linux) and leg B *the same probe compiled for the compat surface and run on the host against glibc `ho_*` mocks*, deferring the in-guest leg to "once the ELF loader lands"; `--device intel` **SKIPs**, and a SKIP is not a pass. So every "T4 green" so far compares Linux against a *model* of HobbyOS, and GB-024's gate sentence cannot be satisfied by any kernel fix. Deliverable: a probe in `src/user/` built **both** ways (Linux static-musl for leg A; HobbyOS intel for leg B), staged into the guest through `T3_STAGE` (`tools/t3_tier.sh`), its stdout captured from the serial console, and a `t4_device.sh` that diffs **return value / errno / stdout** for the W0 row set and **exits non-zero when leg B is absent** instead of skipping. The tier must assert the probe's *arrival in the guest* (the guest's own dispatch/output line), never an image listing — probe57's `mdir` output grepped for `pie.bin` could only ever print 0 (`mdir` renders `PIE      BIN`), and that cost two runs | L | GB-013, GB-014, GB-024 | leg B runs **inside HobbyOS-intel** for the W0 corpus and the diff passes; a deliberately-wrong shim makes the same harness FAIL (it must be able to report both); a missing/absent probe fails the tier rather than skipping it; the row set is the one GB-015 locked (numbers, errno, struct layout), not a new one |
| GB-067 | **Host-staged files are read as ABSENT by the FAT16 driver and then corrupted in place.** Measured on intel, current `main`: a file copied onto `disk.img` after the build (`mcopy -o -i disk.img /tmp/pie_hello ::PIE.BIN`, confirmed by `mdir` and by `od` at offset 284,352 = **sector 555, entry 182 of a 512-entry root**) is **not visible to the guest**. The driver's own dump proves its mount arithmetic is identical to `od`'s (`fat_sector=32 root_dir_sector=544 root_dir_sectors=32 root_entries=512`), yet a raw sweep of all 32 root sectors finds **no entry beginning with the query's first three bytes** — so the read of that sector returns empty. The driver then takes its create-on-miss path and stamps a name-only entry (`attr=0 cluster=0 size=0`) over the real slot, so the image is **damaged** and the next boot inherits it. The 167 files staged *by the build* read correctly every boot, so the correlation is with entries late in the directory; correlation is not cause, which is why the ticket is written this way. Two defects are visible and either is independently wrong: (a) a stale/zeroed sector read, (b) an entry-initialisation bug that leaves `attr=0`. **This is the reason GB-013's DoD is unmet and GB-066's device leg cannot run**, and it silently masquerades as "the fixture was not there". Deliverable: a host-staged file is visible to the guest, a create-on-miss never emits a partially initialised entry, and a regression test that stages a file post-build and asserts the guest can read byte-exact content | S | — | post-build stage of a 4 KiB file, read back in-guest byte-exact; `attr != 0` on any entry the driver creates; evidence in the ticket |
| GB-068 | **M1 BLOCKER, highest priority (§4.3, 2026-10-19): 48 distinct processes are SIGSEGV-killed every intel wave, 31 at the identical address `0x7559DBC8` = `<stack base> + 0xDBC8`.** **Every intel T3 wave carries 44–55 user-mode memory faults and still reports "0 FAIL lines".** Measured across seven runs on `main` (`dist_run1` 54, `live_run1` 55, `final1` 48, `final2` 44, `quiet` 44, `badelf` 54, `fat3` 43) — **including runs with no ELF process whatsoever**, so this predates the GB-013 work and is not caused by it. The lines are `[KERNEL] pid=N (IMAGE) memory fault VA=0x755xDBC8 PC=0x100000…` in `SUBPRB.BIN`, `SHTEST.BIN`, `SHTEST2.BIN`, `SHTEST3.BIN`, `BLKTEST.BIN`, `FATBIG_T.BIN` — a stack-looking address in processes that otherwise "pass". The tier's verdict counted only `FAIL` lines meanwhile, and its fatal-fault check looks for `FATAL kernel-mode fault`, a different string, so these were invisible for the whole session. **The tier now prints `N user memory faults` on every verdict line** (2026-10-19), which is the measure this ticket is judged by. Deliverable: the cause named, and the count driven to 0 on a full intel wave — or an explicit, evidenced statement of why a non-zero baseline is legitimate, written into the plan | M | — | `T3_intel` verdict prints `0 user memory faults` on a full wave; the fault's mechanism is named in the ticket with evidence; a regression guard fails the tier if it regresses |
| GB-069 | **Kernel `#PF` at `cr2=0x1FC001008` (NVMe BAR window) kills the wave — and the mapping is NOT too small: `cpu_pd7[480]` installs a **2 MiB page** (`0x9B` = P\|RW\|PWT\|PCD\|PS) covering `0x1FC000000–0x1FDFFFFF`, so the faulting offset `+0x1008` is inside it.** Measured 2026-10-19: a run ended at 453 lines, `halted=0`, 4 FATAL kernel-mode faults, `vector=14 rip=0x7002A8E5 cr2=0x1FC001008`. Three surviving mechanisms, all different to fix: (1) **PD entry reuse** — `cpu_pd7[480]` is a hardcoded slot at a hardcoded VA in the kernel low range, so any other 4 KiB mapper inside that 2 MiB window rewrites it as a PTE-table pointer and the BAR page disappears; (2) **wrong CR3** — the mapping lives in the kernel's `cpu_pd7`, and a doorbell write from process context uses a CR3 whose entry 480 may be empty; (3) protection. Load-dependent, which is why it read as flakiness. Deliverable: an intel T3 run performs disk I/O and halts with 0 FATAL kernel-mode faults, and the ticket names which of the three it was | S | — | no `vector=14` with `cr2 ∈ [0x1FC000000, 0x1FDFFFFF)` on a run that does disk I/O; the mechanism named with the fault-site evidence (`error_code` P/W/US bits, current CR3, and `cpu_pd7[480]` raw value vs the value written at init) |
| GB-070 | **The M1 gate names a corpus the harness never runs: the boot wave loads 70 programs and only `HELLO.BIN` is from the W0 corpus.** Measured 2026-10-19 on a complete intel wave (63 programs exited, halt, `slot=63 MMTEST.BIN` reached): exit lines for `HELLO.BIN` = 2; **`CAT.BIN`, `SH.BIN`, `LS.BIN`, `GREP.BIN`, `WC.BIN`, `SED.BIN`, `CMP.BIN`, `CUT.BIN`, `TR.BIN`, `SORT.BIN` = 0 each.** `src/kernel/main.c`'s `test_wave_loader` lists 70 programs and every one is a `*TEST`/`_T` binary plus `HELLO.BIN`; `cat`, `sh` and the coreutils are on the image and are **never executed by the wave**. So GB-024's first clause — *"W0 corpus (hello, cat, sh, coreutils-static) boots on intel + writes expected output"* — has never been measurable, and no amount of kernel fixing would satisfy it while the harness does not run the corpus. Deliverable: a corpus run in the intel wave (or a dedicated `MODE=corpus` tier) that boots each corpus member as a program, captures its stdout, and compares it byte-exactly against the same binary run on the Linux box (the T4 style GB-066 established), with a per-member PASS/FAIL line | M | — | every W0 corpus member appears in an intel wave's exited-programs set and its output matches the Linux leg byte-for-byte; a deliberately-broken member fails the run |
| GB-071 | **Family B delivery: a user process holds a kernel-range pointer inside its own CPU's kernel scratch frame — located, delivery unknown.** Minted by PLAN-006 (2026-10-20) when GB-068's family B was split from family A. Measured: the fault address lies in `[target_sp-296, target_sp)`, `target_sp = &__stack_top - cpu*0x10000 - 4096` (`process.c:1557/3141`), `region=NONE` and CR3-equal are *correct* readings; the displacement moved from `-0x28` to `-0xA8` when `__stack_top` was read from a stale build, so it is **not** a constant. **Retired by measurement:** the iretq RSP slot / `enter_user_space → restore_user_sp_helper` (`IN_SCRATCH=1` on 0 of 14 resumes, 45 faults in the run), plus the TLS/`.bss`/v1-base/CR3 list on GB-068. Remaining delivery candidates: a kernel address handed back by a syscall, or a struct field written from a kernel pointer. Deliverable: the delivery named with pid/instruction/register evidence, the fix, and a regression guard | M | — | full intel T3 wave prints `0 user memory faults` of the family-B shape (parsed/raw counts quoted); the delivery is named in the ticket; the guard fails if a kernel address reaches a user process through the fixed path again |
| GB-072 | **A staged program is never dispatched — the boot wave is a hardcoded list, so nothing added to `disk.img` can ever run.** Measured 2026-10-20 by four independent DEEP verifier waves on `main` (`0d38100`/`e2b8e93`): `T4PROBE.BIN` staged post-build and confirmed on the image (`mdir` → 19328 bytes) appears **0** times in 767 / 946 / 999 / 1428 console lines while 63 programs exit, and GB-070-V's corpus gate reports `FAIL HELLO/SH/CAT/LS — not in the intel wave's exited-programs set` on a wave that exited 63 programs. The cause is not the loader: `src/kernel/main.c`'s `test_wave_loader` is **97 literal `load_and_run_program_in_scheduler("X.BIN", …)` calls**, so a staged probe, a W0 corpus member and an SDL binary are all file-system-present and scheduler-absent. This is the single wall behind GB-066, GB-070, GB-013, GB-016…021, GB-068-B, M2's SDL leg and M3's W4 Doom leg. Deliverable: the wave's program list becomes data (a manifest read from the boot image, e.g. `::WAVE.CFG`, or a kernel/MODE selection) so adding a program needs no kernel rebuild | M | — | a program named **only** in the manifest appears in the wave's exited-programs set and prints its own arrival line, on **both** arches, **without a per-program `.c` edit** — adding program N is one `::WAVE.CFG` line and adds no `load_and_run_program_*` literal (a single boot-time manifest read may live in, or be called from, `src/kernel/main.c`; the 97 literals may remain **only** as the no-manifest default); the default (no manifest) still runs the existing 97-program list — same exited count, no coverage lost; a manifest naming an absent file FAILS the tier rather than being silently ignored |
| GB-073 | **The device probe emits one row, and every `-V` DoD names six it does not have — so six M1 verification tickets are unmeasurable by construction.** `src/user/t4_probe.c` contains exactly one `prow("write3", …)`; `uname`, `urandom_*`, `null_*`, `getrandom_16`, `poll_*`, `select_*`, `fcntl_*`, `ioctl_*` and `dent_*` exist **only** in the host-mock probe `src/host/t4_compat_probe.c`, which is a *model* of HobbyOS, not the guest. GB-016-V, GB-017-V and GB-018-V each returned RED on this as their **second** independent ground (after the GB-072 dispatch ground) — three DEEP gates spent proving a row set that cannot exist yet, with GB-019-V/020-V/021-V queued behind the same defect. Deliverable: **one** row table both probes emit (generate the guest probe's rows from the shared list, or a `--check` that diffs the two row-name sets), and `t4_device.sh` failing when a row the DoD names is absent | S | — | `grep -c '^row=' obj/intel/t4_legA.out` covers every row the M1 `-V` DoDs name; a `--check` in `make host_tests` fails when the guest and host probes disagree on the row-name set; deleting one row makes the gate FAIL (a smaller diff is not a pass) |
| GB-076 | **The `-V` gates still pass vacuously: the shared row set holds 11 rows and none of the 10 `fcntl_*`/`ioctl_*`/`dent_*` rows that GB-019-V, GB-020-V and GB-021-V DoDs name.** Measured on `main` @ `5407aa1` by the swarm caretaker and re-confirmed 2026-10-21 at `badc665`: `tests/fixtures/abi/t4_rows.golden` = 11 names (`write3 uname_rc uname_sysname_linux uname_machine_x86_64 getrandom_16 urandom_open urandom_read16 null_open_w null_write3 null_open_r null_read1`), `src/user/t4_probe.c` emits the same 11, `grep -c 'fcntl_\|ioctl_\|dent_'` over both = **0** — while GB-019-V/020-V DoDs name `fcntl_getfl_rdwr`, `fcntl_setfl_append`, `fcntl_getfl_after`, `fcntl_append_bit`, `ioctl_tiocgwinsz_entty`, `ioctl_tcgets_entty`, `dent_created`, `dent_opendir`, `dent_total`, `dent_have_alpha/beta/charlie` (host-model only). So `make t4_device_check` **agrees by construction** on a set that cannot contain what those cards grade: GB-073's own defect class, one level up. This is the second of the two walls behind the three `blocked` verify cards (#87/#88/#89) and it is the cheap one to clear — no kernel, no dispatch, no guest: the rows live in the ioctl/getdents shims already under test elsewhere | S | GB-073 | the row names the three `-V` DoDs cite are in `t4_rows.golden` **and** emitted by the guest probe, so `t4_device_check` can fail on them; the request list is declared once in a machine-readable file rather than in the DoD prose; the caretaker's measurement becomes the failing test first (delete one `dent_*` row -> `--check` FAILS, restore -> PASSES); the host model and guest probe still agree on the full set |
| GB-077 | **Harness programs corrupt their own stacks: 5 execute-at-stack faults and 3 processes parked in `__stack_chk_fail`, all in default-list programs.** Measured on the default 97-program intel wave: `pid=57 PROCTEST.BIN VA=0x17FFFFFF84 PC=0x17FFFFFF84 rw=rx in=PROT region=0x17FF800000+0x00800000 prot=3` (execute at a valid stack VA), `pid=38 IPC_T.BIN` the same shape, `pid=44 MMTEST.BIN` at `VA=0x1800003D34` `in=HOLE`, and `c0=18 c2=26 c7=2` never released because `pid=18 rip=0x7002CAE0` is inside `__stack_chk_fail`. **Proven to be these programs and not the gate's corpus**: the same build and arch with a corpus-only manifest gives `0 faults, 1 boot, halted=1`. **Proven not to be the kernel's entry path**: `[ENTRYSTK]` shows the 64 bytes below `sp` are zero at process entry (x3). So the corruption is generated by the programs' own execution after entry — a return address overwritten on the stack, or the same event caught by the canary | M | — | the default wave completes with **0 user memory faults and 0 processes parked in `__stack_chk_fail`**, `halted=1`; each affected program is either fixed or shown to exercise an unsupported ABI path and moved out of the default list with a stated reason |
| GB-078 | **ELF programs got no SysV startup block: `%rsp` pointed at the stack top with nothing above it, so a musl `_start` died reading `argv`.** Measured on a real `-static-pie` probe entered through the ELF path: `memory fault VA=0x1800000008 PC=0x1000001C5D rw=r in=HOLE rdi=0x17FFFFFFF8` — `%rsp+0x10`, the `argv[0]` slot, eight bytes above the stack region. `argv` existed only in the PCB (`proc_set_argv_array`), which `sys_readargv` serves to a HobbyOS program and a Linux binary cannot see. **Fixed `d10eed6`**: `[argc=1][argv[0]->name][NULL][envp NULL][AT_PAGESZ=4096][AT_NULL][0]` written 16-byte aligned below the stack top via `vm_kwrite`, `%rsp` pointed at `argc`, block written before the entry is set. **faults 1 → 0**, arrival clean, exit clean. Does not yet produce output — that is the syscall dispatch (GB-014) | S | GB-013 | an ELF passes its startup block: no fault at `%rsp+0x10`, `main` sees `argc=1` and `argv[0]`, and `AT_PAGESZ` reads 4096 |
| GB-079 | **`t4_device.sh --check` gives two different verdicts on the identical tree, because one of its two modes reads build output instead of source.** Measured 2026-10-23 on `origin/main` @ `3621f1c`. With `obj/t4_legA.out` present the check reads the probe's **emitted** rows (39) and PASSes; with it absent it falls back to the probe's **source** rows (41) and FAILs on `pipe_write` and `sock_write`, two rows `src/user/t4_probe.c:130/171` emits only on an error path. So the gate's verdict is a property of whether a build artefact happens to exist, and its source mode is red on rows a passing run never emits (`evidence/gb-079/`). The skip line is even printed as `fail: leg A (...) absent — will skip emitted-row re-check`, i.e. it announces a skip and does not fail the run. This is the instrument that grades all six M1 `-V` cards, so a verifier who runs it cold and a verifier who runs it warm can post different verdicts from the same commit | S | GB-073 | `--check` returns the **same** verdict with and without `obj/t4_legA.out` on an unchanged tree (both PASS), and the mode it used is printed in the verdict line (`mode=emitted|source`) rather than being inferable from a lowercase `fail:` note; an extra *unconditional* probe row still FAILs in **both** modes (prove it by adding one and running each); the error-path rows are either in `t4_rows.golden` or explicitly marked conditional in the probe, not silently unreachable |
| GB-082 | **Console writes from different writers interleave *inside a line*, so a `FAIL` line is not a message and every FAIL count is a lower bound.** Measured 2026-10-23 by the Coordinator's gate run on `4fa985d`: **1,422 of 37,299** non-empty intel console lines (3.8%) and **282 of 31,586** ARM lines (0.9%) contain a second message glued mid-line — e.g. `A mkdir /SUB1 r=[ARRIVED] SHTEST.BIN rc>0`, `SH.BIN[WAVE-TRY] [EXITPC] pid=13 …`, `start_scheduler called on CPU [KERNEL] Core 7`. The consequence is not cosmetic: classifying the intel tier's 84 FAIL lines line-by-line, **25 of 59 `[SEDTEST]` lines (42%) and 4 of 7 `[TSORTTEST]` lines (57%) are damaged** — the test's own message truncated or replaced by another writer's text — so GB-057's headline number cannot be diagnosed until this is fixed, and the earlier "222 FAIL lines" figure was measured through the same damage. `1fbe4b1` fixed the *user* path taking `uart_lock` per byte; what remains is **across writers** (user vs kernel vs other CPUs), which is a different lock scope. Deliverable: one atomic write per line for every console writer (or a single serialising path), and an **instrument in the tree** — `tools/t3_console_integrity.py` — that counts mid-line-glued lines with a stated rule, prints `N glued console lines` on every T3 verdict line (the way `N user memory faults` already rides the verdict), and is falsified by a deliberately non-atomic writer | M | GB-057 | **The rule is the one in `tools/gate/t4_glued.sh` — 2+ fused records on one line — not the mint's looser "any second message" rule.** The two disagree by 3× and the ordering flips (in-tree counter: intel **499**, ARM **540**; mint's rule: intel 1422, ARM 282), so a DoD that does not name one rule invites a reader to average them. Both arches report `0 glued console lines` under the in-tree counter on a full wave; the counter's own self-test proves it can fail (a non-atomic writer fixture → count > 0, restored → 0) **and refuses an absent log (exit 2)** — a missing serial is not a clean serial; `grep -a FAIL obj/t3_<arch>.log` lines are attributable to the tag in front of them, and the next GB-057 measurement quotes that attribution ratio |
| GB-083 | **Leg B of the T4 host differential links `compat.c` and never calls it — the "wrong shim must FAIL" criterion is inert.** Measured 2026-10-23 by a DEEP verifier on `5f6e3c3`: changing `ho_open()` in `src/host/compat.c` to return `open(...)+1000` leaves `make t4_compat_parity` at `PASS (exit 0)`. The probe never includes `libc.h`, `src/user_include/unistd.h` does not exist, and nothing remaps `open`→`ho_open` for leg B, so the mock layer is **linked and never called**: the host leg has never compared the shims, only two builds of a probe that does not use them. GB-066's device leg is the real arbiter of kernel behaviour, but this leg is what prints `PASS` in every `make host_tests`, so a harness that cannot fail on a wrong mock is a green line carrying no information — the class GB-079/GB-080/GB-082 existed to remove | S | GB-007, GB-066 | flipping any single `ho_*` return value makes `make t4_compat_parity` exit **non-zero**, and restoring it exits 0; that flip is a `--self-test` case inside `make host_tests`; the harness header or `docs/linux-abi.md` states in one paragraph what leg B proves and what it does not |
| GB-084 | **`open()`'s optional mode argument is relied on by 52 call sites, which is why the host shim cannot honour a supplied mode.** `tools/board/scan_open_mode.py` (minted with this card) finds **52** `open(path, …O_CREAT…)` calls in `src/user` + `src/host` that pass **no mode** — the HobbyOS kernel defaults it, so the tree is internally consistent, but GNU/Linux reads the mode from whatever the ABI register held, which is why GB-083's faithful `ho_open` created files with mode `0000` and turned **eleven** `make host_tests` checks red (task list save/load, integration) the moment it was merged. The interim fix (on `main`) makes `ho_open` default to 0644 and **ignore** a caller-supplied mode — correct for the tree, and a real fidelity gap for the compat track: a musl binary that passes `0600` gets `0644`. Deliverable: every in-tree caller passes an explicit mode, `ho_open` honours `va_arg` again, and the scanner runs in `make host_tests` so a new 2-argument `open(O_CREAT)` fails the suite | M | GB-083 | the scanner reports **0** two-argument `O_CREAT` calls; `ho_open` honours a supplied mode (a test creates a file `0600` and asserts the mode read back, and a file created via the 2-arg path is a build error at review time); reverting one call site to the 2-arg form makes the in-suite scan FAIL |
| GB-085 | **Host tests share fixed `/tmp` paths, so two concurrent suite runs corrupt each other — and every farm runs two or three agent sessions.** Measured 2026-10-23 on the coordinator: two overlapping `make host_tests` runs produced **11 phantom failures** (`FAIL: load: reads TODO.TXT …`, `FAIL: save: TODO.TXT contains exactly the serialized list`, `FAIL: checkpoint round trips every cell`) that all pass when the suites are run one at a time. Cause: `src/host/apps_suite_test.c`/`tasks_test.c` write the fixed path `/tmp/TODO.TXT` and `src/host/antfarm_test.c` writes `/tmp/ANTFARM.TST`, so the second run's `unlink()`/create lands in the middle of the first run's round-trip. On the fleet this is worse than on one box: each farm hosts a worker **and** a verifier session, both of which run `make host_tests`, so a red T1 on a farm cannot currently be attributed to the tree — which is the single most expensive ambiguity this program has (it is what GB-006's worktree false-reds and GB-080's instrument work were both about). Deliverable: every host test that touches the filesystem uses a per-process directory (`mkdtemp` honouring `TMPDIR`), and a regression test proves it — two runs started simultaneously both exit 0 | S | GB-064, GB-080 | two `make host_tests` runs started simultaneously on one host both report `Checks failed: 0` (the test that proves it starts both and compares); `grep -rn '"/tmp/[A-Z]' src/host/*_test.c` finds no fixed path; a deliberately shared path (one test reverted) makes the concurrency test FAIL rather than passing by luck |
| GB-086 | **`POLLTST`'s select-timeout assertion passes at 8 CPUs and FAILS at 1 CPU — and the diagnostic that would say by how much prints nothing.** Measured 2026-10-23 by the Coordinator on `origin/main` @ `2d92fe2`, ARM tier, `T3_SMP=1` vs `-smp 8`: `POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): PASS` at 8 CPUs, `FAIL` at 1 CPU, with `POLLTST elapsed_ms=` printed **empty** — `print_dec((long)elapsed)` produced no digits, so the one number that decides "early return vs ignored timeout vs too-tight bound" is not in the log. The check's own comment says the property is *"no instant return and the timeout is honoured (a broken select returns ~0ms; a timeout-ignoring one blocks until the deadlock rule fires)"*, so the failure is one of those two and the log cannot currently tell which. Deliverable: the elapsed value is readable in the serial at both CPU counts (printed on one line with the check's name, and via a path GB-082's glued-line defect cannot eat), then the cause is named — kernel timer/`select` early return, or an assertion bound that one CPU legitimately violates — with the tree fixed or the bound re-derived from measurement | M | GB-018 | `POLLTST` prints `elapsed_ms=<digits>` on every run at both `T3_SMP=1` and `-smp 8`; the check passes at both CPU counts or the bound is changed **with the measured distribution quoted** (not widened to make it green); a deliberately-broken `select` (returning immediately) still FAILs the check at both counts |
| GB-087 | **Single-CPU intel takes a kernel general-protection fault at process startup — `vector=13 rip=0x0 pid=1` — so the 1-CPU leg of every tier measurement is running on a crashing kernel.** Reported by `wabi` while working GB-086 and **independently reproduced by the Coordinator** on `main` @ `38e94b2` with a POLLTST-only `::WAVE.CFG` and `T3_SMP=1`: `[ARRIVED] POLLTST.BIN rc>0` → `[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0000000000000000 cr2=0x0 pid=1 err=0`. `rip=0` on a GP fault means control was transferred to a null/unpopulated handler vector; `pid=1` is the wave owner. **Arch split, measured:** the same `T3_SMP=1` shape on **ARM completes** (`halted=1`, 8,875 lines, POLLTST's bound check FAILs but the kernel survives) — so this is **intel-only**, which makes it narrower than GB-077/GB-060 and worth chasing on its own. Why it outranks its parent: `T3_SMP=1` exists to separate races from logic bugs (that is how GB-081 was proved a concurrency effect), so while the intel kernel faults on that leg, "the 1-CPU run says X" is not a measurement — and the 8-CPU wave masks the fault the way it masks GB-077's | L | GB-077, GB-060 | the fault is explained with instruction/register/stack evidence on the exact path (process create / first resume / trap exit) — not "some GP at 1 CPU"; `T3_SMP=1` runs the **intel** tier to `halted=1` with **0** `FATAL kernel-mode fault` lines on both a POLLTST-only manifest and the default wave; a deliberately-null vector dispatch at 1 CPU is caught by the same guard (the guard must be able to fail); the existing 1-CPU readings on GB-081/GB-086 are re-read afterwards and confirmed or corrected |

### Milestone M2 — Compat device nodes + SDL (GB-025 … GB-033)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-025 | `/dev/fb0`: mmap → HobbyOS framebuffer (virtio-gpu 2D path) | M | GB-024 | SDL fbcon probes, rect draws |
| GB-026 | `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` ioctls | S | GB-025 | T4 parity |
| GB-027 | `/dev/input/event*` evdev ABI fed from virtio-input | M | GB-025 | SDL evdev sees moves/clicks |
| GB-028 | `nanosleep`/`clock_gettime`/`gettimeofday` parity + timer precision | S | GB-024 | T4 timings within tolerance |
| GB-029 | Audio: survey virtio-sound; stub `/dev/dsp`; SDL dummy/ALSA decision | L | GB-027 | recorded decision (`docs/audio.md`) + silent-safe SDL |
| GB-030 | `memfd_create` + shared-mem for SDL surfaces | M | GB-025 | T4 |
| GB-031 | **[DEEP]** SDL2 static-musl build profile: fbcon + evdev drivers, no X11; freeze exact flag set | L | GB-027 | build reproducibly |
| GB-032 | SDL test app (`surface`, 2D blit, input echo) under compat | M | GB-031 | draws + echoes input |
| GB-033 | **M2 gate:** static-musl SDL app on intel shows window, responds to input, quits clean | M | GB-032 | gate evidence |

### Milestone M3 — Games (GB-034 … GB-042, native catalog = the parallel swarm)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-034 | **DOOM native port** (doomgeneric, GPL): full-screen fb raycaster; *shareware `doom1.wad`* (license note: distribute free shareware WAD only) | L | — | plays + T5 playthrough |
| GB-035 | NES emulator (compact single-source) + homebrew/test ROM (license-clean) | M | — | boots → plays a ROM |
| GB-036 | Roguelike on existing curses port (nano's curses tree) | M | — | T1 logic + T3 |
| GB-037 | Second X11-era arcade port via `src/user/x11` xlib | S | — | T3 |
| GB-038 | Original arcade game by swarm + host golden tests subject | M | — | T1 + T5 |
| GB-039 | **Game launcher** desktop app: scans `/GAMES` (`.BIN` + ELF), launches either kind, escape-to-desktop | M | turtles | launches both tracks |
| GB-040 | Per-game evidence rig: serial log + screendump + replay scripts | S | — | assets land in evidence dir |
| GB-041 | **[DEEP]** **Doom as static-musl ELF under compat** (W4) — resolve interop gotchas (mmap, ioctl, threads) | L | GB-033, GB-034 | **playable on intel** — the S1 moonshot |
| GB-042 | Gamepad/joystick via evdev for arcade titles (optional) | M | GB-027 | axis/buttons register |
| GB-059 | Port the four hand-rolled T5 harnesses (`run_doom_test.py`, `run_stardust_test.py`, `run_xnibbler_test.py`, `run_xcrypt_test.py`) onto `tools/evidence_rig.py`: locate the title by name from the parsed `[MENU]` dump (never a magic slot index — four PRs each asserted their own on the same growing `pinned_games[]` line) and take the repo root from the environment so it works from a `hermes -w` worktree | M | GB-040 | both a native port and a compat title run through the one rig from a worktree; the four hand-rolled copies are deleted |

### Milestone M4 — Integration, testing, acceptance (GB-043 … GB-055)

| ID | Ticket | Wt | Depends | DoD |
|---|---|---|---|---|
| GB-043 | Long-duration **playthrough harness** per game (QMP input scripting + screendump assert; ≤30 s rule; auto-fail) | M | GB-040 | each game has ≥1 replay script |
| GB-044 | Two-arch parity CI per farm VM on every merge | M | GB-005 | run window < best |
| GB-045 | Performance pass: `gfxbench` baseline vs post; boot-time + framerate budget | M | — | numbers + report |
| GB-046 | Memory/leak pass under 30-min game cycling (freeze watchdogs) | M | GB-043 | no unbounded growth |
| GB-047 | Acceptance evidence pack for S0/S1: logs, screenshots, replays, gate history | M | GB-044/45/46 | archive on NAS |
| GB-048 | Re-plan cadence implementation: cron 24h + the three event triggers → coordinator → `deep` `PLAN` ticket | M | GB-003/004 | loop runs end-to-end once |
| GB-049 | Backlog grooming bot: close/await/block moves; blockers lint (deadlock rule) | S | GB-048 | board lint green |
| GB-050 | Flaky-test triage workflow + quarantine lane for timeouts (≥30 s = defect) | S | GB-044 | documented workflow |
| GB-051 | Audio acceptance (if GB-029 lands) | S | GB-029 | T5 with audio assert |
| GB-052 | Docs: per-game READMEs + user guide + `gaming.md` finalization | S | — | docs pass human read |
| GB-053 | Final acceptance run, both arches, full evidence archive | M | GB-047 | S0+S1 signed off |
| GB-054 | **[DEEP]** Program-2 roadmap ticket: Mono (RimWorld) staging, Spring RTS (TA) evaluation, Vulkan-virgl evaluation, dynamic-loader/glibc plan | M | GB-053 | epics + staged tickets |
| GB-055 | Program retrospective + rules update (what to teach `gaming.md`/skills) | S | GB-054 | lessons merged into skills |
| GB-056 | T3 PROCTEST `WEXITSTATUS` mismatch (got=2 for want=1 and want=42) — suites exit 0 anyway (minted by the Coordinator from the GB-009 baseline run; §10/§11.8) | S | — | T3 suite's PROCTEST passes and a printed FAIL fails the suite |
| GB-075 | **The S0/S1 acceptance rig fabricates PASS: it reports games that never loaded, and its liveness check samples only the top ~104 rows of the frame.** Measured by GB-053-V on `0d38100` (2026-10-20): `tools/t5/acceptance_run.py --arch arm` printed `XNIBBLER slot 19 PASS`, `XCRYPT slot 20 PASS`, `NES slot 21 PASS` while the serial log loads exactly **three** games (PONG/MILLIPED/XNIBBLER, all before any menu input) and `XCRYPT`/`NES` appear only in the `mcopy` that stages them and the `[MENU]` dump — never loaded, never a process. `ppm_live` samples `raw[0 : 200000*3]` at stride 9, i.e. the top ~104 of 1080 rows, so a 50×50 white block at row 540 or row 1000 reads as **not live**; the five captured "games" are 96.9–99.8 % black and pairwise mean-abs-diff 0.15–0.97 / 255. This is the M4 evidence instrument, so every S0/S1 acceptance claim resting on it is unsupported. Deliverable: identity and liveness both come from observed events — a title counts as running only when the serial shows it **loaded** and its process appears in the wave's live/exited set, liveness samples the whole frame, and a menu-only screen must FAIL | M | GB-040 | a captured desktop-only frame FAILS the rig (injected, and the injection is the test); a game that never loads FAILS; games that really run differ by a stated measured margin over the whole frame; GB-053's ARM and intel runs both go through this rig with per-title evidence |

---

## 9. Risks & open decisions

| Risk | Mitigation |
|---|---|
| ELF-compat syscall/errno parity churn | T4 differential locked before anything depends on it; Linux-ABI doc frozen by `[decision]` |
| Compat layer steals the whole month → no games | Native catalog (S0) is on its own agent track with zero dependency on the compat spine; S0 delivers no matter what |
| Token ceiling (8–12 agents) | Batch small tickets; long sessions; deterministic work offloaded to tools; FLASH >90% |
| Deadline pressure on syscall breadth | Weight-class ladder: each class shippable + demonstrable alone; W5+ deferred deliberately |
| Long-prefill agent restarts | Lifecycle discipline: agents persist per subsystem; evidence written incrementally |
| Threads/TLS/signals under compat are subtle | `[deep]` planning on the ABI semantics first; T4 multithread tests; small muscle car |
| Licensing (DOOM shareware WAD, game ROMs, TA content) | Ship only freely-distributable assets; document provenance per title |
| Legacy `-mgeneral-regs-only`/parity regressions | two-arch gate; kernel constraint checklist in review |

**Open (Sarah or `decision` tickets):** final endpoint/IP mapping for the two model tiers;
whether audio is in-scope this month; `ARCH=intel` promotion stakes for ARM primary target
(compat is intel-only by design — no ARM ABI change); optional nested-KVM enablement.

---

## 10. Quickstart (for the Coordinator on day 1)

Infrastructure is **already built** (VMs, Forgejo, repos, Hermes, LLVM cache — §§3.3, 11.7).
The Coordinator therefore starts mid-M0:

1. Read `gaming.md` §0–§7, then the board (Forgejo on `.234`).
2. Execute GB-002 (board rollout: `GAMING-BOARD`, labels, milestones — repo + `swarm-bot` exist),
   GB-003 (webhook + lane-move scripts), GB-004 (endpoints + profiles + **throughput benchmark**).
3. GB-008 (plan↔board sync), GB-009 (baseline capture on `main`), then open the M0 gate.
4. M1 ABI core is the critical path (**one owner**); native catalog tickets fan out immediately —
   do not stall S0 waiting for GB-013.
5. Hand the loop over at M3: GB-048 makes the swarm self-steering. Human touchpoints = `decision`
   tickets and milestone gates only.

---

## 11. Operating the swarm (runbook)

Companion to §5 — the concrete "how to run it" given the built fleet (§3.3) and Forgejo (§4).

### 11.1 Access & secrets
- Fleet inventory + known-good verification: **`swarm-fleet.md`** (this repo).
- SSH from the Mac: `ssh -i ~/.ssh/id_proxmox sarah@192.168.0.23x` — passwordless sudo on each.
- Secrets (VM bootstrap pw, Forgejo admin pw, `swarm-bot` API token): kept in
  `~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt` (chmod 600) on the Mac. **Never commit
  secrets to this repo (it is on GitHub).** On swarm VMs export the token as an env var rather
  than writing it into tracked scripts:
  `export FORGEJO_TOKEN=$(cat ~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt | grep swarm-bot | cut -d' ' -f4)`.
- Admin password rotates in the Forgejo UI anytime; the swarm uses the token, so rotation is safe.
- **GitHub API access (PR open/merge):** push is SSH (seeded key, no creds). For GitHub REST API calls
  (open a PR, merge a PR, status), `GITHUB_TOKEN` is stored in `~/.hermes/.env` on every swarm VM
  (chmod 600). Any shell needing it must load it first — sessions started before it was added do NOT
  have it in env:
  `set -a; source ~/.hermes/.env; set +a` (or `export GITHUB_TOKEN=$(grep '^GITHUB_TOKEN=' ~/.hermes/.env | cut -d= -f2-)`).
  `GIT_TERMINAL_PROMPT=0` is also there so git never hangs on a credential prompt. The Coordinator owns
  all merges (mount a merge = PATCH `/repos/sarahkaylor/HobbyOS/pulls/{n}/merge` with the token).

### 11.2 Ticketing (Forgejo) — agent-facing
- Web UI: http://192.168.0.234:3000 — admin user `sarah`.
- REST API: base `http://192.168.0.234:3000/api/v1`, auth header `Authorization: token $FORGEJO_TOKEN`.
- Examples:
  - List open tickets: `curl -s -H "Authorization: token $FORGEJO_TOKEN" "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program/issues?state=open"`
  - Create a ticket: `curl -s -X POST -H "Authorization: token $FORGEJO_TOKEN" -H "Content-Type: application/json" "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program/issues" -d '{"title":"...","body":"...","labels":["abi"]}'`
  - Milestones/labels/projects (kanban) have their own namespaces; reuse the GB-003 lane-move
    scripts instead of ad-hoc curls.
- `swarm-bot` account (all-scopes token) already exists — created 2026-10-16.
- **Board tooling (GB-002/GB-003, in-repo):** `tools/board/` — `board.py` (claim/move/comment/status/lint),
  `import_seed.py` (idempotent §8 → board import), `{claim,lane-move,comment}.sh` (workflow wrappers),
  `hook_server.py` (webhook receiver), `poll.py` (60 s fallback). See `tools/board/README.md`.

### 11.2a Board → Coordinator wiring (built 2026-10-17)

- Webhook `coordinator-board-hook` (repo hooks API) → `http://192.168.0.233:8765/board-hook`,
  served by systemd unit **`hobbyos-board-hook.service`** on `swarm-coordinator`; events land in
  `/var/log/hobbyos/board-events.jsonl` (`wake: true` for lane moves into In Review/Merge Gate,
  `decision` labels, closes).
- **`hobbyos-board-poll.service`** runs the 60 s fallback poll (`poll.py`) into the same inbox.
- Forgejo needed `ALLOWED_HOST_LIST = private` under a new `[webhook]` section in
  `/etc/forgejo/app.ini` on `swarm-ticketing` — without it deliveries to a LAN address are refused
  ("webhook can only call allowed HTTP servers"). The only change is that appended section
  (the file had no `[webhook]` block before); removing it restores the original exactly.

### 11.2b Fleet preconditions discovered on first contact (fix these before any x86 run)

1. **x86 firmware must be writable.** The Makefile passes `-pflash $(EDK2_X86_64)`, and `-pflash` opens
   the image read-write; the cloud-init symlink to `/usr/share/OVMF/OVMF_CODE_4M.fd` makes every x86 QEMU
   run fail with `Could not open ...: Permission denied`. Fix per VM:
   `rm -f ~/.local/share/OVMF/OVMF_CODE_4M.fd && cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/.local/share/OVMF/OVMF_CODE_4M.fd && chmod u+w …`
   (ARM uses `-bios` and was never affected). Applied on .233 + farms 1-3.
2. **`musl-tools`** must be installed wherever the T4 harness runs (`musl-gcc -static` is leg A); without
   it the harness honestly SKIPs and the differential gate is hollow. Installed on .233.
3. **The staging cache is not on every VM, and inter-VM SSH is not actually trusted.** §11.5 says to rsync
   `third_party/llvm-21.1.8/dist-staging` (227 MB, already built on `swarm-farm-1`) instead of paying the
   cold build; it was absent on farms 2 and 3, so `wgames` was blocked on hours of cold staging for its T5
   acceptance runs. Landed on farms 2-3 from `swarm-coordinator` (the coordinator has host keys for every
   VM; **farm-1 -> farm-2/3 rsync fails with `Host key verification failed`** because the cloud-init keys are
   only seeded for sarah's own key pairs — either seed them or route copies through .233).
4. **Workers run `hermes -w`.** A git worktree does **not** contain gitignored trees, so the vendored
   `third_party/*/src` extractions are missing there and vendored lanes (C++/RTTI, WebKit) go red inside a
   worktree while the same tree is green in the main checkout. Seed them (symlink is cheapest) before
   believing a worker's "main is red" report — tracked as GB-006. Landed: `tools/swarm/seed-worktree-vendored.sh`
   (plain `ln -s` only — `ln -sfn` silently type-changes tracked files, which blocks the next rebase).
5. **A non-login/background shell may not have `/usr/sbin` on PATH**, and `mkfs.fat` lives there, so
   `make disk.img` dies with `make: mkfs.fat: No such file or directory` / `Error 127` *after* the whole
   build has succeeded — which reads like a code failure and is not. If a backgrounded build fails only at
   the disk step, re-run it with `export PATH="/usr/sbin:/sbin:$PATH"` (or re-run `make disk.img` in an
   interactive shell; the objects are already built). `tools/run_waves.sh` already carries its own PATH
   handling for the same reason.
6. **`/dev/kvm` exists on the fleet VMs but is NOT usable as provisioned.** `ls -l /dev/kvm` is
   `crw-rw---- root:kvm`, and `sarah` is not in the `kvm` group, so an accelerated run dies with
   `Could not access KVM kernel module: Permission denied` (the device's presence is not the capability —
   that is the trap). The host CPU exposes nested SVM, so the one-line fix is provisioning:
   `sudo usermod -aG kvm sarah` (then a fresh login), or a targeted device ACL. Until that is done, **every
   intel QEMU run is TCG** — tens of minutes per wave, which is why an A/B measurement on `ARCH=intel` is
   still expensive. Tracked as part of GB-006's provisioning checklist. `tools/t3_tier.sh` takes
   `T3_QEMU_ARGS` so the fix is a one-liner away: `T3_QEMU_ARGS="-enable-kvm" make test_intel`.

### 11.3 Model endpoints & Hermes profiles
- Two vLLM OpenAI-compatible endpoints, **recorded + verified 2026-10-17 (GB-004)**:
  - **FLASH — DeepSeek V4 Flash** = `http://llm.localdomain:8000/v1`, `GET /v1/models` →
    `["deepseek-v4-flash"]` (root `/mnt/fast/deepseek-v4-flash-0731`), `max_model_len` 1048576.
    Default for every role; all routine work. Measured **57.7 tok/s** single-stream.
  - **DEEP — DeepSeek v4.1 Flash** = `http://dgx1.localdomain:8000/v1`, `GET /v1/models` →
    `["deepseek-v4.1-flash", "dgx-main"]` (root `/models/deepseek-v4.1-flash`), `max_model_len`
    1048576. Coordinator + farm Verifiers: planning/re-plan/review/verification. Measured **40.5 tok/s** single-stream.
- Verification (GB-004): `swarm-coordinator` resolved `model.default=deepseek-v4.1-flash`,
  `model.provider=dgx` → `hermes chat -q` answered (DEEP-LINK-OK); `swarm-farm-1..3` resolved
  `model.default=deepseek-v4-flash`, `model.provider=flash`, `model.base_url=http://llm.localdomain:8000/v1`
  → answered (FLASH-LINK-OK) with only `llm.localdomain:8000` in the logs. Role provisioning is
  scripted: `tools/swarm/provision-hermes-role.sh` (`ROLE=flash|deep`); it uses `hermes config set`
  only (never hand-edits `config.yaml`) and writes `FORGEJO_TOKEN` to `~/.hermes/.env` (chmod 600),
  never to a tracked file.
- Budget (validated 2026-10-17): ~90% FLASH / ~10% DEEP tokens. FLASH sustains ~94–150 tok/s →
  cap active workers at ~3–5 until GB-004 benchmarks say otherwise. Never put build/test load on
  the inference boxes.
- Per-role Hermes config: one profile per role (`swarm-coordinator`→DEEP, `swarm-worker`→FLASH),
  distinct `HERMES_HOME`; workers run `hermes -w` (worktree) so parallel agents never collide on
  files. Hermes is installed at `sarah:~/.hermes` on every VM.

### 11.4 Standing the swarm up (order matters)
1. **Coordinator first** on `swarm-coordinator` (.233): start in a tmux session with the
   `swarm-coordinator` profile and this doc as its charter. It runs GB-002→GB-004 in order
   (board rollout → endpoint verification → model-split check), then GB-008/009, then opens M0.
   `tmux new -s coord` → run hermes inside so it survives disconnect.
2. **Reviewer + Tester** (FLASH) on .233 / farms — start alongside step 1.
3. **Workers** on the farms: one long-lived `hermes -w` session per subsystem
   (abi/loader/libc, graphics/input/audio, native-ports, tests); each claims `Ready` tickets.
4. **Re-plan cron** (GB-048): `hermes cronjob` daily on the coordinator → `deep` `PLAN` ticket.
5. Evidence/artifacts land on the shared store (NAS `/mnt/fast` or c2flash) under `evidence/` —
   the planner reads them at re-plan time.

### 11.5 Day-to-day operation / monitoring
- Watch the board (UI or API). Every merge passes the full gate (T1–T3 both arches, + T4/T5 where
  the ticket demands). Evidence-before-close; `decision` tickets pause for Sarah.
- **30-second deadlock rule is law:** a hang is a defect — investigate, never just re-run.
- Two-arch parity is law — nothing lands grey on either arch.
- First `make run`/`make test` on farms 2/3 & coordinator triggers the one-time staging build
  unless you first rsync `third_party/llvm-21.1.8/dist-staging` from farm-1 (already built).
- Quiet farm VM? SSH in, check tmux/top/qemu, restart the agent session under tmux.

### 11.6 Stop / cleanup
- Quit agents: `tmux kill-session -t <name>` on each VM.
- Kill stray QEMUs inside a VM: `pkill -f qemu-system`.
- Leave VMs running for resumption, or power off from Proxmox (`qm stop <vmid>` on 192.168.0.19).
- Nothing in this swarm touches the inference boxes or DGX Sparks — stop means stop, no harm.

### 11.7 Known-good verification (done 2026-10-16/17)
- `make host_tests` → **ALL APPS SUITE TESTS PASSED** (farm-1, coordinator).
- `make MODE=unit_tests hobbyos.elf` → 2.1 MB aarch64 ELF linked by `ld.lld` (farm-2).
- `qemu-system-aarch64 -M virt -smp 4 -m 2048 -bios ~/.local/share/AAVMF/AAVMF_CODE.fd`
  boots UEFI to the shell, no crash (farm-2).
- LLVM 21.1.8 dist-staging cached on farm-1.

### 11.8 Execution log (newest first, maintained by the Coordinator)

**2026-10-23 — continuation pass: the M0 gate is opened **with three named exceptions** (PLAN-011), the gate itself becomes honest and immediately proves four reds, and one `Done` over a RED is caught twice — once by a script, once by reading the verdict.**

| what | state |
|---|---|
| **M0 gate: OPEN, not green** | PLAN-011 (#131, `decision`). Clause 1 (board+sync) **met** — `sync_gaming_md.py` `IN SYNC 82 plan/86 board`, `check_verdicts.py` `25 checked, 0 mismatched`; clause 2 (baseline) **met as capture, false as "green"**; clause 3 assessed card-by-card. Exceptions: T3 red on both arches, GB-005 is a script not CI (no `.github/workflows`), GB-001's NAS clause split out. GB-001 closed on its 3 met clauses when its own 2026-10-20 time-box expired → **DECISION-003** (#130) |
| **The gate stopped grading the apparatus** | GB-080 landed (`837df54`): T5 grades by exit status (+5-case `--self-test`), missing tools are an **exit-2 environment failure** not a red verdict, STYLE asks *"did this change add a violation?"* (tree-wide is red on `cstyle.py`'s own defects), and `acceptance_test.py` no longer writes half a frame. `GATE_CI=1` before: **7/7 tiers FAIL, 4 of them for instrument reasons.** After: `T1 PASS · T2 PASS both · STYLE PASS · T3 FAIL both` — and the two reds are real, named, owned |
| **ARM T3 red, two independent causes** | `SHELLTEST WATCHDOG: protocol stalled 25 s` with `SHTEST.BIN` (pid 3) and its child `SH.BIN` (pid 10) both `BLOCKED` → GB-058 back to `In Progress` (a red tier is not "awaiting review"); `[CMPTEST] FAILURES: 2` was in **no ticket** → minted GB-081. 4 FAIL lines total, `halted=1` |
| **intel T3 red, and the count is a lower bound** | 84 FAIL lines (SEDTEST 54, TSORTTEST 7, CMPTEST 6, TACTEST 2), plus `[WAITPID] ECHILD caller=42 CMPTEST.BIN want=-204 children=0` — GB-057's ECHILD shape is still live. But **1,422/37,299 intel console lines (3.8%) and 282/31,586 ARM lines carry a second message glued mid-line**, so **25 of 59 SEDTEST FAIL lines (42%) and 4 of 7 TSORTTEST (57%) are not the text the test printed** → **GB-082** minted and sequenced *before* GB-057's diagnosis. The title's "222 FAIL lines" was measured through the same damage, which is why it never reproduced |
| **A `Done` over a RED, found by the script** | `check_verdicts.py` flagged GB-079-V = RED while GB-079 sat `Done`. Reproduced it myself on `main`: an injected unconditional `prow()` **PASSED** `--check` with `obj/t4_legA.out` present (`mode=emitted` reads a saved file) and failed only without it. Fix = grade **both** extractions and refuse a stale capture (`FAIL: recorded leg A is STALE`), with `--self-test` clause (d) plus a mutation proof. **PR #79 → GREEN by a DEEP verifier at 18:22Z** — case 3 `rc=1`, two independent detections — and merged |
| **The verifier lane was idle 9 h; now it is fed in 20 minutes** | #72 said "no Ready verify … unchanged since 08:52Z" on three DEEP sessions. Cause was structural: two `In Review` cards, neither gate-ready. Minted **GB-079-V** (#128) against PR #79's head; a verifier claimed it and posted a rigorous GREEN matrix **within 20 minutes**, then the card was `Done`. The lesson is not "mint more gates" but that an idle lane needs a card whose instrument exists |
| **Merge queue: four PRs, one gate, and a silent loss caught** | #79/#78/#63/#58 collided on the shared `host_tests:` line (the GB-064 class), so they were gated **on the integrated tip** (PR #82, `e2ca063`): `TEST EXIT: 0`, `Checks failed: 0`, both kernel links 0, every PR's own test executing inside the suite. `resolve_makefile_additions.py` reported *"resolved 0 block(s)"* and `make -n host_tests` parsed — **and it had dropped GB-006's entire side**; caught by `grep -c test_cache Makefile` (0), restored in `e25e923` |
| **And a `Done` over a RED, found by reading** | I closed **GB-006** on a passing `CACHE TEST PASSED`, then `check_verdicts.py` caught GB-006-V's RED 60 s later. Read properly, the RED stands: `test_cache` measures a **probe** compile; the real build still invokes `clang` directly and the cache is VM-local, so "hits > 0 on the build" and "shared" are both unmet. **Reversed to `In Progress`** with the three steps that would meet it. My own comment had already said the caveat — and I closed the card anyway |
| **PR #1 (DOOM) is still `dirty`** | 214 files, 75 k lines, base `3dc6dcb9f`; rebase directive posted to `wgames` (who resumed at 18:29Z). No merge, no rebase forced by me. It remains the S0 deliverable and it cannot merge until it is rebased and gated |

**The lesson this pass, stated as a rule: every red I could see today was visible only after an instrument stopped grading something other than the code.** The gate graded a substring, a PATH, a formatter's own defects and a fixture's half-frame; the t4 check graded a saved file instead of a probe; the console graded interleaved fragments instead of messages; and the board graded a lane label instead of a verdict. Four of the six findings were *measurement* defects, and the two board lies were caught by the two checks built for exactly that (`check_verdicts.py`, and reading the verdict rather than my summary of it).

**2026-10-23 — PLAN-010 re-plan: the M1 chain's middle links are all landed, so the four held `-V` gates came off `blocked` — and the instrument they will use was measured giving two verdicts for one tree.**

| what | state |
|---|---|
| **The chain closed** | GB-013 (relocations), GB-072 (`::WAVE.CFG` manifest dispatch, both arches), GB-073 (shared row set), **GB-076** (the missing `fcntl_/ioctl_/dent_` rows: golden is now 39 names, guest probe emits them, `make t4_device_check` PASSes) are all landed. The four `-V` cards (#87/#88/#89/#91) that stood `blocked` on that chain were moved to `Ready` — the DEEP gate capacity they hold is the program's scarcest resource and it was idle behind work that is now on `main` |
| **GB-076 closed on a proven-failing check, not on a reading** | the DoD clause is *"delete one `dent_*` row → `--check` FAILS"*; run with a golden copy minus `dent_have_charlie`, `t4_device.sh --check` → `FAIL: device probe emits extra row 'dent_have_charlie' not in golden`, exit 1; restored → PASS (`evidence/gb-076/`) |
| **New: GB-079 — the row check is mode-dependent** | with `obj/t4_legA.out` present it reads **emitted** rows (39) and PASSes; with it absent it reads **source** `prow("…")` rows (41) and FAILs on `pipe_write`/`sock_write` (`t4_probe.c:130/171`, emitted only on error paths). Same tree, opposite verdicts, and the skip announces itself in lowercase (`fail: leg A absent — will skip…`) without failing the run. Minted S / GB-073; both runs captured in `evidence/gb-079/` |
| **Nine RED verdicts were contradicted by their parents' lanes** (`check_verdicts.py`: 21 checked, **9 mismatched**) | eight `-V` REDs had parents sitting in `In Review` or `Done`. Per §4.2 a RED parent returns to `In Progress`; all nine were moved back with a comment stating plainly that **the next action is a re-gate, not an edit**, because the REDs were harness REDs (GB-072/073/076) and the harness has since landed. Four of them (GB-007/017/018/045) had been wearing `Done` |
| **What is still the wall** | **GB-077** — the default 97-program wave ends with 3 processes parked in `__stack_chk_fail` and 5 execute-at-stack faults in the harness programs themselves; the corpus-only manifest on the same build is `0 faults, 1 boot, halted=1`. So the W0 corpus leg is clean and the *default* list is not — and M1's "T4 green for W0" stays unclaimable until the device leg is run under GB-079's fixed check |

**The lesson this pass: the board and the tier are both instruments, and both were measured rather than read.** The nine mismatches were not nine dropped balls — they were one landing that nobody propagated; and GB-079 was found only by running the same command twice with a build artefact present and absent, which is a two-minute check nobody had done because the first run said PASS.


**2026-10-21/22 — the M1 corpus finally runs on BOTH arches: six fixes, four of them in the measuring apparatus, and the "teardown triple fault" turned out to be a reaping bug.**

| what | state |
|---|---|
| **GB-013 Done** | `static-PIE relocations` — `elf_apply_relocations()` applies `R_X86_64_RELATIVE`/`R_AARCH64_RELATIVE` over the staged image and **refuses** unknown types. Proven by execution, not inspection: a real musl `-static-pie` maps, enters at `USER_IMG_BASE + e_entry`, and runs its own code (`PC=0x1000001C56`). Before the change the same fixture died at `PC=0` |
| **`target_sp` was the CPU's kernel stack** (`dce9212`) | `schedule()` handed every process `&__stack_top - cpu*0x10000 - 4096`, so argv/rdi pointed into the kernel stack, unmapped in the process's CR3. **Memory faults 60 → 9.** The same bug existed at a **second** resume site in `start_scheduler()` — fixed in `59b3a6f`, giving **0 faults** |
| **The v2 stack is demand-zero** (`d6ca910`, `4ca2a4d`) | the entry frame is written from **kernel mode**, which cannot demand-page; one page was not enough, the full 64 KiB guard is now prefaulted. Triple fault at entry gone; reboots 195/700 s → 15/400 s |
| **GB-072 Done — manifest-driven wave** (`58ac621`) | the wave was ~97 hardcoded literals, so nothing added to `disk.img` could ever run — **including the M1 gate's own W0 corpus**. `test_wave_loader()` now reads `::/WAVE.CFG` and dispatches each member; literals remain the no-manifest default. Verified on **both** arches with a HELLO-only manifest: dispatched, arrived, literals skipped |
| **The gate's manifest parser was broken** (`d04f748`) | `read_manifest()` emitted a **literal backslash-n**, so the last inline member could never match and `--manifest` failed on its own last entry. A FAIL that looks like a detection but is an artifact. `--self-test` had **no test for the parser** — two added, validated by mutation (bug restored → `pass=4 fail=2`) |
| **User stdout was shredded** (`1fbe4b1`) | `console_write_user` used `uart_putc` **per byte**, which takes `uart_lock` per character, so 8 CPUs interleaved inside lines: `HELHALLO.BIN] / mainTSORT.BIN / returned 0`. **This is why the corpus looked silent for several loops** — the text was there but no longer existed as a line. Fixed with one lock hold per chunk (`uart_putc_raw`). `grep -c 'main returned'`: **0 → 1** |
| **The halt was never the predicate** (`2b4b639`) | a CPU going idle cleared its claim but left the process `PROC_STATE_RUNNING`, and LOSTWAKE disposes "RUNNING with no claim". **pid 1 owns the wave and the halt decision**, so after it was reaped nothing ever evaluated `!any_alive` and every CPU spun in `[IDLESTUCK]` past 64,000 rounds. Unclaimed-but-alive now goes to `READY`. `[HALT-CD]` prints, `halted=1` |
| **DECISION-002 moot** | the teardown triple fault (`vm_arch_teardown` + `mov %eax,%cr3` while standing on the dying AS stack) **disappeared with the reaping fix**. Neither option A (defer teardown) nor B (identity-mapped kernel stacks) was needed — the stack was never the problem, the process owning it was killed early. Closed |
| **Where it stands** | intel full wave: **1 boot, 0 triple faults, 635 program exits, 33,310 lines** (was 195 boots / 4 exits). ARM W0 corpus: dispatch ✓ run ✓ output ✓ exit ✓ **halt ✓**. Remaining: 3 processes parked in `__stack_chk_fail` (so the full wave does not halt) and 5 execute-at-stack faults (`PC=0x17FFFFFF84 rw=rx`) — one user-stack-corruption mechanism, cause **not** established |

**The organising lesson of this run: four of the six fixes were in the measuring apparatus, not the OS.** A tier that aborted before printing a verdict (`|| true`), a manifest parser that failed on its own newline, a console that shredded the very output it was reporting, and a `[CLAIMDROP]` trace needed to find a strand. **Every one of them made "nothing happened" look like a property of the system rather than of the instrument.** When a diagnostic says nothing happened, the first hypothesis should be that the diagnostic cannot see.

**2026-10-22 — dedicated cross-lane Reviewer loop `wrev` added (farm-3, FLASH).** Review was previously a Coordinator inline duty (the same DEEP session doing family-B debug/plan/merge), i.e. a serial wall: all 13 In-Review cards carried only Coordinator review comments. The loop reviews every workstream (In-Review + In-Progress-with-PR cards), posts APPROVE/BOUNCE/FLAG verdicts (never RED/GREEN — Verifiers own gates), and **mints missing `GB-nnn-V` gate tickets (§4.2 step 4) for gate-ready cards** so the 3 farm Verifiers gate in parallel. First run: APPROVE GB-031 + minted `GB-031-V` (#118, Ready/verify/deep) — gate fed to the Verifier fleet; then advanced to In-Progress-with-PR cards (GB-006, GB-032, GB-035). It is a distinct flock (`swarm-review@wrev`, unit `swarm-review@.service`, AGENT=wrev-review) so it never collides with wgames/wgames-verify on farm-3.

**2026-10-19/21 — GB-013 CLOSED: static-PIE relocations implemented and proven by execution. The wall is now GB-068, which kills every program.**

| what | state |
|---|---|
| **Relocations were the missing half** | `elf_image.c` had **no relocation handling at all** (`grep -c reloc` → 0). A static-PIE is position-independent only *after* its `R_*_RELATIVE` entries are applied: without them every GOT slot holds a link-time value relative to 0, so the image maps, enters at the right address, and faults at the first indirect call — measured as `PC=0/VA=0`, which reads like a null dereference in user code and is not |
| **Landed (`440eee5`)** | `PT_DYNAMIC` captured at parse (`dyn_vaddr`/`dyn_size`), plus `elf_apply_relocations()` over the laid-out stage before mapping: walk `DT_RELA`, apply `R_X86_64_RELATIVE` (8) / `R_AARCH64_RELATIVE` (1027) as `base + addend`, and **refuse anything else** with `ELF_ERR_RELOC`. A skipped relocation is indistinguishable from a working one until the program jumps into the wrong place, so unknown types are an error. Kernel-free and byte-wise, so it stays host-testable |
| **Proof (not inference)** | Real musl `-static-pie` probe (`DYN`, entry `0x1c18`, 7 `R_X86_64_RELATIVE`, `DT_RELA 0x208 / RELASZ 168 / RELAENT 24`): `[ARRIVED] T4PROBE.BIN rc>0`, **no `relocations:` refusal**, and `PC=0x1000001C56` = `USER_IMG_BASE + 0x1C56` — the probe's own code, past its entry. Before the change the same fixture died at `PC=0` |
| **DoD** | *"static-PIE binary maps+boot, entry set right"* — met, with a printed entry (`0x1000001000` for the 0x1000-entry fixture, `0x1000001C18` for the probe), printed resident segments, and execution past entry. **GB-013 → Done** |
| **What still kills it** | `VA=0x7558DBC8` = `&tf->spsr` for the running CPU — **GB-068**, which every program hits regardless of build. The loader hands over a correct entry and correct relocations; the process then faults because `rdi` holds a kernel stack pointer. Falsifier if that reading is doubted: build any non-PIE program and it dies at the same VA with the same signature |
| **Dependency chain, now explicit** | T4 device leg (GB-076's guest rows) → static-pie probe → loader relocations (**done**) → **GB-068**. The probe now loads and executes; it cannot finish until GB-068 is fixed |

**2026-10-21 — GB-068 narrowed to one measured contradiction; three value-level probes came back negative.**

| what | state |
|---|---|
| **Where the fault actually is** | Disassembling the recorded PCs named it: `print_console`'s **`strlen`** loop (`cmpb $0x0, 0x1(%rdi,%rsi)`), so `rdi` — a `char *` **argument** — holds a kernel stack address. The corruption is present **at or before user entry**, not acquired late in a run; that rules out every "accumulates during execution" theory |
| **`restore_context` probe** | `[BADREG] pid=3 rdi=0x755FDBE0 ctx5=0x755FDBE0 elr=0x7003053B c34=0x1B`. `ctx5 == rdi` (value comes from the PCB); `elr` is a **kernel** address; `context[34]` says **user** CS. Resolved: `0x7003053B` = `v2_image_materialize`, `0x7003349E` = `spinlock_acquire_irqsave` — the resume PCs are **inside the loader**, with user selectors |
| **Three negatives, each killing a theory** | (1) `save_context` never writes a user-CS + kernel-ELR pair — probe fired **0 times**; (2) PCB reuse cannot supply a stale selector — `process.c:1075` zeroes all 36 context slots on every create; (3) field normalisation cannot mix them — the wrapper reads each source before overwriting it, and a double pass would put `0x1B` in the ELR, not a kernel address |
| **What survives** | The two fields are written at **different times by different paths**. Exactly one writer of `context[31]` does not write `context[34]`: `process_create_kernel_internal` (`process.c:1131`, kernel entry point). So the surviving hypothesis is a **lifetime** question — one PCB being both "kernel thread being created" and "user context being saved" — not a value question |
| **Fault population is timing-chosen** | 8 CPUs: 61–113 fault *records* over 56 pids (retries inflate records); **1 CPU: 62 records / 62 pids / 62 CR3s — exactly one fault each, deterministic**. So the fault is not a race; the *victim set* is. Single-CPU is the right microscope but **3 of 4 single-CPU runs hang** (`TIER=124`) |
| **Metric fixed** | The tier now gates on **processes carrying faults** (48–56, stable) rather than records (61–113, retry-inflated) |

**2026-10-19/20 — M1 static-PIE: it now maps, enters and executes; the wall has moved to the syscall table. Plus two
instrument defects of my own, one of which had been silently invalidating three loops of "measurements".**

| what | state |
|---|---|
| **Arrival line (GB-072 remainder) — landed `2bab246`** | `[WAVE-TRY] <name>` / `[ARRIVED] <name> rc>0` printed from `load_and_run_program_v2` (`program_loader.c`), the choke point every wave call reaches. Measured: **64 tried, 64 arrived, zero failures**. Before it, "the program ran" and "the program never loaded" were the same console silence — which is exactly why two GB-067 re-stage probes read as inconclusive |
| **My instrument defect #1: the wrong file, for three loops** | `tools/t3_tier.sh` prints a *summary* to stdout and writes the **guest console** to `obj/t3_<arch>.log`, a path it announces on every run line. I grepped the summary and concluded three times that a working instrument "did not fire", then reverted it and posted the false finding to GB-067 and GB-072. Measured after the correction: `[WAVE-START]` 1, `Mode: TEST` 1, `FATBIG_T` 10 occurrences in that same "silent" console. **Rule: a gate's stdout is a summary; the evidence is the artifact it names** |
| **GB-067 premise REFUTED** | With `T3_STAGE` deleting and re-creating `FATBIG_T.BIN` from the host after the build, the guest logs `[ARRIVED] FATBIG_T.BIN rc>0` — it opened and ran the host-staged file. Root-directory arithmetic matches the driver exactly (`root_dir_sector=544`, `root_dir_sectors=32`, `root_entries=512`) and the root holds **no `0x00` terminator** in 512 slots, so the driver's early stop cannot skip an entry. The "invisible" symptom was the staging window that `T3_STAGE` closes, i.e. **my own probe fixture** |
| **GB-013 static-PIE — maps + entry PROVEN** | Real musl `-static-pie` ELF (`DYN`, no `PT_INTERP`, `e_entry=0x1000`, 0 relocations) staged as an ELF: `v2 loader: PID=3 entry=0x0000001000001000 resident=4`. That is `USER_IMG_BASE + e_entry` exactly as `elf_loader.c:203` computes, with all four `PT_LOAD` segments mapped under their real protections. **"entry set right" is met with a printed value**, not an inference |
| **My instrument defect #2: the flat `.bin`** | The two runs that produced my "zero relocation handling is the root cause" claim had staged an `objcopy -O binary` image — **a flat file has no ELF header**, so the loader took the flat path and never parsed an ELF. `entry=0x1000000000` was the flat default, and `PC=0` was the image's first byte. Retracted on the ticket; the `grep -c reloc` → 0 observation survives but is **not demonstrated as a defect** (the musl fixture needs 0 relocations) |
| **Where the wall now is** | The program runs `_start` to completion and issues **syscall 60 (`exit`)**; the dispatch returns **−14 (`-EFAULT`)** instead of terminating, so execution continues past the call and faults at `PC=0`. **This is GB-014's Linux dispatch, not the loader.** Two concrete asks posted there: `exit` must not return, and `SYS_WRITE_CONSOLE == 1` collides with Linux `write` (different argument convention), which is why the binary's message never reaches the console |
| **`main` did not build from clean — fixed** | `desktop.c:915` calls `games_scan_dir()` since the launcher landed, but only `DESKTOP_BIN` linked `games_scan.o`. `EDITOR_T.BIN`, `APPS_T.BIN`, `FILEDIALOG_ARROW_T_BIN` and `PONG_T_BIN` all link the same desktop wrapper and all failed with `undefined symbol: games_scan_dir`. **Hidden by a stale `obj/intel/desktop.o`** — any tree that had not recompiled `desktop.c` linked fine. Fixed by enumerating every target that links the wrapper (5 hits, 4 link lines) rather than chasing failures one at a time |
| **Tier could not start, and said the wrong thing** | `T3_intel: BUILD FAILED` was really `mkfs.fat: No such file or directory` — the tool is at `/usr/sbin/mkfs.fat` and a non-root PATH omits sbin. `t3_tier.sh` now prepends `/usr/sbin:/sbin`, checks `mkfs.fat`/`mcopy`/`qemu-system-<arch>` before building, and exits **2 (ENVIRONMENT FAILURE)** naming what is missing. Proven both ways: `PATH=/nonexistent` → exit 2; normal PATH → proceeds |
| **Open, and now measurable** | intel T3 halts cleanly with **4 FAIL lines, all GB-065** (framebuffer pixel assertions) and **55–57 user memory faults (GB-068)**. One run with heavy console output reported **104 faults**, i.e. the GB-068 "baseline ceiling 60" moves ~80% with console volume — the ceiling needs re-deriving before it is used as a gate |

**2026-10-19 — GB-068: eleven mechanisms killed, and a debugger finally produced facts instead of inferences.**

| what | state |
|---|---|
| **Method change that mattered** | After ~11 hypotheses died by inference from serial logs, one QEMU gdbstub session (`-s -S`, `hbreak`, hardware watchpoints) produced more than the whole preceding effort. `hbreak` on a user VA works before the process address space is installed; a watchpoint on a PCB field names the writer. Both should be the first move on any "user code faults at a wild address" question in this tree |
| **Measured identity** | The faulting VA is exactly `target_sp(cpu) − 0x28` where `target_sp = &__stack_top - cpu*0x10000 - 0x1000` (`__stack_top = 0x755FEBF0`). Corroborated on five CPUs (`0x755FDBC8`, `0x755EDBC8`, `0x755DDBC8`, `0x755CDBC8`, `0x7558DBC8`). **Not** present as a constant in the kernel image (0 occurrences of each 8-byte value), so it is computed at runtime |
| **First restore vs later** | `restore_context` for user processes: **every first restore has `context[5] = 0`** (correct `argc`), every later one carries the kernel-stack address. So the process starts correct and is corrupted in flight; `save_context` (`process.c:1143`) then persists it |
| **Who writes the frame slot** | `common_trap_wrapper` writes the frame's `rdi` slot on entry with a legitimate user value (`0x62`) — the normal save, not the corruption. A conditioned watchpoint on **cpu 0's** frame slot (`> 0x70000000`) never fired in 240 s: cpu 0 is not the corrupted CPU, so the watch must be placed on the CPU that actually owns a corrupting process |
| **Frame offsets are correct** | `struct trap_frame` (x86_64): `regs[30]` 0..239, `lr` 240, `elr` 248, `spsr` 256, `vector` 264, `error_code` 272, `cs` 280, `ss` 288 — and the asm reads `[rsp+40]` for rdi, `[rsp+248]` RIP, `[rsp+256]` RFLAGS, `[rsp+280]` CS, `[rsp+288]` SS. Every offset matches, so "misaligned restore" is dead |
| **Dead mechanisms (13)** | TLS self-pointer; `tls_size` underflow; `.bss` filesz-vs-memsz; v1-overlay base; wrong CR3; ELF-induced damage; "heap never created"; resume-RSP plant; kernel-stack execution; syscall return (`ret == cr2` 0/57); stale registers (zeroing all 36 context slots changed nothing); **"kernel passes a bad `argc` at entry"** (first restores are 0 — the CPL3 stop I measured was a later re-entry); **"frame restore reads wrong offsets"** (offsets verified) |
| **Still true** | The bad value is a runtime-computed kernel-stack address equal to `target_sp(cpu) − 0x28`; `usp` is a valid user address in every record; CR3 correct 56/56; 49 of 57 faults precede any kernel entry |
| **Next action** | Conditioned watchpoint on the frame slot of the CPU that owns a corrupting process (`0x755xDAF0` for cpu N), stopping on the exact store. Frame-relative address computations do exist in this path (`lea rdi, [rsp+16]` at `trap.c:3086` and `:3204`) |

**2026-10-22 — §4.3 re-plan (PLAN-009, `b101d38`): M1 is a four-link chain, and its second link demanded the impossible.** Summary on PLAN-009 (#120), closed.

| | |
|---|---|
| **Board snapshot** | 119 tickets: Backlog 8 · Ready 10 · In Progress 22 · In Review 12 · Merge Gate 1 · Done 62 · Icebox 3. `lint` clean, `sync_gaming_md.py` IN SYNC 76/76, `check_stale.py` 9 stale >12 h |
| **Milestones** | M0 17/24 · **M1 14/36** · M2 7/11 · M3 7/13 · M4 12/20 |
| **The chain** | `GB-068` (kernel task's stack **is** the top page of the user region — `process.c:1145`, mechanism named 2026-10-22) → **DECISION-002** → `GB-072` (`::WAVE.CFG` dispatch) → `GB-076` guest half → `#87/#88/#89` `-V` gates. Every M2–M4 gate runs through it |
| **Finding 1 — GB-072's DoD was unsatisfiable, and that is why it walked back twice** | The clause *"with no edit to `src/kernel/main.c`"* forbade the only place the dispatcher can live (`test_wave_loader` is in `main.c`; the loader choke point serves spawn/pipe/exec too). Re-scoped in §8 to the **intent** — *no per-program `.c` edit*; one boot-time manifest read may live in or be called from `main.c`; the 97 literals allowed only as the no-manifest default. Re-imported → #101. **The three falsifiable clauses are untouched** (both arches, arrival line, default exited-count parity, absent-file manifest FAILS) |
| **Finding 2 — the idle verifier lane was correct, but one DEEP card was unclaimable** | All 18 `-V` cards are Done or `blocked` behind GB-072, so *"no Ready verify and no Ready debug"* on fourteen passes is honest. But GB-069 (which of three mechanisms caused the NVMe-window `#PF`, with fault-site evidence) is DEEP-class and sat unlabelled in `Ready` — now `debug`+`deep`, one card in the §5.1 debug reserve |
| **Finding 3 — two `decision` cards were in the worker dispatch queue** | DECISION-001/DECISION-002 sat in `Ready`, where any FLASH worker may claim them; the deliverable is Sarah's choice, so a claim is a no-op or an agent answering the human's question. → `Backlog` keeping `decision` |
| **Mint discipline** | **0 implementation tickets, 0 `-V`.** 9 of 12 In Review cards already carry a `-V` (5 Done, 4 held); GB-057/058 *are* the red-tier records and GB-076's guest half is the GB-072 wall — a gate for any of the three grades nothing. Seven adjustments instead (re-scope, 2 labels, 2 lane moves, 1 close, 4 blocker lines) |
| **M0/M4 honest read** | M0 cannot gate: its clause is "baseline suites green" and intel T3 is red; GB-006's cache is measured **unused** (0 hits). GB-075 sits in Merge Gate with a GREEN verdict and its code on `main` (`54860b2`), but DoD clause 4 (GB-053's ARM+intel runs through the rig) is undelivered — **merged ≠ Done**, it rides on GB-053 |
| **Leverage** | One card: **DECISION-002 (#117)**. Behind it GB-072, which for the first time has a DoD a worker can satisfy |

**2026-10-21 — §4.3 re-plan (PLAN-008, `badc665`): the verifier lane was idle for 55 passes because five In Review cards had no card, and every verdict on the board was 25 commits old.** Summary on PLAN-008 (#112), closed.

| | |
|---|---|
| **Board snapshot** | 110 tickets: Ready 26 · In Progress 12 · In Review 13 · Backlog 5 · Icebox 3 · Done 51 · Merge Gate 0. `lint` clean, `sync_gaming_md.py` IN SYNC 76/76. As found: 55 open, including **4 Done-labeled-but-open** (#2/#3/#4/#13, now closed) |
| **Milestones** | M0 15/23 · M1 11/36 · M2 5/10 · M3 6/13 · M4 10/20 — **not comparable to PLAN-007** (cards minted + lanes moved in this pass) |
| **Finding 1 — the idle lane is a board defect** | The verifier's own log (#72, 02:49Z): *"18 verify cards, 0 Ready (15 Done, 3 Backlog+blocked) … fifty-fifth empty pass since 03:59Z."* Fourteen cards sat `In Review` while **five of them had never been given a `-V` at all** (GB-006, GB-032, GB-035, GB-074, GB-075). Minted #107–#111, all `Ready` → 8 Ready verify cards, dispatched by farm on #72 |
| **Finding 2 — every verdict is revision-stale** | All 8 closed `-V` verdicts are pinned to `0d38100`/`98247dd` (2026-10-20 ~03:00) while `main` is `badc665`. Three re-armed `Ready` at `badc665` because their grounds have landed fixes: **#97 GB-070-V** ← `7717c47` (W0 corpus dispatched, 66 exited, four `[ARRIVED]`) + PR #69 (manifest gate); **#94 GB-046-V** ← `c964a0b` (instrument hardening, PR #68); **#91 GB-059-V** ← `2a2b6cc` (rig launches by name). **Deliberately not re-armed:** GB-044-V (needs a per-merge CI on a `main` with no workflows) and GB-034-V (needs DOOM, which exists only on PR #1) — gate order written on parents #44/#9 instead of a repeat RED |
| **Finding 3 — M1's wall unchanged, and today's fix widened it** | `src/kernel/main.c` = **77 literal** `load_and_run_program_in_scheduler` calls; **no kernel-side manifest reader** (`grep -rn WAVE.CFG src/` → 0); `T4PROBE.BIN` not among the 77. `7717c47` dispatched CAT/LS/SH by **adding three more literals** — right for GB-070, wrong for GB-072. GB-072 (#101) moved `In Review → Ready` with the dispatch half spelled out; second half recorded, not papered over: `t4_w0.sh` now prints `PASS (arrival only)`, so the gate clause *"writes expected output"* is still unmeasured |
| **Finding 4 — GB-073's caveat was real** | `tests/fixtures/abi/t4_rows.golden` = 11 rows, `grep -c 'fcntl_\|ioctl_\|dent_'` over golden + guest probe = **0**, while GB-019-V/020-V DoDs name ten such rows that exist only in the host *model*. `t4_device_check` agrees by construction on a set that cannot contain them. **Minted GB-076 (#106)** — needs no kernel, no dispatch, no guest, and unblocks #87/#88/#89 |
| **Mint discipline** | **6 minted** (GB-076 + five `-V`), **6 adjusted** (3 re-armed, GB-072 → Ready, 2 parents given gate order), 4 hygiene closes. No implementation ticket minted to fill the Ready lane |
| **Critical path** | **#43 first, out of band, without waiting for its own CI** — `.github/workflows` is absent from `main` and #43 *is* it, so nothing on the board is checkable until it lands → gate the integrated tip → drain #58/#68 (then #1) → **GB-072 dispatch half** → GB-066 device leg → #87/#88/#89 → GB-024. Parallel: GB-076 and the 8 verify cards |

**2026-10-20 — §4.3 re-plan (PLAN-007): the verification lane's first ten verdicts are 10 RED / 0 GREEN, and nine of
them fail on an instrument, not a feature.** Board 99 → 103 tickets (Backlog 2→5 · Ready 11 · In Progress 30→32 ·
In Review 9→8 · Merge Gate 0 · Done 44 · Icebox 3), `lint` clean, `sync_gaming_md.py` IN SYNC 75/75,
`check_verdicts.py` 10/10 propagated. Pass started and ended on `origin/main` = `e2b8e93`.

| what | state |
|---|---|
| **The collapse** | Ten REDs are three walls. **Wall 1 — a staged program is never dispatched:** `test_wave_loader` is 97 literal `load_and_run_program_in_scheduler` calls, so `T4PROBE.BIN` sits on `disk.img` (19328 B, `mdir`-confirmed) and appears **0** times in four independent waves (767/946/999/1428 lines) that exit 63 programs; `t4_w0.sh` fails HELLO/SH/CAT/LS on the same ground. **Wall 2 — the guest probe emits one row** (`prow("write3")`), while every M1 `-V` DoD's rows exist only in the host-mock probe; GB-016/017/018-V each had this as a second independently failing ground. **Wall 3 — the acceptance rig fabricates PASS** (GB-053-V: two of five "PASS" games were never loaded; `ppm_live` samples the top ~104 of 1080 rows) |
| **Minted 4, and only 4** | **GB-072** dispatch via a manifest (`::WAVE.CFG`) → `wabi`; **GB-073** one row table both probes emit + a `--check` → `wgfx`; **GB-075** acceptance identity/liveness from observed events, with injection as the test → `wgames`; **GB-074** `claim.sh` atomicity → `wgames`. Each is one wall, each has a DoD that can fail |
| **Held 3 (first deliberate hold)** | GB-019-V/020-V/021-V → Backlog + `blocked`: they grade rows that do not exist in a wave that never dispatches them. Three DEEP gates to re-derive a known RED; unblock on GB-072+GB-073, then re-mint (a held `-V` is consumed, not laundered) |
| **M0's gate clause is FALSE, not unmet** | "Baseline suites green on `main`" cannot be claimed: intel T3 exits RED (4 graphics FAILs), ARM T3 never halts (GB-012-V: 226504 lines, `[IDLESTUCK] slot=62 st=9 TORTURE.BIN`, all CPUs `c=-1`), and `make t4_compat_parity` — a `host_tests` prerequisite — prints `PASS` having compared 0 rows when `musl-gcc` is absent (GB-007's new clause: a missing leg must exit non-zero) |
| **"Merged is not Done", twice in one pass** | **GB-065:** PR #47's 8 MB mapping is on `main` and three 2026-10-20 waves still show `FAIL: line startpoint/midpoint/endpoint`; size is no longer an explanation (`vm_map_fb` maps 10 MB), so the next action is to *observe* which mapper GRAPHICS.BIN takes and read a pixel back at y=600..700. **GB-063:** reopened — its Done covered two consecutive *intel* tiers and ARM reproduces the idle-no-halt shape on `main` |
| **GB-071 re-scoped by GB-068's own measurement** | A and B are one defect (user code running on a **kernel stack**). The delivery candidate is now `common_trap_exit`'s saved RSP at `[rsp+240]` — the syscall-return path, never instrumented — because `enter_user_space`'s `restore_user_sp_helper` slot is retired at `IN_SCRATCH=1` on 0 of 14 resumes |
| **4 gate-hours of DEEP capacity spent on foregone REDs** | Two duplicate claims (GB-005-V at 00:44:27/28, GB-010-V at 01:12:25/27 — `claim.sh` reads then moves) plus the three held cards. That is a quarter of a day of the scarcest resource, and it is why GB-074 exists |
| **Tooling hazard recorded** | `PUT /issues/{n}/labels` replaces the whole label set; a lane move written as a bare `PUT` stripped `deep`/`verify`/`wt-*` from seven cards for ~1 minute (restored and verified). `board.py move` unions correctly — scripted moves must too, or dispatch labels vanish |
| **Summary** | Posted on **PLAN-007 (#104)**, closed |

**2026-10-20 — §4.3 re-plan (PLAN-006): 18 of 18 In Review tickets had no gate verdict, so every one now
has a `GB-nnn-V`.** Board 79 → 99 tickets (Backlog 2 · Ready 3→22 · In Progress 19 · In Review 18 ·
Done 34→35 · Icebox 3 · Merge Gate 0), `lint` clean, `sync_gaming_md.py` IN SYNC. Pass started on
`origin/main` = `d341b8a`; it ended on `dd391d4` (the board moved under the pass — see the last row).

| what | state |
|---|---|
| **The finding** | Every In Review card was un-gated: **15** had no verdict of any kind, **GB-034** had a Coordinator RED from 2026-10-17 (pre-fix, so it is a re-verification), **GB-044** had a Coordinator `8/8 PASS` on the PR head (not a Verifier, not the merged tree), **GB-066** had a row-level claim. Minted 18 × `GB-nnn-V` (#80–#97), `verify`+`deep`, Ready, each naming its exact gate, pass criteria, and what a RED does *not* mean; each implementation ticket now carries a comment naming its `-V` |
| **T1 measured, not assumed** | `make host_tests` on `d341b8a`: exit 0, **897 checks, 0 failed**, 58 `PASSED` lines. Of 9 `FAIL` string matches, 8 are the new gates' own negative-case proofs and 1 is a skip note — the first pass that can say that, because `t4_device.sh --self-test` and `t4_w0.sh --self-test` now run **inside** the suite |
| **Two instruments disagree (folded into GB-066-V)** | `src/host/t4_parity.sh --device intel` still prints `SKIP — device leg not wired yet`, while `tools/gate/t4_device.sh` is a real per-row in-guest differential. A reader of the first line concludes the M1 device leg does not exist; a reader of the second concludes it does. One leg, one verdict — GB-066-V settles which script is authoritative |
| **Tooling defect the new ticket kind created** | `issue_by_id()` matched with `^GB-016\b`; a hyphen is a word boundary, so `GB-016-V` matched `GB-016` — and `all_issues()` is **newest-first**, so `move GB-016 Done` would have laned and closed the *verification* ticket. `import_seed.py` had the same `\b` in its upsert matcher. Both now `^(GB-\d{3})(?![\w-])`, verified by assertion after minting (`GB-016` → #17, `GB-016-V` → #84) |
| **GB-068 split by fix site — and one of my own comments was refuted 4 minutes before I posted it** | Family A (the one-byte `.bss` NUL overrun at `align_up(_end)`) is GB-068. Family B was *located* (fault address inside the running CPU's kernel scratch frame `[target_sp-296, target_sp)`), but its first delivery hypothesis is **measured dead**: `enter_user_space → restore_user_sp_helper` shows `IN_SCRATCH=1` on **0 of 14** resumes across a 45-fault run, so GB-061 is *not* the source, and the `-0x28` displacement was measured against a stale `__stack_top` (the running kernel gives `-0xA8`, so it is not a constant). I posted the `-0x28` / "GB-061 owns family B" version from the plan row minutes after the refutation landed on the ticket, without re-reading the ticket — so a correction is posted on GB-068, GB-061 and PLAN-006. **The rule, again: a re-plan reads the ticket, not the plan's summary of the ticket.** |
| **GB-071 minted (the one production ticket)** | Family-B **delivery**, split out because it is a different job from family A with a different instrument and it had no owner: locate what hands a user process a kernel-range pointer (candidates: a syscall return value, or a struct field written from a kernel pointer), name it with pid/instruction evidence, fix it, add a regression guard. `deep`, M1, Ready. Do not re-walk the iretq slot there either |
| **GB-049 given the verify-lane lint** | `unverified-in-review`, `stale-verify`, `orphan-verify`. 18/18 un-gated cards were found by a hand-written grep; that detection belongs in `board.py lint`, not in a re-plan |
| **GB-001 escalated** | `decision` + `blocked`: ssh/sudo/`host_tests` clauses met; the NAS shared-store clause needs a credential on .56 (SMB-only, NFS closed). Cost of the pause is **zero lanes parked**; fallback (local evidence home + `git add -f`, split the mount into its own ticket) stated with a time box |
| **One production ticket minted, and only one** | **GB-071** (family-B delivery). Everything else in M0–M4 is behind the same premise (a user program that runs to completion on intel), so minting it would fill Ready with rows that cannot start |
| **`origin/main` moved during the pass** | `d341b8a` → **`dd391d4`** while this pass ran: `e47acd6` (the family-B correction above) and `dd391d4`, which contains **the same `GB-nnn-V` prefix fix** this pass made in `board.py`/`import_seed.py` — same defect, same fix, landed concurrently. Recorded as `dd391d4`'s work, not claimed here |
| **Milestone read** | M1's gate is now **measurable and red**: both instruments exist and fail honestly. Corrected critical path (see the GB-068 row): **GB-068 family A + GB-071 family B delivery** → faults to zero → GB-070 harness half → GB-066-V real device diff → the six shim `-V`s get their first verdicts. GB-061 keeps its own DoD and is no longer on this path. Milestone % is not comparable across passes now (the `-V`s sit in the same milestones but are gate work): M0 6/20 · M1 5/33 · M2 5/9 · M3 5/12 · M4 6/18 |

**2026-10-19 — Policy (caretaker): verification is its own ticket (`verify`+`deep`), run by per-farm
Verifier sessions on DeepSeek v4.1 Flash (dgx1); workers claim `verify` tickets directly; the re-plan mints
`GB-nnn-V` for every un-gated `In Review` ticket, so gate work parallelizes with production.**

**2026-10-19 — GB-068 characterised: two instruction families, and three instrument defects that hid them.**

| what | state |
|---|---|
| **Fault table, read as rows** | 44 serial-log lines carry `memory fault`; a strict line parse read **25**. The SMP console interleaves writes from 8 CPUs, so 43% of rows are shredded mid-line and silently dropped. Every fault count derived from the log is a lower bound until fault lines are atomic. |
| **Family A** | `movb $0x0,(%r8,%rdx,1)` — a **NUL terminator** — at `align_up(_end)`, exact **4-for-4** against each binary's `_end`, with `rw=w in=HOLE region=NONE`: a static buffer at the top of `.bss` overrunning by one byte. |
| **Family B** | `cmpb $0x0,0x1(%rdi,%rsi,1)` with `rsi=-1` — a **byte scan** — at `<64 KiB-aligned base> + 0xDBC8`, base varying per process, `rw=r in=PROT region=NONE`, CR3 correct: a string helper handed a kernel-range pointer. |
| **Family B — located, delivery still open (corrected same day)** | every family-B fault sits **inside the running CPU's kernel scratch frame region** `[target_sp-296, target_sp)`, with `target_sp = &__stack_top - cpu*0x10000 - 4096` (`process.c:1557/3141`) — so a user process is running with a stack pointer inside **its own CPU's kernel stack**, which is why `region=NONE` and CR3-matches are both *correct* measurements rather than contradictions. **Corrections to what I first posted:** the displacement is `-0xA8` against the running kernel, not `-0x28` (I measured `__stack_top` on a stale `hobbyos.elf` 0x80 lower), so it is not a universal constant; and the **delivery is not the iretq RSP slot** — a probe in `restore_user_sp_helper` comparing the value about to become the user RSP against the scratch region reported **`IN_SCRATCH=1` on 0 of 14 samples across a run with 45 faults**, with legitimate user SPs (`0x23FFF000`, `0x17FFFFFFF8`). So GB-061 is *not* the source through that slot. Next candidates: a kernel address returned to user space by a syscall, or a struct field written from a kernel pointer |
| **Dead mechanism — resume-RSP plant** | `enter_user_space`'s user branch fills the iretq RSP from `gs:[24]` (`user_sp_temp`, bridged from the per-CPU `user_sp[]` by `restore_user_sp_helper`); measured clean on every sampled resume. Do not re-walk |
| **Family C (unparsed)** | one shredded row shows a fault at `0x40000000`, the v1 user base, in `MEMTEST.BIN` — the v1-pointer-into-a-v2-process shape. Not characterisable from one line, but not dismissible either. |
| **Dead mechanisms — do not re-walk** | TLS self-pointer (probe printed `block=0x1000005b90 image=0x10 tls=0x1000005ba0`, all correct), unsigned `tls_size` underflow, `.bss` filesz-vs-memsz, v1-overlay base, wrong CR3 (26/26 equal), ELF-induced damage, and "the heap region is never created" (`sbrk` returns `USER_HEAP_BASE_V2`; `malloc`'s v2 branch uses the same constant). |
| **Instrument defect — symbol identity** | `ho_tls_setup_initial` is genuinely **1,903 bytes**: several `static` helpers in one libc translation unit share a section and one symbol, so every PC in the blob resolves to that name. That is how "15+ unrelated programs fault inside TLS setup" became a plausible finding. `-ffunction-sections -Wl,--gc-sections` makes it answerable by `nm`. |
| **Instrument defect — parse coverage** | my table read 25 of 44 rows and I reported the split as a census. The tier should print `N parsed of M raw`, so a shrinking parse can never look like a clean wave. |
| **Frame-of-reference defect — this checkout** | the main worktree was on the worker branch `gb057-recursion-probe`, so `grep tools/t3_tier.sh` read a stale file and "the tier has no fault count" looked true. It is not: `15c1733` and `59f16a8` add it on `main` (`grep -c` = 5 there). Checkout is now `coordinator-main` tracking `origin/main`; the uncommitted plan edits were stale duplicates of content already on main. |

Rule recorded: **before trusting a tool's silence or its labels, make it say something falsifiable — and count what you parsed against what exists.** Two of today's three instrument failures were tools answering a narrower question than the one asked, read as complete.

**2026-10-19 — §4.3 re-plan (PLAN-005): the gate had no instrument, and the compat track has one wall.**

| what | state |
|---|---|
| **Board snapshot** | 73 tickets: Backlog 5 · Ready 2 · **In Progress 20** · In Review 16 · Merge Gate 0 · Done 30. M0 6/16 · M1 3/19 · M2 5/9 · M3 5/10 · M4 6/14. `sync_gaming_md.py` IN SYNC 65/65 before the pass. Repo: `origin/main` = `26325ac`, 5 open PRs (#43, #31, #30, #26, #1), all `mergeable=null` (GitHub recomputes lazily — nothing merged, nothing green by inspection) |
| **Finding 1 — the M1 gate is not measurable, and that is a plan defect, not a kernel defect** | `src/host/t4_parity.sh` leg B is the probe compiled for the compat surface and run **on the host against glibc `ho_*` mocks**; its own header defers the in-guest leg to "once the ELF loader lands", and `--device intel` SKIPs. So M1's sentence ("T4 green for the W0 corpus") has been evaluated against a *model* of HobbyOS for the entire program. No amount of GB-057/GB-065 work can satisfy it. **Minted GB-066** (M1, L, → Ready `wabi`) = the device leg: a probe built both ways, staged through `T3_STAGE`, stdout captured from serial, diffed on rc/errno/stdout, and *failing* (not skipping) when leg B is absent. §7's M1 gate cell now says what T4 means |
| **Finding 2 — zero ELF binaries have ever executed** | probe57 (committed) put the first static-PIE on the image and produced no `[ELF-DISPATCH]` line; the follow-up showed why: `t3_tier.sh` rebuilds `disk.img`, so the staged fixture was wiped before QEMU — a harness bug, now fixed with `T3_STAGE` (stage after build, before QEMU, fatal on failure). GB-013's mapper is good code with **no caller that has reached it in a guest**; that, not the loader's maths, is why M2's SDL leg and M3's W4 Doom leg are unreachable. Also retired: the `mdir \| grep pie.bin` check I published could only ever print `0` (`mdir` renders `PIE      BIN`) — a zero that could not have been otherwise is not evidence |
| **Finding 3 — the board is holding claims, not progressing** | 20 In Progress vs 2 Ready, and the In Progress set is mostly *correct* — those tickets each hold a runtime leg that one wall blocks. The lanes therefore read as healthy while the track is serialized on a single binary execution. Two tickets were Done-in-fact and still In Progress (**GB-063** → Done: `8995009` counting half + `4cefd7d` owner-side bound, both on `main`, `[ORPHAN-WAIT]` then `System halt` in probe56). One fix was Done-in-fact and **not on `main`** (**GB-065**: `3a86967`, the 4 MB→8 MB framebuffer mapping, stranded on a branch whose PR had already merged at `c654013`) |
| **Adjusted, not minted (deliberately)** | **GB-057** split into a/b/c in the plan: a fixed and proven, b (premature halt, 1/8) and c (LAPIC, ≤1/7) open — b matters because it is the one shape that can turn a green tier into a lie. **GB-058** told to *run the policed ARM tier on current `main` before writing code*, because GB-063's hypothesis was that ARM's stall and intel's never-halt were one predicate, and that predicate changed. **GB-065** given its exact next action (rebase one hunk, build intel, rerun T3) so the last red line in the intel tier is not re-derived by the next reader |
| **Why only one ticket was minted** | The temptation was one ticket per stalled lane (M2 SDL, M3 Doom, M4 evidence). Those are not parallel problems: they are the *same* missing premise — an ELF that runs — plus, for M1, an instrument that can see it. Minting five tickets that all block on GB-013/GB-066 would put five honest-looking rows in Backlog and produce no work. The native track (S0) stays the one place capacity converts to playable games this week |
| **Lane moves** | GB-063 → Done (2 commits named). GB-066 created (#74) → Ready `wabi`. EPICs #66/#67/#68 → Icebox (§4.1: Program-2 vision epics live there, and three cards in Backlog were being counted as backlog). After the pass: Backlog 4 · Ready 3 · In Progress 19 · In Review 16 · Merge Gate 0 · Done 32 · Icebox 3 |
| **Tooling defect found and fixed** | `board.py issue_by_id()` only resolved `GB-nnn` titles, so `comment PLAN-005` and `move EPIC-66` both returned **`no issue for …`** — the tool could not address the two ticket kinds the re-plan loop itself creates. That is the mechanism behind the PLAN-003 phantom (a plan ticket that posted its summary and was never closed or laned) recorded on 2026-10-18. It now accepts `GB-`/`PLAN-`/`EPIC-` prefixes and a bare issue number; `import_seed.py`'s WORKSTREAM map also gained GB-063/064/065/066, which existed only in an uncommitted local copy while the board already carried those labels |
| **Summary** | Posted on **PLAN-005 (#75)**, closed |

**2026-10-18 — §4.3 re-plan (PLAN-004) + two tickets minted.**

| what | state |
|---|---|
| **Re-plan pass** | Board snapshot 70 → 71 tickets, `lint` clean, `sync_gaming_md.py` IN SYNC 64/64. Lanes: Backlog 11→9 / Ready 4→6 / In Progress 16 / In Review 14 / Merge Gate 1 / Done 23→25 / no-lane 1→0. Milestones M0 5/16 · M1 2/18 · M2 5/9 · M3 4/10 · M4 4/14. Summary posted on **PLAN-004 (#71)**, closed. |
| **The re-shape reality forced** | The T4 differential-lock layer is the one M1 lane *not* behind the intel wall (it runs on the host), and it is now essentially complete — GB-015/016/017/018/019/020/022/023 merged or on clean PRs. So M1's remaining gap is no longer "measure the contract"; it is "boot a W0 corpus on intel", which is single-threaded on x64 stability: **GB-057** (instance) / **GB-060** (lock durability — *both* shapes landed, `24a7f8a`, T1 boundary proof 27 checks) / **GB-061** (resume fault) / GB-013 loader `#GP` (`program_loader.c:611`) / new **GB-063**. Both T3 tiers remain uncitable; every green so far is T1/T2/T4-host + ARM T5. |
| **Minted GB-063** (M1, M, → Ready `wabi`) | *Scheduler liveness accounting: a finished wave must reach `System halt`.* probe17's intel wave idled forever instead of halting (every CPU in `safe_wfi`, one slot parked) — the same shape as GB-058's ARM `SHELLTEST WATCHDOG: stalled 25 s`. Likely the **shared** root cause of both tier failures: `any_alive` counting a slot in a state the scheduler does not know how to drain. Provable on ARM ⇒ not behind the intel wall |
| **Minted GB-064** (M0, M, → Ready `wgfx`) | *Self-registering host test suite.* PRs #25–#31 all collided on the same `host_tests:` prerequisite line and recipe body in one night (#27 even replaced #29's recipe lines). The resolver exists; the *cause* is one line every ticket edits. Wildcard/patsubst discovery ends the conflict class and unblocks GB-005/GB-044 |
| **PLAN-003 (#64) tidied** | Its loop posted a complete re-plan but never closed the ticket and never gave it a lane — a phantom open plan ticket on the board. Moved to Done, superseded by PLAN-004 |
| **No drift this pass** | Unlike the last two loops, the In Review set is honest — each holds a genuine runtime/leg it cannot yet evidence (e.g. GB-021's PR #35 is held by a real ABI disagreement: `futex(FUTEX_WAKE)` returns `rc=1` for us vs `rc=0` on Linux, i.e. our shim reports a waiter woken when none was). Nothing corrected |

**2026-10-18 — lane fan-out + board-drift correction (Coordinator, continuation).**

| what | state |
|---|---|
| **Ready lane refilled (it was empty)** | Promoted five genuinely unblocked tickets, each with its deliverable, gate, and an explicit note about what is *not* gated: **wabi** ← GB-017 (dev nodes + `getrandom`), GB-020 (`getdents64` + `/proc/self`), GB-019 (ioctl subset) — the core of each is independent of GB-014's syscall routing, which is only their wiring tail; **wgames** ← GB-043 (long-duration playthrough harness, dep GB-040 Done) and GB-045 (gfxbench baseline, no unmet deps). **wgfx got nothing, deliberately**: every remaining graphics/input/audio Backlog ticket is gated by wgfx's own in-flight work (GB-032 SDL, GB-029 audio), so that lane is full rather than idle — manufacturing a promotion there would have been theatre |
| **Board drift corrected (5 tickets)** | GB-025, GB-027, GB-028, GB-030, GB-042 were sitting in In Review / Merge Gate with their work already merged on `main`. Each was **verified present at `origin/main` before** being moved to Done, naming the PR that landed it. A merged ticket in a review lane tells the next reader that work is pending when it is finished — the same class of lie as an instrument that reports the wrong state |
| **Merge queue** | PR #4 closed as superseded (its work landed via PR #10) with a pointer comment so nobody rebases it. PR #1 (DOOM / GB-034) and PR #2 (STARDUST / GB-038) stay red on their T5 legs; PR #17 (GB-059 rig) stays changes-requested. Nothing in the queue had green gates, so nothing was merged |

**2026-10-18 (later) — worker wave landed into the queue.**

| what | state |
|---|---|
| **Second fan-out wave** | wabi ← GB-016 (`uname` shim) and GB-018 (`poll`/`select`/`pselect6`). GB-017/GB-019 went In Review and GB-020 In Progress within the hour of the first wave, so the lane is genuinely fed rather than nominally |
| **GB-016: I demoted a claimed ticket, then corrected it** | I moved GB-016 In Progress → Ready as part of the wave, reading a lane listing taken a minute earlier — `swarm-farm-1` had claimed it in between. Restored to In Progress with an honest note. **Lesson: re-read the ticket's current lane at the instant of the move** (the board is live and claims arrive in seconds), which is the same stale-view error as reading an instrument once and believing it later |
| **PR #25 merged (GB-017 + GB-019 + GB-020)** | One branch, three tickets, +150 lines of T4 differential probe: `main` now races 31 probe rows, static-musl vs the HobbyOS host compat surface. Reviewed rather than rubber-stamped — the probe deliberately does not diff *random payloads* leg-to-leg (only return counts and errno are contractual, which is the correct call: diffing random bytes would have produced a flaky test that "proves" failure) — and I ran the host leg by hand to confirm the rows are real (`getrandom_16 rc=16`, `urandom_open rc=3`, `urandom_read16 rc=16`, `null_write3 rc=3`, `null_read1 rc=0`). Gate: T1=0, parity PASS |
| **PR #26 (GB-035 NES revision) returned to the worker** | Dirty against `main`: 10 content hunks across 5 files, because it revises files `main` already has from the earlier GB-035 merge (`nes.c` 383→411, `nes_test.c` 130→230). I did **not** resolve it — with 10 hunks inside emulator content, choosing a side is a judgement about the worker's own code. Told them precisely what conflicts and why, plus the two mechanical hints (the `host_tests:`-line resolution rule, and that a wholesale replacement must be *stated* in its EVIDENCE comment) |
| **GB-060 scope amended after probe19** | The stale-owner break did **not** fire in the wild, and the log says why: the halted CPU released `proc_lock` (the earlier fix working), a **live** CPU re-acquired it and then wedged *alive* — a shape the halt-keyed break correctly declines (46 `LOCKFOREVER`, no `[STOLEN]`). So the class has two shapes and only one is closed. Plan row updated: remaining work = a break keyed on a **per-CPU progress heartbeat**, never on elapsed time, with the threshold printed on any break |

**2026-10-17 — merge-queue pass (Coordinator, continuation).**

| what | state |
|---|---|
| **Merge queue drained to the M2 ABI-lock layer** | Four increments landed and gated on the integrated tip (`092edf5`, +461 lines): PR #13 `fb_pixel` B8G8R8A8 packing lock (a2eebdb, credited to GB-025), PR #12 gamepad codes in `tools/linux-abi/probe_abi.c` (ad14334, GB-042 governance), PR #11 GB-042 gamepad/joystick ABI lock (49d523f), PR #6 GB-028 clock/timer ABI lock (092edf5). Gate: `make host_tests` T1=0 with `fb_pixel_test_host` 12, `joypad_test_host` 11 and `clockabi_test_host` 13 checks all executing **inside** the suite, plus `MODE=unit_tests hobbyos.elf` links ARM + intel T2A/T2B=0. All four tickets stay In Review: their runtime legs are not citable while the device tier is red (GB-057/GB-058) |
| **Merge-conflict tooling (and a self-inflicted incident)** | Every native/ABI PR edits the same `host_tests:` prerequisite line, so conflicts are guaranteed. Added `resolve_makefile_additions.py` (prerequisite line → token union; recipe lines → keep both; anything else refused) and the rule that **`make -n host_tests` must parse after any resolution**. I first resolved `#6` with the older line-union script, which glued a recipe line onto the prerequisite line, and then committed the merge **with the markers still in the file** — caught immediately by the parse check and the grep, amended to `092edf5`. Exactly the incident recorded for GB-027 earlier; the *check* is what catches it, not the intent |
| **PR hygiene** | Three of four branches were based on stale mains (the conflict work above), and one PR (#13) carried no ticket id in the title and no board comment — merged against GB-025 because the diff named it, with a note on the ticket asking for the id next time. `git fetch && git rebase origin/main` before opening a PR is now stated on the affected tickets |

**2026-10-17 — continuation session (Coordinator on .233; `wabi`@farm-1, `wgfx`@farm-2,
`wgames`@farm-3 alive and dispatched through the board).**

| what | state |
|---|---|
| **GB-057 root cause #1 (the 222 FAIL lines)** | `src/kernel/arch/x64/trap.c: sys_exit` never remapped the exit code out of rdi into the canonical `regs[0]` slot the shared `process_exit()` reads — on x64 `regs[0]` is **rax**, which at `syscall` entry holds the *syscall number*, so `SYS_EXIT == 2` became every intel exit status (`status=512`, `WEXITSTATUS == 2` for **every** child, whatever it returned). That is why only the status-reading suites failed (SEDTEST/GREPTEST/CMPTEST/TSORTTEST/…). Fixed; ARM was already correct (x0 carries the code there) |
| **GB-057 root cause #2 (early exits / ECHILD)** | `sys_spawn`/`sys_spawn_ex` (both arches) retried the spawn-worker slot for `18000 × 100 ms` — a 30-minute **hot spin** by a caller that is *already parked in WAIT_SPAWN holding a PCB slot*, while its worker needs a free slot from the same table: a circular wait that waiting harder cannot break. Watchdog proof captured (7 of 8 CPUs, `age_ms≈350000`, `syscall=8`, console silent, loader starved). Bounded to ~1 s + the documented `-EAGAIN` path |
| **GB-057 blocker #3 (found, NOT fixed)** | With #2 bounded, the same intel wave runs much further and then **hard-deadlocks on `proc_lock`**: `[LOCKFOREVER] lock=0x74530c70 (= proc_lock)` from 7 CPUs, all in `spinlock_acquire_irqsave+0xe` called from `schedule()`, `process_create_internal_ver()`, `process_thread_create()`; after that the console is fully dead (IRQs off, watchdog silent). Suspect: `process_fork()` holds `proc_lock` across `vm_as_clone_into()` + the 32 MiB `kmemcpy`. Next owner: whoever takes GB-057's remainder |
| **T3 tier is now a gate (GB-056/GB-057 DoD)** | `tools/t3_tier.sh` + `make test` / `make test_intel` / `test_arm` / `test_intel` now police their own console: non-zero exit on any `FAIL` line, on a missing `System halt`, or on the wall-clock deadline (hang = defect). Raw boots stay available as `make test_run_raw` |
| **GB-006 worktree false-reds** | `tools/swarm/seed-worktree-vendored.sh` links every gitignored `third_party/…` tree from the main checkout into a `hermes -w` worktree (idempotent; never clobbers a tracked file — `ln -sfn` does, and silently type-changes tracked files, so never use `-f`). Verified: 13 vendored paths linked; `make host_tests` green in a seeded worktree |
| **GB-001 fleet** | all 6 VMs answer ssh + passwordless sudo; `host_tests` green on coordinator + farm-1. The "shared store mounted" clause is **not met**: 192.168.0.56 is SMB-only (NFS closed) and no NAS credentials exist on .233 → `decision` label for Sarah. Not closed |
| **M0 gate** | opened with two named exceptions (see the gate comment on PLAN/board) — GB-008 sync IN SYNC 58/58 (GB-058 minted), GB-009 baseline archived (T1/T2 green both arches, T3 ARM carries 1 real FAIL (GB-058), T3 intel red → GB-057) |
| **M1 gate corrected** (2026-10-18) | I labelled GB-065 the M1 blocker and that was wrong: `gaming.md:372` makes the M1 gate *"W0 corpus boots on intel + writes expected output, T4 green"*, and GB-065's failures are in `GRAPHICS.BIN` — a T3 member, not the W0 corpus, not T4. Worse, the T4 leg that matters does not exist: `t4_parity.sh` compares musl-Linux against **host glibc `ho_*` mocks** and its own header defers the in-QEMU leg to "once the ELF loader lands"; there is no `src/user/` probe binary, so `--device intel` SKIPs. **T4 green has never meant HobbyOS agrees with Linux.** The loader itself is on `main` (`elf_image.c` ET_DYN/`is_pie`, `elf_loader.c`, `program_loader.c:757`) and GB-013 sits In Review. Next: verify GB-013's DoD by execution (static-PIE maps+boots on intel) and build the in-guest T4 probe |
| **GB-057-a FIXED — the wave halts** (`8995009`) | The wave-end predicate was a negative test, so `PROC_STATE_BLOCKED` counted as live work and a parentless blocked `SPAWN.BIN` kept `any_alive` true forever: `!any_alive` never fired, `scheduler_finished()` never ran, the wave never halted (7 of 8 runs; the trace was already in probe42 as `slot=12 st=5 parent=-1`). Fix: a **waker-liveness** predicate — the scheduler lazily stamps the first pass that sees a slot `BLOCKED` and excludes it only after a 60 s budget, printing `[ORPHAN-WAIT] slot/pid/name/parent/wake_ms/waited_ms` and stating the slot is **not reaped**. Not a state exemption, because a wave whose children wait legitimately on a pipe must not truncate. Proven in two consecutive intel tiers (`halt=2 orphan=1` both, slots 12 and 13, same tail test `MMTEST.BIN`). Line count fell 3,689 -> 1,296 because **2,315 of the old run's lines were `[IDLESTUCK]` spin chatter** — so line count was partly a mark of a *sick* wave, retiring a number I had been quoting as progress. Also landed: `[ALIVE-SET]`/`[IDLECLAIM]` print `<torn:a/b>` instead of quoting a racy pid (probe54 printed `pid=62831` for slot 1). Remaining on GB-057: **b** (premature halt, 1/8, probe54) and **c** (LAPIC fault, <=1/7, the widened spawn storm failed to provoke it twice) |
| **PR #1 (DOOM) / PR #2 (STARDUST)** | rebased onto `origin/main` (both were based on stale mains: 8101128 / e3039f1). PR #1 T1 gate green on the rebased tree (incl. ELF-image/ABI/fb_ioctl/evdev/t4_parity inside `make host_tests`). T5 playthrough still unrun → both stay In Review. Review note: both harnesses hardcode `ROOT=$HOME/Documents/GitHub/HobbyOS` and assert a magic launcher slot (12) that neither PR pins in `desktop.c`'s `pinned_games[]` |
| **PR #1 gate verdict (intel T5)** | **NOT merged.** T1 green; launcher pin fixed by the coordinator (DOOM at idx 18, structurally asserted) and the harness's IWAD false-positive fixed (its own `[disk] IWAD …` build echo was being matched). T5 still **red and real**: `nonzero=1024000, colors=6` over the whole centre region = a flat fill, and the serial shows only `DESKTOP.BIN` was ever loaded → **DOOM never launched**; the screendumps are the desktop background. Handed back with the launch-path + stdin-input (desktop apps get keys on **stdin**, not `get_events()`) work named. Patch: `~/.hermes/cache/scratch/gb057/pr1_coordinator_review_fixes.patch` |
| **Worker lane movement (continuation)** | `wgames` fixed two real STARDUST defects (unpinned title; no stdin input path) and posted **green ARM T5** evidence → GB-038 at Merge Gate (intel T5 + harness de-magic-index still required). `wgfx` posted ABI-lock increments for GB-027 (evdev) and GB-030 (memfd) — accepted as first increments, not as the tickets; both missing the runtime node / PR hygiene. `wabi` pushed a parallel tier-gating branch (`ticket/57-intel-t3-gate`) that duplicates the landed `tools/t3_tier.sh` → redirected to GB-057's remaining blocker (the `proc_lock` deadlock) |
| **Re-plan (PLAN-002)** | M0 gate opened with named exceptions + §4.3 re-plan recorded on the board (ticket #60, closed). Gate rule now in force: no intel tier citable until GB-057 closes, no ARM T3 citable until GB-058 closes |


**2026-10-17 — kickoff (Coordinator on .233; workers `wabi`@farm-1, `wgfx`@farm-2, `wgames`@farm-3 in
long-lived `hermes -w` tmux sessions).**

| what | state |
|---|---|
| Board (GB-002) | 55 seed tickets + PLAN-001; lanes are labels (no Projects REST in this Forgejo build, §4.1) |
| Board automation (GB-003) | webhook `coordinator-board-hook` + 60 s poll, both systemd units on .233 (§11.2a) |
| Model split (GB-004) | DEEP `dgx1:8000` on the coordinator, FLASH `llm:8000` on farms 1-3 (§11.3) |
| Plan sync (GB-008) | `tools/board/sync_gaming_md.py` — IN SYNC, drift detection proven |
| Baseline (GB-009) | T1 green (coordinator + farm-1); **T2 green arm + intel** (344/0 intel); **T3 ARM green (0 FAIL lines), T3 intel red-but-exit-0** (222 FAIL lines) → GB-057. The baseline is **not** complete on intel and no intel gate may cite T3 until it is |
| ABI contract (GB-010) | `docs/linux-abi.md` v1 (measured; `gen_abi_doc.py --check` guards drift) — In Review |
| ELF loader (GB-013) | **Done 2026-10-21.** Parser + segment planner + **relocation pass** (`elf_apply_relocations`, `R_X86_64_RELATIVE`/`R_AARCH64_RELATIVE` applied over the stage, unknown types refused). Proven by execution: a real musl `-static-pie` maps all `PT_LOAD`, enters at `USER_IMG_BASE + e_entry` (printed), and runs its own code past entry (`PC=0x1000001C56`). The program then dies of **GB-068**, which kills every program regardless of build |
| Syscall table (GB-014) | 114 measured numbers + errno locked by goldens (T1); **shims + trap routing open** |
| First worker MR | **GB-026 merged** (fb ABI surface, worker `wgfx`, branch `ticket/26-fb-ioctls`); gate = T1 + both kernel links |
| Second worker MR | **GB-007 harness merged, In Review** (T4 differential host leg, worker `wabi`); device leg gated on GB-013 |
| Fleet pitfall | workers run in `hermes -w` worktrees, which do **not** carry gitignored `third_party/*/src` — vendored lanes (C++) go red in a worktree while `main` is green; seeded as GB-006 |
| Toolchain | `musl-tools` installed on the coordinator so the T4 harness leg A is real (`musl-gcc -static`); belongs in the fleet provisioning (GB-006) |
| Gate discipline (first incident) | a worker closed **GB-034 + GB-038 from the Ready lane** with the T5 playthrough unrun; both were **reopened to In Review** by the coordinator, the blocker (missing staging cache) was fixed, and the T5 evidence is now the gate. Recorded as the reason `tools/board/board.py` never closes from Done alone: `move` to Done closes, everything else is a lane swap |
| Fleet fix (blocker) | every VM's `~/.local/share/OVMF/OVMF_CODE_4M.fd` was a **symlink to the read-only system file**, and the Makefile passes it to `-pflash` — which opens read-write, so **every x86 QEMU run died with `Permission denied`** (ARM was fine: `-bios`). Replaced with a real, user-writable copy on .233 and farms 1-3 (`cp /usr/share/OVMF/OVMF_CODE_4M.fd …` + `chmod u+w`, sha256 identical to the system file). This blocked the whole Intel test path — i.e. the compat track's T2/T3/T4/T5 — until fixed |

Merge path for workers (no GitHub token on the farms): push `ticket/<n>-<slug>` to `origin`, post an
`EVIDENCE` comment on the ticket, and the Coordinator merges from `origin` after running the gate.
The automated merge-gate CI is GB-005 — until it exists the Coordinator runs the gate by hand and
records the output on the ticket.
