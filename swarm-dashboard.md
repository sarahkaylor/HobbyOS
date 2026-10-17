# Swarm Dashboard — design brief (approved 2026-10-17)

A live dashboard for the HobbyOS game-swarm, styled after the **NVIDIA DGX Dashboard**
(dark, single-page, per-unit live panels, ~2 s refresh). Runs on its **own server VM** in the
swarm fleet. Plan-agnostic: progress is computed **only from the issue tracker** so it works
for any future plan the swarm runs.

## Status
**DEPLOYED (2026-10-17).** Live at **http://192.168.0.235/** (nginx front: `http://192.168.0.235/`,
Grafana `http://192.168.0.235/grafana/`, Prometheus `http://192.168.0.235/prometheus/`).
Stack: 4 containers (web/nginx, backend/FastAPI :9810, prometheus v3.5, grafana oss 12.1) on VM 145.
Exporters: `swarm-exporter` systemd sidecar on all 5 VMs (+ `node_exporter` :9100, scraped).
Verified: SPA serves; live view shows coordinator pinned + worker sessions; plan 11.1% (5 milestones,
9 issues seeded as demo); Prometheus targets all `up` (swarmdash + 5× swarm_nodes).
Notable: fleet Hermes has **no AI provider configured** yet (GB-004) — live transcript panels show
the labeled smoke-test fixture until real agent sessions run. Code lives at `/opt/swarmdash` on VM 145
and `~/.hermes/cache/scratch/swarmdash/` on the Mac (build tree).

## Locked decisions
- Style: NVIDIA DGX Dashboard dark single-page, live-updating panels.
- Live fidelity: **full transcript tail (~30 lines) per agent, collapsible**.
- Access: **LAN-only, no login** (bind LAN; revisit auth later if needed).
- Host: **VM 145 `swarm-dashboard`**, 4 vCPU / 8 GiB / 32 G, c2flash, Ubuntu 26.04 server,
  **no GPU**, IP 192.168.0.235.
- Progress source: **issue tracker only** (Forgejo). Discover milestones dynamically;
  treat milestone titles as opaque "phases" (e.g. seed plan `gaming` = M0..M4). Not derived
  from gaming.md.

## The 3 required views
1. **Live Hermes conversations** — one live panel per running agent session; **Coordinator
   conversation pinned at the top**, workers/reviewer/tester below. Full transcript tail per
   agent (~30 lines) + summary line (session id, role, last tool call, turns/tokens, heartbeat).
2. **Tracker issues** — list from Forgejo API, grouped by milestone / label / state, with
   open/blocked counts.
3. **Overall plan progress** — driven by the tracker: for each milestone, open vs done issues
   → weighted overall %. Show per-milestone progress bars + gate badge + last re-plan signal
   (newest PLAN/decision ticket activity).

## Architecture
| Piece | What | Where |
|---|---|---|
| Dashboard app | FastAPI + uvicorn backend; static dark SPA frontend; SSE push (~2 s) for live panes; REST + server-cached proxy for Forgejo | VM 145 |
| Exporter sidecar `swarm-exporter` | reads `$HERMES_HOME/sessions` + process list per VM; POSTs session JSON every ~2 s (heartbeat + tail + last tool call) | systemd unit on all 5 VMs |
| Data | in-memory live state + SQLite (24 h history) | VM 145 |

- Forgejo token: use `swarm-bot` token only (never the password), in an env file chmod 600 on
  VM 145. Copy from `~/.hermes/cache/scratch/swarm-vm-bootstrap-pw.txt`.
- Endpoints: `GET /` SPA; `GET /api/status` aggregate; `GET /api/stream` SSE (live panes);
  `GET /api/issues` cached Forgejo proxy; `GET /api/plan` progress; `POST /api/ingest` (exporters,
  token-restricted).
- Live-view honesty: panels update as agents stream to disk (~1–3 s latency); idle agents show a
  heartbeat pulse, never fake activity.

## Build sequence
1. Create + provision VM 145 (clone of existing VM 144 recipe; add python3-venv, pip).
2. `swarm-exporter` sidecar on the 5 VMs (systemd, ~2 s).
3. Backend + ingest + Forgejo aggregation.
4. Frontend SPA (dgxmon-style).
5. End-to-end test: run one real coordinator session to validate session-file parsing
   (`~/.hermes/sessions/*.jsonl` + `state.db` — confirm exact schema on a live run).
6. Hand over URL `http://192.168.0.235:8080`.

## Notes / validation gaps
- No Hermes sessions exist yet on the fleet (fresh installs) — first live run validates the
  transcript format; exporter should be tolerant of an empty sessions store.
- Forgejo board (labels/milestones/issues) is currently empty — will populate via GB-002/003.
- Per-Sarah policy: **no VM created in this session gets a physical GPU.**
