# HobbyOS Hermes Skills

Agent skills that teach the [Hermes agent](https://github.com/NousResearch) how to
develop HobbyOS competently — encoding the project's non-obvious build/run/test
workflows, the QEMU-display capture tricks, the bare-metal coding rules, and the
remote GPU-over-RDMA setup.

## Skills

| Skill | Purpose |
|-------|---------|
| [`hobbyos-build-and-run`](hobbyos-build-and-run/SKILL.md) | The `ARCH`×`MODE` build matrix, which target for which job, deadlock/shutdown discipline. **Start here.** |
| [`hobbyos-screenshot`](hobbyos-screenshot/SKILL.md) | Capture the QEMU framebuffer as PNG — local Mac qemu (QMP `screendump`) **and** remote Proxmox VM; black-screen check. |
| [`hobbyos-gui-test`](hobbyos-gui-test/SKILL.md) | Drive the desktop with synthetic QMP mouse/keyboard; validate the framebuffer; `qm sendkey` for VGA consoles. |
| [`hobbyos-run-tests`](hobbyos-run-tests/SKILL.md) | The three test tiers (host golden / kernel unit / integration) + two-VM RDMA test; the 30 s deadlock rule. |
| [`hobbyos-add-userland-program`](hobbyos-add-userland-program/SKILL.md) | Exact Makefile recipe to add a new user program + optional host test, with a completion checklist. |
| [`hobbyos-kernel-constraints`](hobbyos-kernel-constraints/SKILL.md) | ARM64 EL1 rules that prevent trap classes (alignment, no SIMD, byte-wise parsing, Device memory). |
| [`hobbyos-proxmox-gpu`](hobbyos-proxmox-gpu/SKILL.md) | Remote Intel/GPU-over-RDMA deploy + serial/screenshot debugging on `192.168.10.174`; current Xid-79/MSI-X blocker. |

## Deploy to local Hermes

```bash
./skills/deploy.sh            # sync into ~/.hermes/skills/hobbyos/ and verify
./skills/deploy.sh --dry-run  # preview changes without touching anything
```

The script copies each skill into the `hobbyos` category under
`~/.hermes/skills/`, overwriting existing versions, prunes skills that were
removed from this repo, and confirms Hermes registered them (`hermes skills
list`). Hermes discovers filesystem skills automatically (they show as source
`local`, auto-enabled) — new `hermes chat` / `hermes -z` sessions pick them up
with no further step. Set `HERMES_HOME` to target a non-default Hermes home.

## Editing

Edit the `SKILL.md` here (frontmatter must start at byte 0 with `---`, include
`name` + `description` ≤1024 chars), then re-run `deploy.sh`. Keep them in the
8–14k-char peer range; push bulky material into a skill's `scripts/` or
`references/`. See `~/.hermes/hermes-agent/skills/software-development/
hermes-agent-skill-authoring/SKILL.md` for the full authoring conventions.
