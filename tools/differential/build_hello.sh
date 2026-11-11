#!/usr/bin/env bash
# build_hello.sh — build the GB-007 differential `hello` probe.
#
# Default: native host cc (the skeleton / reference-compare path, so the T1
# self-test runs without a musl toolchain).  Set CC to the x86_64-musl cross
# compiler to build the HobbyOS-intel differential leg with the same source,
# e.g.:
#   CC=x86_64-linux-musl-gcc ./tools/differential/build_hello.sh
# Emits tools/differential/build/hello (chmod +x).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CC="${CC:-cc}"
# Native leg: this host's cc may not be able to link at all (macOS 27's CLT SDK
# ships libSystem.B.tbd with `arm64e.x1-*`, which its own clang cannot parse ->
# "ld: tapi error: malformed file").  The probe is a no-op on a healthy host and
# on Linux; without it the differential harness fails before it tests anything.
# shellcheck source=/dev/null
. "$(cd ../.. && pwd)/tools/host/macos-sdk.sh"
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "build_hello.sh: cross compiler '$CC' not found; set CC (native 'cc' is the default)" >&2
  exit 2
fi
"$CC" -O2 -static hello.c -o build/hello 2>/dev/null \
  || "$CC" -O2 hello.c -o build/hello   # some toolchains lack -static support
chmod +x build/hello
echo "built: $(pwd)/build/hello"
