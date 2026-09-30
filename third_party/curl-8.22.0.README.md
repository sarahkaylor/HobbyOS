# libcurl 8.22.0 — vendored, host build + smoke (L6 batch 2)

The network stack for both browser tracks (browser.md AD-7; WebKit's curl
network backend + NetSurf's curl fetcher). Built here against the vendored
mbedTLS 3.6.7. Host build + smoke: `src/host/build_curl_host.sh` /
`src/host/curl_smoke.c`.

## Pin & provenance

- browser.md §2 lists this as a W0-time pin ("**libcurl** (8.x, mbedTLS
  backend)", §2 "To pin at W0"); W0.1 deferred the L6 tarballs to vendoring
  time — **recorded here: 8.22.0** (current release on curl.se as of
  2026-09-30; released 2026-09-02; still the 8.x series).
- URL: https://curl.se/download/curl-8.22.0.tar.xz
  (official PGP signature: https://curl.se/download/curl-8.22.0.tar.xz.asc)
- sha256 (computed; curl publishes no sha256 files — see Verification):
  `f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7`
- Committed (AD-11 layout):
  - `curl-8.22.0.tar.xz` (2 953 092 bytes) + `curl-8.22.0.tar.xz.asc` (488 B)
  - `curl-8.22.0.tar.xz.sums` — one `sha256sum -c`-compatible line
  - this README
- Extraction `third_party/curl-8.22.0/` is rebuilt from the tarball
  (gitignored); no patches.
- License: curl license (MIT-like; upstream COPYING).
- **Proposed §2 pin-table row** for the integrator:
  `curl-8.22.0.tar.xz | https://curl.se/download/curl-8.22.0.tar.xz | f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7`

## Verification (2026-09-30)

curl.se publishes no sha256 (only PGP signatures + the reproducible-release
script, https://curl.se/docs/verify.html), so the official-source check here
is the PGP signature:

```
$ gpg --verify curl-8.22.0.tar.xz.asc curl-8.22.0.tar.xz
gpg: Signature made Wed 02 Sep 2026 12:48:56 AM CDT
gpg:                using RSA key 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2
gpg: Good signature from "Daniel Stenberg <daniel@haxx.se>"
Primary key fingerprint: 27ED EAF2 2F3A BCEB 50DB  9A12 5CC9 08FD B71E 12C2
```

(Key fetched from keyserver.ubuntu.com per the verify page; fingerprint
matches the published one. The `.asc` is committed; the key is not — refetch
from the URL above.) The computed sha256 is recorded in `.sums` and the
tarball size matches the upstream asset listing. `sha256sum -c` runs on
every build below.

## Host build recipe

`src/host/build_curl_host.sh` (also builds the mbedTLS prefix first if
absent). Configure flags, exactly as recorded:

```
./configure \
  --prefix=obj/third_party/curl-8.22.0/prefix \
  --disable-shared --enable-static \
  --with-mbedtls=obj/third_party/mbedtls-3.6.7/prefix \
  --with-zlib \
  --without-brotli --without-zstd \
  --without-libidn2 --without-libpsl \
  --without-libssh2 --without-libssh \
  --without-nghttp2 --without-nghttp3 --without-ngtcp2 \
  --without-quiche \
  --disable-ldap --disable-ldaps \
  --disable-ftp --disable-rtsp --disable-dict --disable-telnet \
  --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
  --disable-gopher --disable-mqtt --disable-smb --disable-ipfs \
  --disable-websockets \
  --disable-unix-sockets \
  --disable-manual --disable-docs \
  --enable-http --enable-file
make -j4 && make install
```

- `--with-mbedtls=<prefix>` uses curl's path-based detection (link test
  `-lmbedtls -lmbedx509 -lmbedcrypto`); no pkg-config files required.
  configure reports `SSL: enabled (mbedTLS)` and adds the prefix to
  `CPPFLAGS`/`LDFLAGS`.
- `--enable-https` is **not** a curl option (https arrives with the TLS
  backend; passing it earns an "unrecognized options" warning — do not).
- Toolchain: gcc 15.2.0, GNU Make 4.4.1; full build+smoke ≈ 17 s wall.
- configure summary (recorded in the log): Protocols `file http https`;
  Features `alt-svc AsynchDNS HSTS HTTPS-proxy IPv6 Largefile libz SSL
  threadsafe`; resolver `POSIX threaded`; CA bundle default
  `/etc/ssl/certs/ca-certificates.crt`; `Shared=no, Static=yes`.
- Install prefix: `libcurl.a` (1 481 602 B), `include/curl/`,
  `bin/curl-config`, `lib/pkgconfig/libcurl.pc` (+ the `curl` tool).
- Raw log (fresh extract → smoke):
  `/home/sarah/hobbyos-lanes/l6-libs2/obj/third_party/logs/curl-8.22.0-build-smoke.log`

## Smoke

`src/host/curl_smoke.c` — (1) `curl_version()`/`curl_version_info()`
capability check: TLS backend string, protocol set, `CURL_VERSION_SSL`;
(2) one HTTPS HEAD request (real TLS handshake + HTTP round trip, cert
verification must be 0). Run (exit 0 = all pass):

```
curl_version() = libcurl/8.22.0 mbedTLS/3.6.7 zlib/1.3.1
version=8.22.0 ssl_version=mbedTLS/3.6.7
zlib=1.3.1 brotli=(none) zstd=(none)
features=0x5120028d protocol_count=3
protocols = file http https
PASS: TLS backend = mbedTLS/3.6.7
PASS: http + https + file present
PASS: CURL_VERSION_SSL set
HEAD https://example.com/ -> res=0 (No error) http=200 ssl_verify=0
PASS: HTTPS HEAD request (TLS handshake + HTTP round trip)
SMOKE PASS: libcurl capabilities + HTTPS HEAD OK
```

features 0x5120028d decodes against the vendored `curl.h` as: IPV6, SSL,
LIBZ, ASYNCHDNS, LARGEFILE, HTTPS_PROXY, ALTSVC, HSTS, THREADSAFE.

## Reuse notes (target builds / WPE port)

- Cross build: `CC=<cross-cc> JOBS=n src/host/build_curl_host.sh` with the
  mbedTLS prefix from `build_mbedtls_host.sh` (cross). Suggest adding at
  target time: `--with-ca-bundle=/CERTS/CA.PEM` (AD-7's single CA bundle)
  and, until P1 pthreads land, `--disable-threaded-resolver`.
- The smoke link is: `libcurl.a` + the three mbedTLS `.a` + `-lz -lpthread`.
- HobbyOS libc notes (for later configure passes): curl probes
  `geteuid`/`getpwuid` etc. at configure time (P6.1 adds sane stubs); 8.22
  has no `--disable-geteuid` option. `--disable-unix-sockets` is already in
  the recipe (HobbyOS has no AF_UNIX in F2; revisit if P4 IPC adds it).
- The `curl` CLI binary is built as a side effect and is handy for the
  fixture/E2E harnesses later.
