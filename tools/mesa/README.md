# tools/mesa — Mesa's GL 3.3 frontend for HobbyOS (lane L5, milestone M4)

`build-hobbyos.sh` builds **libGL.so.1** — GLX + `mesa/st` + glapi + softpipe —
for the HobbyOS x86_64 *compat* class, with no LLVM, no DRM, no EGL, no
libglvnd and no shader cache. The kernel gains no graphics ABI: this is a
userspace DSO (plan AD-4/AD-6).

```
tools/mesa/build-hobbyos.sh                 # full build, 45 s on 24 cores
tools/mesa/build-hobbyos.sh --gen-only      # stop after host code generation
tools/mesa/build-hobbyos.sh --self-contained  # hobbyos ABI: link HobbyOS libc
tools/mesa/build-hobbyos.sh --abi hobbyos   # same, without the libc.a link
```

Artifacts land in `obj/mesa/lib/`:

| file | what |
|---|---|
| `libGL.so.1` | 19.2 MB stripped, SONAME `libGL.so.1`, **0 `DT_NEEDED`**, 1378 exports |
| `libGL.so.1.5.0` | symlink (upstream's unversioned name) |
| `libGLX.so.0` | symlink to `libGL.so.1` — the name SDL/Unity `dlopen`s; a non-glvnd build has one DSO, and its glX* entries are real |
| `libGL.a` | the same objects, static (for in-guest probes) |
| `exports.txt`, `undefined.txt` | `nm -D` dumps the build verifies itself against |

## Why it is not a `meson --cross-file` build

A HobbyOS cross build cannot run target binaries, and Mesa's configure probes
`cc.run()` for endianness, `timespec`, the TLS dialect and more. It also runs
bison/flex/python generators inside the build tree. So the script treats the
**host** build as the source of truth for two things and does the cross work
itself:

1. **Generated sources (69 files) are produced on the host and stored as data**
   under `obj/mesa/gen/`, with a `MANIFEST.sha256` so drift is visible. These
   are the glapi dispatch/marshal tables, NIR/GLSL lexers+parsers, format
   tables, and so on — all Mesa-version-specific, none of them generated
   in-guest (the guest never compiles Mesa; it loads the DSO).
2. **Every compile line is replayed from the host `build.ninja`**, retargeted
   at `clang --target=x86_64-none-elf -ffreestanding -nostdinc`. That keeps
   the TU list and the flags upstream's instead of a hand-maintained list that
   rots: the script asks ninja which objects make up libGL and what command
   line produced each.

## What HobbyOS had to grow (and where it lives)

Header shims in `tools/mesa/include/` are Mesa-local by design — none of them
changes the meaning of an upstream file, and the one upstream edit is a marked
patch:

| file | why |
|---|---|
| `endian.h` | HobbyOS is little-endian and has no `<endian.h>`; `src/util/u_endian.h` needs `__BYTE_ORDER` + the `htole*`/`le*toh*` family. This was the **first** blocker — it stopped nearly every TU. |
| `fenv.h` | no `<fenv.h>`; every entry point is a `static inline` no-op (the kernel never unmasks FP traps, so "exceptions already held" is true). |
| `elf.h`, `link.h` | `ElfW(Phdr)`/`Elf64_Nhdr` for `build_id.c` and `u_cpu_detect.c`; `dl_iterate_phdr` is a fail-closed stub. |
| `nl_types.h`, `syslog.h` | pulled in by libc++ `<locale>` and `util/log.c`; fail-closed. |
| `sys/ipc.h`, `sys/shm.h` | MIT-SHM fast path in the sw/xlib winsys. `XLIB_NO_SHM=1` skips it at run time; the declarations are compile-time only. |
| `sys/syscall.h`, `linux/futex.h`, `linux/types.h` | raw `syscall(SYS_futex/getrandom/gettid/kcmp)` and the DRM uapi header's `__u8..__u64`. |
| `sys/file.h`, `sys/eventfd.h`, `sys/inotify.h`, `sys/auxv.h` | cache/notify surfaces; all fail closed. |
| `xf86drm.h`, `asm/ioctl.h` | the 20 DRM entry points Mesa references (the host build used exactly one: `drmGetCap`). All return -1 — there is no DRM device. |
| `X11/Xlibint.h` | libX11's *private* header. An empty shim: it also compiles out libX11's display lock, which the compat `libX11.so.6` does not export. |
| `libsync.h` | Android sync fences, only `u_tests.c`; fail-closed. |

The libc headers gained only genuinely-missing POSIX/C11 declarations
(`static_assert` in `<assert.h>`, the `M_*` constants and `clock_nanosleep` +
`CLOCK_*` in `<math.h>`/`<time.h>`, `alloca`/`qsort_r`/`posix_memalign`/
`secure_getenv` in `<stdlib.h>`, `strndup`, `O_CLOEXEC`/`F_DUPFD`, `NSIG`/
`SIGSYS`, `PATH_MAX`, `id_t`, `cpu_set_t`, `pthread_*_np`, …). Their
implementations live in `src/user/xcompat/gl_stubs.c`.

`patches/0001-glx-xlib-no-private-Xlib-hooks.patch` is the only change to
Mesa's own source: `glx_api.c` walks `Display->ext_procs` (a private libX11
list) to hook `XCloseDisplay`. The compat libX11 has no such layout, so the
hook is compiled out — the display record is freed at process exit instead.

## Two ABIs, and why the default is glibc's

`pthread_mutex_t` is **8 bytes** in HobbyOS's `<pthread.h>` and **40 bytes** in
glibc's, and Mesa stores pthread types *by value* (`struct xmesa_display`'s
`mtx_t`, `std::mutex` in libc++'s GLSL-facing code).  A DSO compiled against
HobbyOS headers but loaded into a glibc process therefore writes 40 bytes over
an 8-byte field: measured, `mtx_init()` zeroed the whole display struct and the
first `glXChooseFBConfig` segfaulted at `xmdpy->screen->is_format_supported`.

So the DSO's headers must match the *runtime* it will be loaded into:

| flavour | headers | libc at run time | used for |
|---|---|---|---|
| default (`--abi glibc`) | glibc's (`--target=x86_64-linux-gnu`), libc++ 21.1.8 rebuilt -fPIC | the compat class's pinned glibc 2.31 | the shipped `libGL.so.1` |
| `--abi hobbyos` (`--self-contained`) | HobbyOS sysroot + `tools/mesa/include` shims | HobbyOS libc, linked in | the no-glibc experiment |

## The two link flavours

* **Default (compat-resolved).** `libGL.so.1` has no `DT_NEEDED` and leaves
  252 symbols undefined. They resolve from the *compat class's* global scope:
  the pinned glibc 2.31 (`libc`, `libm`, `libpthread`, `libdl`, `librt`),
  lane L4's `libX11.so.6`, `libgcc_s.so.1`, plus `__tls_get_addr` from
  `ld.so`. Two symbols (TLS) are why the note "ELF TLS is a hard blocker"
  does not apply here: glibc's `ld.so` is what processes `PT_TLS` in a
  `dlopen`ed DSO, and it does that correctly.
* **`--self-contained`** (measured, experimental). Links `src/libc/src/*.c` +
  `src/user/{libc,malloc}.c` rebuilt `-fPIC` (the tree's `obj/intel/libc.a`
  is `-mcmodel=large` and cannot enter a DSO: measured `R_X86_64_64 against
  local symbol`), plus `gl_stubs.c`. Result: 0 `DT_NEEDED`, 1378 exports,
  **67 undefined** — 23 libX11 + 3 libXext + 13 libc/libm left
  (`__cxa_atexit`, `__cxa_finalize`, the double `sin/cos/exp/...` that
  `math.c` lacks, `sched_getaffinity`) + 4 `__tls_*` symbols that the
  *native* loader's linker script defines and a dlopened DSO cannot see.
  Not the shipped path: it duplicates libc state (a second malloc arena, a
  second errno) inside the DSO.

## Integration (integrator-owned Makefile)

The lane does not touch `Makefile`. The build needs exactly this, placed
next to the existing `include src/user/xcompat/Makefile.inc`:

```make
# Mesa GL for the compat class (lane L5, M4).  Out-of-tree: obj/mesa/lib.
MESA_JOBS ?= 8
mesa_gl:
	JOBS=$(MESA_JOBS) bash tools/mesa/build-hobbyos.sh -j $(MESA_JOBS)
.PHONY: mesa_gl
```

Staging `obj/mesa/lib/libGL.so.1` + the `libGLX.so.0` symlink into `/LIB64`
belongs to the disk recipe (M3's `xcompat_dsos` staging already does the same
for `libX11.so.6`).

## What is *not* here

* EGL/GLES: a hard meson incompatibility with `glx=xlib` (EGL requires DRI).
  A second build is the only route; it also needs `dlopen`, which the GLX
  build does not.
* virgl: the performance answer (plan AD-6) needs virtio-gpu 3D in the kernel.
* Anything that runs at build time in the guest. The guest receives
  `libGL.so.1` + the `libGLX.so.0` symlink and the environment
  `XLIB_NO_SHM=1 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe`.

## Verification

The script fails the build if any of these stops being true:

```
DT_NEEDED: 0
exports:   1378 (glXGetProcAddress, glXSwapBuffers, glGetString present)
size:      20,070,984 bytes stripped
undefined: 252 — every one attributable to glibc/libX11/libgcc_s/ld.so
```

and, beyond the link, the DSO **renders**: `cube.c` (the host proof's own
GLSL-330 textured cube, linked against `obj/mesa/lib/libGL.so.1`) on Xvfb at
1280x720 reports

```
GL_RENDERER : softpipe
GL_VERSION  : 3.3 (Core Profile) Mesa 26.2.6      GLSL: 3.30
readback 1280x720: fnv1a64=0xc0b8ac43346002eb glGetError=0x0
                   non-background=291572/921600 (31.6%)  colours=364
timed: 130 frames in 4.014 s => 32.39 fps (client-side, includes swap)
```

— byte-identical to the host build's reference checksum `0xc0b8ac43346002eb`,
291 572 covered pixels and 364 distinct colours, i.e. the cross-built DSO is
the same renderer, not merely a DSO that loads.

`obj/mesa/obj/tu-summary.json` records the per-TU result (835/835 compiling on
a clean run); `obj/mesa/log/` has the exact command line and the compiler's
output for every TU.

## In the guest (M4 gate): what the host build could not tell us

The host harness proves the DSO renders; it does not prove it *loads into the
compat class*. `tools/gate/m4_gl_probe.py` boots the real kernel, stages
`libGL.so.1` + `libGLX.so.0` in `/LIB64` (`tools/mesa/stage.sh`), launches
`src/user/gl_probe.c` (a glibc PIE whose `DT_NEEDED` are libX11/libdl/libm/
libpthread/libc, the same shape as the game), and grades its serial rows.

Five things only the guest found, each now enforced by the build or the DSO:

1. **The build must use glibc 2.31's headers, not the host's.** `libGL.so.1`
   compiled against the host's glibc names `__isoc23_strtol`, `__isoc23_strtoul`
   and `fstat64`; none exists in 2.31, so `dlopen(RTLD_NOW)` died with
   "undefined symbol: log10" once those were gone — i.e. `libm` is a load-time
   dependency too. The script now extracts the pinned `libc6-dev` (sha256) and
   compiles `-nostdinc` against it, then **audits every undefined symbol against
   the pinned runtime**: 4103 glibc symbols, 29 from libX11, **0 unaccounted**.
2. **`PT_TLS` must fit glibc's static-TLS surplus (1664 bytes).** Mesa keeps a
   4 KiB `debug_dump_flags` scratch buffer in `thread_local`, which made
   `PT_TLS` 4400 bytes → `dlopen` failed with "cannot allocate memory in static
   TLS block" (host and guest alike). `patches/0003-debug-dump-flags-no-tls.patch`
   makes those buffers plain statics: `PT_TLS` is 40 bytes, the DSO keeps
   `DF_STATIC_TLS`, and `-ftls-model=global-dynamic` is only the fallback (see
   `patches/0002`) if a future change grows it again.
3. **`libGL` needs `libX11` symbols the compat DSO did not export**:
   `_XLockMutex_fn`, `_XUnlockMutex_fn`, `_Xglobal_lock` (libX11's private
   thread hooks) and `XDrawString16`/`XFreeFontInfo`/`XGetImage` (glXUseXFont /
   glXGetImage paths). RTLD_NOW resolves *all* of them, so "never called" is not
   good enough.
4. **`XDestroyImage` is a macro, not a function.** `<X11/Xlib.h>` expands it to
   `img->f.destroy_image(img)`, so an XImage whose `f` table is zero turns the
   client's `XDestroyImage()` into a jump to address 0. Measured in the guest as
   `memory fault VA=0x0 PC=0x0` inside `glXChooseFBConfig` →
   `bits_per_pixel()`.
5. **`XGetVisualInfo` must return freeable memory.** Mesa's `choose_visual()`
   ends with `free(vis)`, so returning a pointer into the DSO's `.bss` aborts
   the client with `free(): invalid size`. The compat libX11 now mallocs that
   one structure (its only external symbol) and `XVisualInfo.class` reports
   `TrueColor`, not `InputOutput` (Mesa maps 1 to GLX_GRAY_SCALE and then
   refuses RGB rendering).

With those in place the guest reports, from a real boot:

```
[GL] GL_VERSION  : 3.3 (Core Profile) Mesa 26.2.6
[GL] GL_RENDERER : softpipe
[GL] GLSL        : 3.30
[GL] FBO 256x256 status=0x8cd5 (complete=0x8cd5)
[GL] vertex compile: ok / fragment compile: ok / program link: ok
[GL] readback 256x256: fnv1a64=0x25476e955b66d136 glGetError=0x0
[GL] non-background=22890/65536 (34%) colours=22891
```

— the same checksum and the same 22 890 covered pixels the *host* run of the
same DSO reports, so it is the same renderer and the same frame.

**The present path (fixed, lane L16).** `glXSwapBuffers` now puts a GL 3.3
frame on the desktop: Mesa's `xlib_sw_display` builds an `XImage` over the
display target, `XPutImage` copies it into the compat libX11's shadow, and
`XFlush` blits that to `/dev/fb0` and tells the desktop to present. Three
things had to be true, and the third was not:

1. the `XImage` ABI (above) — `bits_per_pixel`, `format`, `bitmap_unit`,
   `data`, `bytes_per_line`, `f.` table;
2. the client has to wait for the desktop's content rect (`ConfigureNotify`/
   `Expose`) before painting: the shim refuses to blit a window that has not
   been laid out, so an early swap silently drops every pixel and still
   "succeeds" — the trap that hid this for a wave;
3. **`libGL.so.1` must be linked `-z nodelete`** (DF_1_NODELETE). Mesa calls
   plain `atexit()` from inside the DSO (`src/mesa/main/formats.c:499`
   registers `format_array_format_table_destroy`; `util/os_misc.c`,
   `main/extensions.c`, `main/context.c`, `util/strtod.c` do the same). glibc's
   `atexit()` is process-global and is *not* run by `dlclose()`, so after a
   client's `dlclose(libGL)` the process's `exit()` jumped into the unmapped
   text. Measured in the guest, after a *successful* swap and a clean
   `glXMakeCurrent(None)`/`glXDestroyContext`/`XCloseDisplay`/`dlclose`:

   ```
   [KERNEL] pid=3 (GLPROBE.BIN) memory fault VA=0x1081ACFF50 PC=0x1081ACFF50
            rw=rx in=HOLE region=NONE ... -> killed
   ```

   `libGL base + 0xe61f50` is exactly `format_array_format_table_destroy`
   (confirmed by relinking the same objects unstripped). That was the recorded
   "instruction fetch into a non-executable libGL page" — it is an
   **unload** defect at process exit, not a fault in the swap path.
   `DF_1_NODELETE` makes `dlclose` drop the reference without unmapping, which
   is how libglvnd loads a vendor GL driver (`RTLD_NODELETE`); the build now
   fails if the flag disappears.

With those in place `src/user/gl_probe.c` presents a real window and the gate
checks the desktop, not the GL side: `tools/gate/m4_gl_probe.py` screendumps
the window during a 6 s hold and grades the probe's `/dev/fb0` readback
(`present check`, `pixel diff`) plus the four teardown rows.


