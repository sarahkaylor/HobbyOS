#!/usr/bin/env bash
# tools/cp/exports.sh add|remove DIR -- add or remove ONE localhost-only,
# read-only NFS export on the gate host (docs/cyberpunk.md §7.4).
#
# The user approved exactly this export form and nothing wider:
#   DIR 127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)
# DIR must be /srv/nfs/cp-<name>.
#
# Several harnesses may hold exports at the same time (one per concurrent
# guest), so /tmp/cp-exports.lock is held only while /etc/exports is edited,
# never across a boot.  Each call:
#   - snapshots /etc/exports, edits ONLY the line for DIR, and runs exportfs -ra;
#   - verifies that every line that is not a cp export line is byte-identical
#     before and after (so the user's own exports can never change), and that
#     DIR's line is present after `add` and absent after `remove`;
#   - on any failure restores the snapshot taken in this same call and exits 1.
# `remove` of a DIR that has no line is a no-op (rc 0), so it is safe in traps.
#
# Self-test only: CP_EXPORTS_FILE / CP_EXPORTS_LOCK / CP_EXPORTFS / CP_EXPORTS_SUDO
# point it at a scratch file (bash tools/cp/exports_selftest.sh).
set -u
F="${CP_EXPORTS_FILE:-/etc/exports}"
LK="${CP_EXPORTS_LOCK:-/tmp/cp-exports.lock}"
EXPORTFS="${CP_EXPORTFS:-sudo -n exportfs -ra}"
SUDO="${CP_EXPORTS_SUDO-sudo -n}"
OP="${1:-}"; DIR="${2:-}"
case "$OP" in add|remove) ;; *) echo "usage: exports.sh add|remove /srv/nfs/cp-NAME" >&2; exit 2;; esac
[[ "$DIR" =~ ^/srv/nfs/cp-[A-Za-z0-9._-]+$ ]] || { echo "exports.sh: DIR must be /srv/nfs/cp-<name>, got '$DIR'" >&2; exit 2; }
OPTS='127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)'
LINE="$DIR $OPTS"
CPRE='^/srv/nfs/cp-[A-Za-z0-9._-]+ 127\.0\.0\.1\(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534\)$'

exec 196>>"$LK"
flock 196
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
$SUDO cat "$F" > "$T/before" || { echo "exports.sh: cannot read $F" >&2; exit 3; }
# Refuse to touch a file that already holds a cp line in any other form.
if grep -E '^/srv/nfs/cp-' "$T/before" | grep -vqE "$CPRE"; then
  echo "exports.sh: $F has a /srv/nfs/cp-* line that is not the approved form; not touching it" >&2; exit 3
fi
if [ "$OP" = add ]; then
  if grep -qxF "$LINE" "$T/before"; then echo "exports.sh: $DIR already exported" >&2; exit 1; fi
  [ -n "${CP_EXPORTS_FILE:-}" ] || $SUDO mkdir -p "$DIR"
  { cat "$T/before"; [ -s "$T/before" ] && [ -n "$(tail -c1 "$T/before")" ] && echo; echo "$LINE"; } > "$T/after"
else
  grep -qxF "$LINE" "$T/before" || exit 0
  grep -vxF "$LINE" "$T/before" > "$T/after" || true
fi
restore() { $SUDO tee "$F" < "$T/before" > /dev/null; $EXPORTFS; }
$SUDO tee "$F" < "$T/after" > /dev/null || { restore; echo "exports.sh: write failed" >&2; exit 1; }
$EXPORTFS || { restore; echo "exports.sh: exportfs -ra failed; restored" >&2; exit 1; }
$SUDO cat "$F" > "$T/now"
grep -vE "$CPRE" "$T/before" > "$T/b.user" || true
grep -vE "$CPRE" "$T/now" > "$T/n.user" || true
if ! cmp -s "$T/b.user" "$T/n.user"; then restore; echo "exports.sh: non-cp lines changed; restored" >&2; exit 1; fi
if [ "$OP" = add ]; then grep -qxF "$LINE" "$T/now" || { restore; echo "exports.sh: line missing after add; restored" >&2; exit 1; }
else grep -qxF "$LINE" "$T/now" && { restore; echo "exports.sh: line still present after remove; restored" >&2; exit 1; }; fi
echo "exports.sh: $OP $DIR ok (non-cp lines unchanged)"
