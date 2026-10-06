# <PROJECT> Plan — <component> on <pinned upstream>

Status: <proposed / reviewed — record the review state and any endorsed
direction>. Planning only — no code changes; this document is the execution
plan for the work.

<!-- On a revision: bump the title (append `(vN+1)`), update the status, add a
"What changed from vN" diff table (carry-over / now off-path / retired pins
kept for the record) right after §0, and append to the fix log. Executors must
never have to guess which sections are current. -->

Audience: agent sessions + maintainers. Read §0 and §7 before touching
anything; the frozen-interfaces table in §7.3 is binding.

<!-- Guidance: model the doc on the repo's house style (docs/gnu-ports.md:
survey → phases → gates → provenance → fix log). Replace every <angle-bracket>
item; delete all guidance comments before finishing. -->

---

## 0. TL;DR and how to use this document

<!-- The pick/scope in one paragraph; a small table of what the OS side must
build; pointers to milestones (§6) and parallel execution (§7); the evidence
rule. A later one-line review verdict becomes: updated status line, a dated
decision note next to the options, and a fix-log entry — re-scope the losing
option to dormant/optional rather than deleting it. -->

## 1. Selection (port projects)

### 1.1 Requirements
<!-- The ask's constraints, turned into selection criteria. -->

### 1.2 Survey

| Candidate | License | Size / engine | Deps | GUI | Verdict |
|---|---|---|---|---|---|
| <candidate> | <license> | <order-of-magnitude> | <count/kind> | <toolkit> | **Rejected:** <mechanical reason> / **SELECTED** |

<!-- Rejections are load-bearing: give reasons. Size = dependency count.
Check serious candidates against their OWN build/porting docs (toolchain,
language, GUI framework) and record whether the platform path is alive
upstream or fork-maintained; use precedents' timelines to calibrate.
Runners-up needed? -->

### 1.3 Why <chosen>
### 1.4 Runners-up and revisit triggers
### 1.5 Licenses  <!-- static-linking compatibility; where license texts ship -->
### 1.6 Non-goals

## 2. Pinned sources & provenance

| Source | URL | sha256 |
|---|---|---|
| <name-version.tar.xz> | <url> | `<from published checksums / locally hashed>` |

<!-- Plus: tag → peeled commit (`git rev-parse 'vX^{}'`), release dates,
and a note on which tag to pin and why. Large sources: use the project's
published `.sums` / SHA256SUMS files (record that URL) instead of downloading
to hash. Build-config forks get a second, named fallback pin plus the gate
decision that switches to it; superseded pins move to a "retired" row. -->

## 3. Current-state snapshot (grounding)

<!-- Every fact anchored to file:line, from greps run at planning time.
"If any of this has drifted when you start, update this section first";
regeneration commands live in the appendix. -->

## 4. Gap analysis → the work to schedule

<!-- What exists vs what the project needs, as concrete deliverables. -->

## 5. Architecture decisions (binding)

<!-- AD-1..AD-n: each with rationale + explicit revisit criteria. -->

## 6. Milestones

<!-- Per milestone: Goal / Tasks (checkboxes with paths + acceptance) /
Gate (runnable commands) / Evidence. M0 = groundwork, pins, freezes. -->

### M0 — Groundwork, pins, freezes
- [ ] T0.1 <task with path>

**Gate M0:** <runnable command(s)>

### M1 — ...

## 7. Parallel execution plan

### 7.1 Lanes

| Lane | Scope (milestones) | Owns (files/dirs) | Depends on | Runs parallel with |
|---|---|---|---|---|
| L1 | <scope> | <paths> | <deps> | <lanes> |

### 7.2 Waves (critical path)
### 7.3 Frozen interfaces (binding)

| # | Interface | Owner (sole writer) | Change protocol |
|---|---|---|---|
| F1 | <e.g. syscall ABI> | <lane> | <amendment process> |

### 7.4 File ownership map  <!-- which lane touches which file; shared files -->
### 7.5 Session briefs          <!-- per lane: owns/depends/first tasks/DoD/evidence/skills -->
### 7.6 Integration protocol    <!-- worktrees, local commits only, integrator merges, single-writer Makefile -->

## 8. Testing & acceptance strategy

### 8.1 Tiers
### 8.2 New tests inventory

| Test | Kind | Where |
|---|---|---|
| <name> | host / in-OS / E2E | <path> |

### 8.3 Acceptance checklist (v1 "done" = all checked, both arches)
### 8.4 Evidence standards  <!-- no "done" without raw output from the real case -->

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | <risk> | <mitigation> |

## 10. Open questions (close in M0)

## Appendix A — Frozen interface drafts (ABI / protocol grammar sketches)
## Appendix B — Source inventories + regeneration commands (grep blocks)
## Appendix C — Vendoring layout & patch policy

## 11. Fix log (append-only)
