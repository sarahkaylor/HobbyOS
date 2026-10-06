# FS-lane V1 — acceptance harness (run_browser_accept.sh)

One canonical acceptance runner for all browser lanes, host-side, on the ARM
windowed desktop.  Modes + per-request network log + `report.json` v2.

## Files (owned by FS lane V1)

| File | Purpose |
|------|---------|
| `tools/run_browser_accept.sh` | The runner: boots the ARM disk, launches the windowed browser (F1/Apps), drives the address bar per mode, collects evidence, writes `report.json` v2. |
| `tools/br_e2e.py` | QMP/serial session driver: boot, Apps-menu launch, `type-url`/`go`, console receipts, screenshots (unchanged API; the runner drives it over the ctrl socket). |
| `tools/wiki_proxy.py` | Host proxy: raw relay (`/wiki`/any path), `/reader/<Topic>`, `/fixture/<name>` (T0 fixtures), JSONL net log, port-file contract. |
| `tools/test_wiki_proxy.py` | Host-side guard test (no QEMU) for the F-R2 proxy contract, incl. `--port-file`. Run: `python3 tools/test_wiki_proxy.py`. |

## Modes (`--mode`)

| Mode | Row set | Notes |
|------|---------|-------|
| `fixture` | `/fixture/FIX01..FIX05(FIX02D)` + `/fixture/HOME,TALL` | T0 fixtures from fork `continuation/wk3/fixtures` + OS `tests/fixtures/browser`, served byte-identical via the proxy `/fixture` route. |
| `reader` | `/reader/<Topic>` (default `Habitat`) | Real Wikipedia REST extract, minimal HTML. |
| `direct` | `--url https://…` | Direct (non-proxied) leg; classified per on-device evidence. |
| `full` | `--url` or `/wiki/Web_browser` | Raw relay of a full article page. |
| *(default)* | `/wiki/Main_Page` over the proxy | Legacy behavior preserved. |

## Concurrency (`--instance <id>` or `ACCEPT_ID=<id>`)

The runner is **host-global single-instance by default**: fixed `/tmp` sockets
(`/tmp/br-wc-ctrl.sock`, `/tmp/br-wc-qmp.sock`, `/tmp/br-wc-serial.log`) and the
F-R2 contract proxy port-file `/tmp/wiki-proxy-port` (proxy default port 8800).
Wrap default runs in `flock -w 7200 /tmp/fs-accept.lock …`.

With `--instance <id>` the runner scopes every fixed path:

- ctrl/qmp sockets → `/tmp/br-wc-ctrl-<id>.sock`, `/tmp/br-wc-qmp-<id>.sock`
- serial → `/tmp/br-wc-serial-<id>.log`
- proxy port-file → `/tmp/wiki-proxy-port-<id>` (passed as
  `wiki_proxy.py --port-file …`)
- **`--port` must be a distinct listener port** (the runner refuses to run an
  instance on the default 8800; e.g. `--port 8811 --instance a`).

Default behavior is byte-compatible: **no `--instance` → exactly the legacy
paths and `/tmp/wiki-proxy-port`**, so existing consumers (and the F-R2
contract) are untouched.  Each concurrent instance still needs its own flock
file (e.g. `flock -w 7200 /tmp/fs-accept-<id>.lock`).

`wiki_proxy.py` independently supports `--port-file <path>` (and
`$WIKI_PROXY_PORT_FILE`) for any host-side consumer; one-instance binding
(fail loudly on a busy port) is unchanged.  The JSONL netlog may be pointed
anywhere with `--netlog` (default `<log>.jsonl`).

### Two concurrent instances (example)

```sh
flock -w 7200 /tmp/fs-accept-a.lock bash tools/run_browser_accept.sh \
  --mode fixture --instance a --port 8811 --evdir evidence/fs-v1/demo-a &
flock -w 7200 /tmp/fs-accept-b.lock bash tools/run_browser_accept.sh \
  --mode fixture --instance b --port 8812 --evdir evidence/fs-v1/demo-b &
wait
```

## Per-request network log

`wiki_proxy.py` writes structured JSONL (one object per request:
`ts, ts_ms, method, path, route, status, bytes, content_type,
content_encoding, upstream_status, client, guest_host`).  The runner points it
at `<evdir>/net.log.jsonl` and `report.json` v2 references it (relative path,
line count, sha256, route census).

## `report.json` v2 (strict, versioned)

`schema_version: 2`, `toolkit`, `report_schema`, `mode`, `instance`, `proxy`
(with the actual `port_file`), `netlog`, `rows[]` (per-row
`status: pass|gated|fail` + `load_ok, load_ms, open_fail, frames, checksum,
screenshot, net, persist, entry_echo`), `guest_http`, `dns`, `session`
(verdict + row_counts + markers + screenshots + artifact hashes).

Per-row classification:
- **pass** — browser's own `[WIN] load-ok url=<url>` observed + a frame
- **gated** — network layers reached (fetch/entry) but no render, or an
  explicit `open-fail`; evidence recorded, not a hang
- **fail** — nothing observed for the row, or a `FATAL` in the serial

## Gate (fixture leg green)

- Rebuild the disk with the current windowed ELF before any run (main's disk
  carries a stale BROWSER.BIN):
  `make ARCH=arm MODE=desktop disk.img BROWSER_BIN=/home/sarah/webkit-lanes/ib/WebKitBuild/HobbyOS-arm/bin/WebProcess`
- `flock -w 7200 /tmp/fs-accept.lock bash tools/run_browser_accept.sh --mode fixture --evdir evidence/fs-v1/demo-fixture`
- Fixture rows FIX02D (data:image) / FIX05 (inline script) fetch + paint a
  frame but the browser's own `[WIN] FAIL load-timeout` watchdog fires before
  `load-ok` — documented F6/F7 class (owner WN2); they classify **gated**,
  never fail.
