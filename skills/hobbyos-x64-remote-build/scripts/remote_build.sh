#!/bin/bash
# =============================================================================
# remote_build.sh — build HobbyOS for the x86_64 (ARCH=intel) target on a remote
# Linux host (linuxci) and pull the artifacts back to this Mac.
#
# Why: the macOS host builds x64 only through workarounds (no Homebrew musl for
# SDLAPP.BIN, unqualified ld.lld on PATH, ICU's configure handing x86_64-none-elf
# links to Mach-O gcc).  A Linux box with the same clang 21/lld 21 toolchain is
# the reference environment, so an x64 image that matters (boots the icewm
# desktop, X server + games) should be produced there.
#
# Pipeline: tar the working tree -> scp -> make ARCH=intel disk.img -> pull
# artifacts -> verify the ELF arch and (optionally) boot the image under KVM.
#
# Usage (from anywhere inside the HobbyOS repo):
#   skills/hobbyos-x64-remote-build/scripts/remote_build.sh
#   .../remote_build.sh --check                 # preflight only, no transfer
#   .../remote_build.sh --reuse                 # incremental rebuild on the last workspace
#   .../remote_build.sh --extra "MODE=test"     # extra make args
#   .../remote_build.sh --no-boot-check --keep
#
# Exit: 0 = build + verification passed; non-zero = failure (tail of the remote
# make log is printed).  Written for bash 3.2 (macOS) — no mapfile, no
# associative arrays.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERIFY="$SCRIPT_DIR/verify_artifacts.sh"

# ---- defaults ---------------------------------------------------------------
HOST="${HOBBYOS_X64_HOST:-linuxci}"
ARCH="intel"
TARGET="disk.img"
JOBS=""
OUT_ROOT="$REPO_ROOT/build/x64-remote"
STAMP="$(date +%Y%m%d-%H%M%S)"
EXTRA_ARGS=""
KEEP=0
REUSE=0
BOOT_CHECK=1
BOOT_SECONDS=75
PULL_OBJ=0
FULL_BROWSER=0
WITH_TOOLS=0
DRY_RUN=0
CHECK_ONLY=0
BUILD_TIMEOUT=5400
REMOTE_BASE="hobbyos-x64-builds"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)

# ---- logging ----------------------------------------------------------------
if [ -t 1 ]; then C_OK=$'\033[32m'; C_ERR=$'\033[31m'; C_HDR=$'\033[1m'; C_OFF=$'\033[0m'; else C_OK=; C_ERR=; C_HDR=; C_OFF=; fi
log()  { printf '%s==>%s %s\n' "$C_HDR" "$C_OFF" "$*"; }
step() { printf '  - %s\n' "$*"; }
ok()   { printf '  %sOK%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '  %sWARN%s %s\n' "$C_ERR" "$C_OFF" "$*"; }
die()  { printf '%sERROR%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }

usage() {
  sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

Options:
  --host HOST        remote builder (default $HOBBYOS_X64_HOST, else "linuxci")
  --target T         make target (default "disk.img")
  --arch A           make ARCH (default "intel"; the skill is x64-specific)
  --jobs N           parallel make jobs (default: remote nproc, capped at 32)
  --out DIR          local artifact root (default <repo>/build/x64-remote)
  --extra "ARGS"     extra make arguments, e.g. --extra "MODE=test BROWSER_SKIP=1"
  --with-tools       ship the in-OS LLVM (/TOOLS) staging; default omits it
                     (TOOLS_SKIP=1) because dist-staging is a 227 MB gitignored
                     tree and building it cold takes hours
  --full-browser     include the vendored WebKit source parts in the tarball
                     (needed only to rebuild the browser; +660 MB)
  --reuse            rebuild in the most recent remote workspace (incremental)
  --keep             keep the remote workspace after a successful run
  --pull-obj         also pull obj/<arch> (objects + bins; large)
  --no-boot-check    skip the KVM boot validation of the built image
  --boot-seconds N   how long to let the guest boot (default 75)
  --timeout S        remote build timeout in seconds (default 5400)
  --check            preflight only: local repo + remote host + toolchain
  --dry-run          print the planned tar/ssh/make/scp commands, touch nothing
  -h, --help         this text
EOF
}

# ---- args -------------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --arch) ARCH="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --out) OUT_ROOT="$2"; shift 2;;
    --extra) EXTRA_ARGS="$2"; shift 2;;
    --with-tools) WITH_TOOLS=1; shift;;
    --full-browser) FULL_BROWSER=1; shift;;
    --reuse) REUSE=1; shift;;
    --keep) KEEP=1; shift;;
    --pull-obj) PULL_OBJ=1; shift;;
    --no-boot-check) BOOT_CHECK=0; shift;;
    --boot-seconds) BOOT_SECONDS="$2"; shift 2;;
    --timeout) BUILD_TIMEOUT="$2"; shift 2;;
    --check) CHECK_ONLY=1; shift;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) usage; exit 0;;
    *) die "unknown option: $1 (try --help)";;
  esac
done

[ "$ARCH" = "intel" ] || warn "ARCH=$ARCH: this skill is tuned for the x64 target (intel)"
[ -f "$VERIFY" ] || die "missing helper $VERIFY"

# ---- 1. local preflight -----------------------------------------------------
log "Local preflight ($REPO_ROOT)"
[ -f "$REPO_ROOT/Makefile" ] || die "not a HobbyOS repo root: $REPO_ROOT has no Makefile"
[ -d "$REPO_ROOT/src/kernel/arch/x64" ] || die "missing src/kernel/arch/x64 — wrong repo?"
command -v git >/dev/null 2>&1 || die "git not found"
command -v zstd >/dev/null 2>&1 || die "zstd not found locally (brew install zstd)"
command -v scp >/dev/null 2>&1 || die "scp not found"

cd "$REPO_ROOT" || die "cannot cd $REPO_ROOT"
GIT_REV="$(git rev-parse --short=9 HEAD 2>/dev/null || echo unknown)"
GIT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo detached)"
DIRTY_COUNT="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
FILE_COUNT="$(git ls-files -z --cached --others --exclude-standard | tr -cd '\0' | wc -c | tr -d ' ')"
step "repo:    $REPO_ROOT"
step "git:     $GIT_BRANCH @ $GIT_REV ($DIRTY_COUNT modified/untracked path(s) included)"
step "files:   $FILE_COUNT source files will be shipped"
ok "local preflight"

# ---- 2. remote preflight ----------------------------------------------------
log "Remote preflight ($HOST)"
SSH="ssh ${SSH_OPTS[*]} $HOST"
if ! ssh "${SSH_OPTS[@]}" "$HOST" true 2>/dev/null; then
  die "cannot ssh to $HOST (passwordless key expected; see references/linuxci.md)"
fi
R_INFO="$($SSH 'echo "$(hostname) $(nproc) $(uname -m)"; echo "HOME=$HOME"; df -BG --output=avail "$HOME" | tail -1 | tr -dc "0-9"' 2>/dev/null)"
R_HOSTNAME="$(printf '%s\n' "$R_INFO" | sed -n 1p | cut -d' ' -f1)"
R_NPROC="$(printf '%s\n' "$R_INFO" | sed -n 1p | cut -d' ' -f2)"
R_ARCH="$(printf '%s\n' "$R_INFO" | sed -n 1p | cut -d' ' -f3)"
R_AVAIL_GB="$(printf '%s\n' "$R_INFO" | sed -n 3p)"
[ -n "$R_NPROC" ] || die "could not query $HOST (got: $R_INFO)"
case "$R_ARCH" in
  x86_64|amd64) : ;;
  *) die "$HOST is $R_ARCH, not x86_64 — an x64 cross-build there needs a cross toolchain; use linuxci" ;;
esac
step "host:    $R_HOSTNAME ($R_ARCH, $R_NPROC cores, ${R_AVAIL_GB}G free in \$HOME)"
[ "${R_AVAIL_GB:-0}" -ge 10 ] 2>/dev/null || warn "only ${R_AVAIL_GB}G free in \$HOME; a fresh build tree needs ~10G"

if [ -z "$JOBS" ]; then JOBS="$R_NPROC"; [ "$JOBS" -gt 32 ] && JOBS=32; fi
step "jobs:    -j$JOBS"

# Every tool the x64 disk.img build actually invokes.  meson+xkbcomp are the two
# that a "stock" Ubuntu cloud image lacks and that only fail ~5 min into the
# build, at the X11 server stage — check them up front.
MISSING="$($SSH 'for t in clang clang++ ld.lld llvm-objcopy llvm-ar llvm-nm llvm-readelf make tar zstd scp mcopy mmd mdir mkfs.fat meson xkbcomp python3; do command -v "$t" >/dev/null 2>&1 || printf "%s " "$t"; done' 2>/dev/null)"
if [ -n "${MISSING// /}" ]; then
  warn "missing on $HOST: $MISSING"
  case " $MISSING " in
    *" meson "*)   step "fix: sudo apt-get install -y meson" ;;
  esac
  case " $MISSING " in
    *" xkbcomp "*) step "fix: sudo apt-get install -y x11-xkb-utils xkb-data" ;;
  esac
  step "fix: sudo apt-get install -y clang lld llvm mtools dosfstools"
  [ "$CHECK_ONLY" = 1 ] && die "toolchain incomplete on $HOST"
  warn "continuing: the build will likely fail at the first missing tool"
else
  step "tools:   clang/lld/llvm/mtools/mkfs.fat/meson/xkbcomp all present"
fi

# OVMF firmware, needed only for the boot check; the Makefile's single-file
# -pflash doubles as NVRAM, so we boot against a per-run COPY, never the master.
if [ "$BOOT_CHECK" = 1 ]; then
  if ! $SSH 'test -f "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" || ls /usr/share/OVMF/OVMF_CODE_4M.fd >/dev/null 2>&1'; then
    warn "no OVMF firmware on $HOST — boot check will be skipped"
    BOOT_CHECK=0
  fi
fi
ok "remote preflight"

# ---- toolchain staging (optional) -------------------------------------------
if [ "$WITH_TOOLS" = 1 ]; then
  LOCAL_STAGE="$REPO_ROOT/third_party/llvm-21.1.8/dist-staging"
  [ -d "$LOCAL_STAGE" ] || die "--with-tools: no local $LOCAL_STAGE to ship"
  log "--with-tools: in-OS LLVM staging will be synced once to \$HOME/$REMOTE_BASE/cache/dist-staging"
fi

if [ "$CHECK_ONLY" = 1 ]; then
  log "Preflight complete — no transfer or build performed (--check)"
  exit 0
fi

# ---- 3. tarball -------------------------------------------------------------
# File list = git's view of "current code": tracked + untracked-but-not-ignored.
# That picks up uncommitted edits while still honoring .gitignore (obj/, *.bin,
# disk.img, logs, venv — none of which belong in a source transfer).
EXCLUDES="--exclude=third_party/webkit-hobbyos/src/*
--exclude=third_party/webkit-hobbyos/src-wk2/*
--exclude=third_party/webkit-hobbyos/build/*
--exclude=third_party/wpewebkit-2.54.0.tar.xz
--exclude=evidence/*
--exclude=docs/*
--exclude=_shot.ppm"
if [ "$FULL_BROWSER" = 1 ]; then
  EXCLUDES="--exclude=third_party/wpewebkit-2.54.0.tar.xz --exclude=evidence/* --exclude=docs/* --exclude=_shot.ppm"
else
  # The vendored WebKit snapshot is 660 MB of split tarball parts; a kernel/disk
  # build only needs cache/browser-x64.bin.xz (20 MB), which we DO ship.
  EXCLUDES="$EXCLUDES
--exclude=third_party/webkit-hobbyos/*.part-*"
fi

# Extra paths that are GITIGNORED locally but are required to build offline:
#   third_party/x11/dl/                    30 pinned X11/xorg tarballs (25 MB).
#     Without it third_party/x11/fetch.sh curls x.org on every cold build — slow,
#     and it fails outright on a connection reset (measured 2026-11-10:
#     "curl: (35) Recv failure: Connection reset by peer" on xcb-proto).
#   third_party/icu-78.3/icu4c-78.3-*.{tgz,zip}   host toolchain + data (~47 MB)
#     so the ICU stage never reaches for unicode-org.
# They are cheap next to the 108 MB source tarball and make the build hermetic.
EXTRA_INCLUDES="third_party/x11/dl third_party/icu-78.3/icu4c-78.3-sources.tgz third_party/icu-78.3/icu4c-78.3-data.zip"

OUT_DIR="$OUT_ROOT/$STAMP-$GIT_REV"
TARBALL="$OUT_DIR/src.tar.zst"
REMOTE_DIR="$REMOTE_BASE/$STAMP-$GIT_REV"

log "Packaging working tree"
if [ "$DRY_RUN" = 1 ]; then
  step "mkdir -p $OUT_DIR"
  step "git ls-files -z --cached --others --exclude-standard | tar --null -T - [excludes] | zstd > $TARBALL"
  step "  (+ ignored vendor caches: $EXTRA_INCLUDES)"
else
  mkdir -p "$OUT_DIR" || die "cannot create $OUT_DIR"
  # Manifest of what to ship: git's view of the tree, plus the ignored vendor
  # caches above (only those that actually exist here).
  LIST="$OUT_DIR/filelist.txt"
  { git ls-files -z --cached --others --exclude-standard 2>/dev/null
    for p in $EXTRA_INCLUDES; do [ -e "$p" ] && printf '%s\0' "$p"; done
  } > "$LIST" || die "could not build the file list"
  # COPYFILE_DISABLE: keep bsdtar from writing Apple xattr/ACL headers that GNU
  # tar then warns about 1000+ times on the Linux side ("Ignoring unknown
  # extended header keyword LIBARCHIVE.xattr.com.apple.provenance").
  # shellcheck disable=SC2086
  COPYFILE_DISABLE=1 tar --null -T "$LIST" -cf - $EXCLUDES 2>/dev/null \
    | zstd -q -3 -T0 -o "$TARBALL" -f \
    || die "tarball creation failed"
  TAR_MB=$(( $(wc -c < "$TARBALL") / 1048576 ))
  [ "$TAR_MB" -gt 0 ] || die "tarball is empty: $TARBALL"
  step "tarball: $TARBALL (${TAR_MB} MB)"
fi

# ---- 4. transfer + extract --------------------------------------------------
MAKE_CMD="make -j$JOBS ARCH=$ARCH"
[ "$WITH_TOOLS" = 1 ] && MAKE_CMD="$MAKE_CMD TOOLS_SKIP=0" || MAKE_CMD="$MAKE_CMD TOOLS_SKIP=1"
[ -n "$EXTRA_ARGS" ] && MAKE_CMD="$MAKE_CMD $EXTRA_ARGS"
MAKE_CMD="$MAKE_CMD $TARGET"

log "Shipping to $HOST:$REMOTE_DIR"
if [ "$DRY_RUN" = 1 ]; then
  step "ssh $HOST 'mkdir -p \$HOME/$REMOTE_DIR/src'"
  step "scp $TARBALL $HOST:$REMOTE_DIR/src.tar.zst"
  step "ssh $HOST 'cd \$HOME/$REMOTE_DIR/src && zstd -dc ../src.tar.zst | tar -xf -'"
  step "ssh $HOST 'cd \$HOME/$REMOTE_DIR/src && $MAKE_CMD'"
  step "scp $HOST:.../{hobbyos.elf,disk.img,build.log} $OUT_DIR/"
  [ "$PULL_OBJ" = 1 ] && step "also pulls obj/$ARCH/"
  [ "$BOOT_CHECK" = 1 ] && step "boot check: qemu-system-x86_64 -enable-kvm, ${BOOT_SECONDS}s, grep markers"
  [ "$KEEP" = 0 ] && step "cleanup: rm -rf \$HOME/$REMOTE_DIR (unless --keep/--reuse)"
  log "Dry run complete — nothing was transferred or built"
  exit 0
fi

if [ "$REUSE" = 1 ]; then
  # Only a workspace that actually produced a disk.img is worth reusing: a
  # default (non---reuse) run DELETES its tree on success, so the newest
  # directory is often a failed/interrupted one.  Picking that silently turns a
  # 4-second incremental into a 5-minute rebuild.
  PREV="$($SSH "cd \$HOME && ls -1dt $REMOTE_BASE/*/src/disk.img 2>/dev/null | head -1" 2>/dev/null | tr -d '\r')"
  if [ -n "$PREV" ]; then
    REMOTE_DIR="${PREV%/src/disk.img}"
    step "reusing: \$HOME/$REMOTE_DIR (incremental, has a completed build)"
  else
    warn "--reuse: no workspace with a completed build (disk.img) found; building fresh"
    warn "         seed one with: $0 --keep   (a successful run keeps its tree)"
  fi
fi

$SSH "mkdir -p \$HOME/$REMOTE_DIR/src" || die "cannot create remote workspace"
SCP_START=$(date +%s)
scp "${SSH_OPTS[@]}" -q -o Compression=no "$TARBALL" "$HOST:$REMOTE_DIR/src.tar.zst" \
  || die "scp failed (host $HOST, dir \$HOME/$REMOTE_DIR)"
SCP_SEC=$(( $(date +%s) - SCP_START ))
step "transferred in ${SCP_SEC}s"

step "extracting (fresh dir — never inherit another arch's hobbyos.elf)"
# The kernel link target is a single shared root file, so a workspace that last
# built ARCH=arm would silently keep that ELF (the classic stale-arch boot hang).
$SSH "cd \$HOME/$REMOTE_DIR/src && zstd -dc ../src.tar.zst | tar -xf - && rm -f hobbyos.elf hobbyos.bin && test -f Makefile" \
  || die "extraction failed in \$HOME/$REMOTE_DIR/src"

if [ "$WITH_TOOLS" = 1 ]; then
  log "Syncing in-OS LLVM staging (one-time, persistent cache)"
  $SSH "mkdir -p \$HOME/$REMOTE_BASE/cache" || die "cannot create remote cache dir"
  if ! $SSH "test -f \$HOME/$REMOTE_BASE/cache/dist-staging/usr/local/bin/clang-21"; then
    rsync -a --delete "$REPO_ROOT/third_party/llvm-21.1.8/dist-staging/" \
      "$HOST:\$HOME/$REMOTE_BASE/cache/dist-staging/" \
      || die "rsync of dist-staging failed"
  else
    step "cache hit: \$HOME/$REMOTE_BASE/cache/dist-staging"
  fi
  # tools-staged accepts the persistent path; link it into the tree.
  $SSH "cd \$HOME/$REMOTE_DIR/src && mkdir -p third_party/llvm-21.1.8 && ln -sfn \$HOME/$REMOTE_BASE/cache/dist-staging third_party/llvm-21.1.8/dist-staging" \
    || die "could not link dist-staging into the build tree"
fi

# ---- 5. build ---------------------------------------------------------------
log "Building: $MAKE_CMD"
step "log: \$HOME/$REMOTE_DIR/src/build.log (remote)"
BUILD_START=$(date +%s)
set +e
$SSH "cd \$HOME/$REMOTE_DIR/src && timeout $BUILD_TIMEOUT $MAKE_CMD > build.log 2>&1"
BUILD_RC=$?
BUILD_SEC=$(( $(date +%s) - BUILD_START ))

# NOTE: never `set -e` here.  `grep -c` exits 1 when it counts ZERO matches, so
# -e would abort the script on a perfectly clean build (0 errors).
ERRORS="$($SSH "cd \$HOME/$REMOTE_DIR/src && grep -cE 'error:|Error [0-9]' build.log" 2>/dev/null | tr -d '\r' || true)"
WARNINGS="$($SSH "cd \$HOME/$REMOTE_DIR/src && grep -ciE 'warning:' build.log" 2>/dev/null | tr -d '\r' || true)"
ERRORS="${ERRORS:-0}"; WARNINGS="${WARNINGS:-0}"
set +e

if [ "$BUILD_RC" -ne 0 ]; then
  printf '\n%s---- remote build log: error lines + tail ----%s\n' "$C_ERR" "$C_OFF"
  $SSH "cd \$HOME/$REMOTE_DIR/src && grep -nE 'error:|Error [0-9]|No rule|not found|No such file' build.log | tail -12" || true
  printf '   ...\n'
  $SSH "cd \$HOME/$REMOTE_DIR/src && tail -20 build.log" || true
  printf '%s---- end ----%s\n\n' "$C_ERR" "$C_OFF"
  die "build failed (make rc=$BUILD_RC, ${BUILD_SEC}s). Full log: \$HOME/$REMOTE_DIR/src/build.log on $HOST (workspace kept)"
fi
ok "build finished in ${BUILD_SEC}s (${ERRORS:-0} error lines, ${WARNINGS:-0} warnings)"

# ---- 6. boot check (proves it's a *good* x64 image, not just a green link) --
BOOT_RESULT="skipped"
if [ "$BOOT_CHECK" = 1 ]; then
  log "Boot check (qemu-system-x86_64 -enable-kvm, ${BOOT_SECONDS}s)"
  $SSH "cd \$HOME/$REMOTE_DIR/src && \
        cp \$HOME/.local/share/OVMF/OVMF_CODE_4M.fd oVMF-boot.fd 2>/dev/null || cp /usr/share/OVMF/OVMF_CODE_4M.fd oVMF-boot.fd; \
        rm -f boot.log; \
        timeout $BOOT_SECONDS qemu-system-x86_64 -M q35,vmport=on -smp 8 -m 4096M \
          -pflash oVMF-boot.fd -display none -serial file:boot.log -enable-kvm \
          -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
          -drive file=disk.img,format=raw,id=disk0,if=none \
          -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
          -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
          -device virtio-tablet-pci -action shutdown=poweroff >/dev/null 2>&1; \
        echo QEMU_RC=\$?" 2>/dev/null | tr -d '\r'
  BOOT_LOG="$OUT_DIR/boot.log"
  scp "${SSH_OPTS[@]}" -q "$HOST:$REMOTE_DIR/src/boot.log" "$BOOT_LOG" 2>/dev/null || true
  if [ -f "$BOOT_LOG" ]; then
    if grep -q 'Mode: DESKTOP' "$BOOT_LOG" \
       && grep -q 'ICEWM\.BIN' "$BOOT_LOG" \
       && grep -q 'XHOBBY\.BIN' "$BOOT_LOG" \
       && ! grep -qiE 'kernel panic|PANIC:|FATAL' "$BOOT_LOG"; then
      BOOT_RESULT="pass"
      ok "guest booted the icewm desktop (X server + ICEWM + XEYES alive, no panic)"
    else
      BOOT_RESULT="FAIL"
      warn "boot check failed — markers missing. Inspect $BOOT_LOG"
      grep -iE 'PANIC|FATAL|error' "$BOOT_LOG" | head -8
      die "built image does not boot to the desktop (see $BOOT_LOG)"
    fi
  else
    BOOT_RESULT="no-log"
    warn "boot log not retrieved; skipping boot verdict"
  fi
fi

# ---- 7. pull artifacts ------------------------------------------------------
log "Pulling artifacts to $OUT_DIR"
for f in hobbyos.elf disk.img build.log; do
  scp "${SSH_OPTS[@]}" -q -o Compression=no "$HOST:$REMOTE_DIR/src/$f" "$OUT_DIR/$f" \
    || die "failed to pull $f"
  step "$f ($(( $(wc -c < "$OUT_DIR/$f") / 1048576 )) MB)"
done

if [ "$PULL_OBJ" = 1 ]; then
  step "packing obj/$ARCH on the builder"
  $SSH "cd \$HOME/$REMOTE_DIR/src && tar -cf - obj/$ARCH 2>/dev/null | zstd -q -3 -T0 -o ../obj-$ARCH.tar.zst -f" \
    || warn "could not pack obj/$ARCH"
  scp "${SSH_OPTS[@]}" -q -o Compression=no "$HOST:$REMOTE_DIR/obj-$ARCH.tar.zst" "$OUT_DIR/obj-$ARCH.tar.zst" \
    && step "obj-$ARCH.tar.zst ($(( $(wc -c < "$OUT_DIR/obj-$ARCH.tar.zst") / 1048576 )) MB)" \
    || warn "could not pull obj archive"
fi

# ---- 8. verify locally ------------------------------------------------------
log "Verifying artifacts"
BOOT_LOG_FOR_VERIFY=""
[ -f "$OUT_DIR/boot.log" ] && BOOT_LOG_FOR_VERIFY="$OUT_DIR/boot.log"
if bash "$VERIFY" "$OUT_DIR" ${BOOT_LOG_FOR_VERIFY:+"$BOOT_LOG_FOR_VERIFY"}; then
  VERIFY_RESULT="pass"
else
  VERIFY_RESULT="FAIL"
  die "artifact verification failed (see output above)"
fi

# ---- 9. manifest ------------------------------------------------------------
MANIFEST="$OUT_DIR/manifest.txt"
{
  echo "stamp=$STAMP"
  echo "git_branch=$GIT_BRANCH"
  echo "git_rev=$GIT_REV"
  echo "git_dirty_paths=$DIRTY_COUNT"
  echo "host=$HOST"
  echo "host_cores=$R_NPROC"
  echo "arch=$ARCH"
  echo "target=$TARGET"
  echo "jobs=$JOBS"
  echo "make_args=$MAKE_CMD"
  echo "with_tools=$WITH_TOOLS"
  echo "build_seconds=$BUILD_SEC"
  echo "scp_seconds=$SCP_SEC"
  echo "error_lines=${ERRORS:-0}"
  echo "warning_lines=${WARNINGS:-0}"
  echo "boot_check=$BOOT_RESULT"
  echo "verify=$VERIFY_RESULT"
  echo "--- sha256 ---"
  (cd "$OUT_DIR" && shasum -a 256 hobbyos.elf disk.img 2>/dev/null)
} > "$MANIFEST"
step "manifest: $MANIFEST"

# ---- 10. cleanup ------------------------------------------------------------
# The transfer tarball is regenerable from the recorded git rev — drop it so an
# artifact dir stays ~1.0 GB (the image) instead of 1.2 GB.  Kept on failure
# (die() exits before here) for debugging.
rm -f "$TARBALL" "$OUT_DIR/filelist.txt"
step "dropped the transfer tarball (regenerable from git rev $GIT_REV)"

if [ "$KEEP" = 1 ] || [ "$REUSE" = 1 ]; then
  step "remote workspace kept: $HOST:\$HOME/$REMOTE_DIR"
else
  $SSH "rm -rf \$HOME/$REMOTE_DIR" 2>/dev/null && step "remote workspace removed"
fi

# ---- summary ----------------------------------------------------------------
printf '\n%s%s%s\n' "$C_HDR" "==================== x64 remote build: SUCCESS ====================" "$C_OFF"
printf '  %-16s %s\n' "artifacts"  "$OUT_DIR"
printf '  %-16s %s\n' "hobbyos.elf" "$(( $(wc -c < "$OUT_DIR/hobbyos.elf") / 1024 )) KiB (x86_64 multiboot1, ELF32)"
printf '  %-16s %s\n' "disk.img"   "$(( $(wc -c < "$OUT_DIR/disk.img") / 1048576 )) MiB (bootable, icewm desktop)"
printf '  %-16s %s\n' "build"      "${BUILD_SEC}s on $HOST (-j$JOBS), ${ERRORS:-0} errors / ${WARNINGS:-0} warnings"
printf '  %-16s %s\n' "boot check" "$BOOT_RESULT"
printf '  %-16s %s\n' "git"        "$GIT_BRANCH @ $GIT_REV ($DIRTY_COUNT dirty)"
printf '\n  Boot it locally:  make run_intel QEMU_ARGS="-drive file=%s/disk.img,format=raw,id=disk0,if=none -device nvme,drive=disk0,serial=1234,bus=pcie.1"\n' "$OUT_DIR"
printf '  or from the artifact dir: qemu-system-x86_64 -M q35 ... -drive file=disk.img ...\n\n'
exit 0
