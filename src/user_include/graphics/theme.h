#ifndef THEME_H
#define THEME_H

/*
 * theme.h - the shared HobbyOS "Chrome light" color palette.
 *
 * The desktop chrome, window chrome, wallpaper and every color-painted
 * surface draw from these constants; no widget file may carry its own
 * color literals.  The palette is Google Chrome's light-mode UI, using
 * the exact values Google ships (fetched from the Chromium tree
 * 2026-10-14):
 *
 *   - ui/gfx/color_palette.h            (kGoogleBlue600 = #1A73E8,
 *                                        kGoogleGrey050..900, kGoogleRed600,
 *                                        kGoogleYellow600 ...)
 *   - chrome/browser/ui/color/chrome_color_mixer.cc
 *                                       (kColorToolbar = #FFFFFF,
 *                                        toolbar text = Grey 800,
 *                                        toolbar icons = Grey 700,
 *                                        bookmark bar = toolbar)
 *   - chrome/browser/themes/theme_properties.cc
 *                                       (frame / tab-strip = #DEE1E6,
 *                                        active tab + toolbar = #FFFFFF,
 *                                        inactive tab text = Grey 800,
 *                                        tab stroke = ~25% black)
 *
 * Element map (which surface uses what):
 *   desktop wallpaper   THEME_WALLPAPER_TOP -> THEME_WALLPAPER_BOT gradient
 *   taskbar             THEME_SURFACE with a THEME_OUTLINE hairline; the
 *                       Apps button is THEME_ACCENT (open) / soft blue
 *                       (closed); window buttons THEME_SURFACE_1 with an
 *                       THEME_ACCENT outline when focused
 *   start / popup menus THEME_SURFACE, THEME_OUTLINE border; selected row
 *                       THEME_ACCENT_SOFT fill + THEME_ACCENT_DEEP text
 *   window frame        THEME_OUTLINE (inactive), THEME_ACCENT (focused)
 *   title bar           THEME_SURFACE focused / THEME_SURFACE_1 inactive;
 *                       the 2px fold under it is THEME_ACCENT when focused
 *                       (Chrome's active-tab indicator) else THEME_OUTLINE
 *   close button        THEME_RED_SOFT fill, THEME_RED_EDGE border,
 *                       THEME_RED glyph
 *   window content      THEME_SURFACE background, THEME_TEXT ink
 *   terminal mode       THEME_TEXT ink; reverse video = THEME_TEXT field
 *                       with THEME_SURFACE ink; caret THEME_ACCENT
 *   mouse cursor        THEME_TEXT arrow with a THEME_SURFACE outline
 *   browser chrome      mirrors this palette in the WebKit port's
 *                       WK5Chrome.h (same Google hexes, few additions:
 *                       tab strip #DEE1E6, toolbar white)
 */

#include "graphics.h"

/* ---- Accents (Google Blues) ---- */
#define THEME_ACCENT        COLOR(0x1A, 0x73, 0xE8) /* Blue 600: focus, active, accent */
#define THEME_ACCENT_DEEP   COLOR(0x19, 0x67, 0xD2) /* Blue 700: selected text, pressed */
#define THEME_ACCENT_SOFT   COLOR(0xE8, 0xF0, 0xFE) /* Blue 050: selection fill */
#define THEME_ACCENT_HOVER  COLOR(0xD2, 0xE3, 0xFC) /* Blue 100: hover edge */

/* ---- Surfaces ---- */
#define THEME_SURFACE       COLOR(0xFF, 0xFF, 0xFF) /* toolbar, active tab, window content */
#define THEME_TABSTRIP      COLOR(0xDE, 0xE1, 0xE6) /* Chrome frame / tab-strip grey */
#define THEME_BG            COLOR(0xF8, 0xF9, 0xFA) /* Grey 050: base background */
#define THEME_SURFACE_1     COLOR(0xF1, 0xF3, 0xF4) /* Grey 100: menu bars, buttons, fields */
#define THEME_SURFACE_2     COLOR(0xE8, 0xEA, 0xED) /* Grey 200: alt fill, pressed */

/* ---- Lines ---- */
#define THEME_OUTLINE       COLOR(0xDA, 0xDC, 0xE0) /* Grey 300: borders, separators */
#define THEME_OUTLINE_DARK  COLOR(0xBD, 0xC1, 0xC6) /* Grey 400: hairlines, shadows */

/* ---- Text ---- */
#define THEME_TEXT          COLOR(0x20, 0x21, 0x24) /* Grey 900: primary ink */
#define THEME_TEXT_ALT      COLOR(0x3C, 0x40, 0x43) /* Grey 800: toolbar text */
#define THEME_TEXT_SUB      COLOR(0x5F, 0x63, 0x68) /* Grey 700: secondary ink, icons */
#define THEME_TEXT_DISABLED COLOR(0x9A, 0xA0, 0xA6) /* Grey 500: disabled ink */

/* ---- Status ---- */
#define THEME_RED           COLOR(0xD9, 0x30, 0x25) /* Red 600: close, danger */
#define THEME_RED_SOFT      COLOR(0xFC, 0xE8, 0xE6) /* Red 050: soft danger fill */
#define THEME_RED_EDGE      COLOR(0xF6, 0xAE, 0xA9) /* Red 200: soft danger border */
#define THEME_GOLD          COLOR(0xF9, 0xAB, 0x00) /* Yellow 600 (alt accent) */

/* ---- Wallpaper gradient (drawn by graphics.c; see graphics.h) ---- */
#define THEME_WALLPAPER_TOP COLOR(0xF8, 0xF9, 0xFA) /* Grey 050 at the top */
#define THEME_WALLPAPER_BOT COLOR(0xDA, 0xDC, 0xE0) /* Grey 300 at the taskbar */

#endif
