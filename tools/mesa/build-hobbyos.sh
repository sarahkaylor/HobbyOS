#!/usr/bin/env bash
# tools/mesa/build-hobbyos.sh -- build Mesa's GL 3.3 frontend for HobbyOS x86_64.
#
#   Lane L5 (M4).  Produces the compat class's GL:
#
#     obj/mesa/lib/libGL.so.1     GLX + mesa/st + glapi + softpipe, 0 DT_NEEDED
#     obj/mesa/lib/libGLX.so.0    -> libGL.so.1 (the name SDL/Unity dlopen)
#     obj/mesa/lib/libGL.a        the same objects, static (for probes/tests)
#
#   No LLVM, no DRM, no EGL, no libglvnd, no shader cache, no zlib/zstd.
#   The kernel needs no graphics ABI: this is a userspace DSO (AD-4/AD-6).
#
# WHY THIS SCRIPT DOES NOT USE `meson --cross-file`
#
#   A HobbyOS cross build cannot run the target compiler's output, and Mesa's
#   configure probes are `cc.compiles()`/`cc.links()`/`cc.run()` - the run()
#   ones (endianness, timespec layout, TLS dialect, ...) cannot answer for
#   x86_64-none-elf.  It also runs bison/flex/python generators in the build
#   tree.  Both problems disappear if the *host* build is treated as the
#   source of truth for (a) generated sources and (b) the exact per-TU flag
#   vector, which is what this script does:
#
#     1. configure + build Mesa natively for the host with the pinned option
#        set (22.8 s on 32 cores) - this runs every code generator;
#     2. export the 69 generated .c/.h files as *data* under $WORK/gen,
#        with a sha256 manifest so drift is detectable (they are Mesa-version
#        specific; nothing is generated in-guest, and nothing large is
#        committed);
#     3. replay every compile line from the host build's build.ninja against
#        `clang --target=x86_64-none-elf -ffreestanding -nostdinc`, with the
#        HobbyOS sysroot headers + the shims in tools/mesa/include;
#     4. archive + link with ld.lld and verify the result (exports, soname,
#        DT_NEEDED, unresolved-symbol provenance).
#
#   Step 3 is why the TU list and flags are *upstream's*, not a hand-written
#   list that rots: the script asks ninja which objects go into libGL and
#   what command line produced each one.
#
# USAGE
#
#   tools/mesa/build-hobbyos.sh                  # full build (default)
#   tools/mesa/build-hobbyos.sh --gen-only       # stop after step 2
#   tools/mesa/build-hobbyos.sh --self-contained # link HobbyOS libc instead
#                                                # of resolving libc from the
#                                                # compat class (experimental)
#   tools/mesa/build-hobbyos.sh -j 8 --keep-build
#
#   Env: MESA_WORK (default obj/mesa), MESA_CACHE (default
#        $HOME/.cache/hobbyos-mesa), LIBCXX_SRC (default
#        third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src)
#
# PREREQUISITES (host, Ubuntu): meson ninja-build python3-mako python3-yaml
#   flex bison libx11-dev libxext-dev libxfixes-dev libxcb1-dev zlib1g-dev
#   libdrm-dev clang lld  (+ the tree's libc++ pin fetched by
#   third_party/libcxx-21.1.8/fetch.sh, needed for the C++ half of Mesa)
#
# LICENSE: Mesa is MIT; this script is part of the HobbyOS tree (GPL-2.0).
#   The Mesa tarball is fetched, never committed (see docs/rimworld-plan.md §2).

set -euo pipefail

# ---------------------------------------------------------------------------
# Pin (docs/rimworld-plan.md §2, MESA-NOTES.md §1)
# ---------------------------------------------------------------------------
MESA_VERSION=26.2.6
MESA_SHA256=b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92
MESA_URL=https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz
MESA_SIG_URL=${MESA_URL}.sig
MESA_KEYS_URL=https://docs.mesa3d.org/release-maintainers-keys.asc

# The compat class runs glibc 2.31 (docs/rimworld-plan.md §2).  Compiling the
# glibc flavour against the *host's* headers is what made the first DSO
# unloadable in the guest: the host's <stdlib.h>/<sys/stat.h> redirect strtol
# to __isoc23_strtol and fstat to fstat64, neither of which exists in 2.31, so
# dlopen(RTLD_NOW) failed with "undefined symbol".  So the glibc flavour
# compiles against the pinned runtime's own headers, and the link audit at the
# end checks every undefined symbol against the pinned runtime's symbol tables.
# Both .debs are pinned by sha256 (plan §2).
GLIBC_VERSION=2.31-0ubuntu9.18
GLIBC_RT_URL=http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_${GLIBC_VERSION}_amd64.deb
GLIBC_RT_SHA256=1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0
GLIBC_DEV_URL=http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_${GLIBC_VERSION}_amd64.deb
GLIBC_DEV_SHA256=11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a

# Bumped whenever the flag vector changes in a way the object cache cannot
# see (it keys on mtimes): r2 = -ftls-model=global-dynamic + the 2.31 header pin.
CROSS_FLAGS_REV=4

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$(cd "${SCRIPT_DIR}/../.." && pwd)
WORK=${MESA_WORK:-${TREE}/obj/mesa}
CACHE=${MESA_CACHE:-${HOME}/.cache/hobbyos-mesa}
SRC=${CACHE}/mesa-${MESA_VERSION}
HOSTBUILD=${WORK}/host-build
GEN=${WORK}/gen
STAGE=${WORK}/stage
OBJ=${WORK}/obj
LIBDIR=${WORK}/lib
LIBCXX_SRC=${LIBCXX_SRC:-${TREE}/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src}

CC=${CC:-clang}
CXX_DRIVER=${CXX_DRIVER:-clang}
LD=${LD:-ld.lld}
AR=${AR:-$(command -v llvm-ar || command -v ar)}
STRIP=${STRIP:-$(command -v llvm-strip || command -v strip)}
CLANG_RES=$(${CC} -print-resource-dir)/include

JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
SELF_CONTAINED=0
GEN_ONLY=0
# Which C library ABI the DSO is compiled against.  Not cosmetic:
# pthread_mutex_t is 8 bytes in HobbyOS's headers and 40 in glibc's, and Mesa
# stores pthread types by value (struct xmesa_display::mutex, std::mutex in
# the GLSL compiler).  A DSO compiled against HobbyOS headers but resolved
# against glibc corrupts memory at the first mtx_init - measured: SIGSEGV in
# xmesa_init_display with the whole display struct zeroed.
#   abi=glibc    -> the compat class's libGL (it runs inside a glibc process)
#   abi=hobbyos  -> --self-contained (links HobbyOS libc, its own pthread)
ABI=glibc
KEEP_BUILD=0

while [ $# -gt 0 ]; do
  case "$1" in
    -j|--jobs)         JOBS="$2"; shift 2 ;;
    --gen-only)        GEN_ONLY=1; shift ;;
    --self-contained)  SELF_CONTAINED=1; ABI=hobbyos; shift ;;
    --abi)             ABI="$2"; [ "$ABI" = hobbyos ] && SELF_CONTAINED=1; shift 2 ;;
    --keep-build)      KEEP_BUILD=1; shift ;;
    --work)            WORK="$2"; OBJ=${WORK}/obj; LIBDIR=${WORK}/lib
                       HOSTBUILD=${WORK}/host-build; GEN=${WORK}/gen
                       STAGE=${WORK}/stage; shift 2 ;;
    -h|--help)         sed -n '2,50p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

say()  { printf '\n=== %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'build-hobbyos: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. Preflight: Mesa's build needs python-mako and python-yaml *for the host
#    configure step* (glapi/NIR/GLSL table generation).  A venv on PATH wins;
#    otherwise point MESA_PYTHON at an interpreter that has them.
# ---------------------------------------------------------------------------
preflight() {
  local py=${MESA_PYTHON:-python3}
  "$py" - <<'EOF' || die "python3 needs mako>=0.8 and pyyaml for Mesa's generators \
(apt install python3-mako python3-yaml, or: python3 -m venv .venv && .venv/bin/pip install mako pyyaml \
and prepend .venv/bin to PATH)"
import sys
try:
    import mako, yaml  # noqa: F401
except Exception as exc:  # pragma: no cover - the message is the point
    print(f"    python3 {sys.version.split()[0]} at {sys.executable}: {exc}")
    sys.exit(1)
print(f"    python3 {sys.version.split()[0]} at {sys.executable}: mako {mako.__version__}, pyyaml ok")
EOF
  for t in meson ninja flex bison curl; do
    command -v "$t" >/dev/null || die "missing host tool: $t"
  done
  [ -d /usr/include/X11 ] || die "missing /usr/include/X11 (apt install libx11-dev)"
  [ -d /usr/include/GL ] || die "missing /usr/include/GL (apt install libx11-dev)"
}

# ---------------------------------------------------------------------------
# 1. Fetch + verify
# ---------------------------------------------------------------------------
fetch_mesa() {
  say "1/6 fetch Mesa ${MESA_VERSION}"
  mkdir -p "$CACHE"
  cd "$CACHE"
  if [ ! -f "mesa-${MESA_VERSION}.tar.xz" ]; then
    curl -fL -O "$MESA_URL"
    curl -fL -O "$MESA_SIG_URL" || note "signature unavailable (continuing with sha256)"
  fi

  local got
  got=$(sha256sum "mesa-${MESA_VERSION}.tar.xz" | cut -d' ' -f1)
  [ "$got" = "$MESA_SHA256" ] || die "sha256 mismatch: got $got, want $MESA_SHA256"
  note "sha256 ok: $got"

  # GPG is the project's real provenance mechanism (release notes carry the
  # sha256, but the tarballs are signed).  Best-effort: a missing key or a
  # gpg-less host must not block a verified-by-hash build.
  if [ -f "mesa-${MESA_VERSION}.tar.xz.sig" ] && command -v gpg >/dev/null; then
    if [ ! -f release-maintainers-keys.asc ]; then
      curl -fL -o release-maintainers-keys.asc "$MESA_KEYS_URL" || true
    fi
    if [ -f release-maintainers-keys.asc ]; then
      GNUPGHOME=${GNUPGHOME:-${CACHE}/gnupg}
      mkdir -p "$GNUPGHOME"; chmod 700 "$GNUPGHOME"
      gpg --batch --quiet --import release-maintainers-keys.asc 2>/dev/null || true
      if gpg --batch --verify "mesa-${MESA_VERSION}.tar.xz.sig" \
             "mesa-${MESA_VERSION}.tar.xz" 2>/dev/null; then
        note "gpg: good signature"
      else
        note "gpg: signature NOT verified (hash still pins the bytes)"
      fi
    fi
  fi

  if [ ! -d "$SRC" ]; then
    note "extracting to $SRC"
    rm -rf "$SRC.tmp"; mkdir -p "$SRC.tmp"
    tar -xf "mesa-${MESA_VERSION}.tar.xz" -C "$SRC.tmp" --strip-components=1
    mv "$SRC.tmp" "$SRC"
  fi

  # The compat build cannot use glx=xlib's private-Xlib hooks (see the patch);
  # apply it idempotently so a re-run over an already-patched tree is a no-op.
  local p
  for p in "${SCRIPT_DIR}"/patches/*.patch; do
    [ -e "$p" ] || continue
    if patch -p1 -R --dry-run -s -f -d "$SRC" < "$p" >/dev/null 2>&1; then
      note "patch already applied: $(basename "$p")"
    else
      patch -p1 -s -d "$SRC" < "$p" || die "patch failed: $p"
      note "applied patch: $(basename "$p")"
    fi
  done
}

# ---------------------------------------------------------------------------
# 1b. The glibc 2.31 pin: headers to compile against, runtime DSOs to audit
#     the link against.  Without this the DSO compiles against the *host's*
#     glibc and picks up symbols the guest's 2.31 cannot resolve (measured:
#     __isoc23_strtol, __isoc23_strtoul, fstat64), which is a build that loads
#     on the builder and fails with "undefined symbol" in the guest.
# ---------------------------------------------------------------------------
ensure_glibc_pin() {
  [ "$ABI" = glibc ] || return 0
  say "1b/6 glibc ${GLIBC_VERSION} pin (compile headers + runtime symbol audit)"
  mkdir -p "$CACHE"

  if [ -z "${GLIBC231_DEV:-}" ]; then
    GLIBC231_DEV=${CACHE}/glibc231-dev
  fi
  if [ ! -f "${GLIBC231_DEV}/usr/include/stdio.h" ]; then
    local deb=${CACHE}/libc6-dev_${GLIBC_VERSION}_amd64.deb
    if [ ! -f "$deb" ]; then
      note "fetching $(basename "$deb")"
      curl -fsSL -o "$deb" "$GLIBC_DEV_URL" || die "cannot fetch $GLIBC_DEV_URL"
    fi
    local got; got=$(sha256sum "$deb" | cut -d' ' -f1)
    [ "$got" = "$GLIBC_DEV_SHA256" ] || die "libc6-dev sha256 mismatch: got $got"
    note "libc6-dev sha256 ok"
    rm -rf "$GLIBC231_DEV"; mkdir -p "$GLIBC231_DEV"
    dpkg-deb -x "$deb" "$GLIBC231_DEV" || die "cannot extract $deb"
  fi

  if [ -z "${GLIBC231_RT:-}" ]; then
    GLIBC231_RT=${CACHE}/glibc231-rt
  fi
  if [ ! -f "${GLIBC231_RT}/lib/x86_64-linux-gnu/libc.so.6" ]; then
    local deb=${CACHE}/libc6_${GLIBC_VERSION}_amd64.deb
    if [ ! -f "$deb" ]; then
      note "fetching $(basename "$deb")"
      curl -fsSL -o "$deb" "$GLIBC_RT_URL" || die "cannot fetch $GLIBC_RT_URL"
    fi
    local got; got=$(sha256sum "$deb" | cut -d' ' -f1)
    [ "$got" = "$GLIBC_RT_SHA256" ] || die "libc6 sha256 mismatch: got $got"
    note "libc6 sha256 ok"
    rm -rf "$GLIBC231_RT"; mkdir -p "$GLIBC231_RT"
    dpkg-deb -x "$deb" "$GLIBC231_RT" || die "cannot extract $deb"
  fi

  # -nostdinc, because "the host's /usr/include" must not be reachable for
  # *libc* headers; clang's resource dir still supplies stddef.h/stdarg.h.
  # The one deliberate exception is /usr/include itself, which is where the
  # kernel UAPI lives (linux/limits.h, linux/errno.h from linux-libc-dev) --
  # glibc 2.31's own headers #include those, and the pin's .deb does not carry
  # them.  Every libc name still resolves in the pin first (it comes earlier).
  GLIBC_INC="-nostdinc -isystem ${GLIBC231_DEV}/usr/include \
-isystem ${GLIBC231_DEV}/usr/include/x86_64-linux-gnu -isystem ${CLANG_RES} \
-isystem /usr/include"
  note "headers: ${GLIBC231_DEV}/usr/include (pin ${GLIBC_VERSION})"
  note "runtime audit: ${GLIBC231_RT}/lib/x86_64-linux-gnu"
}

# ---------------------------------------------------------------------------
# 2. Host configure + build (code generation only in practice)
# ---------------------------------------------------------------------------
host_build() {
  say "2/6 host meson configure + build (runs every code generator)"
  if [ ! -f "${HOSTBUILD}/build.ninja" ]; then
    CC=gcc CXX=g++ meson setup "$HOSTBUILD" "$SRC" \
      -Dgallium-drivers=softpipe \
      -Dllvm=disabled \
      -Dplatforms=x11 \
      -Dglx=xlib \
      -Dglvnd=disabled \
      -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
      -Dgbm=disabled \
      -Dopengl=true \
      -Dvulkan-drivers= -Dvideo-codecs= -Dgallium-va=disabled \
      -Dzstd=disabled -Dshader-cache=disabled \
      -Dbuild-tests=false -Dtools= -Dhtml-docs=disabled \
      > "${WORK}/host-setup.log" 2>&1 || { tail -30 "${WORK}/host-setup.log"; die "meson setup failed"; }
  fi
  ninja -C "$HOSTBUILD" -j "$JOBS" > "${WORK}/host-ninja.log" 2>&1 \
    || { tail -30 "${WORK}/host-ninja.log"; die "host ninja failed"; }
  note "host build ok ($(grep -c '^\[[0-9]*/[0-9]*\]' "${WORK}/host-ninja.log" || true) edges logged)"
}

# ---------------------------------------------------------------------------
# 3. Export the generated sources/headers as data + manifest
# ---------------------------------------------------------------------------
export_generated() {
  say "3/6 export generated sources/headers (data, not codegen)"
  rm -rf "$GEN"; mkdir -p "$GEN"
  python3 - "$HOSTBUILD" "$GEN" <<'PY'
import hashlib, os, re, shutil, sys

build, gen = sys.argv[1], sys.argv[2]
skip_rules = {"c_COMPILER", "cpp_COMPILER", "STATIC_LINKER", "cpp_LINKER",
              "c_LINKER", "SHSYM", "phony", "PHONY"}
outs = []
for line in open(os.path.join(build, "build.ninja")):
    m = re.match(r"^build (.+?): (\S+)(?: .*)?$", line)
    if not m:
        continue
    targets, rule = m.group(1), m.group(2)
    if rule in skip_rules:
        continue
    for t in targets.split():
        if t.endswith((".c", ".h", ".hpp", ".cpp")) and not t.startswith(".."):
            outs.append(t)

manifest = []
missing = []
for rel in sorted(set(outs)):
    src = os.path.join(build, rel)
    if not os.path.exists(src):
        missing.append(rel)      # a target we do not build (other drivers)
        continue
    dst = os.path.join(gen, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    h = hashlib.sha256(open(src, "rb").read()).hexdigest()
    manifest.append(f"{h}  {rel}")

open(os.path.join(gen, "MANIFEST.sha256"), "w").write("\n".join(manifest) + "\n")
print(f"    {len(manifest)} files exported, {len(missing)} declared-but-unbuilt")
for m in missing:
    print(f"      skipped: {m}")
PY

  # Stage the real X11/GL *interface* headers.  HobbyOS's native Xlib shim
  # (src/user/x11/include) is a different ABI and lacks Visual/XImage; the
  # compat class's libX11.so.6 (lane L4) implements the real client ABI, so
  # Mesa must be compiled against the real declarations.  Interface headers
  # only - nothing is linked from the host.
  say "   staging X11/GL headers (libx11-dev, build data only)"
  rm -rf "${STAGE}/x11-include"; mkdir -p "${STAGE}/x11-include"
  for d in /usr/include/X11 /usr/include/GL; do
    [ -d "$d" ] || die "missing $d (apt install libx11-dev)"
    cp -a "$d" "${STAGE}/x11-include/"
  done
  note "staged $(find "${STAGE}/x11-include" -type f | wc -l) headers"
}

# ---------------------------------------------------------------------------
# 4. Cross-compile every TU that goes into libGL
# ---------------------------------------------------------------------------
cross_compile() {
  say "4/6 cross-compile (abi=${ABI})"
  # The per-TU cache keys on object mtime vs log mtime, which cannot see a
  # change of ABI (same source, same flags except the header set) or a change
  # of the glibc header pin.  Switching either must not reuse objects: that is
  # a silent mixed-ABI DSO (measured: hobbyos-ABI objects under a glibc link
  # segfault at the first mtx_init; host-glibc objects name __isoc23_strtol).
  local stamp="$ABI"
  [ "$ABI" = glibc ] && stamp="$ABI:${GLIBC231_DEV:-host}:${GLIBC_VERSION}:r${CROSS_FLAGS_REV}"
  if [ "$(cat "${WORK}/.abi" 2>/dev/null)" != "$stamp" ]; then
    note "ABI/header pin changed to ${stamp}: clearing the object cache"
    rm -rf "$OBJ" "${WORK}/log"
    echo "$stamp" > "${WORK}/.abi"
  fi
  mkdir -p "$OBJ" "${WORK}/log"

  if [ ! -d "${LIBCXX_SRC}/libcxx/include" ]; then
    note "libc++ source tree missing; running third_party/libcxx-21.1.8/fetch.sh"
    "${TREE}/third_party/libcxx-21.1.8/fetch.sh"
  fi
  local libcxx_inc=${LIBCXX_SRC}/libcxx/include

  CC="$CC" CXX_DRIVER="$CXX_DRIVER" LD="$LD" AR="$AR" \
  CLANG_RES="$CLANG_RES" JOBS="$JOBS" \
  MESA_BUILD="$HOSTBUILD" MESA_SRC="$SRC" TREE="$TREE" ABI="$ABI" \
  OBJ="$OBJ" LOGDIR="${WORK}/log" LIBCXX_INC="$libcxx_inc" \
  STAGE_INC="${STAGE}/x11-include" GLIBC_INC="${GLIBC_INC:-}" \
  python3 - <<'PY'
import concurrent.futures, glob, json, os, re, shlex, subprocess, sys

E = os.environ
BUILD, SRC, TREE = E["MESA_BUILD"], E["MESA_SRC"], E["TREE"]
OBJ, LOGDIR, STAGE_INC, LIBCXX_INC = E["OBJ"], E["LOGDIR"], E["STAGE_INC"], E["LIBCXX_INC"]
CC, CXX_DRIVER, AR, CLANG_RES = E["CC"], E["CXX_DRIVER"], E["AR"], E["CLANG_RES"]
JOBS = int(E["JOBS"])
ABI = E.get("ABI", "glibc")

# The archives that make up libGL, in Meson's link order (the two
# link_whole targets are marked below).  Kept in sync with
# src/gallium/targets/libgl-xlib/meson.build.
ARCHIVES = [
    "src/c11/impl/libmesa_util_c11.a",
    "src/compiler/glsl/glcpp/libglcpp.a",
    "src/compiler/glsl/libglsl.a",
    "src/compiler/libcompiler.a",
    "src/compiler/nir/libnir.a",
    "src/compiler/spirv/libvtn.a",
    "src/gallium/auxiliary/libgallium.a",
    "src/gallium/auxiliary/libgalliumvl_stub.a",
    "src/gallium/drivers/softpipe/libsoftpipe.a",
    "src/gallium/frontends/glx/xlib/libxlib.a",
    "src/gallium/winsys/sw/xlib/libws_xlib.a",
    "src/mesa/glapi/glapi/libglapi_bridge.a",
    "src/mesa/glapi/shared-glapi/libglapi.a",
    "src/mesa/libmesa.a",
    "src/mesa/libmesa_sse41.a",
    "src/util/blake3/libblake3.a",
    "src/util/libmesa_util.a",
    "src/util/libmesa_util_clflush.a",
    "src/util/libmesa_util_clflushopt.a",
    "src/util/libmesa_util_simd.a",
]
LINK_WHOLE = {"src/gallium/frontends/glx/xlib/libxlib.a",
              "src/mesa/glapi/glapi/libglapi_bridge.a"}


def ninja_inputs(target):
    out = subprocess.check_output(["ninja", "-C", BUILD, "-t", "query", target],
                                  text=True)
    objs, in_inputs = [], False
    for line in out.splitlines()[1:]:
        s = line.strip()
        if s.startswith("input:"):
            in_inputs = True
            s = s[len("input:"):].strip()
            if not s:
                continue
        if s.startswith("outputs:"):
            in_inputs = False
            continue
        if in_inputs and s.endswith(".o"):
            objs.append(s)
    return objs


def parse_build_ninja():
    table, txt = {}, open(os.path.join(BUILD, "build.ninja")).read().splitlines()
    for i, line in enumerate(txt):
        m = re.match(r"^build (\S+): (c_COMPILER|cpp_COMPILER) (\S+?)(?: \|\||$)", line)
        if not m:
            continue
        obj, kind, src = m.group(1), m.group(2), m.group(3)
        args = []
        for j in range(i + 1, min(i + 12, len(txt))):
            if txt[j].startswith("build "):
                break
            m2 = re.match(r"^\s*ARGS = (.*)$", txt[j])
            if m2:
                args = shlex.split(m2.group(1))
                break
        table[obj] = (src, kind, args)
    return table


# Host *include* paths: only the freestanding flavour must never see them; the
# glibc flavour wants them (drm.h lives in /usr/include/libdrm).
HOST_PATH_PREFIXES = ("-I/usr/", "-isystem/usr/", "-isystem/usr")
# Compiler-flag spellings that must go in both flavours: gcc-only or
# meson-noise flags clang rejects.
DROP_PREFIXES = ("-fdiagnostics-color", "-flifetime-dse", "-Wno-error=",
                 "-Winvalid-pch")
DROP_EXACT = {
    "-pthread", "-mtls-dialect=gnu2",
    # GCC-only builtins clang does not implement; Mesa's generated
    # nir_constant_expressions.c then takes its plain-C fallback.
    "-DHAVE___BUILTIN_ADD_OVERFLOW_P", "-DHAVE___BUILTIN_SUB_OVERFLOW_P",
    # Not in HobbyOS.  compress.c/crc32.c test them with #ifdef, so the flag
    # must be absent (not =0).  This also drops the host build's libz link.
    "-DHAVE_ZLIB", "-DHAVE_COMPRESSION", "-DHAVE_ZSTD",
}


def rewrite(args, kind):
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-I", "-isystem", "-idirafter"):
            skip = True
            continue
        if a in DROP_EXACT or a.startswith(DROP_PREFIXES):
            continue
        if ABI != "glibc" and a.startswith(HOST_PATH_PREFIXES):
            continue
        out.append(a)

    if ABI == "glibc":
        # The compat class is a glibc process: compile against *the pinned
        # glibc 2.31's* headers so every by-value libc type (pthread_mutex_t,
        # struct stat, FILE, ...) has the layout the runtime will use AND
        # every libc call names a symbol 2.31 actually exports.  Using the
        # host's headers here was measured to produce __isoc23_strtol /
        # fstat64, which the guest's 2.31 cannot resolve at dlopen.
        pre = shlex.split(E["GLIBC_INC"]) + [
               "--target=x86_64-linux-gnu", "-D__linux__=1",
               "-D_FILE_OFFSET_BITS=64", "-mno-red-zone",
               "-fno-stack-protector", "-fno-exceptions", "-fno-rtti",
               # Measured: this DSO is dlopen()ed, so its PT_TLS must fit in
               # glibc's 1664-byte static-TLS surplus (a DT_FLAGS STATIC_TLS
               # object that does not fit fails with "cannot allocate memory in
               # static TLS block").  Mesa's TLS is kept tiny by
               # 0003-debug-dump-flags-no-tls.patch; with that, the default
               # initial-exec model is both correct and fastest (no
               # __tls_get_addr per dispatch).  If a future change grows
               # PT_TLS past the surplus, rebuild with
               # -ftls-model=global-dynamic -DMESA_TLS_GLOBAL_DYNAMIC (see
               # 0002-tls-model-global-dynamic.patch) instead.
               "-Wno-error", "-g0"]
        if kind == "cpp_COMPILER":
            pre += ["-I", E["LIBCXX_INC"], "-nostdinc++", "--std=c++23"]
        return pre + out

    pre = [
        "--target=x86_64-none-elf", "-ffreestanding", "-nostdinc",
        # HobbyOS's compat class speaks the Linux ABI: Mesa's
        # src/util/detect_os.h keys DETECT_OS_LINUX/DETECT_OS_POSIX on
        # __linux__, and os_misc.c/os_time.c hard-error without it.
        "-D__linux__=1", "-D__unix__=1",
        "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE",
        "-mno-red-zone", "-fno-stack-protector", "-fno-exceptions", "-fno-rtti",
    ]
    if kind == "cpp_COMPILER":
        # libc++'s C-wrapper headers (<stdlib.h>, <stddef.h>, ...) must be
        # found before the C sysroot: they set _LIBCPP_* and include_next.
        pre += ["-I", LIBCXX_INC, "-nostdinc++", "--std=c++23"]
    pre += [
        "-isystem", CLANG_RES,
        "-I", os.path.join(TREE, "tools/mesa/include"),
        "-I", os.path.join(TREE, "src/include"),
        "-I", os.path.join(TREE, "src/libc/include"),
        "-I", os.path.join(TREE, "src/user_include"),
        "-Wno-error", "-g0",
    ]
    # Last: the real X11/GL interface headers, so Mesa's own include/GL wins.
    return pre + out + ["-I", STAGE_INC]


def main():
    objs = []
    for a in ARCHIVES:
        objs += ninja_inputs(a)
    # The shared-library target's own frontend object is named directly on
    # Meson's link line (it defines xlib_create_screen and is in no archive).
    objs.append("src/gallium/targets/libgl-xlib/libGL.so.1.5.0.p/xlib.c.o")
    objs = sorted(set(objs))
    table = parse_build_ninja()

    missing = [o for o in objs if o not in table]
    if missing:
        print("!! objects with no compile command:", missing[:5])
        sys.exit(1)

    os.makedirs(os.path.join(OBJ, "ar"), exist_ok=True)
    os.makedirs(LOGDIR, exist_ok=True)
    results = {}

    def compile_one(obj):
        src, kind, args = table[obj]
        flat = obj.replace("/", "_")
        out = os.path.join(OBJ, flat)
        log = os.path.join(LOGDIR, flat + ".log")
        # The cache must see a *source* newer than its object, not just a log
        # older than it: applying a tools/mesa/patches/*.patch rewrites the
        # source in place, and keying on (object, log) alone silently shipped
        # the pre-patch object (measured: 0004 added a `[GLX] context request`
        # string the DSO did not contain and the guest still got NULL).
        srcpath = src if os.path.isabs(src) else os.path.join(BUILD, src)
        src_ok = (not os.path.exists(srcpath) or
                  os.path.getmtime(out) >= os.path.getmtime(srcpath))
        if os.path.exists(out) and os.path.exists(log) and \
           os.path.getmtime(out) >= os.path.getmtime(log) and src_ok:
            return obj, "cached", ""
        argv = ([CC] + rewrite(args, kind) + ["-c", src, "-o", out])
        with open(log, "w") as lf:
            lf.write(" ".join(shlex.quote(x) for x in argv) + "\n\n")
            lf.flush()
            p = subprocess.run(argv, stdout=lf, stderr=lf, cwd=BUILD)
        if p.returncode == 0:
            return obj, "ok", ""
        lines = open(log).read().splitlines()
        first = ""
        for k, ln in enumerate(lines):
            if re.search(r"(fatal error|error):", ln):
                first = " | ".join(lines[k:k + 2])[:300]
                break
        return obj, "fail", first

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=JOBS) as ex:
        for obj, status, err in ex.map(compile_one, objs):
            results[obj] = {"status": status, "error": err}
            done += 1
            if done % 100 == 0 or done == len(objs):
                nfail = sum(1 for v in results.values() if v["status"] == "fail")
                print(f"    {done}/{len(objs)} compiled ({nfail} failed)", flush=True)

    ok = sum(1 for v in results.values() if v["status"] in ("ok", "cached"))
    fail = [o for o, v in results.items() if v["status"] == "fail"]
    json.dump({"total": len(objs), "ok": ok, "fail": len(fail), "results": results},
              open(os.path.join(OBJ, "tu-summary.json"), "w"), indent=1)

    print(f"    RESULT {ok}/{len(objs)} TUs compiled, {len(fail)} failed")
    if fail:
        with open(os.path.join(LOGDIR, "FAILED.txt"), "w") as f:
            for o in fail:
                f.write(f"{o}\n    {results[o]['error']}\n")
        for o in fail[:10]:
            print(f"      FAIL {o}\n           {results[o]['error']}")
        sys.exit(1)

    # Group the objects back into the archives Meson would have produced.
    groups = {}
    for obj in objs:
        flat = obj.replace("/", "_")          # src_mesa_libmesa.a.p_main_light.c.o
        out = os.path.join(OBJ, flat)
        for a in ARCHIVES:
            if flat.startswith(a.replace("/", "_") + ".p_"):
                groups.setdefault(a, []).append(out)
                break
    grouped = {p for lst in groups.values() for p in lst}
    else_objs = [os.path.join(OBJ, o.replace("/", "_")) for o in objs
                 if os.path.join(OBJ, o.replace("/", "_")) not in grouped]
    for a, lst in groups.items():
        path = os.path.join(OBJ, "ar", a.replace("/", "_"))
        if os.path.exists(path):
            os.remove(path)
        subprocess.run([AR, "rcs", path] + sorted(lst), check=True)
    with open(os.path.join(OBJ, "ar", "ORDER.txt"), "w") as f:
        for a in ARCHIVES:
            f.write(os.path.join(OBJ, "ar", a.replace("/", "_")) + "\n")
    with open(os.path.join(OBJ, "ar", "EXTRA.txt"), "w") as f:
        f.write("\n".join(else_objs) + "\n")
    with open(os.path.join(OBJ, "ar", "ALL.txt"), "w") as f:
        f.write("\n".join(sorted(os.path.join(OBJ, o.replace("/", "_")) for o in objs)) + "\n")
    print(f"    {len(groups)} archives + {len(else_objs)} standalone object(s)")


main()
PY
}

# ---------------------------------------------------------------------------
# 5. C++ runtime (the GLSL compiler is C++; glibc has no C++ runtime)
# ---------------------------------------------------------------------------
build_cxx_runtime() {
  say "5/6 C++ runtime for libGL (libc++ 21.1.8 rebuilt -fPIC)"
  local out=${WORK}/cxxrt
  mkdir -p "$out"
  local libcxx=${LIBCXX_SRC}/libcxx
  local abi=${LIBCXX_SRC}/libcxxabi

  # The GLSL/NIR compilers use std::string, std::mutex, std::error_code.  The
  # tree's libcxx.a is built -mcmodel=large and *not* PIC, so it cannot go
  # into a shared object (R_X86_64_64 against local symbols - measured).  It
  # is also a freestanding -fno-exceptions build, which is what Mesa wants,
  # so only the TUs that actually define referenced symbols are rebuilt here.
  local tus="
libcxx/src/new.cpp
libcxx/src/mutex.cpp
libcxx/src/mutex_destructor.cpp
libcxx/src/condition_variable.cpp
libcxx/src/condition_variable_destructor.cpp
libcxx/src/verbose_abort.cpp
libcxx/src/hash.cpp
libcxx/src/functional.cpp
libcxx/src/memory.cpp
libcxx/src/string.cpp
libcxx/src/error_category.cpp
libcxx/src/thread.cpp
libcxx/src/system_error.cpp
libcxx/src/stdexcept.cpp
libcxx/src/exception.cpp
libcxx/src/future.cpp
libcxxabi/src/cxa_guard.cpp
libcxxabi/src/cxa_virtual.cpp
libcxxabi/src/abort_message.cpp
libcxxabi/src/stdlib_stdexcept.cpp
"

  local f n rc=0 cxxobjs=()
  local abi_flags sysinc
  if [ "$ABI" = glibc ]; then
    # -D_GNU_SOURCE: glibc's <pthread.h> hides pthread_cond_clockwait behind it,
    # and libc++ 21 uses that for condition_variable::wait_until.
    abi_flags="--target=x86_64-linux-gnu -D_GNU_SOURCE"
    sysinc="${GLIBC_INC}"
  else
    abi_flags="--target=x86_64-none-elf -ffreestanding -nostdinc -isystem ${CLANG_RES}"
    sysinc="-I ${TREE}/src/libc/include -I ${TREE}/src/include -I ${TREE}/src/user_include"
  fi
  for f in $tus; do
    n=$(basename "$f" .cpp)
    cxxobjs+=("${out}/${n}.o")
    # shellcheck disable=SC2086
    ${CXX_DRIVER} --driver-mode=g++ $abi_flags \
      -I "${libcxx}/include" -I "${libcxx}/src" -I "${libcxx}/src/include" \
      -I "${abi}/include" $sysinc \
      -nostdinc++ -std=c++23 -fno-exceptions -fno-rtti -fPIC -O2 \
      -ftls-model=global-dynamic \
      -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES \
      -DLIBCXX_BUILDING_LIBCXXABI \
      -c "${LIBCXX_SRC}/${f}" -o "${out}/${n}.o" \
      > "${out}/${n}.log" 2>&1 || { rc=1; echo "    FAIL $f"; grep -m3 'error:' "${out}/${n}.log" || true; }
  done
  [ "$rc" = 0 ] || die "C++ runtime build failed"

  # The two hand-written pieces: libc gaps + the C++ ABI glue.
  local inc_flags
  if [ "$ABI" = glibc ]; then
    inc_flags="${GLIBC_INC}"   # 2.31's headers carry the correct by-value layouts
  else
    inc_flags="-isystem ${CLANG_RES} -I ${TREE}/tools/mesa/include -I ${TREE}/src/include -I ${TREE}/src/libc/include -I ${TREE}/src/user_include"
  fi
  # shellcheck disable=SC2086
  ${CC} $([ "$ABI" = glibc ] && echo "--target=x86_64-linux-gnu" || echo "--target=x86_64-none-elf -ffreestanding -nostdinc") $inc_flags \
    -mno-red-zone -fno-stack-protector -fPIC -std=c11 -O2 -Wall \
    -ftls-model=global-dynamic \
    -DHB_MESA_SELF_CONTAINED=${SELF_CONTAINED} \
    -c "${TREE}/src/user/xcompat/gl_stubs.c" -o "${out}/gl_stubs.o"
  # libc++'s include dir must come FIRST (its C-wrapper headers include_next
  # into the HobbyOS sysroot); hence a separate flag order from $inc_flags.
  # shellcheck disable=SC2086
  ${CXX_DRIVER} --driver-mode=g++ $([ "$ABI" = glibc ] && echo "--target=x86_64-linux-gnu" || echo "--target=x86_64-none-elf -ffreestanding -nostdinc") \
    -I "${libcxx}/include" -I "${libcxx}/src" -I "${abi}/include" \
    $inc_flags -nostdinc++ -std=c++23 -fno-exceptions -fno-rtti \
    -fPIC -O2 -D_LIBCPP_BUILDING_LIBRARY \
    -c "${TREE}/src/user/xcompat/gl_cxx_glue.cpp" -o "${out}/gl_cxx_glue.o"

  rm -f "${out}/libcxx-pic.a"
  ${AR} rcs "${out}/libcxx-pic.a" "${cxxobjs[@]}"
  note "${#cxxobjs[@]} libc++ TUs -> cxxrt/libcxx-pic.a (+ gl_stubs.o, gl_cxx_glue.o)"
}

# ---------------------------------------------------------------------------
# 6. Link + verify
# ---------------------------------------------------------------------------
link_libgl() {
  say "6/6 link libGL.so.1 / libGLX.so.0 / libGL.a"
  mkdir -p "$LIBDIR"
  local verscript=${SRC}/src/gallium/targets/libgl-xlib/libgl-xlib.sym
  local whole=() rest=() extra=()
  mapfile -t rest < "${OBJ}/ar/ORDER.txt"
  mapfile -t extra < "${OBJ}/ar/EXTRA.txt"

  # link_whole first (Meson: link_whole: [libxlib, libglapi_bridge]).
  local a
  for a in "${rest[@]}"; do
    case "$(basename "$a")" in
      src_gallium_frontends_glx_xlib_libxlib.a|src_mesa_glapi_glapi_libglapi_bridge.a)
        whole+=("$a") ;;
      *) ;;
    esac
  done
  local notwhole=()
  for a in "${rest[@]}"; do
    case "$(basename "$a")" in
      src_gallium_frontends_glx_xlib_libxlib.a|src_mesa_glapi_glapi_libglapi_bridge.a) ;;
      *) notwhole+=("$a") ;;
    esac
  done

  local cxx_extra=("${WORK}/cxxrt/libcxx-pic.a")
  local extra_libs=()
  if [ "$SELF_CONTAINED" = 1 ]; then
    # HobbyOS libc, rebuilt -fPIC (obj/intel/libc.a is -mcmodel=large).
    local piclibc=${WORK}/libc-pic.a
    if [ ! -f "$piclibc" ]; then
      note "rebuilding HobbyOS libc with -fPIC for the self-contained DSO"
      # regex_internal.c / regcomp.c / regexec.c are #included by regex.c,
      # not translation units of their own (the tree's Makefile archives only
      # libc_regex.o); compiling them standalone fails on types that regex.c
      # defines first.
      local cfiles; cfiles=$(ls "${TREE}"/src/libc/src/*.c | grep -vE '/(regex_internal|regcomp|regexec)\.c$')
      local pdir=${WORK}/libc-pic; mkdir -p "$pdir"
      # Mirror the Makefile's USER_CFLAGS include order exactly: src/include
      # carries kernel-flavoured headers (regex.h, ...) that must NOT shadow
      # the libc ones, so src/user_include comes first as it does there.
      local libc_cflags="--target=x86_64-none-elf -ffreestanding -nostdinc
        -isystem ${CLANG_RES} -I ${TREE}/src/user_include
        -I ${TREE}/src/user_include/graphics -I ${TREE}/src/include
        -I ${TREE}/src/libc/include -mno-red-zone -fno-stack-protector
        -fPIC -O2 -w"
      for f in $cfiles; do
        # shellcheck disable=SC2086
        ${CC} $libc_cflags -c "$f" -o "${pdir}/$(basename "$f" .c).o" \
          || note "libc-pic: $(basename "$f") did not rebuild (skipped)"
      done
      # The public heap (malloc/calloc/realloc/free/aligned_alloc) lives in
      # src/user/malloc.c, and the syscall layer in src/user/libc.c - both are
      # separate objects in every HobbyOS link flavour, so both are needed.
      for f in "${TREE}/src/user/libc.c" "${TREE}/src/user/malloc.c"; do
        # shellcheck disable=SC2086
        ${CC} $libc_cflags -c "$f" -o "${pdir}/user_$(basename "$f" .c).o" \
          || note "libc-pic: $f did not rebuild (skipped)"
      done
      rm -f "$piclibc"; ${AR} rcs "$piclibc" "${pdir}"/*.o
    fi
    extra_libs=("$piclibc")
  fi

  local common=(-shared -soname libGL.so.1 --version-script "$verscript"
                -z noexecstack -z nodelete --build-id)
  # -z nodelete is load-bearing, not cosmetic.  Mesa calls plain atexit() from
  # inside the DSO (src/mesa/main/formats.c:499 registers
  # format_array_format_table_destroy; util/os_misc.c, main/extensions.c,
  # main/context.c and util/strtod.c do the same).  glibc's atexit() is
  # process-global, so those handlers are NOT scoped to this DSO and are NOT
  # run by dlclose().  If dlclose() unmaps the segments, the process's later
  # exit() jumps into what is now a hole: measured in the guest as
  #   memory fault VA=0x0000001081ACFF50 PC=0x0000001081ACFF50 rw=rx
  #   in=HOLE region=NONE
  # where libGL's base + 0xe61f50 is exactly format_array_format_table_destroy.
  # DF_1_NODELETE makes dlclose drop the reference without unmapping, which is
  # also how libglvnd loads a vendor GL driver (RTLD_NODELETE).
  # Self-contained: gl_stubs.c and libc.a overlap on the handful of float
  # functions libc.a *does* have (expf/sqrtf/fabsf/rintf), on atexit and on
  # _assert_fail.  The archive comes first, so libc's definitions win and the
  # stub is only the fallback - exactly what we want.  lld needs permission.
  [ "$SELF_CONTAINED" = 1 ] && common+=(--allow-multiple-definition)
  ${LD} "${common[@]}" -o "${LIBDIR}/libGL.so.1" \
    --whole-archive "${whole[@]}" --no-whole-archive \
    --start-group "${extra[@]}" "${notwhole[@]}" "${extra_libs[@]}" --end-group \
    "${cxx_extra[@]}" "${WORK}/cxxrt/gl_stubs.o" "${WORK}/cxxrt/gl_cxx_glue.o"

  ln -sf libGL.so.1 "${LIBDIR}/libGLX.so.0"
  ln -sf libGL.so.1 "${LIBDIR}/libGL.so"       # so `-lGL` finds the shared one
  # The static archive is for in-guest probes: flatten libcxx-pic.a into it
  # (`ar` would otherwise store the nested archive as one opaque member).
  { echo "create ${LIBDIR}/libGL.a"
    for o in $(cat "${OBJ}/ar/ALL.txt") \
               "${WORK}/cxxrt/gl_stubs.o" "${WORK}/cxxrt/gl_cxx_glue.o"; do
      echo "addmod $o"
    done
    echo "addlib ${WORK}/cxxrt/libcxx-pic.a"
    echo save
    echo end
  } | ${AR} -M

  ${STRIP} --strip-unneeded "${LIBDIR}/libGL.so.1" -o "${LIBDIR}/libGL.so.1.stripped"
  mv "${LIBDIR}/libGL.so.1.stripped" "${LIBDIR}/libGL.so.1"
  ln -sf libGL.so.1 "${LIBDIR}/libGL.so.1.5.0"

  # ---- verification (fails the build if a claim stops being true) ----------
  local needed exports undef
  # Dump the symbol tables once: the checks below read files (a `nm | grep -q`
  # pipeline would trip `set -o pipefail` on SIGPIPE).
  nm -D --defined-only "${LIBDIR}/libGL.so.1" > "${LIBDIR}/exports.txt"
  nm -D -u "${LIBDIR}/libGL.so.1" > "${LIBDIR}/undefined.txt"
  needed=$(readelf -d "${LIBDIR}/libGL.so.1" | grep -c NEEDED || true)
  exports=$(awk '$2 ~ /^[TWiDB]$/' "${LIBDIR}/exports.txt" | wc -l)
  undef=$(wc -l < "${LIBDIR}/undefined.txt")
  note "DT_NEEDED:   ${needed}  (must be 0: the compat loader resolves libc from its own scope)"
  note "exports:     ${exports}"
  note "undefined:   ${undef} (all from glibc/libX11/libgcc_s/ld.so - see the report)"
  note "size:        $(stat -c%s "${LIBDIR}/libGL.so.1") bytes stripped"
  [ "$needed" = 0 ] || die "libGL.so.1 grew a DT_NEEDED"
  # A dlopen()ed DSO that asks for static TLS must fit glibc's 1664-byte
  # surplus (measured: 4400 bytes -> "cannot allocate memory in static TLS
  # block"; dynamic TLS for that block then hung in the guest's first dispatch
  # call).  So: either no STATIC_TLS at all, or a PT_TLS under the surplus.
  local tls_bytes tls_flags
  tls_flags=$(readelf -d "${LIBDIR}/libGL.so.1" | grep -c STATIC_TLS || true)
  tls_bytes=$(readelf -lW "${LIBDIR}/libGL.so.1" | awk '/^  TLS/{print $6}')
  tls_bytes=$(( ${tls_bytes:-0} ))     # hex from readelf; bash arithmetic is fine
  if [ "${tls_flags:-0}" != 0 ]; then
    [ "${tls_bytes:-999999}" -le 1664 ] \
      || die "libGL.so.1 needs ${tls_bytes} bytes of static TLS (glibc's surplus is 1664): shrink PT_TLS or compile with -ftls-model=global-dynamic"
    note "TLS:         ${tls_bytes} bytes, static (initial-exec, fits the 1664-byte surplus)"
  else
    note "TLS:         ${tls_bytes} bytes, dynamic (no STATIC_TLS)"
  fi
  grep -q " glXGetProcAddress$" "${LIBDIR}/exports.txt" \
    || die "glXGetProcAddress is not exported"
  grep -q " glGetString$" "${LIBDIR}/exports.txt" \
    || die "glGetString is not exported"
  grep -q " glXSwapBuffers$" "${LIBDIR}/exports.txt" \
    || die "glXSwapBuffers is not exported"
  # A DSO that registers process-global atexit() handlers must not be unloadable
  # (see the -z nodelete note in link_libgl): without DF_1_NODELETE a client's
  # dlclose() unmaps the text, and its exit() then jumps into the hole.
  local nodelete
  nodelete=$(readelf -d "${LIBDIR}/libGL.so.1" | grep -c NODELETE || true)
  [ "${nodelete:-0}" != 0 ] \
    || die "libGL.so.1 lost DF_1_NODELETE: a client that dlclose()s it will crash in exit()"
  note "df_1:        NODELETE (dlclose keeps the DSO mapped; its atexit() handlers stay valid)"
  [ -L "${LIBDIR}/libGLX.so.0" ] || die "libGLX.so.0 missing"

  # ---- the loadability audit ----------------------------------------------
  # libGL has no DT_NEEDED: every undefined symbol must already be in the
  # compat process.  That set is the pinned glibc 2.31 (+ libgcc_s, ld.so) and
  # obj/xcompat/libX11.so.6.  A symbol outside it is a DSO that links on this
  # host and dies at dlopen() in the guest with "undefined symbol" - the exact
  # failure this audit exists to make impossible to ship.
  if [ "$ABI" = glibc ] && [ -d "${GLIBC231_RT:-}" ]; then
    local xcompat_libx11=${TREE}/obj/xcompat/libX11.so.6
    python3 - "${LIBDIR}/undefined.txt" "$GLIBC231_RT" "$xcompat_libx11" <<'PY' \
      || die "libGL has undefined symbols the compat process cannot provide"
import glob, os, subprocess, sys

undef_file, rt_dir, xcompat = sys.argv[1], sys.argv[2], sys.argv[3]

def defined(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path],
                         capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in "TWiDBRVSu":
            syms.add(p[2].split("@")[0])
    return syms

prov = set()
for dso in glob.glob(os.path.join(rt_dir, "lib/x86_64-linux-gnu/*.so*")):
    if os.path.islink(dso) or not os.path.isfile(dso):
        continue
    prov |= defined(dso)

xprov = defined(xcompat) if os.path.exists(xcompat) else set()
xnames = set()
missing, x_missing = [], []
for line in open(undef_file):
    p = line.split()
    if len(p) < 2:
        continue
    weak = p[-2] in ("w", "v")
    name = p[-1].split("@")[0]
    if weak or name in prov:
        continue
    if name in xprov or name[:1] == "X" or name.startswith("_X"):
        xnames.add(name)
        if xprov and name not in xprov:
            x_missing.append(name)
        continue
    missing.append(name)

print(f"    audit: {len(prov)} glibc-2.31 symbols; "
      f"{len(xnames)} from libX11; {len(missing)} unaccounted")
if missing:
    print("    UNACCOUNTED (would fail dlopen in the guest):")
    for m in sorted(missing):
        print("      " + m)
if x_missing:
    print("    libX11.so.6 does NOT export (libGL needs them):")
    for m in sorted(x_missing):
        print("      " + m)
if missing or x_missing:
    sys.exit(1)
PY
  fi

  echo
  note "artifacts: ${LIBDIR}/{libGL.so.1,libGL.so.1.5.0,libGLX.so.0,libGL.a}"
  note "guest staging: copy libGL.so.1 + the libGLX.so.0 symlink to /LIB64"
  note "guest env: XLIB_NO_SHM=1 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe"
}

# ---------------------------------------------------------------------------
main() {
  mkdir -p "$WORK"
  local t0; t0=$(date +%s)
  preflight
  fetch_mesa
  ensure_glibc_pin
  host_build
  export_generated
  [ "$GEN_ONLY" = 1 ] && { say "gen-only: done ($(( $(date +%s) - t0 )) s)"; return 0; }
  cross_compile
  build_cxx_runtime
  link_libgl
  say "done in $(( $(date +%s) - t0 )) s"
  if [ "$KEEP_BUILD" = 0 ]; then
    note "host build kept at ${HOSTBUILD} (delete it to reclaim ~1.5 GB; generated data is in ${GEN})"
  fi
}

main "$@"
