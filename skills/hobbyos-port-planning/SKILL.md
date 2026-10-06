---
name: hobbyos-port-planning
description: "Use when planning a large HobbyOS port or feature effort."
version: 1.0.0
author: Hermes Agent
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, planning, porting, multi-agent, provenance, roadmap]
    related_skills: [hobbyos-development, hobbyos-textutils-port, hobbyos-add-userland-program, hobbyos-kernel-constraints, hobbyos-desktop-compositor, hobbyos-run-tests]
---

# HobbyOS: Port & Feature Planning (multi-agent plan documents)

## Overview

Big moves on HobbyOS arrive as **plan documents**: a survey picks the target, an
audit maps the OS-side gap, and the document is decomposed so several agent
sessions can execute it in parallel without colliding. `browser.md` (repo root —
the browser-program doc; its selection has already been rewritten once, so it
doubles as the worked example of a re-selection and of a two-track /
dormant-contingency structure) is the flagship instance; `docs/gnu-ports.md` is
the house style. Producing one is a research job first and a writing job second — an
ungrounded plan is worse than none, because future sessions will trust it.

## When to Use

- "Find/select software to port", "plan support for X", "write <name>.md",
  re-selecting or revising an existing plan doc, roadmaps for multi-session
  work on HobbyOS.
- When **executing** such a plan: pick your lane from its §7 lanes table, stay in
your lane's owned paths, honor the frozen-interface table (single writer per
file), and never tick a gate you did not actually run.

Don't use for: single-app implementation (→ [[hobbyos-add-userland-program]]),
running tests (→ [[hobbyos-run-tests]]), kernel rules (→
[[hobbyos-kernel-constraints]]).

## Deliverable

One repo-root markdown doc in the gnu-ports.md style; skeleton in
`templates/plan-doc.md`. Planning sessions write ONLY that artifact — run
`git status` before and after; other sessions may have in-flight edits in this
tree, leave them alone. The user often says "your role is not to change code":
honor it literally. Leave the doc uncommitted and offer the commit (repo rule:
local commits only, one phase = one commit) — but re-check `git log` / `git
status` immediately before reporting or committing: the user may have committed
the doc themselves between turns, in which case your late edits become a small
follow-up commit on top (never amend or re-commit theirs, never push, match
the repo's message style).

## Procedure

1. **Extract the selection constraints from the ask** and encode them as survey
   filters (e.g. "graphical, not curses", "open source with sources", "not as
   complex as Chromium"). The survey table must list rejected candidates WITH
   reasons — the rejections are as load-bearing as the pick, and runners-up get
   explicit revisit/fallback triggers.
2. **Survey candidates on the web** for current releases, dates, licenses and
   dependency counts. Dependency count is the real size metric; "it's smaller"
   from memory is not evidence. For every serious candidate, pull its **own**
   build/porting docs (`Porting.md`, `INSTALL`, `BuildInstructions*`, port
   READMEs) and map each hard requirement — toolchain, language runtime, GUI
   framework, graphics stack — onto what this OS has or has scheduled;
   requirements drift (an engine remembered as plain C++ may now require a Rust
   toolchain and a Qt-class UI). Check whether the platform integration you
   would target is still alive **upstream for non-first-party platforms** — if
   it survives only in other forks, price the ongoing fork maintenance
   explicitly. Find precedents (small/exotic OSes, embedded lineages, in-tree
   non-mainstream ports) and use their timelines to calibrate scope.
3. **Analyze the chosen source before sizing OS work.** Shallow-clone the exact
   tag into `~/.hermes/cache/scratch/` (shallow clones do NOT carry tags — fetch
   explicitly: `git fetch --depth=1 origin refs/tags/<tag>:refs/tags/<tag>`).
   Grep the **dependency surface** — the actual API calls the port needs from the
   OS (Xlib/Xft calls, POSIX calls) — and read `configure.ac`/`configh` for the
   feature pins. That call list, not the app's LOC, is what the OS must provide;
   it becomes the plan's inventory appendix.
4. **Audit the repo for every subsystem the work touches** and anchor each fact
   to file:line (grep — never trust memory or a compacted transcript). Find the
   precedent to copy: xcalc for X11 userland apps, nano for vendored C trees
   (byte-identical vendoring + marked surgical patches + hand-pinned config),
   textutils for transcription ports.
5. **Pin provenance for real**: download the exact tarballs, `sha256sum` them,
   record tag→commit with `git rev-parse 'vX.Y.Z^{}'` (an annotated tag's object
   sha is not the commit — peel with `^{}`). For large sources, prefer the
   project's published checksum files (`.sums`, `SHA256SUMS`, release metadata)
   over downloading to hash, and record the sums URL so anyone can re-verify. For a dependency the
   **consumer builds itself** (browser-engine-class pins like Skia/ICU), pin the
   *recipe* too: take the consumer's own compile definitions and file list from
   its build files, and prove fidelity by content-diffing the fetched upstream
   against the subset the consumer bundles — identical content is the
   pin-fidelity proof.
   When the build config has a real fork (old vs new graphics stack, legacy vs
   modern API), pin **both** candidates and make "switch to the fallback pin" a
   named, timeboxed gate decision — never leave the fallback implicit. Keep
   superseded pins in a "retired" row of the table rather than deleting them.
   Put the table in the doc.
6. **Write the plan** from the template. Freeze interfaces in M0, before any lane
   starts. Every milestone gets: tasks (checkboxes, with paths), a runnable Gate,
   and an Evidence requirement. Anything you could not verify becomes an explicit
   "verify" task — never a guessed constant.
7. **Design the parallel execution** (rules below) and give each lane a session
   brief (owns / depends / first tasks / DoD / evidence / skills to load).
8. **Report**: artifact path, the pick + why, headline scope, offer commit /
   start of M0. Keep it short.

## Plan-doc lifecycle (re-selection, review, supersession)

- **When the ask's bar moves after the plan is delivered, re-run the selection —
  never patch around a dead pick.** A requirement change that touches the
  selection premise (a new must-have capability, a new named acceptance
  target) is a re-plan trigger: re-survey, then rewrite the doc in place as
  vN+1 — bump the title/status line, add a "what changed from vN" diff table,
  list what carries over and what is now off the critical path, keep retired
  pins in the record, and append to the fix log.
- **When the requirement names live services, capture what they actually serve
  as dated evidence** — plain `curl` probes, more than one User-Agent; watch
  for JS-required redirect shells, server-rendered-only content, and vendor
  "lite" editions. Probe output belongs in the doc: it decides the real bar,
  and it survives context loss.
- **Record maintainer verdicts in the doc, not just chat.** A one-line review
  becomes: updated status line, a dated decision note where the options were
  laid out, and a fix-log entry. Re-scope the losing option to an *optional
  contingency / dormant by default* instead of deleting it — executors must be
  able to tell scheduled from merely documented, and a dormant fallback keeps
  its value.
- **The doc must be executable without the planning session's context**: dated
  evidence, pins, frozen interfaces, gates, commands, and per-lane briefs all
  inside it, living in the repo so every session reads the same file.
- **Fix logs read newest-first.** A §11-style fix log in practice carries the
  latest entries at the section head and the oldest at the file tail, despite
  the "append-only" wording — read from the `## 11` header *down* for current
  state; reading from the file end shows the wrong end.

## Parallel-execution design (what makes these plans work)

- **Freeze interfaces before lanes start**: ABI numbers, protocol grammar,
  header surfaces, config-pin sets — one designated writer each. Lanes code
  against the frozen text, not against each other.
- **Host-side prep is the early parallelism engine**: patch validation on a
  normal Linux build of the upstream, fixture authoring, config-header seeds
  need no OS build and run while kernel lanes are still in flight.
- **One writer per file; an integrator owns shared files.** Makefile, disk
  recipe, test-wave lists, and the plan doc itself flow through a single
  integrator; lanes submit diffs in their reports instead of editing them.
- **Two-build rule for third-party patches**: a patch must apply and run on a
  host build of the pristine upstream before it is trusted in the OS build — a
  patch that only ever compiled in the OS tree hides its own bugs.
- **Gates run on the merged tree** by the integrator; a lane may not tick a gate
  it did not run (the repo's evidence rule applies to plans as much as code).
- **Design-note-first for the deepest lane**: the lane that will rewrite the
  kernel/ABI core delivers a design-note-only artifact first; the integrator
  reviews it and appends binding resolutions (open questions answered,
  conditions, consents) to the note before implementation is dispatched.
  Provisional ABI numbers stay renumberable while nothing implements them —
  shift them before first ship, never after.

## HobbyOS facts to re-verify at plan time (they drift — grep to confirm)

- **Syscalls**: check `src/include/syscall.h` for the next free number and
  `SYS_MAX` (block 41–59 is reserved for the posix.md sequence; recent work
  started at 65). A new syscall needs the header, both arch dispatch paths, fd/
  process glue, and libc wrappers; keep legacy callers (e.g. the blocking
  `SYS_CONNECT`) working. Dispatch is an if/else chain, never a function-pointer
  table (load-base rule) — see [[hobbyos-kernel-constraints]].
- **FPU**: userland floating point is unsupported on both arches — no FP enable,
  no FP context save/restore. Any float-using port (C++ toolkits, layout
  engines) carries an FPU workstream; see [[hobbyos-kernel-constraints]] for
  what enabling it requires.
- **Loader**: since P2 the v2 loader demand-pages images and the old ~1 MiB
  small-program window is gone (`USER_IMG_SIZE` is 128 MiB at last check) —
  grep `program_loader.c` / `vm.c` for the live caps before promising a size.
- **libc**: verify which POSIX surfaces exist (`src/libc/include/`) before
  promising any; sockets/netdb/select were absent at last check.
- **Desktop protocol**: keys arrive as composed bytes/ESC sequences; no
  modifier-state message, no Alt tracking, no wheel forwarding; one message =
  one atomic `write()`. Contract in [[hobbyos-desktop-compositor]].
- **Disk image**: FAT16, 256 MiB, 8.3 names only — the kernel FAT reader matches
  8.3; VFAT long names written by mcopy are silently ignored, so use `_T.BIN`
  style suffixes for anything on the image.
- **QEMU networking** is user-mode (slirp): host = 10.0.2.2, DNS = 10.0.2.3 —
  reusable for HTTP/TLS fixtures instead of real-internet tests.
- **Engine/runtime-class ports** (JS engines, browser engines, large C++
  frameworks): consult `browser.md`'s P-stage milestones — they enumerate the
  OS-wide prerequisites (threads, VM/address-space rework, IPC, libc++) that
  anything this heavy will need, with their current status.

## Pitfalls

1. **Enumerating an app's API surface from memory.** The port's cost lives in
   its dependency call list (every Xlib call, every POSIX function). Grep it out
   of the pinned tree; the appendix inventory + its regeneration command is a
   deliverable, not a nicety — documents drift, the command refreshes them.
2. **Guessing constants you could not verify** (loader limits, struct fields,
   message formats). Mark them "verify" tasks. One wrong constant discredits the
   whole doc for every future session that trusts it.
3. **Writing the plan as a narrative.** Multi-agent plans compose from tables:
   lanes, ownership, waves, frozen interfaces, gates. A session must be able to
   find its lane in under a minute.
4. **Letting two docs own one truth.** Project state lives in the plan doc;
   skills carry only the workflow. Don't mirror repo facts into skills — they
   rot (this skill says "verify", never "the number is X forever").
5. **Touching other work in the tree.** Planning sessions check `git status`,
   report pre-existing modifications if relevant, and change none of them.
6. **Restating `docs/gnu-ports.md` conventions** instead of referencing them.
7. **8.3-name collisions** for anything headed to `disk.img`.

## Verification Checklist

- [ ] Survey table includes rejected candidates with mechanical reasons.
- [ ] Candidate hard requirements and platform-path liveness were checked
      against the candidate's own docs, not reputation.
- [ ] Provenance table has real sha256s — from published checksum files or a
      local hash, never recalled or partial — plus tag→commit.
- [ ] Every current-state fact has a file:line anchor and a regeneration command.
- [ ] Milestones each have tasks with paths, a runnable gate, an evidence rule.
- [ ] Lanes table + frozen-interface table + ownership map exist; every shared
      file has a single designated writer.
- [ ] Un-verified items are explicit "verify" tasks; no invented constants.
- [ ] Doc carries its review state (status line), dated decision notes, and an
      append-only fix log; revisions include a "what changed" diff.
- [ ] `git status`: the plan doc is the only thing this session created.
