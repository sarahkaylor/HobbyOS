# L23 — the GLX context-version clamp: Unity's 4.5 request served as 3.3 core

Status: **implemented and gated in the guest.** A client that asks
`glXCreateContextAttribsARB` for **4.5 core with Unity's exact attribute list**
now gets a live context instead of NULL; it reports
`GL_VERSION 3.3 (Core Profile) Mesa 26.2.6`, compiles GLSL 330, renders the
reference triangle (`fnv1a64=0x25476e955b66d136`) and presents it. A 3.3
request is still served unchanged, a 2.1 request is honoured, and the rewrite
is logged once: `[GLX] context request 4.5 core -> 3.3 core`.

## 1. The measured failure (before)

`src/user/gl_probe.c` now asks for the context the game asks for, taken
verbatim from the apitrace capture (`docs/rimworld-evidence.md` §6):

```
{GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 5,
 GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
 GLX_CONTEXT_RELEASE_BEHAVIOR_ARB, GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB,
 GLX_CONTEXT_OPENGL_NO_ERROR_ARB, False, 0}
```

and creates a second context sharing with the first, as Unity does. Against the
pre-clamp `libGL.so.1`, in-guest, on the real desktop:

```
[GL] ctx request unity-4.5-core     -> ctx=(nil) X-error=0x2 (BadValue)
[GL] ctx request plain-4.5-core     -> ctx=(nil) X-error=0x9 (GLXBadFBConfig)
[GL] ctx request core-3.3           -> ctx=0x1040116190 X-error=0x0
[GL] ctx request compat-2.1         -> ctx=0x10400a27c0 X-error=0x0
[GL] ctx served core-3.3        : 3.3 (Core Profile) Mesa 26.2.6
[GL] ctx served compat-2.1      : 3.3 (Compatibility Profile) Mesa 26.2.6
[GL] FAIL the 4.5 core request returned NULL -- the GLX frontend must clamp it to the best context it can serve (AD-5)
[GL] using the 3.3 context -> 0x1040116190
[GL] readback 256x256: fnv1a64=0x25476e955b66d136 glGetError=0x0
[GL] teardown: clean
[GL] done rc=1
```

Two independent refusals, both in the frontend, not in the driver:

| request | refusal | where |
|---|---|---|
| Unity's list, 4.5 core | `BadValue` (0x2) **before** any context is made | `glx_api.c`'s attribute parser: `default:` → `generate_error(BadValue)`; it has no case for `GLX_CONTEXT_RELEASE_BEHAVIOR_ARB` (0x2097) or `GLX_CONTEXT_OPENGL_NO_ERROR_ARB` (0x31B3) |
| classic attribs, 4.5 core | `GLXBadFBConfig` (0x9), NULL | `st_manager.c:1062` — `st->ctx->Version (33) < attribs->major*10+minor (45)` → `ST_CONTEXT_ERROR_BAD_VERSION` |

So the game would die at its first GL call after window creation: there is no
fallback attempt in Unity (measured), and `generate_error()` is silent unless
the client installed an X error handler. The 3.3 and 2.1 rows prove the rest of
the stack was healthy all along — the render below them is byte-identical to
the M4 gate's.

## 2. The fix: `tools/mesa/patches/0004-glx-clamp-context-version.patch`

The frontend is ours (we build `libGL.so.1` from Mesa's `glx=xlib` target), so
the clamp goes there, not in a wrapper DSO that a `dlopen`-by-soname client
would never resolve through:

* an **unknown context attribute is skipped**, with a one-line notice
  (`[GLX] ignoring unknown context attribute 0x...`), instead of failing the
  request. Release-behaviour FLUSH is what this frontend already does
  (`XMesaSwapBuffers` flushes), and `NO_ERROR=false` is the safe direction
  (error reporting stays on), so both are honoured in substance;
* a **version above the served version** (`HOBBYOS_GLX_SERVE_MAJOR/MINOR`,
  3.3 — the AD-5 target this Mesa build advertises) is clamped **after** the
  first attempt fails, and retried with the same profile, flags and render
  type. Trying first means a future 4.x driver keeps 4.x: the retry never fires
  when the request is servable;
* the retry is announced once per process:
  `[GLX] context request 4.5 core -> 3.3 core`;
* a request at or below 3.3 never reaches the retry, so lower versions pass
  through unchanged, and the returned context is a real Mesa context reporting
  the version it implements — no fake, no version spoofing.

## 3. Evidence (real boot, `tools/gate/m4_gl_probe.py`, exit 0)

```
[GLX] context request 4.5 core -> 3.3 core
[GL] ctx request unity-4.5-core     -> ctx=0x1040116190 X-error=0x0
[GL] ctx request plain-4.5-core     -> ctx=0x10400a27c0 X-error=0x0
[GL] ctx request core-3.3           -> ctx=0x105853e330 X-error=0x0
[GL] ctx request compat-2.1         -> ctx=0x1064c43030 X-error=0x0
[GL] ctx request unity-4.5-shared   -> ctx=0x107105c7d0 X-error=0x0
[GL] ctx served unity-4.5-core : 3.3 (Core Profile) Mesa 26.2.6
[GL] ctx served unity-4.5-shared: 3.3 (Core Profile) Mesa 26.2.6
[GL] ctx served core-3.3        : 3.3 (Core Profile) Mesa 26.2.6
[GL] ctx served compat-2.1      : 3.3 (Compatibility Profile) Mesa 26.2.6
[GL] using the 4.5-requested (clamped) context -> 0x1040116190
[GL] GL_VERSION  : 3.3 (Core Profile) Mesa 26.2.6
[GL] GL_RENDERER : softpipe
[GL] GLSL        : 3.30
[GL] readback 256x256: fnv1a64=0x25476e955b66d136 glGetError=0x0
[GL] non-background=22890/65536 (34%) colours=22891 bg=13,13,26,255
[GL] desktop rect 2,34 1916x1018 fnv1a64=0x039795a9519bfdca nonbg=683570/1950488
[GL] pixel diff: 96 of 1950488 differ (0.0049%)
[GL] present check: desktop holds the frame (only the re-stamped cursor differs)
[GL] teardown: clean
[GL] done rc=0
[EXITPC] pid=3 last_user_pc=0x1080B60146 code=0x0
```

Every row the lane asked for is here and is *the same code path* as before:
the 4.5-requested context is the one that renders (the
`using the 4.5-requested (clamped) context` row), the FBO checksum is the M4
gate's `0x25476e955b66d136`, the presented frame's desktop hash and 0.0049 %
cursor-only diff are the L16 numbers, and teardown is clean.

The 2.1 row is "honoured": Mesa returns a compatibility-profile context and
reports **3.3 (Compatibility Profile)** — a higher version than requested,
which `GLX_ARB_create_context` allows. A refusal would also have been
acceptable if it had been clean (a context-less row plus an X error), but this
one is served.

## 4. A build-cache trap this lane had to fix

The first after-run still got NULL even though the patch had been applied:
`build-hobbyos.sh`'s per-TU cache keyed on **object vs log mtime only**, so a
source rewritten by a patch was never recompiled — the DSO silently shipped
the pre-patch object. The cache now also requires the object to be newer than
its **source** (`os.path.getmtime(out) >= os.path.getmtime(srcpath)`).

Measured consequence before the fix: `strings libGL.so.1` had no clamp notice,
the guest reported `BadValue`, and the build log said
`applied patch: 0004-...` — a green build with the wrong bytes. Any future
Mesa patch has the same failure mode, which is why the check belongs in the
script rather than in a lane's shell history.

## 5. Files

| file | change |
|---|---|
| `tools/mesa/patches/0004-glx-clamp-context-version.patch` | new: unknown-attribute tolerance + version clamp + notices |
| `src/user/gl_probe.c` | asks for Unity's 4.5 core list, a shared second context, 3.3 and 2.1 rows; renders on the 4.5-requested context; `resolve_gl()` now reports its own lookups, not the process-wide failure flag |
| `tools/mesa/build-hobbyos.sh` | per-TU cache also keys on the source mtime (patch → rebuild) |

## 6. Plan amendment for L0 (`docs/rimworld-plan.md`, integrator-owned)

AD-5 is currently written twice (§5 has both "GL target is 3.3 core, but the
*request* is 4.5 core" and a duplicate "GL target is 3.3 core"). Collapse them
into one decision, with the measured clamp:

> **AD-5 (amended, L23).** The request is 4.5 core, the served context is 3.3
> core, and the *frontend* performs the rewrite: `glXCreateContextAttribsARB`
> (Mesa `glx=xlib`, `tools/mesa/patches/0004`) skips unknown context attributes
> and, when the driver's core profile cannot satisfy the requested version,
> retries at 3.3 with the same profile/flags and logs
> `[GLX] context request 4.5 core -> 3.3 core`. Measured in-guest with Unity's
> exact attribute list (RELEASE_BEHAVIOR=FLUSH, NO_ERROR=false) plus a shared
> second context: `ctx=0x...`, `GL_VERSION 3.3 (Core Profile)`, GLSL 330,
> readback `fnv1a64=0x25476e955b66d136`, presented frame 96/1,950,488 pixels
> (cursor only), clean teardown. Before the clamp the same request returned
> NULL twice over: `BadValue` for the unknown attributes and `GLXBadFBConfig`
> for the version. See `docs/rimworld-m4-glx-clamp.md`.

and append to §11 (fix log):

> - 2026-11-04 — **GLX clamp lands: a 4.5-core request is served as 3.3
>   core.** Mesa's `glx=xlib` frontend rejected Unity's attribute list
>   (`BadValue`) and any version above the driver's core profile
>   (`GLXBadFBConfig` → NULL); `0004-glx-clamp-context-version.patch` makes
>   unknown attributes non-fatal and retries at 3.3, announcing the rewrite
>   once. In-guest: `[GLX] context request 4.5 core -> 3.3 core`,
>   `GL_VERSION 3.3 (Core Profile)`, `fnv1a64=0x25476e955b66d136`, present diff
>   0.0049 %, `done rc=0`; M4 gate PASS, units 398/0. The build's per-TU cache
>   now also keys on source mtime — a patched Mesa source was silently shipping
>   its pre-patch object.
