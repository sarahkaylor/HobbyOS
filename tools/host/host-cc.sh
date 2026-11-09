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
# Off Darwin the probe is a no-op and this is just `cc`.
set -eu
_root=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=/dev/null
. "$_root/tools/host/macos-sdk.sh"
exec "${HB_SDK_CC:-cc}" "$@"
