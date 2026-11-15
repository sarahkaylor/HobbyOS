# Cyberpunk track — pinned sources

The executable copy of this table is `tools/cp/fetch-runtime.sh` (`PINS=`);
a row changes in both places or in neither. Everything here is fetched into
`third_party/cp-runtime/dl/` (git-ignored, `.gitignore` "Cyberpunk track"
block), never committed. Pinned 2026-11-14 on `linuxci`; every sha256 below was
computed from the downloaded file and re-verified by a second
`bash tools/cp/fetch-runtime.sh` run (idempotent: "(cached)").

Regenerate / verify: `bash tools/cp/fetch-runtime.sh` (exit 0 = all verified);
`bash tools/cp/fetch-runtime.sh --list` prints this table.

## Upstream runtime (docs/cyberpunk.md §2)

| Component | Version | URL | sha256 | License |
|---|---|---|---|---|
| Wine (upstream **stable**) | 11.0 (2026-01-13 signature date) | `https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz` | `c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701` | LGPL-2.1-or-later (`LICENSE`: "version 2.1 of the License, or (at your option) any later version") |
| ↳ detached signature | — | `…/11.0/wine-11.0.tar.xz.sign` | `854cf21b1fdb3f01f581413e5c9a50b3150d692895b15f5c58c190aff1adacfa` | — |
| ↳ signed sha512 list | — | `…/11.0/sha512sums.asc` | `d5116cb39bb546fe039f7bc34f6920d744e7cc43be254ecacd615cba427c7130` | — |
| vkd3d-proton | 3.1 (released 2026-10-28) | `https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v3.1/vkd3d-proton-3.1.tar.zst` | `b85ea8219b71093a3924913dd7aae6172333dc0c069983ed510816d67f02120b` | LGPL-2.1 (GitHub license API: `LGPL-2.1`, `LICENSE`) |
| DXVK | 3.1.1 (released 2026-09-15) | `https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz` | `40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608` | Zlib (GitHub license API: `Zlib`, `LICENSE`) |
| Vulkan-Loader | tag `vulkan-sdk-1.4.363.0` | `https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz` | `941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7` | Apache-2.0 (`LICENSE.txt`: "majority … Apache 2.0", exceptions more permissive) |
| Vulkan-Headers | tag `vulkan-sdk-1.4.363.0` (same tag as the loader) | `https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz` | `4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e` | Apache-2.0 and MIT (`LICENSE.md`) |

Provenance notes, each from a real query on 2026-11-14:

- **Wine.** `https://dl.winehq.org/wine/source/11.0/` lists only `wine-11.0`
  and its release candidates — no `11.0.x` maintenance release exists yet, so
  11.0 is the current stable. (The 11.x development series is at 11.22; not
  stable, not pinned.) **GPG verified**: the key was fetched from WineHQ's own
  GitLab, `https://gitlab.winehq.org/julliard.gpg` (key file sha256
  `11c6f3c9d746df8fa75348da94327fbd0a27eb903ab717d630c4f396816f655a`),
  fingerprint `DA23 579A 74D4 AD9A F9D3 F945 CEFA C8EA AF17 519D`
  ("Alexandre Julliard <julliard@winehq.org>"). `gpg --verify` of the
  `.sign`: `Good signature … Signature made Tue Jan 13 15:57:02 2026 UTC`;
  the clear-signed `sha512sums.asc` also verifies and its `wine-11.0.tar.xz`
  line (`a2c3db14…c29428d1b`) equals the tarball's sha512. The key is not
  certified by any key we trust (gpg's "not certified" warning) — trust rests
  on WineHQ's TLS-served GitLab key matching the signature. `fetch-runtime.sh`
  pins the fingerprint and fails on any other key.
- **vkd3d-proton / DXVK.** `api.github.com/repos/<repo>/releases/latest`
  (non-prerelease). GitHub's per-asset `digest` field (`sha256:…`) equals the
  sha256 computed locally for both. The release tarballs carry prebuilt PE
  DLLs only (`x64/d3d12.dll`, `x64/d3d12core.dll`; DXVK `x64/dxgi.dll`,
  `d3d11.dll`, `d3d10core.dll`, `d3d9.dll`, `d3d8.dll`, plus `x32/`) and **no
  license file**; the license is the upstream repository's.
- **Vulkan loader/headers.** The newest tags are `v1.4.364 … v1.4.368` and
  the newest SDK tag is `vulkan-sdk-1.4.363.0`; both repositories carry the
  SDK tag, so the pin is the SDK tag (the release-tested pairing). The
  archives are GitHub's generated tag tarballs (`/archive/refs/tags/`); their
  bytes are normally stable but not guaranteed by GitHub — if a re-fetch ever
  mismatches, re-pin from a `git clone --branch vulkan-sdk-1.4.363.0` and
  record the commit.

## glibc 2.31 sysroot set (AD-2: the RimWorld pin, reused)

| Package | URL | sha256 | Used for |
|---|---|---|---|
| `libc6_2.31-0ubuntu9.18_amd64.deb` | `http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb` | `1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0` | runtime + link (same row as `docs/rimworld-plan.md` §2) |
| `libc6-dev_2.31-0ubuntu9.18_amd64.deb` | `…/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb` | `11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a` | headers, crt objects (as `tools/mesa/build-hobbyos.sh:89-90`) |
| `linux-libc-dev_5.4.0-218.238_amd64.deb` | `…/l/linux/linux-libc-dev_5.4.0-218.238_amd64.deb` | `0aa210458df2ab7bc4536e8edf0f9423a7018e74e2eac0b5777fd2a36f170c2f` | UAPI headers (as `tools/mesa/build-hobbyos.sh:97-99`) |
| `libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb` | `…/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb` | `4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c` | `libgcc_s.so.1` runtime (ntdll.so needs it) |
| `libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb` (**new**) | `…/g/gcc-10/libgcc-10-dev_10.5.0-1ubuntu1~20.04_amd64.deb` | `ebe5e967fca32ea5811946180f7822ee749857861eadaa29514e65857ce3b897` (= `SHA256:` in focal-updates `main/binary-amd64/Packages.gz`) | the GCC crt*/libgcc{,_eh}.a clang links from inside the sysroot |

Licenses: glibc LGPL-2.1-or-later; libgcc GPL-3.0-or-later WITH
GCC-exception-3.1; linux-libc-dev GPL-2.0 WITH Linux-syscall-note.

Why `libgcc-10-dev` was added (measured, see `tools/cp/build-wine.sh` header):
with the host's gcc 15 `libgcc_eh.a` in the sysroot, configure's `-static-pie`
probe failed on `undefined reference to '_dl_find_object'` (a GLIBC_2.35
symbol) and Wine silently fell back to a fixed-address preloader. The focal
gcc-10 runtime objects match the 2.31 era.

## Not pinned here (and why)

- **Mesa** (lavapipe/venus) — already pinned at 26.2.6 by RimWorld; P5 lane.
- **X client libraries** (libX11/xcb/Xi/Xrandr/…) — the plan's P0 list names
  them; they are only needed by `winex11.drv` (P4), which this console-only
  phase does not build. Pin them in P4 against the same sysroot.
- **MinGW** — not needed: Wine 11.0's configure accepts plain `clang` as the
  PE cross compiler (`configure.ac:430-431`, `x86_64_CC=clang`) using its own
  headers and CRT, with `lld-link`/`llvm-dlltool` from the host's LLVM 21.1.8.
  Nothing was installed on linuxci.
