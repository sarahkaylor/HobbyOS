# mbedTLS 3.6.7 — vendored, host build + smoke (L6 batch 2)

The TLS backend for both browser tracks (browser.md AD-7: libcurl + mbedTLS).
This directory vendors the pinned source; the host build and its smoke live
in `src/host/build_mbedtls_host.sh` / `src/host/mbedtls_smoke.c`.

## Pin & provenance

- Pin: browser.md §2 (carry-over pin) — mbedtls-3.6.7.tar.bz2
  - URL: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  - sha256: `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`
- Committed (AD-11 layout: tarball + sums + README; extraction gitignored):
  - `mbedtls-3.6.7.tar.bz2` (5 473 689 bytes)
  - `mbedtls-3.6.7.tar.bz2.sums` — one `sha256sum -c`-compatible line
  - this README
- Extraction `third_party/mbedtls-3.6.7/` is rebuilt from the tarball
  (gitignored); no patches.
- License: dual Apache-2.0 OR GPL-2.0-or-later (upstream LICENSE).

## Verification (2026-09-30)

Computed sha256 matched **three independent records** — the browser.md §2 pin
and the official `mbedtls-3.6.7-sha256sum.txt` asset from the same GitHub
release:

```
$ sha256sum mbedtls-3.6.7.tar.bz2
a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6  mbedtls-3.6.7.tar.bz2

$ cat mbedtls-3.6.7-sha256sum.txt        # github.com/Mbed-TLS/mbedtls release asset
a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6  mbedtls-3.6.7.tar.bz2
```

Release asset size also matched (5 473 689 bytes). `sha256sum -c` on the
committed `.sums` is re-run on every build by the script below.

## Host build recipe

`src/host/build_mbedtls_host.sh` (idempotent; any cwd; `REPO`/`CC`/`JOBS`
overridable). Exact steps, no CMake needed:

```
# 1. verify the pinned tarball, unpack if the tree is missing
(cd third_party && sha256sum -c mbedtls-3.6.7.tar.bz2.sums)
tar -xjf third_party/mbedtls-3.6.7.tar.bz2 -C third_party

# 2. static library build (default mbedTLS config; `make lib` = library/ only;
#    the mbedTLS Makefile builds static-only unless SHARED is defined)
make -j4 lib

# 3. stage a prefix for curl (headers + the 3 static libs; skips programs/)
mkdir -p obj/third_party/mbedtls-3.6.7/prefix/{lib,include}
cp -r include/mbedtls include/psa obj/third_party/mbedtls-3.6.7/prefix/include/
cp library/libmbed{crypto,x509,tls}.a obj/third_party/mbedtls-3.6.7/prefix/lib/

# 4. smoke (see below), linked against the prefix
```

- Toolchain used: `cc` = gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0, GNU Make 4.4.1.
- Default config (`include/mbedtls/mbedtls_config.h`): SHA-256, entropy,
  CTR_DRBG and PSA crypto are all on.
- Resulting libs: `libmbedcrypto.a` 1 031 030 B, `libmbedtls.a` 559 264 B,
  `libmbedx509.a` 121 762 B; prefix 4.1 MiB total.
- Raw log (fresh extract → smoke):
  `/home/sarah/hobbyos-lanes/l6-libs2/obj/third_party/logs/mbedtls-3.6.7-build-smoke.log`

## Smoke

`src/host/mbedtls_smoke.c` — (1) the RNG path curl's mbedTLS backend uses
(entropy pool → CTR_DRBG seeded with a personalization string, two 32-byte
draws must differ and be non-zero), (2) SHA-256 against FIPS 180-4
known-answer vectors for `""` and `"abc"`. Run (exit 0 = all pass):

```
mbedTLS 3.6.7 (MBEDTLS_VERSION_NUMBER 0x03060700)
rng block 1 = 540486f71881803b688d9bbc49b6d0ba0329ef7ad02605c528091dfc511c7b0d
rng block 2 = faf7569d45de19fb5b8d66b9177ceedac6e4b85ecb6299b6ccaaef24629a2601
PASS: rng (two 32-byte draws differ, non-zero)
sha256(empty) = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
PASS: sha256(empty)
sha256(abc) = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
PASS: sha256(abc)
SMOKE PASS: mbedTLS RNG + SHA-256 vectors OK
```

## Reuse notes (target builds / WPE port)

- The script is host-only wrappers around the upstream build; the HobbyOS
  cross build reuses it as `CC=<cross-cc> JOBS=n REPO=<repo>
  src/host/build_mbedtls_host.sh` (no host-only flags are baked in).
- The GNU make build does **not** install pkg-config files (the CMake build
  does). curl consumes the prefix by path (`--with-mbedtls=<prefix>`),
  verified working; if the WPE CMake build wants `mbedtls.pc`, generate it
  from `pkgconfig/mbedtls.pc.in` or point CMake at the prefix.
- CMake is not present on this workstation (W0.5 noted `ruby`/`gperf` gaps;
  cmake is another); none of the three L6 batch-2 builds needs it.
- Target deltas TBD at port time: entropy source on HobbyOS is
  `SYS_GETRANDOM` (F1.4) — a custom `mbedtls_entropy` source replaces the
  `/dev/urandom` poll; TLS 1.3 client path is the one curl's backend uses.
