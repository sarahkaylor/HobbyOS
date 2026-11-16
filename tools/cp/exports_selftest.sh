#!/usr/bin/env bash
# tools/cp/exports_selftest.sh -- exercises tools/cp/exports.sh against a
# scratch copy of an exports file (never /etc/exports).  Exit 0 = all PASS.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
export CP_EXPORTS_FILE="$T/exports" CP_EXPORTS_LOCK="$T/lock" CP_EXPORTFS=true CP_EXPORTS_SUDO=""
O='127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)'
printf '%s\n' '# user comment' '/srv/nfs/export *(rw,sync,no_subtree_check,insecure,no_root_squash)' \
  '/srv/nfs/rimworld *(ro,sync,all_squash)' > "$CP_EXPORTS_FILE"
cp "$CP_EXPORTS_FILE" "$T/orig"
fails=0
ok()   { echo "PASS $*"; }
bad()  { echo "FAIL $*"; fails=$((fails+1)); }
X() { bash "$HERE/exports.sh" "$@" > "$T/out" 2>&1; }

X add /srv/nfs/cp-a && grep -qxF "/srv/nfs/cp-a $O" "$CP_EXPORTS_FILE" && ok "add a" || bad "add a: $(cat "$T/out")"
X add /srv/nfs/cp-b && grep -qxF "/srv/nfs/cp-b $O" "$CP_EXPORTS_FILE" && ok "add b alongside a" || bad "add b"
X add /srv/nfs/cp-a; [ $? = 1 ] && ok "second add of a refused" || bad "second add of a not refused"
X remove /srv/nfs/cp-a && ! grep -q '^/srv/nfs/cp-a ' "$CP_EXPORTS_FILE" && grep -qxF "/srv/nfs/cp-b $O" "$CP_EXPORTS_FILE" \
  && ok "remove a keeps b" || bad "remove a"
X remove /srv/nfs/cp-a && ok "remove of absent a is a no-op" || bad "remove absent a"
X remove /srv/nfs/cp-b && cmp -s "$CP_EXPORTS_FILE" "$T/orig" && ok "after removing both, file is byte-identical to the original" || bad "file not restored: $(diff "$T/orig" "$CP_EXPORTS_FILE")"
X add /etc; [ $? = 2 ] && ok "DIR outside /srv/nfs/cp-* refused" || bad "bad DIR accepted"
X add '/srv/nfs/cp-x y'; [ $? = 2 ] && ok "DIR with a space refused" || bad "space DIR accepted"
echo '/srv/nfs/cp-wide *(rw,sync)' >> "$CP_EXPORTS_FILE"; cp "$CP_EXPORTS_FILE" "$T/w"
X add /srv/nfs/cp-c; [ $? = 3 ] && cmp -s "$CP_EXPORTS_FILE" "$T/w" && ok "a wide cp line makes it refuse and leave the file alone" || bad "wide cp line not refused"
cp "$T/orig" "$CP_EXPORTS_FILE"
CP_EXPORTFS=false X add /srv/nfs/cp-d; [ $? = 1 ] && cmp -s "$CP_EXPORTS_FILE" "$T/orig" && ok "exportfs failure restores the file" || bad "exportfs failure not restored"
printf '/srv/nfs/export *(rw)' > "$CP_EXPORTS_FILE"; cp "$CP_EXPORTS_FILE" "$T/nonl"
X add /srv/nfs/cp-e && [ "$(sed -n 2p "$CP_EXPORTS_FILE")" = "/srv/nfs/cp-e $O" ] && ok "add to a file without a final newline" || bad "no-final-newline add: $(cat -A "$CP_EXPORTS_FILE")"
echo "exports_selftest: $fails failure(s)"
[ $fails -eq 0 ]
