#!/usr/bin/env bash
# tools/cp/fetch-runtime.sh — download + verify the Cyberpunk track's pinned
# runtime sources into third_party/cp-runtime/ (git-ignored).
#
# Pins live in docs/cp-pins.md; this table is the executable copy of it.  A row
# changes in both places or in neither.
#
#   Wine 11.0 (stable)          dl.winehq.org            sha256 + GPG (Julliard)
#   vkd3d-proton 3.1            GitHub release asset     sha256
#   DXVK 3.1.1                  GitHub release asset     sha256
#   Vulkan-Loader / -Headers    KhronosGroup tag archive sha256
#     vulkan-sdk-1.4.363.0
#   glibc 2.31 sysroot set      archive.ubuntu.com       sha256 (the RimWorld
#     (libc6, libc6-dev,        (focal-updates pool)     pins, docs/rimworld-plan.md
#      linux-libc-dev 5.4,                               §2 and tools/mesa/
#      libgcc-s1 10.5,                                   build-hobbyos.sh:86-99;
#      libgcc-10-dev 10.5)                               libgcc-10-dev is new:
#                                                        build-wine.sh header)
#
# Idempotent: a file already present with the right sha256 is not re-fetched; a
# file present with the WRONG sha256 is a hard error (it is never silently
# replaced — inspect it).  Downloads go to <name>.part and are renamed only
# after the hash matches.
#
# GPG: Wine's tarball is signed by Alexandre Julliard.  The key is fetched from
# WineHQ's own GitLab (https://gitlab.winehq.org/julliard.gpg) into a temporary
# GNUPGHOME and must have the pinned fingerprint; the signature on the tarball
# and the signed sha512sums.asc are both checked.  If gpg or the key is
# unavailable the script says so and relies on the sha256 alone (CP_REQUIRE_GPG=1
# makes that an error).
#
# Usage:  bash tools/cp/fetch-runtime.sh [--list]
# Env:    CP_RUNTIME (default <repo>/third_party/cp-runtime)
# Exit:   0 all verified, 1 verification failure, 2 tooling/network error.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="${CP_RUNTIME:-$ROOT/third_party/cp-runtime}"
DL="$DEST/dl"

# name | url | sha256 | license
PINS='wine-11.0.tar.xz|https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz|c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701|LGPL-2.1-or-later
wine-11.0.tar.xz.sign|https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz.sign|854cf21b1fdb3f01f581413e5c9a50b3150d692895b15f5c58c190aff1adacfa|-
wine-11.0-sha512sums.asc|https://dl.winehq.org/wine/source/11.0/sha512sums.asc|d5116cb39bb546fe039f7bc34f6920d744e7cc43be254ecacd615cba427c7130|-
vkd3d-proton-3.1.tar.zst|https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v3.1/vkd3d-proton-3.1.tar.zst|b85ea8219b71093a3924913dd7aae6172333dc0c069983ed510816d67f02120b|LGPL-2.1
dxvk-3.1.1.tar.gz|https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz|40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608|Zlib
Vulkan-Loader-vulkan-sdk-1.4.363.0.tar.gz|https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz|941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7|Apache-2.0
Vulkan-Headers-vulkan-sdk-1.4.363.0.tar.gz|https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz|4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e|Apache-2.0 AND MIT
libc6_2.31-0ubuntu9.18_amd64.deb|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb|1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0|LGPL-2.1-or-later
libc6-dev_2.31-0ubuntu9.18_amd64.deb|http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb|11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a|LGPL-2.1-or-later
linux-libc-dev_5.4.0-218.238_amd64.deb|http://archive.ubuntu.com/ubuntu/pool/main/l/linux/linux-libc-dev_5.4.0-218.238_amd64.deb|0aa210458df2ab7bc4536e8edf0f9423a7018e74e2eac0b5777fd2a36f170c2f|GPL-2.0 WITH Linux-syscall-note
libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb|4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c|GPL-3.0-or-later WITH GCC-exception-3.1
libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb|http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb|ebe5e967fca32ea5811946180f7822ee749857861eadaa29514e65857ce3b897|GPL-3.0-or-later WITH GCC-exception-3.1'

WINE_KEY_URL=https://gitlab.winehq.org/julliard.gpg
WINE_KEY_FPR=DA23579A74D4AD9AF9D3F945CEFAC8EAAF17519D

say() { printf 'cp-fetch: %s\n' "$*"; }

if [ "${1:-}" = "--list" ]; then
  printf '%s\n' "$PINS" | awk -F'|' '{printf "%-46s %s\n  %s\n", $1, $3, $2}'
  exit 0
fi

command -v curl >/dev/null || { say "curl missing"; exit 2; }
command -v sha256sum >/dev/null || { say "sha256sum missing"; exit 2; }
mkdir -p "$DL" || exit 2

fail=0
while IFS='|' read -r name url sha lic; do
  [ -n "$name" ] || continue
  f="$DL/$name"
  if [ -f "$f" ]; then
    got=$(sha256sum "$f" | cut -d' ' -f1)
    if [ "$got" = "$sha" ]; then
      say "ok      $name  sha256 $sha  (cached)"
      continue
    fi
    say "MISMATCH $name: have $got, pinned $sha — refusing to overwrite; inspect/remove it"
    fail=1; continue
  fi
  if ! curl -fsSL --retry 3 -o "$f.part" "$url"; then
    say "FETCH FAILED $url"; rm -f "$f.part"; exit 2
  fi
  got=$(sha256sum "$f.part" | cut -d' ' -f1)
  if [ "$got" != "$sha" ]; then
    say "MISMATCH $name: downloaded $got, pinned $sha (left as $f.part)"
    fail=1; continue
  fi
  mv "$f.part" "$f"
  say "ok      $name  sha256 $sha  (fetched, $(stat -c %s "$f") B, $lic)"
done <<EOF
$PINS
EOF
[ "$fail" = 0 ] || { say "FAIL — sha256 verification failed"; exit 1; }

# --- Wine GPG ----------------------------------------------------------------
gpg_ok=0
if command -v gpg >/dev/null; then
  GH=$(mktemp -d) || exit 2
  trap 'rm -rf "$GH"' EXIT
  if curl -fsSL --retry 3 -o "$GH/julliard.gpg" "$WINE_KEY_URL" &&
     GNUPGHOME="$GH" gpg --batch --quiet --import "$GH/julliard.gpg" 2>/dev/null; then
    fpr=$(GNUPGHOME="$GH" gpg --batch --with-colons --fingerprint 2>/dev/null |
          awk -F: '/^fpr:/{print $10; exit}')
    if [ "$fpr" != "$WINE_KEY_FPR" ]; then
      say "GPG key fingerprint $fpr != pinned $WINE_KEY_FPR"; exit 1
    fi
    if GNUPGHOME="$GH" gpg --batch --status-fd 1 --verify "$DL/wine-11.0.tar.xz.sign" \
         "$DL/wine-11.0.tar.xz" 2>/dev/null | grep -q "VALIDSIG $WINE_KEY_FPR"; then
      say "ok      wine-11.0.tar.xz  GPG VALIDSIG $WINE_KEY_FPR (Alexandre Julliard, key from $WINE_KEY_URL)"
    else
      say "GPG signature on wine-11.0.tar.xz did NOT verify"; exit 1
    fi
    if GNUPGHOME="$GH" gpg --batch --status-fd 1 --decrypt "$DL/wine-11.0-sha512sums.asc" \
         2>/dev/null > "$GH/sums" && grep -q "VALIDSIG $WINE_KEY_FPR" "$GH/sums"; then
      want=$(awk '$2=="wine-11.0.tar.xz"{print $1}' "$GH/sums")
      have=$(sha512sum "$DL/wine-11.0.tar.xz" | cut -d' ' -f1)
      if [ -n "$want" ] && [ "$want" = "$have" ]; then
        say "ok      wine-11.0.tar.xz  sha512 matches the signed sha512sums.asc"
        gpg_ok=1
      else
        say "signed sha512sums.asc does not match wine-11.0.tar.xz"; exit 1
      fi
    else
      say "signed sha512sums.asc did NOT verify"; exit 1
    fi
  else
    say "WARNING: could not fetch/import $WINE_KEY_URL — Wine GPG NOT verified (sha256 only)"
  fi
else
  say "WARNING: gpg not installed — Wine GPG NOT verified (sha256 only)"
fi
if [ "$gpg_ok" != 1 ] && [ "${CP_REQUIRE_GPG:-0}" = 1 ]; then
  say "FAIL — CP_REQUIRE_GPG=1 and GPG verification did not complete"; exit 1
fi

say "all pins verified into $DL"
exit 0
