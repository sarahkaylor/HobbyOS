# WK7 Error Page — in-browser error document for failed navigations

**Patch:** `HobbyOS/patches/WK7-errorpage.patch` (unified diff, one file:
`HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp`).
**Validated:** `git -C ~/Documents/GitHub/webkit-hobbyos apply --check` -> OK (read-only; 4 hunks, 199 lines).
**Scope:** read-only work; the repo tree was never edited, nothing was built or run.

---

## 1. Confirmed failure evidence (serial receipts + screenshots)

Two in-OS runs produced the exact failure receipts; neither rendered anything to the window.

**Class A — TLS/CA verification failure (`wklane-s0-google/serial.log`, lines 1085-1086):**

```
[WIN] net fetch url=https://www.google.com ok=0 status=0 bytes=0
      effective=https://www.google.com err=no-CA-bundle(/CERTS/CA.PEM missing;
      verification will fail)SSL peer certificate or SSH remote key was not OK
[WIN] load open-fail url=https://www.google.com status=0 bytes=0
      err=no-CA-bundle(/CERTS/CA.PEM missing; verification will fail)SSL peer certificate or SSH remote key was not OK
```

`/tmp/s0-after-fail.png` (inspected visually): the previous `HOME.HTM` page is still painted
(`POK`, green block, two `[click]` links); the address field still reads `HOME.HTM`;
**zero error text** on screen.

**Class B — unresolvable host / DNS / curl 6 (`wklane-agentf-s6/serial.log`, lines 3166-3167):**

```
[WIN] net fetch url=https://nope-hobbyos.invalid/ ok=0 status=0 bytes=0
      effective=https://nope-hobbyos.invalid/ err=Could not resolve hostname
[WIN] load open-fail url=https://nope-hobbyos.invalid/ status=0 bytes=0
      err=Could not resolve hostname
```

`wklane-agentf-s6/shots/06-bad-domain.png` (inspected visually): the previous
DuckDuckGo results page is still displayed; address field still shows the *old*
`https://html.duckduckgo.com/html/`; **zero error text** on screen.

**Root cause** (confirmed against the driver source, `WK5WindowDriver.cpp`):

`WK5State::loadNetworkUrl()` line ~2580:

```cpp
if (!okNet || sink.status < 200 || sink.status >= 300 || body.isEmpty()) {
    wk5emit("[WIN] load open-fail url=%s status=%d bytes=%zu err=%s", ...);
    return false;
}
```

On failure it only emits the receipt and returns `false`. The caller
`loadForHistory()` / `navigateByName()` then does **nothing** (`if (!ok) return;`):
no document swap, no `m_current` change, no omnibox update, no `m_phase` change.
The previous page stays painted forever; the address bar never shows the attempted
URL (it shows the OLD `m_current`); the user gets no message at all.

---

## 2. What the patch renders for each failure class

The failure branch of `loadNetworkUrl()` now classifies the error, builds a small
self-contained HTML document whose static `<title>` is **`Can't reach this page`**
(so the load-ok doc-title census is assertable), and renders it through the
**same substitute-document path a successful load already uses**:
`file:///WNPAGE.HTM` facade URL + `<base href>` injection + `SubstituteData` +
`FrameLoader::load`. `m_current` is set to the marker `about:error`; `m_errorUrl`
holds the attempted URL and the omnibox (`buildChromeState`) displays it.

| Class | Detection | Rendered reason line |
|---|---|---|
| TLS/CA | `!okNet` and `sink.error` contains SSL/certificate/CA/verify (covers `no-CA-bundle(...)SSL peer certificate...`) | `Secure connection failed: TLS / CA certificate verification failed.` |
| DNS / curl 6 | `!okNet` and `sink.error` contains `resolve` (covers `Could not resolve hostname`) | `The server address could not be found (DNS could not resolve the hostname).` |
| HTTP 4xx/5xx | `okNet` but `status < 200 || status >= 300` | `The server returned an HTTP error status.` + `HTTP status: <N>` |
| other connect | `!okNet`, neither above | `Connection failed.` + curl detail |

The error doc body shows the **escaped attempted URL** (`<p><b>URL:</b> ...`), the
class reason, and either the HTTP status or the raw curl error (`<b>Detail:</b>`).
All interpolated strings are HTML-escaped (`&`, `<`, `>`, `"`, `'`).

New trace (reads only provably-alive args: `url` parameter, `sink.status`,
`body.size()`, string-literal class, `sink.error` array):

```
[WIN] errpage url=<url> status=<code> bytes=<n> class=tls|dns|http|conn err=<curl text>
```

The original `[WIN] net fetch ...` and `[WIN] load open-fail ...` receipts are
**kept byte-for-byte**, so the harness still sees the failure signal unchanged.

---

## 3. Verification plan (on-OS, no host build needed)

Trigger one failing URL per class and assert on the serial receipts + screenshots:

1. **TLS/CA:** `https://www.google.com` (CA.PEM not staged).
   **DNS:** `https://nope-hobbyos.invalid/`.
   **HTTP:** a URL that returns a `404`/`500`, e.g. `https://httpbin.org/status/404`
   (or any local static server returning 4xx/5xx).
2. Assert, per class, in order:
   ```
   [WIN] net fetch url=<u> ok=0|1 status=... (unchanged)
   [WIN] load open-fail url=<u> status=... err=...      (unchanged — harness contract)
   [WIN] errpage url=<u> status=... class=tls|dns|http
   [WIN] load-ok url=about:error ...                     (error doc reached Complete)
   [WIN] doc title=Can't reach this page|<bodylen>|...   (title census)
   [WIN] frame ... nonZero>0                              (error page painted, not blank)
   ```
3. Screenshot now shows the **error page** (heading + attempted URL + reason) and a
   **non-empty omnibox showing the attempted URL** (`st.current` from `m_errorUrl`).
4. Because the substitutes render through `WNPAGE.HTM`, `file:///WNPAGE.HTM` must
   already exist on the guest disk (the runner stages it for successful loads — same
   dependency; unchanged).

---

## 4. Edge cases handled (and why they cannot regress the success path)

- **Success path unchanged.** The `else` branch keeps the exact
  `[WIN] load start url=... (%zu B, effective=%s)` receipt and renders the real
  body exactly as before; `m_errorUrl` stays empty so the omnibox shows
  `m_current` as always. The `/PAGE-NET.HTM` persist block is skipped only when
  `isError` (a synthetic error body is not "fetched evidence").
- **Legit empty 2xx (e.g. 204 No Content).** The `else if (body.isEmpty())` branch
  emits the original `load open-fail` receipt and returns `false` **without**
  rendering an error page — the current page is kept, no navigation happens. This
  is exactly the previous behavior for that input; it just must not become an
  error page (a 204 is not a failure).
- **Harness receipts preserved.** `net fetch` + `load open-fail` are emitted
  unchanged on every failure; new `errpage`/`load-ok url=about:error`/`doc title`
  lines are additive only.
- **Reload on an error page** now re-attempts `m_errorUrl` (the real URL) instead
  of the `about:error` marker (`reload()`), instead of dispatching the marker as a
  fixture name.
- **Marked chrome state is cleared** on any successful load (`loadForHistory`,
  `navigateByName`), on tab switch/close, so a later good page doesn't show a stale
  error URL in the omnibox.
- **Error pages are not pushed into history** (`loadNetworkUrl` returns `false`
  for them), so Back/Forward never return to a dead page; `back()`/`forward()`
  semantics are unchanged.
- **Display-mutex pattern untouched** — no new `m_dispMu` use; `setTitlePrompt()` /
  `buildChromeState()` reuse the existing engine-thread paths.
- **No navigation-away on failure of a failure**: if a substitute render itself has
  no frame, the existing `return false` guards remain.
