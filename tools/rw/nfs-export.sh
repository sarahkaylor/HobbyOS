#!/usr/bin/env bash
# tools/rw/nfs-export.sh — host side of M5: serve the RimWorld tree read-only
# over NFSv3/UDP to a HobbyOS guest (QEMU slirp: guest sees the host at
# 10.0.2.2).
#
# The game is NEVER copied into the repository.  This script copies it to a
# host directory outside the tree (default /srv/nfs/rimworld) and exports that
# directory read-only.  Only this script and the probe reference it.
#
# Usage:
#   tools/rw/nfs-export.sh sync          refresh the copy + (re)export + verify
#   tools/rw/nfs-export.sh check         verify only (no copy, no export edit)
#   tools/rw/nfs-export.sh export-only   add/refresh the export line, no copy
#
# Env:
#   RW_SRC       source tree   (default /mnt/fast/games/steamapps/common/RimWorld)
#   RW_DEST      exported dir  (default /srv/nfs/rimworld)
#   RW_GUEST_HOST  host as the GUEST sees it (default 10.0.2.2, the slirp alias)
#   RW_FIXED_MOUNTD=1  pin rpc.mountd to UDP/TCP port 20048 via an
#                      /etc/nfs.conf.d drop-in (needed only on hosts where
#                      the guest cannot use the portmapper-discovered port;
#                      it restarts nfs-mountd, so it is opt-in).
#
# Exit: 0 all verifications passed, 1 a verification failed, 2 environment.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${RW_SRC:-/mnt/fast/games/steamapps/common/RimWorld}"
DEST="${RW_DEST:-/srv/nfs/rimworld}"
GUEST_HOST="${RW_GUEST_HOST:-10.0.2.2}"
EXPORT_OPTS="ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534"
MOUNTD_PORT=20048
ACTION="${1:-sync}"

say() { printf '%s\n' "$*"; }
die() { printf 'nfs-export: %s\n' "$*" >&2; exit 2; }

[ -d "$SRC" ] || die "source tree not found: $SRC (set RW_SRC)"
[ "$(id -u)" -ne 0 ] || die "run as the invoking user, not root (the script sudo's only where it must)"

have_sudo() { sudo -n true 2>/dev/null; }
have_sudo || die "passwordless sudo is required for exportfs/mount"

tree_stats() { # dir -> "<files> <bytes>"
  local files bytes
  files=$(find "$1" -type f | wc -l | tr -d ' ')
  bytes=$(find "$1" -type f -printf '%s\n' | awk '{s+=$1} END {printf "%d", s+0}')
  printf '%s %s' "$files" "$bytes"
}

# ---------------------------------------------------------------- copy ----
copy_tree() {
  say "== copy: $SRC -> $DEST"
  if [ ! -d "$DEST" ]; then
    sudo -n mkdir -p "$DEST"
    sudo -n chown "$(id -u):$(id -g)" "$DEST"
  fi
  [ -w "$DEST" ] || { sudo -n chown "$(id -u):$(id -g)" "$DEST"; }
  # -a keeps modes/timestamps; no -H: the Steam copy has no hardlinks
  # (find -links +1 == 0), so the byte count is preserved exactly.
  local t0 t1
  t0=$(date +%s.%N)
  rsync -a --delete "$SRC"/ "$DEST"/
  t1=$(date +%s.%N)
  local secs
  secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
  local st files bytes
  st=$(tree_stats "$DEST"); files=${st% *}; bytes=${st#* }
  say "   rsync wall=${secs}s  files=$files  bytes=$bytes ($(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}') MiB)"
  say "   $((bytes / (1024 * 1024))) MiB would NOT fit the 1 GiB FAT16 image (627 MiB free) — that is why M5 is NFS."

  # The guest's AUTH_UNIX credential is uid=0,gid=0 and the export keeps the
  # default root_squash, so every request lands as nobody: the served tree
  # must be world-readable.  rsync -a copies the source root's 0770 mode.
  chmod -R a+rX "$DEST"
  say "   modes: chmod -R a+rX (guest presents uid 0, export root_squashes to nobody)"
}

# -------------------------------------------------------------- export ----
ensure_export() {
  say "== export: $DEST ($EXPORT_OPTS)"
  local line="$DEST *($EXPORT_OPTS)"
  if ! grep -qsE "^[[:space:]]*${DEST//./\\.}[[:space:]]" /etc/exports; then
    sudo -n cp -a /etc/exports "/etc/exports.l6-backup.$(date +%Y%m%d%H%M%S)"
    printf '%s\n' "$line" | sudo -n tee -a /etc/exports >/dev/null
    say "   added to /etc/exports: $line"
  else
    say "   /etc/exports already lists $DEST"
  fi
  sudo -n exportfs -ra
  say "   exportfs -ra done"
}

pin_mountd() {
  [ "${RW_FIXED_MOUNTD:-0}" = "1" ] || return 0
  say "== mountd: pinning rpc.mountd to port $MOUNTD_PORT (opt-in, restarts nfs-mountd)"
  local drop=/etc/nfs.conf.d/90-hobbyos-l6-mountd.conf
  printf '[mountd]\nport=%s\n' "$MOUNTD_PORT" | sudo -n tee "$drop" >/dev/null
  sudo -n systemctl restart nfs-mountd.service
  sleep 1
  # rpcinfo -p columns: program vers proto port service
  if ! rpcinfo -p 127.0.0.1 | awk '$2==3 && $3=="udp" && $5=="mountd" {print $4}' | grep -qx "$MOUNTD_PORT"; then
    say "   FAILED: mountd did not register on $MOUNTD_PORT"; rpcinfo -p | grep mountd || true; exit 1
  fi
  say "   mountd v3/udp is on $MOUNTD_PORT (rpcinfo -p)"
}

# -------------------------------------------------------------- verify ----
verify_host() {
  say "== verify: showmount -e 127.0.0.1"
  showmount -e 127.0.0.1 | sed 's/^/   /'

  local mnt
  mnt=$(mktemp -d "${TMPDIR:-/tmp}/rw-nfs-check.XXXXXX")
  trap 'sudo -n umount "$mnt" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true' RETURN
  say "== verify: mount -t nfs -o ro,vers=3,proto=udp,nolock 127.0.0.1:$DEST $mnt"
  # Host kernels >= 6.5 dropped the NFSv3/UDP CLIENT (the server still serves
  # UDP; the guest uses UDP).  So fall back to TCP for the host-side check and
  # say so — the guest probe is what proves the UDP path.
  if ! sudo -n mount -t nfs -o ro,vers=3,proto=udp,nolock "127.0.0.1:$DEST" "$mnt"; then
    say "   (host kernel refused NFSv3/UDP client — retrying over TCP; the guest uses UDP)"
    sudo -n mount -t nfs -o ro,vers=3,nolock "127.0.0.1:$DEST" "$mnt" || { say "   MOUNT FAILED"; return 1; }
  fi
  local n
  n=$(ls "$mnt" | wc -l | tr -d ' ')
  say "   mounted; root entries=$n"
  ls -la "$mnt" | sed -n '1,12p' | sed 's/^/   /'
  for f in RimWorldLinux_Data start_RimWorld.sh UnityPlayer.so; do
    [ -e "$mnt/$f" ] && say "   present: $f" || { say "   MISSING: $f"; return 1; }
  done
  # Byte-exact read of the first 64 KiB of UnityPlayer.so — the same range the
  # guest probe checksums, so the two numbers can be compared directly.
  local host_sum guest_sum
  host_sum=$(head -c 65536 "$mnt/UnityPlayer.so" | sha256sum | cut -d' ' -f1)
  say "   host sha256(head -c 65536 UnityPlayer.so) = $host_sum"
  sudo -n umount "$mnt"
  rmdir "$mnt"
  trap - RETURN
  say "   unmounted"
}

# ----------------------------------------------------------------- main ----
case "$ACTION" in
  sync)        copy_tree; ensure_export; pin_mountd; verify_host ;;
  export-only) ensure_export; pin_mountd; verify_host ;;
  check)       verify_host ;;
  *)           die "usage: $0 [sync|export-only|check]" ;;
esac

say ""
say "== guest mount line (HobbyOS shell / probe constant):"
say "   mount \"$GUEST_HOST:$DEST\" \"/NFS\""
say "   (SYS_MOUNT source \"$GUEST_HOST:$DEST\", target \"/NFS\")"
say ""
say "nfs-export: OK"
