# WK-7 (HobbyOS formpost): POST form submits lose their field values

**Driver (read-only evidence):** `~/Documents/GitHub/webkit-hobbyos/HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp`
**Fix location:** `~/Documents/GitHub/webkit-hobbyos/Source/WebKit/WebProcess/WebCoreSupport/WebFrameLoaderClient.cpp` (WebKit process layer, the WK-8 navigation-claim hook)
**Deliverables:** `HobbyOS/patches/WK7-formpost.patch` (+ this `.md`). `git apply --check` passes; nothing was edited, built, or run.

---

## 1. Symptom

On `https://html.duckduckgo.com/html/`, clicking the `q` search box, typing `hobby os`,
and pressing Enter navigates back to a **bare, empty search page** (same 3108-byte HTML),
not to a results page. The query never reaches the server.

## 2. Reproduction — verbatim serial chain (`wklane-re6-s6/serial.log`, lines 1902–2138)

```
[WIN] focus=INPUT|q|search_form_input_homepage|text                      ; click focused the real <input name=q>
[WIN] page-key ch=0x68 key=h  ...  [WIN] page-value=hobby                  ; 8 real insert receipts (h,o,b,b,y,space,o,s)
[WIN] page-value=hobby os                                                 ; value confirm before Enter
[WIN] page-key ch=0x0a key=Enter
[EDIT] entry phase=keydown vk=13 name=[] text=[] canEdit=1
[EDIT] kd-miss vk=13 name=[]
[EDIT] entry phase=keypress vk=0 name=[InsertNewline] text=[] canEdit=1  ; single-line <input> Enter left unhandled -> implicit submit
[NS] schedule post-cancel fid=7 loc=1 complete=1                          ; ScheduledFormSubmission queued on NavigationScheduler
[NS] schedule post-completed fid=7
[NS] startTimer fire-branch main=1 fid=7 delay=0 loc=1
[NS] timerFired enter main=1 fid=7
[NS] fire loc=1
[POL] decide url=https://html.duckduckgo.com/html/ navdata=ok             ; <-- form submission reaches policy: ACTION URL ONLY
[POL] self-driven auto-use url=https://html.duckduckgo.com/html/
[WIN] engine-nav url=https://html.duckduckgo.com/html/ claimed=1          ; <-- driver claims the bare action URL
...
[WIN] page-value=hobby os
[WIN] engine-nav dispatch url=https://html.duckduckgo.com/html/
[WIN] nav name=https://html.duckduckgo.com/html/
[WIN] curl-perform begin url=https://html.duckduckgo.com/html/
[WIN] net fetch url=https://html.duckduckgo.com/html/ ok=1 status=200 bytes=3108 effective=https://html.duckduckgo.com/html/ err=
[WIN] load start url=https://html.duckduckgo.com/html/ (3108 B, effective=https://html.duckduckgo.com/html/)   ; empty search page again
```

Note the URL at every hop is the bare action `https://html.duckduckgo.com/html/` — there is
**never a `?q=...`**. The 3108 B body is exactly the empty search homepage (the same page the
user started on), proving the server received no query.

## 3. Root cause — what drops the data and where

The form is `<form name="x" id="search_form_homepage" action="/html/" method="post">` — `method=post`.
The correct WebCore path is followed **all the way to policy**:

1. `HTMLInputElement::defaultEventHandler` → implicit submit → `HTMLFormElement::submit` → `FormSubmission::create` — the field values are serialized into a `FormData` (`FormSubmission.cpp`).
2. `FormSubmission::populateFrameLoadRequest` (`FormSubmission.cpp:272`) sets, for `Method::Post`: `requestURL` = **action URL only** (`requestURL()` returns `m_action` — the fields go into the **body**, not the URL, line 261–270), plus `setHTTPMethod("POST")`, `setHTTPBody(m_formData)`, `setHTTPContentType("application/x-www-form-urlencoded")`.
3. `NavigationScheduler::ScheduledFormSubmission::fire` → `FrameLoader::loadFrameRequest` → `FrameLoader::loadPostRequest` (`FrameLoader.cpp:4004`) rebuilds `workingResourceRequest` **preserving** method, body, content type, and URL, then → `loadWithNavigationAction` → `PolicyChecker::checkNavigationPolicy`.
4. `WebFrameLoaderClient::dispatchDecidePolicyForNavigationAction` (`WebFrameLoaderClient.cpp:199`) is handed that request. **At this point the request still carries the data:** `request.httpMethod()=="POST"`, `request.httpBody()` = `q=hobby+os`, `request.httpContentType()` = `application/x-www-form-urlencoded`.
5. The WK-8 claim hook (`WebFrameLoaderClient.cpp:232-236`) forwards **only the URL string** to the driver:

```cpp
if (&wk5EngineNavRequested && m_frame->isMainFrame()
    && wk5EngineNavRequested(request.url().string().utf8().data(), 1)) {
```

The POST body and method are silently discarded here. The driver claims the URL (`[WIN] engine-nav ... claimed=1`), resolves policy `Ignore`, and on the next engine tick navigates `navigateByName → loadForHistory → loadNetworkUrl → fetchUrl → wk5NetFetch` — and `WK5WindowDriver.cpp:666` calls `handle.enableHttpGetRequest()`, i.e. a **plain GET with no request body**. So `GET /html/` (no query, no body) hits the server, which answers with the empty homepage.

**Net:** the form data is dropped at the boundary between the WebCore/WebKit policy layer and the
shell's GET-only fetch path. Data *exists* at step 4, is *dropped* at step 5, and *cannot be
resent* mechanically at step 6 (`enableHttpGetRequest`, no body).

## 4. The fix (chosen: option a — GET-equivalent URL)

Patch `WebFrameLoaderClient.cpp`, the WK-8 claim block, to fold a urlencoded POST body into the
action URL's query **before** handing the URL to the driver — exactly the URL WebCore would have
produced for `method=get` (`FormSubmission::requestURL()` does `requestURL.setQuery(m_formData->flattenToString())` for GET):

```cpp
if (&wk5EngineNavRequested && m_frame->isMainFrame()) {
    URL claimedURL(request.url());
    if (equalLettersIgnoringASCIICase(request.httpMethod(), "POST"_s)
        && request.httpContentType().startsWith("application/x-www-form-urlencoded"_s)) {
        if (RefPtr formData = request.httpBody()) {
            String body = formData->flattenToString();
            if (!body.isEmpty())
                claimedURL.setQuery(body);
        }
    }
    { /* [POL] wt-nav claim url=...  trace (new read-only receipt) */ }
    if (wk5EngineNavRequested(claimedURL.string().utf8().data(), 1)) {
        m_frame->didReceivePolicyDecision(listenerID, PolicyDecision { std::nullopt, PolicyAction::Ignore, { }, std::nullopt });
        return;
    }
}
```

The body is already percent-encoded form-urlencoded data (`q=hobby+os`), so it drops straight
into the query; `WK5WindowDriver::wk5NetFetch` then does `GET /html/?q=hobby+os`, which the
server decodes. The `form-urlencoded` content-type guard means only foldable bodies are touched.

Deliberately **not** changed (minimal): the driver's `s_wk5PendingNav`/`nav[]` buffers (2048 B —
short search queries fit; noted below), the GET-only fetch itself, and no `multipart/form-data`
handling. A faithful POST (header + urlencoded body in `wk5NetFetch`) is documented follow-up.

### Why this cannot break GET forms or same-document submits

- **GET / other navigations:** the fold is guarded by `request.httpMethod() == "POST"` and a
  urlencoded content type. GET form submissions already have their query in `request.url()` and
  flow through unchanged (`claimedURL = request.url()`, same as today). Link clicks,
  address-bar loads, `<meta refresh>`, and JS `location` navigations are all GET/empty-method →
  untouched.
- **Same-document / fragment submits:** fragment navigations resolve in
  `FrameLoader::shouldPerformFragmentNavigation` before policy and never reach this hook;
  sub-frame submissions are excluded by the pre-existing `m_frame->isMainFrame()` guard; and a
  POST to the same URL is now *better* (fields preserved) rather than dropped.
- **multipart/form-data POST** (file uploads; content type `multipart/form-data; boundary=...`):
  fails the startsWith guard → no fold → behaves exactly as before (GET of the action, data
  dropped — pre-existing limitation, unchanged scope, documented follow-up).
- **Empty-body POST:** `flattenToString()` is empty → `claimedURL` stays the plain action → byte
  identical to current behavior.
- **Non-http(s) actions** (`javascript:`, `mailto:`): the driver's `wk5ClaimableNavUrl` already
  declines those, so the claim (and the fold) is irrelevant for them.

## 5. Verification (integrator asserts after: open html.duckduckgo.com/html/ → click `q` → type `hobby os` → Enter)

| # | Receipt | Expected |
|---|---------|----------|
| 1 | `[WIN] focus=INPUT|q|search_form_input_homepage|text` | present (unchanged) |
| 2 | `[WIN] page-value=hobby os` | present (unchanged, repeat typing) |
| 3 | `[POL] decide url=https://html.duckduckgo.com/html/ navdata=ok` | present (raw action, unchanged) |
| 4 | `[POL] wt-nav claim url=https://html.duckduckgo.com/html/?q=hobby+os` | **NEW — the fold** |
| 5 | `[WIN] engine-nav url=https://html.duckduckgo.com/html/?q=hobby+os claimed=1` | query now in the driver receipt |
| 6 | `[WIN] engine-nav dispatch url=https://html.duckduckgo.com/html/?q=hobby+os` | query survives engine-tick handoff |
| 7 | `[WIN] curl-perform begin url=https://html.duckduckgo.com/html/?q=hobby+os` | the actual GET fetches the query |
| 8 | `[WIN] net fetch url=https://html.duckduckgo.com/html/?q=hobby+os ok=1 status=200 bytes=<LARGE> ...` | a **results** page: bytes ≫ 3108 (expect ~10–20 KB) |
| 9 | `[WIN] load start url=... (N B, effective=...)`, then a `doc title=` / census with many `links` | DuckDuckGo results page with result links |

The hard pass/fail signal: **receipt #8's URL contains `q=hobby+os` and its byte count is
substantially above the 3108 B empty page**, and a follow-on doc census shows result links.
Good-bye line: the old failure `net fetch ... bytes=3108` fetches the *same* bare action URL
again; after the fix every url= line from `engine-nav` onward carries `?q=hobby+os`.

## 6. Follow-up (out of scope for this wave)

A faithful `POST` in `WK5WindowDriver::wk5NetFetch` — set the method + `Content-Type` +
urlencoded/`multipart` body on the `CurlHandle` (mirrors `handle.enableHttpGetRequest()` with a
POST variant) — is the complete fix for login forms and file uploads. The query-fold keeps
search/lookup forms (the reported bug class) working with a minimal, single-hunk change and
provides a correct GET fallback regardless. Note the driver's 2048 B pending-nav copy truncates
very large folded queries (pre-existing; not expanded here).
