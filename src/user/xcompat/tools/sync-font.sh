#!/usr/bin/env bash
# sync-font.sh -- regenerate src/user/xcompat/font8x8.h from the native
# desktop's system font (src/user_include/graphics/font.h).
#
# The compat DSO draws XDrawString with the same 8x8 glyphs the native Xlib
# uses, but it is built -nostdlib and must not include the userland graphics
# tree, so the table is vendored.  Running this after a font change keeps the
# two in lockstep; the host ABI test does not compare glyph data, so this
# script IS the drift control.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SRC="$ROOT/src/user_include/graphics/font.h"
DST="$ROOT/src/user/xcompat/font8x8.h"
[ -f "$SRC" ] || { echo "sync-font: no $SRC" >&2; exit 1; }

python3 - "$SRC" "$DST" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
start = text.index('static const uint8_t font8x8[96][8] = {')
end = text.index('};', start) + 2
table = text[start:end].replace('static const uint8_t font8x8[96][8] = {',
                                'static const uint8_t xc_font8x8[96][8] = {')
header = '''/*
 * font8x8.h - the 8x8 ASCII font the compat Xlib draws text with.
 *
 * VENDORED from src/user_include/graphics/font.h (the native desktop's system
 * font), so a compat client's XDrawString renders the same glyphs the native
 * Xlib does.  The compat DSO is built -nostdlib and must not include the
 * userland graphics tree, so the table is copied here.  Regenerate with
 *
 *     bash src/user/xcompat/tools/sync-font.sh
 *
 * Layout: 96 glyphs (ASCII 32..127), 8 rows each, MSB = leftmost pixel, row 7
 * is the baseline row (the same convention as the native library).
 */
#ifndef HOBBYOS_XCOMPAT_FONT8X8_H
#define HOBBYOS_XCOMPAT_FONT8X8_H

#include <stdint.h>

'''
open(dst, 'w').write(header + table + '\n\n#endif /* HOBBYOS_XCOMPAT_FONT8X8_H */\n')
print(f"sync-font: {dst} regenerated ({table.count(chr(10))} glyph rows)")
PY
