# Host capabilities for HobbyOS runners — the single place that knows what the
# machine running QEMU can do.  Source this; do not re-derive it per script.
#
#   . tools/host/hostcaps.sh
#   make run QEMU_ARGS="$HB_QEMU_ACCEL -qmp unix:$sock,server,nowait"
#
# Two host classes are supported: Linux with KVM, and anything else (macOS on
# Apple silicon in practice) where QEMU falls back to TCG.  The guest is always
# x86_64; only the *host* side changes.
#
# Why one file: the same host facts were being guessed in four places
# (build-hobbyos.sh, run-rimworld-desktop.sh, m4_gl_probe.py, docs), and every
# guess was wrong somewhere — `-enable-kvm` is fatal on macOS, `fuser` and
# `readelf` do not exist there, and Homebrew hides llvm in a keg.  A runner that
# shells out to `hostcaps --json` gets the same answers the scripts do.

HB_HOST_OS=$(uname -s)
HB_HOST_ARCH=$(uname -m)

# --- acceleration -----------------------------------------------------------
# -enable-kvm is a hard error on a QEMU built without KVM ("invalid option"), so
# a host that cannot accelerate must say nothing at all and let QEMU pick TCG.
if [ "$HB_HOST_OS" = Linux ] && [ -w /dev/kvm ]; then
  HB_ACCEL=kvm
  HB_QEMU_ACCEL="-enable-kvm"
else
  HB_ACCEL=tcg
  HB_QEMU_ACCEL=""
fi

# --- QEMU -------------------------------------------------------------------
# Homebrew installs to opt/homebrew on arm64 and usr/local on Intel; the firmwares
# live under the qemu share dir rather than /usr/share.
for d in /opt/homebrew/share/qemu /usr/local/share/qemu /usr/share/qemu \
         /usr/share/OVMF /usr/share/edk2/x64; do
  [ -d "$d" ] && HB_QEMU_SHARE="$d" && break
done
for f in "$HB_QEMU_SHARE/edk2-x86_64-code.fd" \
         "$HB_QEMU_SHARE/OVMF_CODE_4M.fd" \
         /usr/share/OVMF/OVMF_CODE_4M.fd \
         "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd"; do
  [ -f "$f" ] && HB_OVMF="$f" && break
done

# --- LLVM ---------------------------------------------------------------
# Homebrew's llvm is keg-only: llvm-ar/llvm-readelf/llvm-nm are NOT on PATH, and
# the fallbacks (Apple ar/readelf-less) either fail or silently misbehave.
for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
  [ -d "$d" ] || continue
  case ":$PATH:" in *":$d:"*) ;; *) PATH="$d:$PATH" ;; esac
done
export PATH
HB_LLVM_BIN=$(dirname "$(command -v llvm-readelf 2>/dev/null || command -v readelf 2>/dev/null || echo /nonexistent/x)")
HB_READELF=${HB_READELF:-$(command -v llvm-readelf 2>/dev/null || command -v readelf 2>/dev/null || true)}
HB_AR=${HB_AR:-$(command -v llvm-ar 2>/dev/null || true)}

# --- file locking -----------------------------------------------------------
# BSD `fuser` is not psmisc's; macOS has no fuser at all. Kill whoever holds the
# image so a previous run cannot keep the write lock (host.sh callers pass the img).
hb_release_image() {
  _img=$1
  [ -n "$_img" ] || return 0
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "$_img" >/dev/null 2>&1 || true
  elif command -v lsof >/dev/null 2>&1; then
    for _p in $(lsof -t "$_img" 2>/dev/null); do kill "$_p" 2>/dev/null || true; done
  fi
  return 0
}

hb_size_of() { if [ "$HB_HOST_OS" = Darwin ]; then stat -f%z "$1"; else stat -c%s "$1"; fi; }

# Print the facts as shell assignments or as JSON, so a Python runner need not
# duplicate the logic above.
case "${1:-}" in
  --json)
    printf '{"os":"%s","arch":"%s","accel":"%s","qemu_accel":"%s","ovmf":"%s","readelf":"%s","ar":"%s"}\n' \
      "$HB_HOST_OS" "$HB_HOST_ARCH" "$HB_ACCEL" "$HB_QEMU_ACCEL" "${HB_OVMF:-}" "${HB_READELF:-}" "${HB_AR:-}"
    ;;
  --print)
    echo "host:    $HB_HOST_OS/$HB_HOST_ARCH"
    echo "accel:   $HB_ACCEL${HB_QEMU_ACCEL:+ ($HB_QEMU_ACCEL)}"
    echo "ovmf:    ${HB_OVMF:-MISSING}"
    echo "readelf: ${HB_READELF:-MISSING}"
    echo "ar:      ${HB_AR:-MISSING}"
    ;;
esac
