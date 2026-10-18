# Feasibility re-derivation: RimWorld / DSP / Total Annihilation on HobbyOS

*GB-011 (DEEP). Author: wgames (swarm-farm-3). Re-derives each game into a
HobbyOS-deployable design: what the loop actually is, what it needs from the
kernel, what it needs from the loader, sized against what exists — with every
capability cited as either **landed** (on `main`) or **ticketed** (board ID).
No capability is assumed that is neither landed nor ticketed.*

Capability ledger used throughout (board state 2026-10-18, games/native + ABI tracks):

| Capability | State | Ref |
|---|---|---|
| Native game ABI (C, own graphics lib, fbcon framebuffer) | **landed** | PONG/MILLIPED/XNIBBLER/XCRYPT/NES on `main` |
| Shared evidence rig / perf base / replays | **landed** | `tools/evidence_rig.py`, GB-046/043/045 merged+InReview |
| ELF dual-ABI compat (Linux syscall translation) | **ticketed** | GB-014 (In Progress) |
| Linux ABI reference doc | **ticketed** | GB-010 (In Review) |
| Threads under compat (clone→HobbyOS, futex) | **ticketed** | GB-021 (Backlog) |
| SDL2 static-musl profile (fbcon + evdev) | **ticketed** | GB-031 (Ready) |
| SDL app under compat (window, input echo) | **ticketed** | GB-032 (In Progress) |
| Audio (virtio-sound stub /dev/dsp) | **ticketed** | GB-029 (In Progress), GB-051 (Backlog) |
| OpenGL / Vulkan (any 3D, incl. software) | **NOT ticketed** (gap) | — |
| Mono (C#) runtime | **NOT ticketed** (gap it is the roadmap) | GB-054 stages it (Backlog) |
| intel spawn/fork | **ticketed** | GB-057 (In Progress) |

---

## 1. RimWorld — the Mono text-factory with a 2D view

**The loop**: a persistent colony sim. Ticks a scatter of pawns + world grid;
parses/runs a large amount of Ludeon *XML mod + C# behavior*; renders each tick
to a 2D isometric-ish tileset; streams a heavy save file; plays audio. Its
head is C#/Mono (the game is shipped as IL that runs on Mono/Unity-style CLR),
not a thin C++ core.

**Kernel needs**:
- Threads (a real CLR + game threads) — **GB-021 ticketed**, not landed.
- Robust memory/dynamic allocation at scale (RimWorld is a memory-heavy sim) —
  landed at native-ABI level (HEAP/MEM tests green); a CLR adds its own
  allocator on top, which is loader/runtime work, not kernel.
- File/save IO + a large virtual filesystem — landed for native; under compat it
  rides GB-014's open/read/write translation.
- Audio — **GB-029 ticketed** (not landed).
- Input — landed (virtio-input → desktop, games on `main` read keys).

**Loader / runtime needs**: a **Mono-compatible CLR** running Linux-managed IL
against the compat syscall surface (GB-014) + threads (GB-021). This is exactly
what **GB-054 (Program-2 roadmap: Mono (RimWorld) staging) already stages as a
Backlog ticket**. There is no Mono capability on `main`.

**Sizing vs what exists**:
- Native 2D + input + memory: exist.
- Compat ABI (GB-014), Linux thr/ABI (GB-021), SDL (GB-031), audio (GB-029):
  ticketed, **none landed**.
- Mono runtime: not built, gated behind the compat stack; GB-054 is the staging
  ticket.

**Verdict**: **not feasible this program's S0/S1 native track.** Feasible as a
Program-2 epic only *after* GB-014+GB-021 land and a Mono runtime is ported onto
them (GB-054 is the roadmap anchor). Epic (below) stages exactly those
dependencies + the Mono-port sub-tickets; no new capability is assumed.

---

## 2. Dyson Sphere Program (Unity/DSP) — the pack factory at scale

**The loop**: a 3D planetary logistics factory. A Unity engine head runs a
very large IL2CPP/AOT C++ core; the visible surface is a **real-time 3D**
camera+mesh pipeline (planets, belts, conveyor networks) — not a 2D tileset.

**Kernel needs**:
- Threads + futex — **GB-021 ticketed**.
- mmap/vmem at builder-game scale (huge world state) — landed at native level
  (vm v2/memfd are in the selfhost kernel core **GB-012**, ticketed-in-Ready).
- Interrupt/signal plumbing for a real-time loop — ticketed (GB-023 signals,
  Backlog).
- Audio — **GB-029 ticketed**.
- Input — landed native key/mouse (evidence rig drives mouse+keys).

**Loader / runtime needs**:
- An **ELF dual-ABI** loader running the IL2CPP AOT binary — **GB-014** (+ GB-012
  selfhost loader) ticketed, not landed.
- **SDL** for window + device input — **GB-031/GB-032 ticketed**, not landed.
- A **3D graphics path (OpenGL or Vulkan, incl. a software renderer such as
  Mesa-lvmpipe on the fbcon framebuffer)** — **NO capability and NO ticket
  exists**. This is a hard gap this doc must not paper over: the game's core
  surface is 3D.

**Sizing vs what exists**: none of the compat head (ELF, SDL, GL) is landed; the
3D/GL capability has no ticket at all.

**Verdict**: **infeasible this program.** The epic cannot be staged without
inventing a GL capability, which the gate forbids — so the DSP epic is staged
to require first opening the 3D-graphics gap ticket (a Mesa/software-GL-on-fbcon
track), and every other dependency is cited to GB-014/GB-021/GB-029/GB-031.

---

## 3. Total Annihilation (TA) / Spring RTS — the 3D RTS diamond

**The loop**: a real-time strategy over a **3D terrain** with a software-rendered
command layer. The classic engine is C++ (the Game and the Spring RTS re-host
are both C++); it turns a large map + unit graph each tick and draws terrain
with an OpenGL backend.

**Kernel needs**: threads/futex (**GB-021**), mmap/vmem at map scale (**GB-012**,
Ready), IO + input (landed native; compat via GB-014), audio (**GB-029**),
deterministic timer/tick accounting (landed: kernel timer/time math green).

**Loader / runtime needs**: ELF compat (**GB-014**), **SDL** (**GB-031/GB-032**),
and an **OpenGL surface** for its terrain renderer.

**Sizing vs what exists**: like DSP, the compat head is not landed and the GL
capability has **no ticket**.

**Verdict**: **infeasible this program**; the TA/Spring epic is staged to
depend on the same 3D-graphics gap ticket + the compat/SDL/audio stack. The
deterministic-tick + C++ ABI parts are the smallest tail, but the 3D surface
is the wall.

---

## Cross-cutting conclusion

All three share one shape: the **native S0/S1 graphics path is 2D (fbcon)**, and
all three games are either **managed/3D (DSP/TA) or managed-plus-2D (RimWorld)**.
The bottleneck is not the native game tech — it is that the **compat (ELF+sdl)
and any-3D (GL) capabilities are not landed**, and 3D has no ticket at all.

Concretely the re-derivation sizes:
- RimWorld: blocked on Mono (GB-054 anchor) behind GB-014+GB-021.
- DSP: blocked on GB-014+GB-021+GB-031 + a **to-open** GL gap ticket.
- TA: blocked on GB-014+GB-031 + the **same to-open** GL gap ticket.

No capability assumed that is not landed or ticketed. Three staged epics follow
on the board (`program-2/` backlogs), each with staged sub-tickets that either
cite existing capability tickets or open the one missing gap (GL).
