#!/usr/bin/env bash
# third_party/x11/build.sh -- cross-build the HobbyOS X11 / IceWM stack.
#
#   bash third_party/x11/build.sh --arch arm|intel [--prefix DIR]
#        [--stage clientlibs|extlibs|server|icewm|clients|all] [-j N]
#        [--only PKG[,PKG...]] [--force]
#
# Frozen layout (icewm.md §4.2):
#   prefix  default third_party/x11/build/<arch>/prefix (gitignored); lanes
#           share ~/hobbyos-icewm-lanes/prefix/<arch> via --prefix.
#   bins    third_party/x11/build/<arch>/bin/   (flat .BIN, 8.3 names, §4.3)
#   disk    third_party/x11/build/<arch>/disk/  (on-disk data tree, §4.4)
#   (X11_BUILD_DIR=<dir> relocates build/<arch>/ — bin/, disk/, src/, obj/.)
#
# Stages live in sourced files build-<stage>.sh next to this script; each
# defines `stage_<stage>` (one file per owning lane keeps merges trivial):
#   clientlibs  build-clientlibs.sh  (lane xlibs-core)
#   extlibs     build-extlibs.sh     (lane xlibs-ext)
#   server      build-server.sh      (lane xserver)
#   icewm       build-icewm.sh       (lane icewm)
#   clients     build-clients.sh     (lane session)
#   all         every stage above whose file exists, in that order.
#
# Helpers available to stage files (see below): hb_log, hb_die, hb_src
# (fresh extract + patches), hb_done/hb_mark (stamps in the prefix),
# hb_autotools, hb_meson, hb_cmake, hb_link_flat, hb_need_libcxx, hb_pkg_lock.
# Packages are cached by stamp in <prefix>/.stamps/<pkg> (tarball sha256 +
# patch-set hash + recipe version); --force or --only rebuilds.
set -euo pipefail

HB_X11_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH=""; PREFIX=""; STAGE="clientlibs"; JOBS="${JOBS:-8}"; ONLY=""; FORCE=0
usage() { sed -n '2,8p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    --prefix) PREFIX="$2"; shift 2;;
    --stage) STAGE="$2"; shift 2;;
    -j) JOBS="$2"; shift 2;;
    -j*) JOBS="${1#-j}"; shift;;
    --only) ONLY="$2"; shift 2;;
    --force) FORCE=1; shift;;
    -h|--help) usage;;
    *) echo "build.sh: unknown argument $1" >&2; usage;;
  esac
done
case "$ARCH" in arm|intel) ;; *) echo "build.sh: --arch arm|intel required" >&2; usage;; esac

HB_BUILD="${X11_BUILD_DIR:-$HB_X11_DIR/build/$ARCH}"
mkdir -p "$HB_BUILD"
HB_BUILD="$(cd "$HB_BUILD" && pwd)"
[ -n "$PREFIX" ] || PREFIX="$HB_BUILD/prefix"
# shellcheck source=cross/env.sh
. "$HB_X11_DIR/cross/env.sh" "$ARCH" "$PREFIX" "$HB_BUILD/tools"
HB_SRC_DIR="$HB_BUILD/src"; HB_OBJ_DIR="$HB_BUILD/obj"
HB_BIN_DIR="$HB_BUILD/bin"; HB_DISK_DIR="$HB_BUILD/disk"
HB_LOG_DIR="$HB_BUILD/logs"; HB_DL="${X11_DL_DIR:-$HB_X11_DIR/dl}"
HB_JOBS="$JOBS"; HB_FORCE="$FORCE"; HB_ONLY="$ONLY"
mkdir -p "$HB_SRC_DIR" "$HB_OBJ_DIR" "$HB_BIN_DIR" "$HB_DISK_DIR" "$HB_LOG_DIR" "$HB_PREFIX/.stamps"
export HB_BUILD HB_SRC_DIR HB_OBJ_DIR HB_BIN_DIR HB_DISK_DIR HB_LOG_DIR HB_DL HB_JOBS

# Toolchain identity folded into every package key: the cross/ tree (flags,
# overlays, transitional compat) and the arch.  Changing any of it rebuilds.
HB_ENV_KEY="$(cd "$HB_X11_DIR/cross" && find . -type f | LC_ALL=C sort | xargs cat | sha256sum | cut -c1-16) $HB_ARCH $(clang --version | head -1)"

hb_log() { printf '[x11:%s] %s\n' "$HB_ARCH" "$*"; }
hb_die() { printf '[x11:%s] ERROR: %s\n' "$HB_ARCH" "$*" >&2; exit 1; }

# hb_want <pkg>: honour --only.
hb_want() { [ -z "$HB_ONLY" ] && return 0; case ",$HB_ONLY," in *",$1,"*) return 0;; esac; return 1; }

# hb_key <pkg> <tarball> [recipe-salt]: cache key = tarball sha + patches + salt.
hb_key() {
  local pkg="$1" tb="$2" salt="${3:-}" ph=""
  if [ -d "$HB_X11_DIR/patches/$pkg" ]; then
    ph="$(cat "$HB_X11_DIR/patches/$pkg"/*.patch 2>/dev/null | sha256sum | cut -c1-16)"
  fi
  printf '%s %s %s %s\n' "$(cut -d' ' -f1 "$HB_X11_DIR/sums/$tb.sha256")" "$ph" "$salt" "$HB_ENV_KEY" | sha256sum | cut -c1-32
}
# hb_done <pkg> <key>: 0 when the prefix already holds this exact build.
hb_done() {
  [ "$HB_FORCE" = 1 ] && return 1
  [ -n "$HB_ONLY" ] && hb_want "$1" && return 1
  [ -f "$HB_PREFIX/.stamps/$1" ] && [ "$(cat "$HB_PREFIX/.stamps/$1")" = "$2" ]
}
hb_mark() { printf '%s\n' "$2" > "$HB_PREFIX/.stamps/$1"; }

# hb_pkg_lock <pkg>: serialise installs of one package into a shared prefix
# across concurrently building lanes (released when the subshell exits).
hb_pkg_lock() { exec 9>"$HB_PREFIX/.stamps/.$1.lock"; flock 9; }

# hb_src <pkg> <tarball>: fresh extraction into $HB_SRC_DIR/<pkg> (the
# tarball's top dir stripped) + every patches/<pkg>/*.patch in name order
# (patch -p1).  Echoes the source dir.
hb_src() {
  local pkg="$1" tb="$2" d="$HB_SRC_DIR/$1"
  [ -f "$HB_DL/$tb" ] || hb_die "missing $HB_DL/$tb (run fetch.sh)"
  rm -rf "$d"; mkdir -p "$d"
  tar -C "$d" --strip-components=1 -xf "$HB_DL/$tb"
  if [ -d "$HB_X11_DIR/patches/$pkg" ]; then
    local p
    for p in "$HB_X11_DIR/patches/$pkg"/*.patch; do
      [ -f "$p" ] || continue
      patch -d "$d" -p1 -s --no-backup-if-mismatch < "$p" >&2 || hb_die "patch $p failed"
    done
  fi
  printf '%s\n' "$d"
}

# hb_run <logname> <cmd...>: run quietly into logs/<logname>.log; on failure
# print the tail and die.
hb_run() {
  local name="$1"; shift
  if ! "$@" >>"$HB_LOG_DIR/$name.log" 2>&1; then
    tail -40 "$HB_LOG_DIR/$name.log" >&2
    hb_die "$name failed: $* (log: $HB_LOG_DIR/$name.log)"
  fi
}

# hb_autotools <pkg> <tarball> [--salt S] [--make "targets"] [--install "targets"]
#              [--pre FUNC] -- [configure args...]
# Out-of-tree configure in $HB_OBJ_DIR/<pkg> with $HB_CONFIGURE_ARGS.
# --pre FUNC runs in the source dir after patching (e.g. cache tweaks).
hb_autotools() {
  local pkg="$1" tb="$2"; shift 2
  local salt="" mk="" inst="install" pre=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --salt) salt="$2"; shift 2;;
      --make) mk="$2"; shift 2;;
      --install) inst="$2"; shift 2;;
      --pre) pre="$2"; shift 2;;
      --) shift; break;;
      *) break;;
    esac
  done
  hb_want "$pkg" || { hb_log "$pkg: skipped (--only)"; return 0; }
  local key; key="$(hb_key "$pkg" "$tb" "$salt $*")"
  if hb_done "$pkg" "$key"; then hb_log "$pkg: cached"; return 0; fi
  (
    hb_pkg_lock "$pkg"
    if hb_done "$pkg" "$key"; then hb_log "$pkg: cached (built by another lane)"; exit 0; fi
    hb_log "$pkg: building"
    : > "$HB_LOG_DIR/$pkg.log"
    local src b; src="$(hb_src "$pkg" "$tb")"; b="$HB_OBJ_DIR/$pkg"
    rm -rf "$b"; mkdir -p "$b"
    if [ -n "$pre" ]; then (cd "$src" && "$pre") >>"$HB_LOG_DIR/$pkg.log" 2>&1 || hb_die "$pkg: $pre failed"; fi
    # shellcheck disable=SC2086
    (cd "$b" && hb_run "$pkg" "$src/configure" $HB_CONFIGURE_ARGS "$@")
    # shellcheck disable=SC2086
    (cd "$b" && hb_run "$pkg" make -j"$HB_JOBS" $mk)
    # shellcheck disable=SC2086
    (cd "$b" && hb_run "$pkg" make $inst)
    find "$HB_PREFIX/lib" -maxdepth 1 -name '*.la' -delete 2>/dev/null || true
    hb_mark "$pkg" "$key"
    hb_log "$pkg: ok"
  )
}

# hb_meson <pkg> <tarball> [--salt S] -- [meson -D options...]
hb_meson() {
  local pkg="$1" tb="$2"; shift 2; local salt=""
  [ "${1:-}" = --salt ] && { salt="$2"; shift 2; }
  [ "${1:-}" = -- ] && shift
  hb_want "$pkg" || { hb_log "$pkg: skipped (--only)"; return 0; }
  local key; key="$(hb_key "$pkg" "$tb" "$salt $*")"
  if hb_done "$pkg" "$key"; then hb_log "$pkg: cached"; return 0; fi
  (
    hb_pkg_lock "$pkg"
    if hb_done "$pkg" "$key"; then hb_log "$pkg: cached (built by another lane)"; exit 0; fi
    hb_log "$pkg: building (meson)"
    : > "$HB_LOG_DIR/$pkg.log"
    local src b; src="$(hb_src "$pkg" "$tb")"; b="$HB_OBJ_DIR/$pkg"
    rm -rf "$b"
    hb_run "$pkg" meson setup "$b" "$src" \
      --cross-file "$HB_TOOLS/meson-paths.ini" --cross-file "$HB_X11_DIR/cross/meson-$HB_ARCH.ini" \
      --prefix "$HB_PREFIX" --libdir lib --buildtype release -Ddefault_library=static "$@"
    hb_run "$pkg" ninja -C "$b" -j"$HB_JOBS"
    hb_run "$pkg" ninja -C "$b" install
    hb_mark "$pkg" "$key"
    hb_log "$pkg: ok"
  )
}

# hb_cmake <pkg> <tarball> [--salt S] -- [cmake -D options...]
hb_cmake() {
  local pkg="$1" tb="$2"; shift 2; local salt=""
  [ "${1:-}" = --salt ] && { salt="$2"; shift 2; }
  [ "${1:-}" = -- ] && shift
  hb_want "$pkg" || { hb_log "$pkg: skipped (--only)"; return 0; }
  local key; key="$(hb_key "$pkg" "$tb" "$salt $*")"
  if hb_done "$pkg" "$key"; then hb_log "$pkg: cached"; return 0; fi
  (
    hb_pkg_lock "$pkg"
    if hb_done "$pkg" "$key"; then hb_log "$pkg: cached (built by another lane)"; exit 0; fi
    hb_log "$pkg: building (cmake)"
    : > "$HB_LOG_DIR/$pkg.log"
    local src b; src="$(hb_src "$pkg" "$tb")"; b="$HB_OBJ_DIR/$pkg"
    rm -rf "$b"
    hb_run "$pkg" cmake -G Ninja -S "$src" -B "$b" \
      -DCMAKE_TOOLCHAIN_FILE="$HB_X11_DIR/cross/toolchain-hobbyos-c.cmake" \
      -DHOBBYOS_ARCH="$HB_ARCH" -DHOBBYOS_X11_TOOLS="$HB_TOOLS" -DHOBBYOS_X11_PREFIX="$HB_PREFIX" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HB_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib "$@"
    hb_run "$pkg" ninja -C "$b" -j"$HB_JOBS"
    hb_run "$pkg" ninja -C "$b" install
    hb_mark "$pkg" "$key"
    hb_log "$pkg: ok"
  )
}

# hb_need_libcxx: build the repo's obj/<arch>/libcxx.a (C++ stages).
hb_need_libcxx() {
  [ -f "$HB_OBJ/libcxx.a" ] || hb_run libcxx make -C "$HB_REPO" ARCH="$HB_ARCH" -j"$HB_JOBS" "obj/$HB_ARCH/libcxx.a"
}

# hb_link_flat [--cxx] [--gc] <OUT.BIN> <inputs...>: static flat HobbyOS
# program (crt0 + inputs + libc.a [+ libcxx.a], -T src/user/linker.ld,
# objcopy -O binary).  OUT relative names land in $HB_BIN_DIR.
hb_link_flat() {
  local args=()
  while [ $# -gt 0 ]; do case "$1" in --cxx|--gc) args+=("$1"); shift;; *) break;; esac; done
  local out="$1"; shift
  case "$out" in /*) ;; *) out="$HB_BIN_DIR/$out";; esac
  [[ " ${args[*]:-} " == *" --cxx "* ]] && hb_need_libcxx
  bash "$HB_X11_DIR/cross/link-flat.sh" --arch "$HB_ARCH" --prefix "$HB_PREFIX" "${args[@]}" --out "$out" -- "$@"
}

# The repo sysroot pieces every stage links against.
hb_run libc make -C "$HB_REPO" ARCH="$HB_ARCH" -j"$HB_JOBS" "obj/$HB_ARCH/libc.a" "obj/$HB_ARCH/crt0.o"

# hb_runtime_key: hash of what every flat .BIN links last (crt0.o, libc.a,
# libcxx.a when present, the linker script).  Final-link stamps (XHOBBY,
# IceWM) fold it in, so a libc fix relinks them; the prefix's static
# libraries do not depend on it and are not rebuilt.
hb_runtime_key() {
  cat "$HB_OBJ/crt0.o" "$HB_OBJ/libc.a" "$HB_REPO/src/user/linker.ld" \
      $( [ -f "$HB_OBJ/libcxx.a" ] && echo "$HB_OBJ/libcxx.a") | sha256sum | cut -c1-16
}

ALL_STAGES="clientlibs extlibs server icewm clients"
if [ "$STAGE" = all ]; then
  stages=""
  for s in $ALL_STAGES; do [ -f "$HB_X11_DIR/build-$s.sh" ] && stages="$stages $s"; done
else
  stages="$STAGE"
fi
for s in $stages; do
  f="$HB_X11_DIR/build-$s.sh"
  [ -f "$f" ] || hb_die "stage '$s' has no $f yet (owned by another lane)"
  # shellcheck disable=SC1090
  . "$f"
  if [ -f "$HB_X11_DIR/pins/$s.txt" ]; then
    hb_run fetch bash "$HB_X11_DIR/fetch.sh" --stage "$s"
  fi
  hb_log "== stage $s"
  "stage_$s"
  hb_log "== stage $s done"
done
hb_log "prefix: $HB_PREFIX"
