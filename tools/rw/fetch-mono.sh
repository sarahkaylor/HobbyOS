#!/usr/bin/env bash
# tools/rw/fetch-mono.sh — the official Mono 6.12 x86_64 Linux build, pinned by
# sha256, laid out as a self-contained tree the compat track can run.
#
# PROVENANCE (measured 2026-11-02)
# ------------------------------
# mono-project.com/archive has NO Linux tarball for 6.12: /archive/6.12.0/
# contains only macos-10-universal/ and windows-installer/ (the same is true of
# 6.8.0 and 5.20.1).  The official Linux x86_64 build is published as .debs in
# the mono-project APT repository, so that is what this script pins: the
# snapshot dist for 6.12.0.199, with each package's sha256 taken from that
# snapshot's own Packages index:
#
#   https://download.mono-project.com/repo/ubuntu/dists/focal/snapshots/6.12.0.199/main/binary-amd64/Packages.gz
#
# The set is the minimum that makes `mono --version` and `mono hello.exe` work
# (runtime + mscorlib + the System assemblies hello.exe references).  Nothing
# is redistributed: the tree lands in third_party/rw-mono, which is git-ignored,
# exactly like the glibc pin.
#
# Two more C++/zlib runtime pieces come from focal-updates, because mono-sgen's
# NEEDED list is libz.so.1 + libstdc++.so.6 and the workstation's copies require
# GLIBC_2.38 (unloadable under the 2.31 pin).  sha256 from the focal-updates
# Packages index, same rule as everything else.
#
# Output: $OUT/root/{usr/bin/mono-sgen,usr/lib/mono/4.5/*.dll,etc/mono/**,
#                     lib/libstdc++.so.6,lib/libz.so.1}
#         $OUT/hello.exe  (built here, once)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${RW_MONO_DIR:-$ROOT/third_party/rw-mono}"
DEBS="$OUT/debs"
TREE="$OUT/tree"
BASE="https://download.mono-project.com/repo/ubuntu"
VER="6.12.0.199-0xamarin2+ubuntu2004b1"

# sha256 from the snapshot's Packages index, then the pool path.
PKGS="
741a7bff08503d11b6f05627471fc7e99928e0d5b6a955388ecfdfbd74d53dd5 mono-runtime_${VER}_amd64.deb pool/main/m/mono/mono-runtime_${VER}_amd64.deb
8dd839669846757c536df80be9f8540fc90d7d3057406c63c3c7e9d85275e9ae mono-runtime-sgen_${VER}_amd64.deb pool/main/m/mono/mono-runtime-sgen_${VER}_amd64.deb
bd9616124e8f10f433148bda9f936173eab5f0a3ce98decedd15adf8271f095b mono-runtime-common_${VER}_amd64.deb pool/main/m/mono/mono-runtime-common_${VER}_amd64.deb
fa3149a4de6c6c2327df2d6d5e05e2210ad3f921ea6028574ed8997be4b0e9fd libmono-corlib4.5-cil_${VER}_all.deb pool/main/m/mono/libmono-corlib4.5-cil_${VER}_all.deb
d07ca65e5a09fc8f25a07452e6e97e58ac22e2b8d6f4ada732de9701806ecb2e libmono-system4.0-cil_${VER}_all.deb pool/main/m/mono/libmono-system4.0-cil_${VER}_all.deb
b0b853b25a58db4f0b8cbe5b981239ab533d52d169d5561a6cff206f08f5d30c libmono-system-core4.0-cil_${VER}_all.deb pool/main/m/mono/libmono-system-core4.0-cil_${VER}_all.deb
531459ab993b532a2393af6d7de400eb54ccac0e74ae8bba3c95a57f84efb207 libmono-security4.0-cil_${VER}_all.deb pool/main/m/mono/libmono-security4.0-cil_${VER}_all.deb
c4cf6b3c5ea904bd2f127f15e1bd5e17400a17e04068f64339c1f1cbfcfabbd0 libmono-system-configuration4.0-cil_${VER}_all.deb pool/main/m/mono/libmono-system-configuration4.0-cil_${VER}_all.deb
c75d4942e2376e9e33fbdb2f7c1b568717de8f7f1c4e8f0b02ac7768effd7e52 libmono-system-xml4.0-cil_${VER}_all.deb pool/main/m/mono/libmono-system-xml4.0-cil_${VER}_all.deb
f86d2037a51ddc2d159a49b6cd39c228553a84ed30a7553e399dd1e360cf9eae mono-4.0-gac_${VER}_all.deb pool/main/m/mono/mono-4.0-gac_${VER}_all.deb
901a54443ab305de8f4d170f5ce882c5b8f2058b070ec8c3f07e7abc4d170874 mono-gac_${VER}_all.deb pool/main/m/mono/mono-gac_${VER}_all.deb
"

UBUNTU="http://archive.ubuntu.com/ubuntu"
fetch_ubuntu() { # $1 sha256, $2 file, $3 pool path
  mkdir -p "$DEBS"
  if [ ! -f "$DEBS/$2" ]; then
    echo "mono: fetching $2"
    curl -fsS -o "$DEBS/$2" "$UBUNTU/$3" || { echo "mono: FETCH FAILED $2"; exit 2; }
  fi
  echo "$1  $DEBS/$2" | sha256sum -c --quiet - || { echo "mono: SHA256 MISMATCH $2"; exit 2; }
}

fetch() { # $1 sha256, $2 file, $3 pool path
  mkdir -p "$DEBS"
  if [ ! -f "$DEBS/$2" ]; then
    echo "mono: fetching $2"
    curl -fsS -o "$DEBS/$2" "$BASE/$3" || { echo "mono: FETCH FAILED $2"; exit 2; }
  fi
  echo "$1  $DEBS/$2" | sha256sum -c --quiet - || { echo "mono: SHA256 MISMATCH $2"; exit 2; }
}

if [ ! -x "$OUT/root/usr/bin/mono-sgen" ]; then
  echo "$PKGS" | while read -r sha file path; do
    [ -n "${sha:-}" ] && fetch "$sha" "$file" "$path"
  done
  rm -rf "$TREE"
  mkdir -p "$TREE"
  fetch_ubuntu 7f9222342d3551d063bf651319ec397c39278eeeb9ab5950ae0e8c28ef0af431 \
    libstdc++6_10.5.0-1ubuntu1~20.04_amd64.deb \
    pool/main/g/gcc-10/libstdc++6_10.5.0-1ubuntu1~20.04_amd64.deb
  fetch_ubuntu bf67018f5303466eb468680b637a5d3f3bb17b9d44decf3d82d40b35babcd3e0 \
    zlib1g_1.2.11.dfsg-2ubuntu1.5_amd64.deb \
    pool/main/z/zlib/zlib1g_1.2.11.dfsg-2ubuntu1.5_amd64.deb
  for d in "$DEBS"/*.deb; do dpkg-deb -x "$d" "$TREE"; done

  # A FLAT 4.5 directory, no GAC symlink farm: FAT16 has no symlinks, and
  # mono resolves `System` etc. from the framework directory.  16 MiB total.
  R="$OUT/root"
  rm -rf "$R"
  mkdir -p "$R/usr/bin" "$R/usr/lib/mono/4.5" "$R/etc/mono/4.5" "$R/lib"
  cp "$TREE/usr/bin/mono-sgen" "$R/usr/bin/"
  cp "$TREE"/usr/lib/x86_64-linux-gnu/libstdc++.so.6.* "$R/lib/" 2>/dev/null || true
  cp "$TREE"/lib/x86_64-linux-gnu/libz.so.1.* "$R/lib/" 2>/dev/null || true
  ln -sf libstdc++.so.6.0.28 "$R/lib/libstdc++.so.6" 2>/dev/null || true
  ln -sf libz.so.1.2.11 "$R/lib/libz.so.1" 2>/dev/null || true
  ls -l "$R/lib" | head
  cp "$TREE/usr/lib/mono/4.5/mscorlib.dll" "$R/usr/lib/mono/4.5/"
  for f in "$TREE"/usr/lib/mono/gac/*/*/*.dll; do
    cp -n "$f" "$R/usr/lib/mono/4.5/" 2>/dev/null || true
  done
  cp "$TREE"/etc/mono/config "$R/etc/mono/" 2>/dev/null || true
  cp "$TREE"/etc/mono/4.5/*.config "$R/etc/mono/4.5/" 2>/dev/null || true
  # The intermediate dpkg tree must go: it contains the game's own Mono DSO
  # names (libMonoPosixHelper.so), and tools/rw/check-no-game-files.sh bans
  # those paths anywhere in the worktree.  Only the flat runtime tree stays.
  rm -rf "$TREE"
  du -sh "$R"
  "$R/usr/bin/mono-sgen" --version | head -1
fi

# hello.exe: compiled against MONO'S OWN mscorlib so the assembly targets the
# runtime under test.  mcs.exe from the same tree cannot see source files on
# this host (measured: it emits CS2001 with no stat/open for the path), so the
# build uses Roslyn from the .NET SDK with -nostdlib -r:mscorlib.dll.  The
# runtime exercised by the gate is still the official 6.12 build, untouched.
if [ ! -f "$OUT/hello.exe" ]; then
  CSC="$(ls -d /usr/lib/dotnet/sdk/*/Roslyn/bincore/csc.dll 2>/dev/null | head -1)"
  [ -n "$CSC" ] || { echo "mono: no Roslyn csc.dll (install the .NET SDK) — cannot build hello.exe"; exit 2; }
  mkdir -p "$OUT/src"
  cat > "$OUT/src/hello.cs" <<'EOF'
using System;
using System.Threading;

class Hello {
  static int counter;
  static void Main() {
    Console.WriteLine("Hello from C# on HobbyOS");
    Console.WriteLine("runtime: " + Environment.Version);
    Thread t = new Thread(delegate() { counter = 42; });
    t.Start();
    t.Join();
    Console.WriteLine("thread counter=" + counter);
    Console.WriteLine("HELLO-OK");
  }
}
EOF
  dotnet "$CSC" -nologo -nostdlib \
    -r:"$OUT/root/usr/lib/mono/4.5/mscorlib.dll" \
    -out:"$OUT/hello.exe" "$OUT/src/hello.cs" || { echo "mono: csc failed"; exit 2; }
fi
ls -l "$OUT/hello.exe"
echo "mono: pinned 6.12.0.199 tree at $OUT/root"
