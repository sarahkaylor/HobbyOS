# shellcheck shell=bash
# tools/host/macos-sdk.sh -- make the macOS host toolchain able to link.
#
#   . tools/host/macos-sdk.sh      (source; idempotent, no-op off Darwin)
#
# The problem: macOS 27's Command Line Tools SDK ships
# /…/MacOSX27.x.sdk/usr/lib/libSystem.B.tbd with `arm64e.x1-macos` /
# `arm64e.x1-maccatalyst` in its arch list, which the clang shipped in the
# same CLT cannot parse.  Every *native* link then dies with
#
#     ld: tapi error: malformed file ... unknown architecture
#
# and each build system reports it in its own way -- autoconf says "native C
# compiler is not working", meson says "Compiler gcc cannot compile programs".
# It is not a project defect and it hits every host-side stage (ICU, the X11
# build-time generators, Mesa's host code generators), so the probe lives
# here and the stages source it.
#
# The fix: try the default compiler once.  If it cannot link, pin SDKROOT to
# the newest installed SDK that can (26.5 on the machine this was written on).
# On a healthy host the probe links and nothing is changed.

if [ "$(uname -s)" = "Darwin" ] && [ -z "${SDKROOT:-}" ]; then
  _hbsdk_probe="$(mktemp "${TMPDIR:-/tmp}/hbsdk-XXXXXX.c")"
  printf 'int main(void){return 0;}\n' > "$_hbsdk_probe"
  if ! ${HB_SDK_CC:-cc} "$_hbsdk_probe" -o "$_hbsdk_probe.out" 2>/dev/null; then
    for _hbsdk_root in /Library/Developer/CommandLineTools/SDKs \
                        /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs; do
      [ -d "$_hbsdk_root" ] || continue
      # newest first; skip the versionless symlinks (MacOSX.sdk -> 27.x.sdk),
      # the versioned directories are always present and the symlink would
      # only re-attempt the SDK that just failed.
      for _hbsdk in $(ls -1d "$_hbsdk_root"/MacOSX*.sdk 2>/dev/null | sort -r); do
        [ -L "$_hbsdk" ] && continue
        if SDKROOT="$_hbsdk" ${HB_SDK_CC:-cc} "$_hbsdk_probe" -o "$_hbsdk_probe.out" 2>/dev/null; then
          SDKROOT="$_hbsdk"; export SDKROOT
          echo "== host: default macOS SDK unparseable by the installed clang; pinned SDKROOT=$_hbsdk ==" >&2
          break 2
        fi
      done
    done
    [ -n "${SDKROOT:-}" ] || echo "== host: WARNING no linkable macOS SDK found; native stages will fail ==" >&2
  fi
  rm -f "$_hbsdk_probe" "$_hbsdk_probe.out"
fi
unset _hbsdk_probe _hbsdk _hbsdk_root
