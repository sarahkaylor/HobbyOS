#!/bin/sh
# tools/host/host-cc.sh -- the host compiler, with this host's SDK quirk handled.
#
# Used as HOST_CC by the Makefile's host-test rules, so every native compile and
# link goes through one place:
#
#     HOST_CC = tools/host/host-cc.sh
#
# Why: macOS 27's Command Line Tools SDK ships libSystem.B.tbd listing
# `arm64e.x1-macos`, which the clang in the same CLT cannot parse, so EVERY
# native link dies with "ld: tapi error: malformed file ... unknown architecture".
# tools/host/macos-sdk.sh already knows how to pin SDKROOT; it was sourced by the
# vendored cross stages but not by the host tests, so `make host_tests` failed on
# a defect the tree had already solved elsewhere.
#
# Off Darwin this used to be just `cc`, which is gcc on the linuxci runner.  That
# is wrong for this layer: the host-test rules and the xcompat DSO stages are
# written against CLANG's interface — `--target=x86_64-linux-gnu`,
# `-nostdlib++`, `-nostdinc++`, `-fuse-ld=lld` — so a gcc `cc` fails them with
# "unrecognized command-line option" before any test runs.  The project pins
# LLVM 21 everywhere (third_party/llvm-21.1.8, the CI PATH), so pick clang when
# it exists and fall back to cc only where it does not.  Apple's `cc` IS clang,
# so Darwin keeps its path (and its SDKROOT pinning) unchanged.
set -eu
_root=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=/dev/null
. "$_root/tools/host/macos-sdk.sh"
if [ -z "${HB_SDK_CC:-}" ] && [ "$(uname -s)" != Darwin ] && command -v clang >/dev/null 2>&1; then
  HB_SDK_CC=clang
fi
exec "${HB_SDK_CC:-cc}" "$@"
