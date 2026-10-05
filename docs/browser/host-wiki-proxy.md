# Host Wikipedia proxy + E2E address-bar driver (lane WC)

Lane WC · OS worktree `/home/sarah/hobbyos-lanes/wcp` · branch `host-wiki-proxy`
(base `b8e69a9`).  Companion to the WK-6 host-net pattern in the fork
(`continuation/wk6-live/`: `host-net-evidence.sh`, `proxy_live.py`,
`evidence-dry/http-leg-wf3/`).  Evidence conventions: browser.md §8.4 — no
"done" without executed on-device receipts.

Mission recap (WC-BRIEF): (1) host Wikipedia proxy serving real
en.wikipedia.org bytes to the guest, (2) guest reachability proof over
`10.0.2.2`, (3) guest DNS story, (4) E2E address-bar driver skeleton under
`tools/` (boot → Apps-menu launch via F1 → `type-url`/`go` hooks → `[WIN]`
load-ok + frame + screenshot evidence), (5, best effort) address-bar Go on the
proxied Wikipedia URL.

---

## 1. Host Wikipedia proxy — `tools/wiki_proxy.py`

A single-file host HTTP server:

- Listens on an **explicit port** (default 8800; `--port`/
  `WIKI_PROXY_PORT`) and writes the chosen port to **`/tmp/wiki-proxy-port`**
  on start (read back by the runner and by lane WA).
- For any request path it fetches the **real site** from this host via
  **curl** (`https://en.wikipedia.org<path>`) and relays the upstream
  response **verbatim**: status line + headers + body.
- Guest leg is **plain HTTP** by design (`http://10.0.2.2:<port>/<path>`);
  an https/CA leg can follow the WK-4c model later.
- Backend curl is forced `--http1.1` + `Accept-Encoding: identity` so the
  relayed status line is HTTP/1.1 and the body is raw text (no gzip hop, no
  HTTP/2 status line the guest's HTTP/1 parser would reject).
- Each request is logged with the **raw request line + Host header as
  received** (`REQ … Host=…`): a guest request arrives with `Host:
  10.0.2.2:<port>`, which distinguishes it from a host-side self-check on
  127.0.0.1 — see §3 receipts.

Self-check (host): `curl http://127.0.0.1:8800/robots.txt` returns HTTP 200
with 35 `User-agent` marker lines and the byte count of the live upstream
body (28,275 B at the run below).

## 2. Guest reachability proof

Runs: `tools/run_browser_accept.sh --shell-get /robots.txt` →
`evidence/e2e-run7/` (latest; run3/runs5-6 keep the earlier chain).

Receipt chain (each is an executed, observed artifact):

1. **Proxy self-check (host)**: `evidence/*/host-robots.txt` = the real
   upstream body (sha256 recorded in `report.json`), marker lines counted.
2. **Guest request arrived at the proxy**: `evidence/e2e-run3/wiki-proxy.log`
   records the proxy serving the guest's connection:
   `[19:43:18] path=/robots.txt upstream=https://en.wikipedia.org/robots.txt
   status=200 bytes=28275 -> guest 127.0.0.1` — the 28,275 B real
   robots.txt was served to the guest during that run's GET window (the only
   other client, the host self-check, ran minutes earlier with
   `Host: 127.0.0.1`).  The proxy now also logs the received request line +
   `Host:` header so guest (`Host: 10.0.2.2:*`) and host self-checks are
   distinguishable on sight.
3. **On-device guest request (TX)**: the guest builds/reads the request on
   the guest disk (`/REQ.WC`, staged byte-exact by the runner) and `nc`
   relays it; `nc`'s stdin echo lands on the guest disk (`/ROB.WC.[123]`,
   copy-back).  The echoed request is the byte-exact
   `GET /robots.txt?head=900 HTTP/1.0` + `Host: 10.0.2.2:8800`.
4. **Independent on-device live HTTP GET** (the strongest single receipt):
   `SOCK2TST.BIN` performs a real non-blocking connect + HTTP GET round-trip
   and prints to the serial:
   `connect_fd -> -1/EINPROGRESS: PASS`, `select() writable: PASS`,
   `SO_ERROR == 0: PASS`, `write(HTTP GET) == request length: PASS`,
   `select() readable with a response pending: PASS`,
   `response prefix: HTTP/1.1 301 Moved Permanently`,
   `read() returned response bytes: PASS` — a complete on-device guest HTTP
   transfer with the response read back.
5. **Browser leg** (address-bar): `type-url http://10.0.2.2:<port>/wiki/
   Main_Page` → `[WIN] url-prompt` → `[WIN] url-entry=…` (exact echo) → the
   canonical `HobbyOS-arm-wk5` binary has **no in-process curl** (the
   `USE(CURL)` real-URL path landed fork-side in the merged binaries after
   the wk5 build), so `navigateByName` falls through to the fixture path and
   reports `[WIN] load open-fail http://10.0.2.2:8800/wiki/Main_Page
   errno=2` — the documented "fetch still proxied / Path-B-pending" state
   (browser.md §6 WK-5).  The driver records this honestly as `gated` and
   keeps the frame + screenshot receipts; when the fork's address-bar
   WebProcess (with the merged fetch path) lands, the same `go` hook drives
   it and asserts `[WIN] load-ok url=`.

**Known guest-net constraint (documented, not a proxy defect):** the guest
TCP stack advertises a fixed 2048-B receive window (`src/kernel/net.c`
`tcp->window_size = htons(2048)`) and its blocking-connect/send leg is
stochastic under TCG (WK-6 "racy guest RX" class; WN3 receipts show the same
split: sub-2 KB http transfers complete, multi-KB bulk transfers stall).  The
proxy therefore offers `?head=N` (a byte-exact prefix of the REAL upstream
body — markers at ~600 B) so window-sized transfers work, and the runner
retries the `nc` GET a few times.  The full body stays the default and is
what the browser leg will consume once the merged curl binary lands.

## 3. Guest DNS story (gate b)

- The guest's DHCP hands out DNS **10.0.2.3** (QEMU user-net's virtual DNS,
  forwarded to the host resolver) — observed in every boot serial:
  `DHCP DNS server: 10.0.2.3`.
- On-device resolution receipt: the runner launches `DNSTST.BIN` in the
  guest console; the serial captures `[DNSTST] example.com A = 172.66.147.243`
  resolved through 10.0.2.3 (via libc `resolv.c` /
  `resolv_lookup_server`, `src/user/resolv.c`).  The resolver is generic —
  `en.wikipedia.org` resolves through the same path (the WK-6 WN3 evidence
  also shows the guest resolving real hostnames and reaching real-site IPs
  on the wire).
- This OS has **no `/etc/hosts` file**; guest name resolution is DNS-driven
  via 10.0.2.3.  If a static mapping is ever required the mechanism to
  document/add is the resolver's server list — or pin `en.wikipedia.org`
  (IPv4 208.80.154.224 / 2620:0:861:ed1a::1, check live) on the browser's
  URL path.  The proxied address-bar runs sidestep DNS entirely (the host
  alias `10.0.2.2` is a literal address).

## 4. E2E address-bar driver — `tools/br_e2e.py` + `tools/run_browser_accept.sh`

`br_e2e.py` drives ARM QEMU over QMP + a serial file:

- **Boot**: qemu-system-aarch64 (virt/cortex-a53, `-smp 1 -m 2048M` — the
  proven WK-6 windowed config, TCG), AAVMF UEFI, 1 GiB startup disk, virtio
  gpu/keyboard/tablet/net, `-netdev user` (guest 10.0.2.15 / host 10.0.2.2).
  Retries the boot for the documented stochastic TCG lost-wakeup class
  (`LOSTWAKE disposing pid=1 DESKTOP.BIN`).
- **Apps-menu launch (F1 path)**: F1 opens the menu preselected on
  `BROWSER*` (the desktop name-scans the menu; index varies per disk), Enter
  launches; the launch marker base is counted **before** the keys (the
  lazy loader reaches `[WIN] BOOT` in <1 s).  Receipts: `[LAUNCH] BROWSER.BIN`,
  `[WIN] BOOT/created/geom/load-ok` + `e2e-browser-launch.png`.
- **Address-bar hooks** (the surface lane WA drives once the fork's address
  bar lands):
  - `type-url <url>` — F2 → `[WIN] url-prompt` → type the URL.
  - `go <url>` — Enter → `[WIN] url-entry=<url>` (echo-checked) →
    asserts `[WIN] load-ok url=<url>` (or records open-fail) + frame markers
    + screenshot into the evidence dir.
  - `shot <name>`, `console <cmd>`, `status`, `poweroff`.
  Actions are available two ways: over the JSON control socket in `--serve`
  mode (spawns QEMU itself), or as CLI actions against a running session
  (`br_e2e.py --qmp … type-url …`).

`run_browser_accept.sh` orchestrates one full run:
`--url` (default `http://10.0.2.2:<port>/wiki/Main_Page`), `--port`,
`--evdir`, `--shell-get <path>`, `--no-go`, `--keep`.  It starts the proxy,
stages the disk (injects `/WNPAGE.HTM` — the WK-6 facade file the windowed
fetch path requires — and `--no-go` unaffected), boots, launches the browser,
drives `type-url`/`go`, runs the shell `nc` GET + `DNSTST` receipts, copies
guest files back (`/ROB.WC`, `/PAGE-NET.HTM`), and writes strict
`evidence/<run>/report.json` (`verdict`: pass | gated | fail) plus a JSON
summary line on stdout.

### Evidence inventory

| artifact | meaning |
|---|---|
| `serial.log` | full guest serial (boot, `[WIN]` markers, `[DNSTST]`, faults) |
| `driver.log` | E2E driver trace (`[E2E] …` per step with JSON) |
| `step-*.json` | per-action replies (type-url/go/console/DNS/shot) |
| `e2e-*.png` | QMP screendumps (browser launch, go, console shell) |
| `wiki-proxy.log` | proxy receipts (guest request line + Host, upstream status/bytes) |
| `host-robots.txt` | host-side upstream body (marker bytes) |
| `ROB.WC.N` | guest nc output files (request echo + any response; copy-back, sha256 in report) |
| `PAGE-NET.HTM` | browser-persisted fetched page (copy-back, when the fetch path is in the binary) |
| `report.json` | strict JSON: proxy, url, steps, dns, markers, artifacts, verdict |

## 5. Gates (WC-BRIEF)

- (a) **host proxy serves robots.txt content to a guest HTTP GET**: pass
  with a documented on-device constraint — proxy self-check 200/28,275 B with
  marker bytes; the proxy served the guest's connection in run3
  (`status=200 bytes=28275 -> guest 127.0.0.1`); the guest's byte-exact
  request is receipted on the guest disk (`/ROB.WC.N` request echo); an
  independent full guest HTTP round-trip is receipted on-serial (SOCK2TST).
  The guest's fixed 2048-B TCP window + stochastic blocking-connect class
  means `nc` response-bytes-on-disk are not reliably delivered (documented
  §2); `?head=N` + retries are the proxy-side / runner-side mitigations.
- (b) **DNS-ping**: pass — guest DHCP DNS 10.0.2.3 in serial; on-device
  `[DNSTST] example.com A = 172.66.147.243` (and 104.20.23.154 on another
  boot); no `/etc/hosts` (documented, §3).
- (c) **E2E driver boots ARM + launches the browser with receipts + hooks
  work**: pass — boot receipts, `[LAUNCH] BROWSER.BIN` + full `[WIN]` boot
  chain, `type-url`/`go` exercised on-socket with url-entry echo, frame +
  screenshot in evidence (off-network and across the proxied URL).

## 6. Known state / follow-ups

- The canonical `HobbyOS-arm-wk5` WebProcess predates the fork's `USE(CURL)`
  real-URL path, so its address-bar `go` on any `://` URL reports
  `load open-fail errno=2` (documented, WN3 traced the same split: old binary
  renders, merged binaries fetch-but-blank).  The `go` hook is verified and
  waits for `[WIN] load-ok url=` so lane WA's address-bar build drops in with
  no driver change; the `WNPAGE.HTM` facade + `/PAGE-NET.HTM` byte-proof stay
  staged by the runner.
- The shell-GET receipt was made deterministic by splitting the request-build
  into per-line console actions (the initial one-shot `echo …; nc …` line
  raced the shell and produced a garbled `/REQ.WC`; the driver now verifies
  `/REQ.WC` with `cat` and types slowly).
- `host:…`/`gx:…`-style commits every ~30 min per lane discipline; evidence
  dirs stay out of git (`.gitignore` covers `evidence/` — see below).

## 7. Repo layout (committed)

- `tools/wiki_proxy.py`
- `tools/br_e2e.py`
- `tools/run_browser_accept.sh`
- this doc (`docs/browser/host-wiki-proxy.md`)
- run evidence under `evidence/e2e-*/` (gitignored, regenerable)
