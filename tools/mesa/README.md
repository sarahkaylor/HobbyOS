# tools/mesa — Mesa's GL 3.3 frontend for HobbyOS (lane L5, milestone M4)

`build-hobbyos.sh` builds **libGL.so.1** — GLX + `mesa/st` + glapi + softpipe —
for the HobbyOS x86_64 *compat* class, with no LLVM, no DRM, no EGL, no
libglvnd and no shader cache. The kernel gains no graphics ABI: this is a
userspace DSO (plan AD-4/AD-6).

```
tools/mesa/build-hobbyos.sh                 # full build, ~6 min on 24 cores
tools/mesa/build-hobbyos.sh --gen-only      # stop after host code generation
tools/mesa/build-hobbyos.sh --self-contained  # link HobbyOS libc instead of
                                              # resolving libc from glibc
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

## The two link flavours

* **Default (compat-resolved).** `libGL.so.1` has no `DT_NEEDED` and leaves
  258 symbols undefined. They resolve from the *compat class's* global scope:
  the pinned glibc 2.31 (`libc`, `libm`, `libpthread`, `libdl`, `librt`),
  lane L4's `libX11.so.6`, `libgcc_s.so.1`, plus `__tls_get_addr` from
  `ld.so`. Two symbols (TLS) are why the note "ELF TLS is a hard blocker"
  does not apply here: glibc's `ld.so` is what processes `PT_TLS` in a
  `dlopen`ed DSO, and it does that correctly.
* **`--self-contained`** (measured, experimental). Links `src/libc/src/*.c` +
  `src/user/{libc,malloc}.c` rebuilt `-fPIC` (the tree's `obj/intel/libc.a`
  is `-mcmodel=large` and cannot enter a DSO: measured `R_X86_64_64 against
  local symbol`), plus `gl_stubs.c`. Result: 0 `DT_NEEDED`, 1378 exports,
  **45 undefined** — 23 libX11 + 3 libXext + 13 libc/libm left
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
size:      19,211,944 bytes stripped
undefined: 258 — every one attributable to glibc/libX11/libgcc_s/ld.so
```

`obj/mesa/obj/tu-summary.json` records the per-TU result (835/835 compiling on
a clean run); `obj/mesa/log/` has the exact command line and the compiler's
output for every TU.
