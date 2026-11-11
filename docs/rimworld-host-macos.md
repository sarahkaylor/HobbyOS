# RimWorld on HobbyOS from a macOS host (Apple silicon)

Status: **partial — the desktop rung is reproduced on this host; the game run is
blocked on the payload.** Every row below was produced on this machine
(macOS 27.2, Apple-silicon laptop, QEMU 10.2.2 from Homebrew) on 2026-11-08.
Companion docs: `docs/rimworld-plan.md` (the track), `docs/l83-icewm-launcher.md`
(the Linux-host desktop path this ports).

## 1. What changed vs. the Linux host

Two host facts decide everything else:

| | Linux workstation (the original win) | this macOS laptop |
|---|---|---|
| CPU virtualisation | KVM (`-enable-kvm`) | **TCG only** — no `/dev/kvm`, so every guest instruction is emulated; the host is arm64 and the guest x86_64 |
| GPU acceleration | `virtio-gpu-gl-pci` + `egl-headless,gl=on` → virgl → host GPU | **unavailable**: Homebrew QEMU has no `virtio-gpu-gl-pci` and no `egl-headless` backend, so `/GLMODE` can only be `softpipe` (Mesa renders on the guest CPU) |
| Assets | NFSv3/UDP export from `/srv/nfs/...` | macOS *can* serve it (`/sbin/nfsd` still supports `-u`, `rpcbind` still ships) but only **as root** — see §5 |

Consequence: the macOS path is the *slow* path by construction. It is still a
real HobbyOS desktop with the real X server, IceWM and HBHOST — which is what
the game needs — but frame rates are not comparable to the virgl result.

## 2. Host prerequisites (Homebrew)

```sh
brew install llvm mtools dosfstools qemu \
             pkg-config meson autoconf automake ninja bison \
             xkbcomp xkeyboard-config dpkg \
             libx11 libxext libxfixes libxrandr libxi libxxf86vm \
             libxdamage libxcomposite libxshmfence
```

`xkbcomp` + `xkeyboard-config` are needed by the X server stage (the guest's
keymap is compiled on the host); `dpkg` is needed by Mesa's glibc-2.31 pin
extract; the `libx*` formulae are Mesa's host configure dependencies (`xrandr`
is what actually gates it); a Homebrew `bison` is required because macOS ships
GNU Bison **2.3** from 2006, which mangles Mesa's `.y` grammars instead of
failing cleanly. `xkeyboard-config` must land at `/opt/homebrew/share/X11/xkb`,
which is what the bottle does.

Python: Mesa's generators need `mako`, `pyyaml` and `packaging` (3.14 dropped
`distutils`, and Mesa accepts `packaging` in its place):

```sh
python3 -m venv venv && ./venv/bin/pip install mako pyyaml packaging
MESA_PYTHON=$PWD/venv/bin/python3 bash tools/mesa/build-hobbyos.sh
```

## 3. Build and boot (verified)

```sh
cd ~/Documents/GitHub/HobbyOS

# Wipe the shared ELF when switching ARCH (Makefile pitfall: TARGET is one file)
rm -f hobbyos.elf .arch-stamp

TOOLS_SKIP=1 make -j8 ARCH=intel MODE=desktop DESKTOP=icewm hobbyos.elf disk.img
```

That builds the kernel, a 1 GiB FAT16 `disk.img` carrying `XSESSION.BIN`,
`XHOBBY.BIN`, `ICEWM.BIN`, `ICEWMBG.BIN`, `ICEWMHNT.BIN`, `HBHOST.BIN` and the
X clients. First time it also cross-builds the whole X11/IceWM stack
(`third_party/x11/build.sh --arch intel --stage all`, ~10 min here).

Boot it headless (a `cocoa` window also works, but a headless run is what makes
the screenshots scriptable):

```sh
rm -f /tmp/hb-qmp.sock
TOOLS_SKIP=1 make run ARCH=intel MODE=desktop DESKTOP=icewm \
  QEMU_DISPLAY=none QEMU_ARGS="-qmp unix:/tmp/hb-qmp.sock,server,nowait"

# in another shell: take a picture, and prove it is not a blank frame
python3 tools/rw/qmp_shot.py --qmp /tmp/hb-qmp.sock \
  --out obj/mac-evidence/01-icewm-desktop.ppm --wait 20 --interval 15
sips -s format png obj/mac-evidence/01-icewm-desktop.ppm \
  --out obj/mac-evidence/01-icewm-desktop.png
```

### What that produces (this host, measured)

| check | evidence |
|---|---|
| the guest is the intel build | `make … ARCH=intel` → `hobbyos.elf`, `disk.img` 1 GiB, `mdir` shows the icewm payload |
| the X server, WM and a client run | serial: `[ALIVE-SET] … /XHOBBY.BIN`, `/ICEWM.BIN`, `/XEYESR.BIN`, `XSESSION.BIN` |
| the desktop paints | `obj/mac-evidence/01-icewm-desktop.png` — 1920×1080, IceWM taskbar, workspace buttons 1-4, clock, xeyes window with decorations |
| host input reaches the guest | QMP `sendkey ctrl-esc` → `obj/mac-evidence/02-start-menu.png`, Start menu open with `Files (X11) / Apps / Games / All programs / …` |
| first paint latency | ≈45 s from QEMU start to a non-blank frame (TCG, 8 vCPU) |

`Apps` is the first submenu — that is where `gen-icewm-menu.sh` mints
`prog "RimWorld" app HBHOST.BIN RWLAUNCH.BIN` when `RWLAUNCH.BIN` is on the
disk. On a disk without the payload the row is simply absent, which is what the
screenshot above shows.

## 4. Host-OS defects this port had to fix

All four are environmental (host toolchain), not kernel-side; the fixes are in
`third_party/x11/`, `tools/host/` and `tools/rw/qmp_shot.py`.

| # | symptom | cause | fix |
|---|---|---|---|
| 1 | freetype `configure: error: native C compiler is not working` | macOS 27 CLT SDK's `libSystem.B.tbd` lists `arm64e.x1-macos`, which the same CLT's clang cannot parse → **every native link** fails `ld: tapi error: malformed file` | `tools/host/macos-sdk.sh`: probe once, pin `SDKROOT` to the newest installed SDK that links (`MacOSX26.5` here); sourced by `third_party/x11/cross/env.sh` and `tools/mesa/build-hobbyos.sh` |
| 2 | `stat: illegal option -- c` and `(  bytes)` in every stage log line | `stat -c %s` is GNU-only; BSD wants `-f%z` | `hb_size_of` helper in `third_party/x11/build.sh`, used by the five stage files |
| 3 | `xkm: host xkbcomp missing`, then `no xkeyboard-config at /opt/homebrew/share/X11/xkb` | X-server stage compiles the guest keymap on the host | `brew install xkbcomp xkeyboard-config` (documented in §2) |
| 4 | no way to capture the screen | the Linux harness uses `egl-headless`/pmemsave; neither exists on macOS | `tools/rw/qmp_shot.py` — QMP `screendump`, with a blank-frame check so a black frame cannot pass as evidence |

## 5. What is still open

### 5.1 The payload (hard blocker on this machine)

This laptop's Steam install is the **macOS** depot only — `RimWorldMac.app`,
755 MB, no `RimWorldLinux`, no `UnityPlayer.so`, nothing matching the
fingerprints in `docs/rimworld-plan.md` §2. The track runs the *Linux x86_64*
build (`RimWorldLinux` is a 14,720 B `ET_EXEC` stub; the engine is
`UnityPlayer.so`), so the macOS build is not a substitute.

Get the Linux depot of the copy you already own — from the Steam client's
console (`Help → Steam Support`-style console is not needed; the URL
`steam://open/console` opens it), then:

```
download_depot 294100 294103
```

`294103` is `RimWorld (Linux)`; the client reports the download directory
(`…/steamapps/content/app_294100/depot_294103`). Then verify before staging:

```sh
shasum -a 256 <depot>/RimWorldLinux | cut -c1-16       # expect 0bfe51325720c950
shasum -a 256 <depot>/UnityPlayer.so | cut -c1-16      # expect 4c64f497b3f67fdb
```

A newer public build than the pin (`1.6.4871 rev598`) would print different
hashes — that is a fact to record, not necessarily a blocker (the pin exists to
make runs comparable).

### 5.2 Serving the assets from macOS

The guest's NFS client is NFSv3 **over UDP** and discovers `mountd`/`nfsd`
through the portmapper on port 111 (`src/kernel/nfs.c`), i.e. it needs
`rpcbind` + `nfsd` on the host loopback — both privileged. `tools/rw/nfs-export.sh`
is Linux-shaped (`/srv/nfs`, `sudo -n`, `id -u` checks) and cannot be used as-is.

What macOS provides (checked on this machine): `/usr/sbin/rpcbind`,
`/sbin/nfsd` (`usage: nfsd [-NRrtuv] …`, `-u` = serve UDP clients, and by
default both UDP and TCP), `/usr/bin/showmount`; **`exportfs` is gone**, so the
export table is `/etc/exports` + `nfsd checkexports`.

The macOS counterpart is a small root-only script (export a directory read-only
with `-mapall=nobody`, start rpcbind, `nfsd enable && nfsd start`, then verify
with `rpcinfo -p | grep -E 'nfs|mountd'`). It is **not written yet** because it
needs the payload directory to exist and it needs your sudo password — it will
be written against a real tree rather than guessed at.

### 5.3 The runner's host assumptions

`tools/rw/run-rimworld-desktop.sh` is Linux-shaped in three places that a
macOS run must not inherit:

| line | what it does | macOS |
|---|---|---|
| `:546` | `QEMU_GPU=gl QEMU_DISPLAY=egl-headless` when `RW_QEMU_GL=1` | must force softpipe: there is no GL device to boot |
| `:564` | `-enable-kvm` in `QEMU_ARGS` | must be dropped (Makefile already guards on `/dev/kvm`; this line does not) |
| `:629` | `fuser -k disk.img` | macOS has no `fuser`; `lsof` + `kill` |

These are deliberately **not** patched yet: none of them can be exercised
without the payload, and an unexercised patch is worse than a documented gap.

## 6. GL: what the guest can and cannot get here

`/GLMODE` selects the guest's renderer: `virgl` needs the host GL device (absent
on macOS), so the only reachable renderer is `softpipe` — Mesa's software
rasteriser built for HobbyOS by `tools/mesa/build-hobbyos.sh`, staged into
`/LIB64` as `libGL.so.1` + `libGLX.so.0` by `tools/mesa/stage.sh`. That build
is host-side only (Mesa configured natively to run its code generators, then
every TU replayed against `clang --target=x86_64-linux-gnu` against the pinned
glibc 2.31 headers), so it is not affected by the missing virgl.

**Status on macOS: builds, and the script's own audit passes.**

```
RESULT 840/840 TUs compiled, 0 failed
DT_NEEDED:   0        exports: 1379
undefined:  253       audit: 4103 glibc-2.31 symbols; 29 from libX11; 0 unaccounted
TLS:         40 bytes, static (fits glibc's 1664-byte surplus)
df_1:        NODELETE size: 20,194,088 bytes stripped
```

`obj/mesa/lib/{libGL.so.1,libGLX.so.0,libGL.a}` (43 s incremental): the DSO is
linked against the pinned glibc 2.31 headers and every undefined symbol is
accounted for against what the compat process provides.

**And it renders here.** `python3 tools/gate/m4_gl_probe.py` passes on this host
under TCG:

```
[GL] GL_VERSION  : 3.3 (Core Profile) Mesa 26.2.6
[GL] GL_RENDERER : softpipe
[GL] px blue-vertex (160,211): gl=0607f1ff desktop[rowflip]=ff0607f1
m4: PASS — libGL.so.1 loaded in the guest, rendered a GL 3.3 core triangle,
    PRESENTED it to a window, and tore the stack down
```

So the guest GL stack is no longer a hypothesis on macOS: it is the same result
the Linux farm produced, from a different host OS and CPU architecture.

The gate needed the same host abstraction the runners use. `tools/host/hostcaps.sh`
is now the single answer to "what can this machine do" (`--json` for the Python
runners): acceleration (`-enable-kvm` only where /dev/kvm exists), the x86_64
firmware path (Homebrew ships `edk2-x86_64-code.fd`, not `OVMF_CODE_4M.fd`), the
llvm keg, and a portable image-lock release. Everything the gate got wrong was a
host fact baked in as a constant: `-enable-kvm`, `fuser -k`, `readelf`, bare
`gcc` for the probe, `/usr/bin/python3` forced onto PATH for mako, and a second
glibc sysroot that lacked the kernel UAPI headers the DSO had.

The xcompat DSOs needed one Makefile line too: `--target=x86_64-linux-gnu` and
`-fuse-ld=lld`. Without them a macOS host compiles arm64 Darwin objects and
hands them to `ld64.lld` ("unknown argument '-z'").

Five more host leaks had to be closed after the first pass, all in the
host→target translation:

| symptom | cause | fix |
|---|---|---|
| 10 TUs failing (`qsort_r`, `libunwind.h`, NEON) | Meson probes the **host**; the target is glibc/x86_64 | drop the inherited capability macros (`HAVE_{BSD,GNU}_QSORT_R`, `HAVE_QSORT_S`, `HAVE_LIBUNWIND`, `HAVE_XLOCALE_H`) and set the GNU `qsort_r` answer explicitly; skip host-arch TUs (`*_neon.c`, `*_aarch64.c`) |
| 21 archives of 96 bytes, then `version script ... symbol not defined` | Homebrew's `llvm-ar` is keg-only, so `command -v llvm-ar \|\| command -v ar` fell through to **Apple's `ar`**, which silently drops non-Mach-O members | put `/opt/homebrew/opt/llvm/bin` on PATH and fail loudly if `llvm-ar` is absent |
| `relocation R_X86_64_TPOFF32 ... cannot be used with -shared` | target TUs were compiled without **-fPIC**, so `__thread` defaulted to local-exec (only the C++ runtime was PIC) | `-fPIC` for both flavours (required for any DSO), cache rev bumped |
| 11 undefined `blake3_*{avx2,avx512,sse2,sse41}` | blake3's dispatch lists one impl per ISA, selected from the **host** arch; an arm64 host never compiles the x86 ones | `-DBLAKE3_NO_{AVX2,AVX512,SSE2,SSE41}` on the target: portable path, same output |
| `staged 0 headers` | Homebrew's `include/X11`/`include/GL` are symlink trees; `cp -a` kept them relative and dangling | `cp -RL` |
| verifier `readelf: command not found`, `stat -c%s` | GNU tools | `llvm-readelf` + portable `size_of` |

### What was already fixed on the way in (macOS host)

| symptom | cause | fix |
|---|---|---|
| `missing /usr/include/X11` | header roots are Linux-shaped | probe `/usr/include`, `/usr/local/include`, `$HOMEBREW_PREFIX/include`; GL interface headers fall back to Mesa's own tree |
| `dpkg-deb: command not found` | `dpkg` not part of a macOS toolchain | `brew install dpkg` |
| meson "mako module >= 0.8.0 required" | `MESA_PYTHON` was checked by the script but meson resolves `python3` through `PATH` | prepend the chosen interpreter's directory |
| "One of Python (3.x) packaging or distutils is required" | Python 3.14 removed `distutils` | install `packaging` |
| bison: `glcpp-parse.y: syntax error, unexpected identifier` | Apple's GNU Bison 2.3 | prefer the Homebrew keg's bison (>= 3) |
| `libsync.h: not found` | that header is libdrm's; macOS has no libdrm package | the repo's own shim (`tools/mesa/include/libsync.h`) is put on the **host** include path only, via `CPATH`, so the recorded compile lines stay canonical |
| host `libGL.1.dylib` link: undefined `_XcPresentMark` | Mesa's xlib target references a symbol HobbyOS's patched Xlib adds; the system Xlib has no such symbol | `-Wl,-undefined,dynamic_lookup` on Darwin (the host dylib is disposable — only its side effects are used) |
| `linux/errno.h`, `linux/limits.h` not found | glibc 2.31's own headers include the kernel UAPI, which on Linux comes from `linux-libc-dev` in `/usr/include` | new pin `linux-libc-dev_5.4.0-218.238` (focal era, sha256-recorded) unpacked into the same sysroot |
| `du -cb: illegal option` (exit 64 kills the build) | GNU `du --bytes` | portable byte count |
| `libGL.so.1.5.0.p/xlib.c.o` not found | the frontend object path carries the *library file name* (dylib on macOS) | locate it by source file instead of soname |
| `libmesa_sse41.a` / `libmesa_util_clflushopt.a` query fails | Mesa emits those only when the **host** is x86_64 | skip an archive the host never built, with a note |
