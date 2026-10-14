# THEME-DESKTOP-REPORT — desktop + wallpaper → Chrome-light

Lane D (desktop + wallpaper) of the HobbyOS "Chrome light" theming workstream.
Branch `theme/ui-desktop` (base: main @ `722e3b4`).

## Conversion

`src/user/desktop.c` and the wallpaper block of `src/user/graphics/graphics.c`
now draw from the shared palette in `src/user_include/graphics/theme.h`.
Both files `#include "theme.h"`.

### before → after (desktop.c)

| Where | Old | New |
|---|---|---|
| `launch_app_named` window content bg | `COLOR(16, 18, 30)` | `THEME_SURFACE` |
| right-click app menu fill | `COLOR(200, 200, 200)` | `THEME_SURFACE` |
| right-click app menu item text | `COLOR(0, 0, 0)` | `THEME_TEXT` |
| Apps-menu (F1) fill | `COLOR(200, 200, 200)` | `THEME_SURFACE` |
| Apps-menu item text | `COLOR(0, 0, 0)` | `THEME_TEXT` |
| taskbar gradient (call kept) | `COLOR(48,54,74), COLOR(30,33,46)` | `THEME_SURFACE, THEME_SURFACE` |
| taskbar top hairline | `COLOR(96, 166, 255)` | `THEME_OUTLINE` |
| Apps button fill (open/closed) | `COLOR(96,166,255) / COLOR(64,74,100)` | `THEME_ACCENT / THEME_ACCENT_SOFT` |
| Apps button outline | `COLOR(140, 150, 180)` | `THEME_ACCENT` (open) / `THEME_ACCENT_HOVER` (closed) |
| Apps button text | `COLOR(240, 242, 248)` | `THEME_SURFACE` (open) / `THEME_ACCENT_DEEP` (closed) |
| taskbar window-button fill | `COLOR(52, 58, 78)` | `THEME_SURFACE_1` |
| window-button outline foc/unfoc | `COLOR(96,166,255) / COLOR(90,96,116)` | `THEME_ACCENT / THEME_OUTLINE` |
| window-button text foc/unfoc | `COLOR(226, 230, 240)` | `THEME_TEXT / THEME_TEXT_SUB` |
| clock text | `COLOR(220, 226, 240)` | `THEME_TEXT_SUB` |
| start menu fill | `COLOR(232, 234, 240)` | `THEME_SURFACE` |
| start menu outline | `COLOR(96, 166, 255)` | `THEME_OUTLINE` |
| start menu "more ^/v" hints | `COLOR(220, 226, 240)` | `THEME_TEXT_SUB` |
| start menu selected-row fill | `COLOR(96, 166, 255)` | `THEME_ACCENT_SOFT` |
| start menu selected-row text | `COLOR(255, 255, 255)` | `THEME_ACCENT_DEEP` |
| start menu normal-row text | `COLOR(20, 20, 24)` | `THEME_TEXT` |
| wallpaper full-span call | `COLOR(16,20,38), COLOR(44,56,96)` | `THEME_WALLPAPER_TOP, THEME_WALLPAPER_BOT` |

### before → after (graphics.c wallpaper block only)

| Constant | Old | New |
|---|---|---|
| `WALLPAPER_TOP_R/G/B` | 12, 16, 34 | 0xF8, 0xF9, 0xFA (grey-050, == `THEME_WALLPAPER_TOP`) |
| `WALLPAPER_BOT_R/G/B` | 34, 44, 72 | 0xDA, 0xDC, 0xE0 (grey-300, == `THEME_WALLPAPER_BOT`) |
| block comment | "deep navy … dark slate" | light-grey gradient description (mechanism text kept) |
| `WALLPAPER_BAND_GAIN` | 12 | 12 (unchanged; soft cool band kept) |

The 3 remaining `COLOR(r, g, b)` calls in graphics.c are the dynamic row/gradient
mechanism (band math + plain ramp interpolator) — kept unchanged per brief.

## Gates

### cstyle
```
$ python3 tools/cstyle.py format src/user/desktop.c src/user/graphics/graphics.c \
  && python3 tools/cstyle.py check src/user/desktop.c src/user/graphics/graphics.c
cstyle: 0 file(s) updated, 2 checked
cstyle: all 2 file(s) are properly formatted
```

### both-arch compile (zero warnings, -Wall -Wextra)
```
$ make obj/arm/desktop.o obj/arm/user_graphics.o
clang -O2 -Wall -Wextra -g ... --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 \
  -c src/user/desktop.c -o obj/arm/desktop.o
clang ... -c src/user/graphics/graphics.c -o obj/arm/user_graphics.o        # rc 0

$ make ARCH=intel obj/intel/desktop.o obj/intel/user_graphics.o
clang ... --target=x86_64-none-elf ... -mcmodel=large \
  -c src/user/desktop.c -o obj/intel/desktop.o
clang ... -c src/user/graphics/graphics.c -o obj/intel/user_graphics.o      # rc 0
```

### leftover literal scan
```
$ grep -n 'COLOR(' src/user/desktop.c src/user/graphics/graphics.c
src/user/graphics/graphics.c:174:  return COLOR(r, g, b);
src/user/graphics/graphics.c:287:  return COLOR(r, g, b);
src/user/graphics/graphics.c:307:    graphics_draw_hline(x, y + row, w, COLOR(r, g, b));
```
(only the pre-existing dynamic mechanism calls; no literal palette colors remain)

### make host_tests
(pending — appended after run)
