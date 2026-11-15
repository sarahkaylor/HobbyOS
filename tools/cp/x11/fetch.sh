#!/usr/bin/env bash
# tools/cp/x11/fetch.sh — download + verify the real X11 client libraries for
# the Cyberpunk track's compat processes (docs/cp-x11.md §1) into the
# git-ignored third_party/cp-runtime/x11/.
#
# Source: https://xorg.freedesktop.org/releases/individual/ ONLY.  Every
# tarball and its detached .sig is pinned by sha256 (table below = the
# executable copy of docs/cp-x11.md §1; a row changes in both or neither).
#
# Verification, per tarball:
#   1. sha256 of the tarball and of its .sig must match the pin (hard error;
#      a present file with the wrong hash is never silently replaced).
#   2. gpg: the .sig is checked against tools/cp/x11/x11-signers.asc (public
#      keys taken from Ubuntu's debian/upstream/signing-key.asc files, see
#      docs/cp-x11.md §1.2) in a throw-away GNUPGHOME, and the VALIDSIG
#      fingerprint must equal the row's pinned signer.  Signers whose key is
#      not in the committed keyring (Matt Turner's ed25519 key for xcb-proto
#      and libxcb) are reported "sha256-only"; CP_REQUIRE_GPG=1 makes that an
#      error.  An expired-but-valid signer key (EXPKEYSIG) is accepted — the
#      signature predates expiry and the fingerprint is pinned.
#
# Idempotent: verified files are not re-fetched.  Downloads go to <f>.part and
# are renamed only after the hash matches.
#
# Usage:  bash tools/cp/x11/fetch.sh [--list]
# Env:    CP_RUNTIME (default <repo>/third_party/cp-runtime) -> $CP_RUNTIME/x11/dl
#         CP_REQUIRE_GPG=1
# Exit:   0 all verified, 1 verification failure, 2 tooling/network error.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RT="${CP_RUNTIME:-$ROOT/third_party/cp-runtime}"
DL="$RT/x11/dl"
KEYS="$ROOT/tools/cp/x11/x11-signers.asc"
BASE=https://xorg.freedesktop.org/releases/individual

# Signer fingerprints (primary or signing subkey, as gpg's VALIDSIG reports).
AC_NEW=3AB285232C46AE43D8E192F4DAB0F78EA6E7E2D2   # Alan Coopersmith (2023-)
AC_OLD=2F7DBCAD5B30E668BBD516C1CFDF148828C642A7   # Alan Coopersmith (older; expired)
TED=19882D92DDA4C400C22C0D56CC2AF4472167BE03      # Thomas E. Dickey
MT=16238A7D92448B02B7B10344B6B1CEAE5103DB07       # Matt Turner (ed25519; key not in keyring)

# file | subdir | sha256(tarball) | sha256(.sig) | signer fpr
PINS="xorgproto-2026.1.tar.xz|proto|f9bfe4a9ed8c8ab9d2a3b0d49797f046052dadd06b7a8b45dbffaffb137e8290|5551ac3d462c583037209cfd905c8b04c8e585b4c6d59dc032719c1c1f638f90|$AC_NEW
xcb-proto-1.17.0.tar.xz|xcb|2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c|7d057fc8432560b81e10b61f8ac1f646977f5dc1e2b5d4149b823c6966fbd561|$MT
libpthread-stubs-0.5.tar.xz|xcb|59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc|5a669e5d454b8c323adc90cb52d2f03e3c5943cf5bc51e1d1dec8c11c11b89de|$AC_OLD
libXau-1.0.12.tar.xz|lib|74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb|7f8953455d6e8838cc4adb8d83f7c72498c6c8a1641b0e7396de2d6fe8e42219|$AC_OLD
libXdmcp-1.1.5.tar.xz|lib|d8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c|d1d724f603af4cf86e3603f95bf39f77b22a7c2897c2a667ff1a5d216e24e7a6|$AC_OLD
libxcb-1.17.0.tar.xz|xcb|599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55|2829ef79617e521913e7ed36cd7c494f1300f901e2e9d1bd6795ecf212bd7ebf|$MT
xtrans-1.6.0.tar.xz|lib|faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92|fd24b80cdcbeb03b582623bf9f770004a66473b17d5b3163301915794acc3c35|$AC_OLD
libX11-1.8.14.tar.xz|lib|bbc78c46217174655c2b32b138ad119f3ac15f50808b4d0b3a2677a4b1afc950|96634abc2fb2e46b56d907d0c43109137ebac6c2dcd20d9e5707cf6cc53dabea|$AC_NEW
libXext-1.3.7.tar.xz|lib|6c643c7035cdacf67afd68f25d01b90ef889d546c9fcd7c0adf7c2cf91e3a32d|d7c28f4d970d8fbce47e9d1582acd79245e7b8dac23cb9c40ff01ec6ac446f59|$AC_NEW
libXrender-0.9.13.tar.xz|lib|3fe88c609df6782e1e7af2e4de36b2563d70cd70ff7dcb1cda561ad64912bb64|2cb12768fb1a22d4becac1935e4619ad87b2c2810b58a6c0ee44e82cc9abc65c|$AC_NEW
libXfixes-6.0.2.tar.xz|lib|39f115d72d9c5f8111e4684164d3d68cc1fd21f9b27ff2401b08fddfc0f409ba|eb9482ea063ff6efd9fbe06331a54d89eb7ebc2a101d17c87b9fa1c5807ee551|$AC_NEW
libXi-1.8.4.tar.xz|lib|ddd6057b89a91903bc2f884d55c8467440906945e482c36547260dff28661afc|119d75656bba66c051c2f25d8759b0f821402e48835ee4522a49a17028ad4b49|$AC_NEW
libXrandr-1.5.5.tar.xz|lib|72b922c2e765434e9e9f0960148070bd4504b288263e2868a4ccce1b7cf2767a|c7e7af06e2deaa7857b8c970ef880a7e74b5c93de6dd55c038c9500785ef581a|$AC_NEW
libXcursor-1.2.3.tar.xz|lib|fde9402dd4cfe79da71e2d96bb980afc5e6ff4f8a7d74c159e1966afb2b2c2c0|3f38a63e4015f3ce6549ea37392dcc02f757fa225c38a64e9066694bef22c708|$TED
libXinerama-1.1.6.tar.xz|lib|d00fc1599c303dc5cbc122b8068bdc7405d6fcb19060f4597fc51bd3a8be51d7|36732002aa8efd1ab7f5b84856b978068467a7e609fe6802463ee4b64428458a|$AC_NEW
libxshmfence-1.3.3.tar.xz|lib|d4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10|390720b5ca8dc04ec8243aa4c7d229c4b65362a9b3f6403872f0dae906473674|$AC_OLD"

say() { printf 'cp-x11-fetch: %s\n' "$*"; }

if [ "${1:-}" = "--list" ]; then
  printf '%s\n' "$PINS" | awk -F'|' -v b="$BASE" '{printf "%-30s %s\n  %s/%s/%s\n", $1, $3, b, $2, $1}'
  exit 0
fi

command -v curl >/dev/null || { say "curl missing"; exit 2; }
command -v sha256sum >/dev/null || { say "sha256sum missing"; exit 2; }
mkdir -p "$DL" || exit 2

# get FILE URL SHA256 -> 0 ok, 1 mismatch, 2 network
get() {
  local f="$1" url="$2" want="$3" have
  if [ -f "$DL/$f" ]; then
    have=$(sha256sum "$DL/$f" | cut -d' ' -f1)
    [ "$have" = "$want" ] && return 0
    say "MISMATCH (existing) $f: have $have want $want — not replacing; inspect it"; return 1
  fi
  curl -fsSL --retry 3 -o "$DL/$f.part" "$url" || { rm -f "$DL/$f.part"; say "download failed: $url"; return 2; }
  have=$(sha256sum "$DL/$f.part" | cut -d' ' -f1)
  if [ "$have" != "$want" ]; then
    say "MISMATCH (download) $f: have $have want $want"; rm -f "$DL/$f.part"; return 1
  fi
  mv "$DL/$f.part" "$DL/$f"
}

GNUPGHOME_TMP=""
if command -v gpg >/dev/null && [ -f "$KEYS" ]; then
  GNUPGHOME_TMP=$(mktemp -d)
  trap 'rm -rf "$GNUPGHOME_TMP"' EXIT
  GNUPGHOME="$GNUPGHOME_TMP" gpg --batch --quiet --import "$KEYS" 2>/dev/null ||
    { say "cannot import $KEYS"; exit 2; }
else
  say "gpg or $KEYS unavailable: sha256-only"
  [ "${CP_REQUIRE_GPG:-0}" = 1 ] && exit 2
fi

fail=0 net=0 ngpg=0 nsha=0 n=0
while IFS='|' read -r f sub sha sigsha fpr; do
  [ -n "$f" ] || continue
  n=$((n+1))
  get "$f" "$BASE/$sub/$f" "$sha"; r=$?
  [ $r = 1 ] && fail=1; [ $r = 2 ] && net=1
  get "$f.sig" "$BASE/$sub/$f.sig" "$sigsha"; r2=$?
  [ $r2 = 1 ] && fail=1; [ $r2 = 2 ] && net=1
  [ $r = 0 ] && [ $r2 = 0 ] || continue
  how="sha256"
  if [ -n "$GNUPGHOME_TMP" ]; then
    st=$(GNUPGHOME="$GNUPGHOME_TMP" gpg --batch --status-fd 1 --verify "$DL/$f.sig" "$DL/$f" 2>/dev/null)
    got=$(printf '%s\n' "$st" | awk '$2=="VALIDSIG"{print $3}')
    if printf '%s\n' "$st" | grep -qE '^\[GNUPG:\] (GOODSIG|EXPKEYSIG) ' && [ "$got" = "$fpr" ]; then
      how="sha256+gpg($fpr)"; ngpg=$((ngpg+1))
    elif printf '%s\n' "$st" | grep -q '^\[GNUPG:\] NO_PUBKEY'; then
      how="sha256-only (signer $fpr not in keyring)"; nsha=$((nsha+1))
      [ "${CP_REQUIRE_GPG:-0}" = 1 ] && fail=1
    else
      say "GPG FAIL $f (VALIDSIG='$got', want $fpr)"; fail=1; continue
    fi
  else
    nsha=$((nsha+1))
  fi
  say "ok  $f  $sha  [$how]"
done <<EOF
$PINS
EOF

[ $fail = 0 ] || { say "VERIFICATION FAILED"; exit 1; }
[ $net = 0 ] || { say "network errors (re-run)"; exit 2; }
say "all $n tarballs sha256-verified into $DL ($ngpg also gpg-verified, $nsha sha256-only)"
