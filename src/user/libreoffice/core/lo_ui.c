/*
 * src/user/libreoffice/core/lo_ui.c
 *
 * Reusable Linux Adwaita / GTK3 UI Toolkit with Colibre/Breeze Icons
 * for the LibreOffice suite on HobbyOS.
 */

#include "lo_ui.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Helper for text drawing with AA or fallback */
static void ui_draw_text(lo_vcl_surface_t *surf, int x, int y, const char *text, int size_px, int font_id, uint32_t color) {
    if (lo_vcl_draw_text_aa(surf, x, y, text, size_px, font_id, color) != LO_OK) {
        lo_vcl_draw_text(surf, x, y, text, color);
    }
}

static int ui_measure_text(const char *text, int size_px, int font_id) {
    int w = lo_vcl_measure_text_aa(text, size_px, font_id);
    if (w <= 0 && text) {
        w = (int)strlen(text) * 8;
    }
    return w;
}

/* =========================================================================
 * Procedural Colibre / Breeze Multi-Color Icon Renderer (35+ Icons)
 * ========================================================================= */
void lo_ui_draw_icon(lo_vcl_surface_t *surf, int x, int y, lo_ui_icon_id_t icon_id, int size) {
    if (!surf || icon_id == LO_ICON_NONE) return;

    /* Center 16x16 inside 20x20 or scale */
    if (size > 16) {
        x += (size - 16) / 2;
        y += (size - 16) / 2;
    }

    switch (icon_id) {
        case LO_ICON_NEW:
            /* White sheet with folded top-right corner and blue text lines */
            lo_vcl_draw_rect(surf, x + 3, y + 1, 10, 14, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 3, y + 1, 10, 14, 0x005E5C64, false);
            /* Folded corner */
            lo_vcl_draw_triangle(surf, x + 9, y + 1, x + 12, y + 1, x + 12, y + 4, 0x00D0D0D0, true);
            lo_vcl_draw_line(surf, x + 9, y + 1, x + 12, y + 4, 0x005E5C64, 1);
            /* Text lines */
            lo_vcl_draw_line(surf, x + 5, y + 6, x + 10, y + 6, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 8, x + 11, y + 8, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 10, x + 11, y + 10, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 12, x + 9, y + 12, 0x001C71D8, 1);
            break;

        case LO_ICON_OPEN:
            /* Amber manilla folder with white paper peeking out */
            lo_vcl_draw_rect(surf, x + 2, y + 3, 12, 10, 0x00E5A50A, true);
            /* Peeking paper */
            lo_vcl_draw_rect(surf, x + 4, y + 2, 8, 4, 0x00FFFFFF, true);
            lo_vcl_draw_line(surf, x + 5, y + 3, x + 10, y + 3, 0x001C71D8, 1);
            /* Front flap */
            lo_vcl_draw_rect(surf, x + 1, y + 6, 14, 8, 0x00F5C211, true);
            lo_vcl_draw_rect(surf, x + 1, y + 6, 14, 8, 0x00C49000, false);
            break;

        case LO_ICON_SAVE:
            /* Blue 3.5" diskette with silver shutter and white label */
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x001C71D8, true);
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x0015539E, false);
            /* Silver sliding shutter */
            lo_vcl_draw_rect(surf, x + 4, y + 1, 8, 6, 0x00D0D0D0, true);
            lo_vcl_draw_rect(surf, x + 6, y + 2, 2, 3, 0x001C71D8, true);
            /* White label */
            lo_vcl_draw_rect(surf, x + 4, y + 8, 8, 6, 0x00FFFFFF, true);
            lo_vcl_draw_line(surf, x + 5, y + 10, x + 10, y + 10, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 12, x + 9, y + 12, 0x001C71D8, 1);
            break;

        case LO_ICON_SAVE_AS:
            lo_ui_draw_icon(surf, x, y, LO_ICON_SAVE, 16);
            /* Yellow pencil overlay */
            lo_vcl_draw_line(surf, x + 9, y + 9, x + 14, y + 4, 0x00F6D32D, 2);
            lo_vcl_draw_rect(surf, x + 13, y + 3, 2, 2, 0x00E01B24, true);
            break;

        case LO_ICON_PRINT:
            /* Grey chassis printer with white paper and green LED */
            lo_vcl_draw_rect(surf, x + 2, y + 5, 12, 7, 0x005E5C64, true);
            /* Top paper */
            lo_vcl_draw_rect(surf, x + 4, y + 1, 8, 4, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 4, y + 1, 8, 4, 0x009A9996, false);
            /* Bottom output paper */
            lo_vcl_draw_rect(surf, x + 4, y + 10, 8, 4, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 4, y + 10, 8, 4, 0x009A9996, false);
            /* Paper lines */
            lo_vcl_draw_line(surf, x + 5, y + 12, x + 10, y + 12, 0x001C71D8, 1);
            /* Green LED */
            lo_vcl_draw_rect(surf, x + 11, y + 7, 2, 2, 0x0033D17A, true);
            break;

        case LO_ICON_PDF:
            /* White document with red PDF ribbon/badge */
            lo_vcl_draw_rect(surf, x + 3, y + 1, 10, 14, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 3, y + 1, 10, 14, 0x00E01B24, false);
            /* Red banner */
            lo_vcl_draw_rect(surf, x + 2, y + 6, 12, 5, 0x00E01B24, true);
            /* Inner white 'PDF' hint */
            lo_vcl_draw_line(surf, x + 4, y + 8, x + 6, y + 8, 0x00FFFFFF, 1);
            lo_vcl_draw_line(surf, x + 8, y + 8, x + 11, y + 8, 0x00FFFFFF, 1);
            break;

        case LO_ICON_CUT:
            /* Crossed silver shears with blue finger loops */
            lo_vcl_draw_line(surf, x + 3, y + 3, x + 12, y + 12, 0x00C0C0C0, 2);
            lo_vcl_draw_line(surf, x + 12, y + 3, x + 3, y + 12, 0x00C0C0C0, 2);
            /* Pivot pin */
            lo_vcl_draw_rect(surf, x + 7, y + 7, 2, 2, 0x002E3436, true);
            /* Blue handles */
            lo_vcl_draw_rect(surf, x + 2, y + 11, 4, 3, 0x001C71D8, false);
            lo_vcl_draw_rect(surf, x + 10, y + 11, 4, 3, 0x001C71D8, false);
            break;

        case LO_ICON_COPY:
            /* Dual overlapping white pages with blue lines */
            lo_vcl_draw_rect(surf, x + 5, y + 1, 9, 11, 0x00E4E4E4, true);
            lo_vcl_draw_rect(surf, x + 5, y + 1, 9, 11, 0x009A9996, false);
            lo_vcl_draw_rect(surf, x + 2, y + 4, 9, 11, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 2, y + 4, 9, 11, 0x003584E4, false);
            lo_vcl_draw_line(surf, x + 4, y + 7, x + 8, y + 7, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 4, y + 10, x + 9, y + 10, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 4, y + 12, x + 7, y + 12, 0x003584E4, 1);
            break;

        case LO_ICON_PASTE:
            /* Brown clipboard with silver clip and white document */
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 13, 0x00865E3C, true);
            lo_vcl_draw_rect(surf, x + 4, y + 5, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_line(surf, x + 5, y + 8, x + 10, y + 8, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 10, x + 9, y + 10, 0x001C71D8, 1);
            /* Silver clip */
            lo_vcl_draw_rect(surf, x + 6, y + 1, 4, 3, 0x00C0C0C0, true);
            lo_vcl_draw_rect(surf, x + 6, y + 1, 4, 3, 0x005E5C64, false);
            break;

        case LO_ICON_UNDO:
            /* Curved counter-clockwise blue arrow */
            lo_vcl_draw_triangle(surf, x + 2, y + 7, x + 6, y + 4, x + 6, y + 10, 0x003584E4, true);
            lo_vcl_draw_line(surf, x + 6, y + 7, x + 10, y + 7, 0x003584E4, 2);
            lo_vcl_draw_line(surf, x + 10, y + 7, x + 12, y + 9, 0x003584E4, 2);
            lo_vcl_draw_line(surf, x + 12, y + 9, x + 12, y + 12, 0x003584E4, 2);
            break;

        case LO_ICON_REDO:
            /* Curved clockwise blue arrow */
            lo_vcl_draw_triangle(surf, x + 14, y + 7, x + 10, y + 4, x + 10, y + 10, 0x003584E4, true);
            lo_vcl_draw_line(surf, x + 10, y + 7, x + 6, y + 7, 0x003584E4, 2);
            lo_vcl_draw_line(surf, x + 6, y + 7, x + 4, y + 9, 0x003584E4, 2);
            lo_vcl_draw_line(surf, x + 4, y + 9, x + 4, y + 12, 0x003584E4, 2);
            break;

        case LO_ICON_BOLD:
            /* Bold serif 'B' */
            lo_vcl_draw_line(surf, x + 4, y + 2, x + 4, y + 13, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 4, y + 2, x + 9, y + 2, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 4, y + 7, x + 9, y + 7, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 4, y + 13, x + 10, y + 13, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 9, y + 3, x + 11, y + 5, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 11, y + 5, x + 9, y + 7, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 10, y + 8, x + 12, y + 10, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 12, y + 10, x + 10, y + 12, 0x002E3436, 2);
            break;

        case LO_ICON_ITALIC:
            /* Slanted serif 'I' */
            lo_vcl_draw_line(surf, x + 6, y + 2, x + 12, y + 2, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 10, y + 2, x + 6, y + 13, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 4, y + 13, x + 10, y + 13, 0x002E3436, 2);
            break;

        case LO_ICON_UNDERLINE:
            /* Underlined 'U' */
            lo_vcl_draw_line(surf, x + 4, y + 2, x + 4, y + 9, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 11, y + 2, x + 11, y + 9, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 5, y + 11, x + 10, y + 11, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 3, y + 14, x + 12, y + 14, 0x002E3436, 2);
            break;

        case LO_ICON_FONT_COLOR:
            /* Letter 'A' with red underline bar */
            lo_vcl_draw_line(surf, x + 4, y + 10, x + 8, y + 2, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 8, y + 2, x + 12, y + 10, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 6, y + 7, x + 10, y + 7, 0x002E3436, 1);
            lo_vcl_draw_rect(surf, x + 2, y + 12, 12, 3, 0x00E01B24, true);
            break;

        case LO_ICON_HIGHLIGHT_COLOR:
            /* Marker pen with yellow underline bar */
            lo_vcl_draw_line(surf, x + 10, y + 2, x + 4, y + 8, 0x005E5C64, 3);
            lo_vcl_draw_triangle(surf, x + 4, y + 8, x + 2, y + 10, x + 5, y + 11, 0x00F6D32D, true);
            lo_vcl_draw_rect(surf, x + 2, y + 12, 12, 3, 0x00F6D32D, true);
            break;

        case LO_ICON_ALIGN_LEFT:
            lo_vcl_draw_line(surf, x + 2, y + 3, x + 13, y + 3, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 6, x + 9, y + 6, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 9, x + 13, y + 9, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 12, x + 8, y + 12, 0x002E3436, 2);
            break;

        case LO_ICON_ALIGN_CENTER:
            lo_vcl_draw_line(surf, x + 2, y + 3, x + 13, y + 3, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 4, y + 6, x + 11, y + 6, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 9, x + 13, y + 9, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 5, y + 12, x + 10, y + 12, 0x002E3436, 2);
            break;

        case LO_ICON_ALIGN_RIGHT:
            lo_vcl_draw_line(surf, x + 2, y + 3, x + 13, y + 3, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 6, y + 6, x + 13, y + 6, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 9, x + 13, y + 9, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 7, y + 12, x + 13, y + 12, 0x002E3436, 2);
            break;

        case LO_ICON_ALIGN_JUSTIFY:
            lo_vcl_draw_line(surf, x + 2, y + 3, x + 13, y + 3, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 6, x + 13, y + 6, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 9, x + 13, y + 9, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 2, y + 12, x + 13, y + 12, 0x002E3436, 2);
            break;

        case LO_ICON_BULLETS:
            lo_vcl_draw_rect(surf, x + 2, y + 3, 2, 2, 0x003584E4, true);
            lo_vcl_draw_line(surf, x + 6, y + 4, x + 13, y + 4, 0x002E3436, 1);
            lo_vcl_draw_rect(surf, x + 2, y + 7, 2, 2, 0x003584E4, true);
            lo_vcl_draw_line(surf, x + 6, y + 8, x + 13, y + 8, 0x002E3436, 1);
            lo_vcl_draw_rect(surf, x + 2, y + 11, 2, 2, 0x003584E4, true);
            lo_vcl_draw_line(surf, x + 6, y + 12, x + 13, y + 12, 0x002E3436, 1);
            break;

        case LO_ICON_NUMBERS:
            lo_vcl_draw_line(surf, x + 3, y + 3, x + 3, y + 5, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 6, y + 4, x + 13, y + 4, 0x002E3436, 1);
            lo_vcl_draw_line(surf, x + 2, y + 7, x + 4, y + 7, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 4, y + 8, x + 2, y + 9, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 2, y + 9, x + 4, y + 9, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 6, y + 8, x + 13, y + 8, 0x002E3436, 1);
            lo_vcl_draw_line(surf, x + 2, y + 11, x + 4, y + 11, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 4, y + 12, x + 2, y + 12, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 4, y + 13, x + 2, y + 13, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 6, y + 12, x + 13, y + 12, 0x002E3436, 1);
            break;

        case LO_ICON_TABLE:
            /* Blue/white 3x3 grid with filled header */
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 12, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 4, 0x003584E4, true);
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 12, 0x0015539E, false);
            /* Grid lines */
            lo_vcl_draw_line(surf, x + 6, y + 2, x + 6, y + 14, 0x0015539E, 1);
            lo_vcl_draw_line(surf, x + 10, y + 2, x + 10, y + 14, 0x0015539E, 1);
            lo_vcl_draw_line(surf, x + 2, y + 6, x + 14, y + 6, 0x0015539E, 1);
            lo_vcl_draw_line(surf, x + 2, y + 10, x + 14, y + 10, 0x0015539E, 1);
            break;

        case LO_ICON_IMAGE:
            /* Picture frame with blue sky, yellow sun, and green hills */
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 12, 0x0062A0EA, true);
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 12, 0x005E5C64, false);
            /* Yellow sun */
            lo_vcl_draw_rect(surf, x + 10, y + 4, 2, 2, 0x00F6D32D, true);
            /* Green hills */
            lo_vcl_draw_triangle(surf, x + 2, y + 13, x + 6, y + 8, x + 11, y + 13, 0x0057E389, true);
            lo_vcl_draw_triangle(surf, x + 7, y + 13, x + 11, y + 9, x + 14, y + 13, 0x0033D17A, true);
            break;

        case LO_ICON_FIND:
            /* Blue magnifying glass */
            lo_vcl_draw_circle_aa(surf, x + 6, y + 6, 4, 0x003584E4, false);
            lo_vcl_draw_line(surf, x + 9, y + 9, x + 14, y + 14, 0x002E3436, 2);
            break;

        case LO_ICON_ZOOM_IN:
            lo_ui_draw_icon(surf, x, y, LO_ICON_FIND, 16);
            lo_vcl_draw_line(surf, x + 4, y + 6, x + 8, y + 6, 0x003584E4, 1);
            lo_vcl_draw_line(surf, x + 6, y + 4, x + 6, y + 8, 0x003584E4, 1);
            break;

        case LO_ICON_ZOOM_OUT:
            lo_ui_draw_icon(surf, x, y, LO_ICON_FIND, 16);
            lo_vcl_draw_line(surf, x + 4, y + 6, x + 8, y + 6, 0x003584E4, 1);
            break;

        case LO_ICON_FUNCTION:
        case LO_ICON_CALC_FX:
            /* 'fx' math symbol in purple */
            lo_vcl_draw_line(surf, x + 3, y + 3, x + 7, y + 3, 0x009141AC, 1);
            lo_vcl_draw_line(surf, x + 5, y + 3, x + 4, y + 13, 0x009141AC, 1);
            lo_vcl_draw_line(surf, x + 3, y + 7, x + 7, y + 7, 0x009141AC, 1);
            lo_vcl_draw_line(surf, x + 8, y + 7, x + 12, y + 12, 0x009141AC, 1);
            lo_vcl_draw_line(surf, x + 12, y + 7, x + 8, y + 12, 0x009141AC, 1);
            break;

        case LO_ICON_SUM:
        case LO_ICON_CALC_SIGMA:
            /* Greek sigma '∑' in blue */
            lo_vcl_draw_line(surf, x + 4, y + 2, x + 12, y + 2, 0x001C71D8, 2);
            lo_vcl_draw_line(surf, x + 4, y + 2, x + 8, y + 7, 0x001C71D8, 2);
            lo_vcl_draw_line(surf, x + 8, y + 7, x + 4, y + 13, 0x001C71D8, 2);
            lo_vcl_draw_line(surf, x + 4, y + 13, x + 12, y + 13, 0x001C71D8, 2);
            break;

        case LO_ICON_SHAPES:
            /* Overlapping circle and blue rectangle */
            lo_vcl_draw_rect(surf, x + 2, y + 2, 8, 8, 0x003584E4, false);
            lo_vcl_draw_circle_aa(surf, x + 10, y + 10, 4, 0x00E66100, false);
            break;

        case LO_ICON_SLIDESHOW:
            /* Monitor frame with green play triangle */
            lo_vcl_draw_rect(surf, x + 1, y + 2, 14, 9, 0x005E5C64, false);
            lo_vcl_draw_triangle(surf, x + 6, y + 4, x + 6, y + 9, x + 11, y + 6, 0x0033D17A, true);
            lo_vcl_draw_line(surf, x + 8, y + 11, x + 8, y + 13, 0x005E5C64, 1);
            lo_vcl_draw_line(surf, x + 5, y + 13, x + 11, y + 13, 0x005E5C64, 1);
            break;

        case LO_ICON_DATABASE:
        case LO_ICON_BASE_TABLES:
            /* 3 stacked silver/purple database cylinders */
            lo_vcl_draw_rect(surf, x + 3, y + 2, 10, 3, 0x00613583, true);
            lo_vcl_draw_rect(surf, x + 3, y + 6, 10, 3, 0x00613583, true);
            lo_vcl_draw_rect(surf, x + 3, y + 10, 10, 3, 0x00613583, true);
            lo_vcl_draw_line(surf, x + 3, y + 2, x + 3, y + 13, 0x002E3436, 1);
            lo_vcl_draw_line(surf, x + 12, y + 2, x + 12, y + 13, 0x002E3436, 1);
            break;

        case LO_ICON_BASE_QUERIES:
            lo_ui_draw_icon(surf, x, y, LO_ICON_FIND, 16);
            lo_vcl_draw_line(surf, x + 3, y + 12, x + 7, y + 12, 0x00613583, 1);
            break;

        case LO_ICON_BASE_FORMS:
            lo_vcl_draw_rect(surf, x + 3, y + 2, 10, 12, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 3, y + 2, 10, 12, 0x00613583, false);
            lo_vcl_draw_rect(surf, x + 5, y + 4, 6, 2, 0x00613583, true);
            lo_vcl_draw_line(surf, x + 5, y + 8, x + 11, y + 8, 0x009A9996, 1);
            lo_vcl_draw_line(surf, x + 5, y + 11, x + 9, y + 11, 0x009A9996, 1);
            break;

        case LO_ICON_BASE_REPORTS:
            lo_ui_draw_icon(surf, x, y, LO_ICON_NEW, 16);
            lo_vcl_draw_rect(surf, x + 8, y + 8, 5, 5, 0x00613583, true);
            break;

        /* Brand Module Icons */
        case LO_ICON_APP_SOFFICE:
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 12, 0x0033393B, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 8, 0x00FFFFFF, true);
            lo_vcl_draw_triangle(surf, x + 8, y + 4, x + 12, y + 4, x + 12, y + 8, 0x003584E4, true);
            break;

        case LO_ICON_APP_WRITER:
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x001C71D8, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_line(surf, x + 5, y + 6, x + 10, y + 6, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 8, x + 11, y + 8, 0x001C71D8, 1);
            lo_vcl_draw_line(surf, x + 5, y + 10, x + 9, y + 10, 0x001C71D8, 1);
            break;

        case LO_ICON_APP_CALC:
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x0026A269, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_line(surf, x + 4, y + 7, x + 11, y + 7, 0x0026A269, 1);
            lo_vcl_draw_line(surf, x + 4, y + 10, x + 11, y + 10, 0x0026A269, 1);
            lo_vcl_draw_line(surf, x + 8, y + 4, x + 8, y + 12, 0x0026A269, 1);
            break;

        case LO_ICON_APP_IMPRESS:
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x00E66100, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_triangle(surf, x + 6, y + 6, x + 6, y + 10, x + 10, y + 8, 0x00E66100, true);
            break;

        case LO_ICON_APP_DRAW:
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x00E5A50A, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 5, y + 5, 3, 3, 0x00E5A50A, true);
            lo_vcl_draw_circle_aa(surf, x + 9, y + 9, 2, 0x001C71D8, true);
            break;

        case LO_ICON_APP_MATH:
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x009141AC, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_line(surf, x + 5, y + 6, x + 10, y + 6, 0x009141AC, 1);
            lo_vcl_draw_line(surf, x + 7, y + 6, x + 6, y + 11, 0x009141AC, 1);
            lo_vcl_draw_line(surf, x + 9, y + 8, x + 11, y + 11, 0x009141AC, 1);
            break;

        case LO_ICON_APP_BASE:
            lo_vcl_draw_rect(surf, x + 2, y + 1, 12, 14, 0x00613583, true);
            lo_vcl_draw_rect(surf, x + 4, y + 4, 8, 9, 0x00FFFFFF, true);
            lo_vcl_draw_rect(surf, x + 5, y + 5, 6, 2, 0x00613583, true);
            lo_vcl_draw_rect(surf, x + 5, y + 8, 6, 2, 0x00613583, true);
            lo_vcl_draw_rect(surf, x + 5, y + 11, 6, 2, 0x00613583, true);
            break;

        /* Draw Vector Tool Icons */
        case LO_ICON_DRAW_SELECT:
            /* Arrow pointer */
            lo_vcl_draw_triangle(surf, x + 3, y + 2, x + 3, y + 12, x + 7, y + 9, 0x002E3436, true);
            lo_vcl_draw_line(surf, x + 6, y + 9, x + 11, y + 14, 0x002E3436, 2);
            break;

        case LO_ICON_DRAW_RECT:
            lo_vcl_draw_rect(surf, x + 2, y + 4, 12, 8, 0x003584E4, false);
            break;

        case LO_ICON_DRAW_ROUND_RECT:
            lo_vcl_draw_rounded_rect(surf, x + 2, y + 4, 12, 8, 2, 0x003584E4, 1);
            break;

        case LO_ICON_DRAW_ELLIPSE:
            lo_vcl_draw_circle_aa(surf, x + 8, y + 8, 5, 0x003584E4, false);
            break;

        case LO_ICON_DRAW_LINE:
            lo_vcl_draw_line(surf, x + 2, y + 13, x + 13, y + 2, 0x003584E4, 2);
            break;

        case LO_ICON_DRAW_POLYGON:
            lo_vcl_draw_triangle(surf, x + 8, y + 2, x + 2, y + 13, x + 14, y + 13, 0x003584E4, false);
            break;

        case LO_ICON_DRAW_BEZIER:
            lo_vcl_draw_line(surf, x + 2, y + 12, x + 6, y + 4, 0x003584E4, 2);
            lo_vcl_draw_line(surf, x + 6, y + 4, x + 10, y + 12, 0x003584E4, 2);
            lo_vcl_draw_line(surf, x + 10, y + 12, x + 14, y + 4, 0x003584E4, 2);
            break;

        case LO_ICON_DRAW_TEXT:
            lo_vcl_draw_rect(surf, x + 2, y + 2, 12, 12, 0x009A9A9A, false);
            lo_vcl_draw_line(surf, x + 5, y + 5, x + 11, y + 5, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 8, y + 5, x + 8, y + 11, 0x002E3436, 2);
            break;

        case LO_ICON_DRAW_CONNECTOR:
            /* Crisp stepped elbow connector: ┌─┘ with amber/orange terminal nodes */
            /* Start node (top-left) */
            lo_vcl_draw_rect(surf, x + 2, y + 3, 3, 3, 0x00E5A50A, true);
            /* Horizontal step top */
            lo_vcl_draw_line(surf, x + 5, y + 4, x + 8, y + 4, 0x001C71D8, 2);
            /* Vertical drop middle */
            lo_vcl_draw_line(surf, x + 8, y + 4, x + 8, y + 11, 0x001C71D8, 2);
            /* Horizontal step bottom */
            lo_vcl_draw_line(surf, x + 8, y + 11, x + 11, y + 11, 0x001C71D8, 2);
            /* End node (bottom-right) */
            lo_vcl_draw_rect(surf, x + 11, y + 10, 3, 3, 0x00E66100, true);
            break;

        case LO_ICON_CALC_EQUAL:
            lo_vcl_draw_line(surf, x + 3, y + 6, x + 13, y + 6, 0x002E3436, 2);
            lo_vcl_draw_line(surf, x + 3, y + 10, x + 13, y + 10, 0x002E3436, 2);
            break;

        case LO_ICON_CALC_CURRENCY:
            lo_vcl_draw_line(surf, x + 8, y + 2, x + 8, y + 14, 0x0026A269, 1);
            lo_vcl_draw_line(surf, x + 5, y + 4, x + 10, y + 4, 0x0026A269, 1);
            lo_vcl_draw_line(surf, x + 5, y + 8, x + 10, y + 8, 0x0026A269, 1);
            lo_vcl_draw_line(surf, x + 5, y + 12, x + 10, y + 12, 0x0026A269, 1);
            break;

        case LO_ICON_CALC_PERCENT:
            lo_vcl_draw_circle_aa(surf, x + 5, y + 5, 2, 0x002E3436, true);
            lo_vcl_draw_line(surf, x + 12, y + 4, x + 4, y + 12, 0x002E3436, 2);
            lo_vcl_draw_circle_aa(surf, x + 11, y + 11, 2, 0x002E3436, true);
            break;

        case LO_ICON_CALC_DECIMAL:
            lo_vcl_draw_rect(surf, x + 3, y + 11, 2, 2, 0x002E3436, true);
            lo_vcl_draw_rect(surf, x + 7, y + 6, 3, 6, 0x002E3436, false);
            lo_vcl_draw_rect(surf, x + 11, y + 6, 3, 6, 0x002E3436, false);
            break;

        default:
            lo_vcl_draw_rect(surf, x + 3, y + 3, 10, 10, 0x003584E4, false);
            break;
    }
}

/* =========================================================================
 * UI Initialization & Layout
 * ========================================================================= */
void lo_ui_init(lo_ui_state_t *ui, int w, int h) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->window_width = w;
    ui->window_height = h;
    ui->menu_bar.active_menu = -1;
    ui->menu_bar.hover_menu = -1;
    ui->menu_bar.hover_item = -1;
    ui->active_combo_row = -1;
    ui->active_combo_item = -1;

    /* Status bar default */
    ui->status_bar.zoom_percent = 100;
    ui->status_bar.zoom_min = 20;
    ui->status_bar.zoom_max = 400;
    strcpy(ui->status_bar.stats_text, "Ready");
    strcpy(ui->status_bar.mode_text, "Default");

    lo_ui_layout(ui, w, h);
}

static void lo_ui_modal_layout(lo_ui_state_t *ui);

void lo_ui_layout(lo_ui_state_t *ui, int w, int h) {
    if (!ui) return;
    ui->window_width = w;
    ui->window_height = h;

    /* 1. Menu Bar: (0, 0, w, 24) */
    ui->menu_bar.bounds.x = 0;
    ui->menu_bar.bounds.y = 0;
    ui->menu_bar.bounds.w = w;
    ui->menu_bar.bounds.h = 24;

    int mx = 8;
    for (int i = 0; i < ui->menu_bar.menu_count; i++) {
        lo_ui_menu_t *m = &ui->menu_bar.menus[i];
        int tw = ui_measure_text(m->title, 12, LO_VCL_FONT_SANS);
        m->bounds.x = mx;
        m->bounds.y = 2;
        m->bounds.w = tw + 14;
        m->bounds.h = 20;
        m->dropdown_w = 220;
        mx += m->bounds.w + 4;
    }

    /* 2. Toolbars: each 28px */
    int cur_y = 24;
    for (int t = 0; t < ui->toolbar_count; t++) {
        lo_ui_toolbar_t *tb = &ui->toolbars[t];
        tb->bounds.x = 0;
        tb->bounds.y = cur_y;
        tb->bounds.w = w;
        tb->bounds.h = 28;

        int item_x = tb->has_grip ? 16 : 8;
        for (int i = 0; i < tb->item_count; i++) {
            lo_ui_tool_item_t *item = &tb->items[i];
            item->bounds.y = cur_y + 2;
            item->bounds.h = 24;

            if (item->type == LO_UI_WIDGET_SEPARATOR) {
                item->bounds.x = item_x;
                item->bounds.w = 6;
                item_x += 6;
            } else if (item->type == LO_UI_WIDGET_COMBO) {
                item->bounds.x = item_x;
                item->bounds.w = item->combo.bounds.w > 0 ? item->combo.bounds.w : 120;
                item->combo.bounds = item->bounds;
                item->combo.popup_w = item->bounds.w + 20;
                item_x += item->bounds.w + 4;
            } else {
                item->bounds.x = item_x;
                item->bounds.w = 26;
                item_x += 28;
            }
        }
        cur_y += 28;
    }

    /* 3. Status Bar: (0, h - 22, w, 22) */
    ui->status_bar.bounds.x = 0;
    ui->status_bar.bounds.y = h - 22;
    ui->status_bar.bounds.w = w;
    ui->status_bar.bounds.h = 22;

    int right_x = w - 10;
    ui->status_bar.zoom_plus_rect.x = right_x - 55;
    ui->status_bar.zoom_plus_rect.y = h - 18;
    ui->status_bar.zoom_plus_rect.w = 14;
    ui->status_bar.zoom_plus_rect.h = 14;

    ui->status_bar.zoom_track_rect.x = right_x - 145;
    ui->status_bar.zoom_track_rect.y = h - 12;
    ui->status_bar.zoom_track_rect.w = 80;
    ui->status_bar.zoom_track_rect.h = 4;

    ui->status_bar.zoom_minus_rect.x = right_x - 165;
    ui->status_bar.zoom_minus_rect.y = h - 18;
    ui->status_bar.zoom_minus_rect.w = 14;
    ui->status_bar.zoom_minus_rect.h = 14;

    lo_ui_modal_layout(ui);
}

void lo_ui_get_doc_bounds(const lo_ui_state_t *ui, int *out_x, int *out_y, int *out_w, int *out_h) {
    if (!ui) return;
    int top_y = 24 + ui->toolbar_count * 28;
    int bot_y = ui->window_height - 22;
    if (out_x) *out_x = 0;
    if (out_y) *out_y = top_y;
    if (out_w) *out_w = ui->window_width;
    if (out_h) *out_h = (bot_y > top_y) ? (bot_y - top_y) : 0;
}

/* =========================================================================
 * UI Rendering
 * ========================================================================= */
void lo_ui_render_menu_bar(lo_ui_state_t *ui, lo_vcl_surface_t *surf) {
    if (!ui || !surf) return;
    lo_ui_rect_t b = ui->menu_bar.bounds;

    /* Background & bottom separator */
    lo_vcl_draw_rect(surf, b.x, b.y, b.w, b.h, LO_COLOR_BG_CHROME, true);
    lo_vcl_draw_line(surf, b.x, b.y + b.h - 1, b.x + b.w - 1, b.y + b.h - 1, LO_COLOR_SEPARATOR, 1);

    for (int i = 0; i < ui->menu_bar.menu_count; i++) {
        lo_ui_menu_t *m = &ui->menu_bar.menus[i];
        bool is_active = (i == ui->menu_bar.active_menu);
        bool is_hover = (i == ui->menu_bar.hover_menu);

        if (is_active) {
            lo_vcl_fill_rounded_rect(surf, m->bounds.x, m->bounds.y, m->bounds.w, m->bounds.h, 3, LO_COLOR_SELECT_ACCENT);
        } else if (is_hover) {
            lo_vcl_fill_rounded_rect(surf, m->bounds.x, m->bounds.y, m->bounds.w, m->bounds.h, 3, LO_COLOR_HOVER_PILL);
        }

        uint32_t text_col = is_active ? LO_COLOR_SELECT_TEXT : LO_COLOR_TEXT_PRIMARY;
        ui_draw_text(surf, m->bounds.x + 7, m->bounds.y + 4, m->title, 12, LO_VCL_FONT_SANS, text_col);

        /* Draw mnemonic underline */
        if (m->mnemonic) {
            const char *p = strchr(m->title, m->mnemonic);
            if (!p && m->mnemonic >= 'a' && m->mnemonic <= 'z') p = strchr(m->title, m->mnemonic - 32);
            if (!p && m->mnemonic >= 'A' && m->mnemonic <= 'Z') p = strchr(m->title, m->mnemonic + 32);
            if (p) {
                int prefix_len = (int)(p - m->title);
                char temp[32];
                if (prefix_len > 0 && prefix_len < 31) {
                    strncpy(temp, m->title, prefix_len);
                    temp[prefix_len] = '\0';
                } else {
                    temp[0] = '\0';
                }
                int off_x = ui_measure_text(temp, 12, LO_VCL_FONT_SANS);
                char mchar[2] = { *p, '\0' };
                int mw = ui_measure_text(mchar, 12, LO_VCL_FONT_SANS);
                lo_vcl_draw_line(surf, m->bounds.x + 7 + off_x, m->bounds.y + 17,
                                 m->bounds.x + 7 + off_x + mw, m->bounds.y + 17, text_col, 1);
            }
        }
    }
}

void lo_ui_render_toolbars(lo_ui_state_t *ui, lo_vcl_surface_t *surf) {
    if (!ui || !surf) return;

    for (int t = 0; t < ui->toolbar_count; t++) {
        lo_ui_toolbar_t *tb = &ui->toolbars[t];
        uint32_t bg = (t == 0) ? LO_COLOR_BG_CHROME : LO_COLOR_BG_SECONDARY;
        if (tb->bg_color != 0) bg = tb->bg_color;

        lo_vcl_draw_rect(surf, tb->bounds.x, tb->bounds.y, tb->bounds.w, tb->bounds.h, bg, true);
        lo_vcl_draw_line(surf, tb->bounds.x, tb->bounds.y + tb->bounds.h - 1,
                         tb->bounds.x + tb->bounds.w - 1, tb->bounds.y + tb->bounds.h - 1, LO_COLOR_SEPARATOR, 1);

        /* Drag grip: 2 columns of 3 subtle dots */
        if (tb->has_grip) {
            for (int r = 0; r < 3; r++) {
                int gy = tb->bounds.y + 8 + r * 4;
                lo_vcl_draw_rect(surf, 4, gy, 2, 2, LO_COLOR_GRIP, true);
                lo_vcl_draw_rect(surf, 8, gy, 2, 2, LO_COLOR_GRIP, true);
            }
        }

        for (int i = 0; i < tb->item_count; i++) {
            lo_ui_tool_item_t *item = &tb->items[i];
            lo_ui_rect_t b = item->bounds;

            if (item->type == LO_UI_WIDGET_SEPARATOR) {
                lo_vcl_draw_line(surf, b.x + 2, b.y + 2, b.x + 2, b.y + b.h - 3, LO_COLOR_SEPARATOR, 1);
            } else if (item->type == LO_UI_WIDGET_COMBO) {
                lo_vcl_fill_rounded_rect(surf, b.x, b.y, b.w, b.h, 3, LO_COLOR_BG_WHITE);
                lo_vcl_draw_rounded_rect(surf, b.x, b.y, b.w, b.h, 3, LO_COLOR_SEPARATOR, 1);

                const char *sel = (item->combo.selected_index >= 0 && item->combo.selected_index < item->combo.item_count)
                                  ? item->combo.items[item->combo.selected_index] : "";
                const char *draw_sel = sel;
                char combo_buf[64];
                int max_text_w = b.w - 22; /* Leave >= 16px right padding for dropdown triangle */
                if (max_text_w < 10) max_text_w = 10;
                if (sel && sel[0]) {
                    int tw = ui_measure_text(sel, 11, LO_VCL_FONT_SANS);
                    if (tw > max_text_w) {
                        size_t len = strlen(sel);
                        if (len >= sizeof(combo_buf) - 4) len = sizeof(combo_buf) - 5;
                        while (len > 0) {
                            memcpy(combo_buf, sel, len);
                            combo_buf[len] = '\0';
                            strcat(combo_buf, "...");
                            if (ui_measure_text(combo_buf, 11, LO_VCL_FONT_SANS) <= max_text_w) break;
                            len--;
                        }
                        if (len == 0) {
                            len = strlen(sel);
                            if (len >= sizeof(combo_buf)) len = sizeof(combo_buf) - 1;
                            while (len > 0) {
                                memcpy(combo_buf, sel, len);
                                combo_buf[len] = '\0';
                                if (ui_measure_text(combo_buf, 11, LO_VCL_FONT_SANS) <= max_text_w) break;
                                len--;
                            }
                        }
                        draw_sel = combo_buf;
                    }
                }
                ui_draw_text(surf, b.x + 6, b.y + 6, draw_sel, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

                /* Down arrow ▾ */
                lo_vcl_draw_triangle(surf, b.x + b.w - 12, b.y + 9, b.x + b.w - 6, b.y + 9, b.x + b.w - 9, b.y + 13, LO_COLOR_TEXT_SECONDARY, true);
            } else {
                /* Button / Toggle */
                if (item->is_active) {
                    lo_vcl_fill_rounded_rect(surf, b.x, b.y, b.w, b.h, 3, LO_COLOR_ACTIVE_PILL);
                    lo_vcl_draw_rounded_rect(surf, b.x, b.y, b.w, b.h, 3, LO_COLOR_ACTIVE_BORDER, 1);
                } else if (item->is_hovered) {
                    lo_vcl_fill_rounded_rect(surf, b.x, b.y, b.w, b.h, 3, LO_COLOR_HOVER_PILL);
                }

                lo_ui_draw_icon(surf, b.x + 5, b.y + 4, item->icon_id, 16);
            }
        }
    }
}

void lo_ui_render_status_bar(lo_ui_state_t *ui, lo_vcl_surface_t *surf) {
    if (!ui || !surf) return;
    lo_ui_status_bar_t *sb = &ui->status_bar;
    lo_ui_rect_t b = sb->bounds;

    /* Background and top divider */
    lo_vcl_draw_rect(surf, b.x, b.y, b.w, b.h, LO_COLOR_BG_SECONDARY, true);
    lo_vcl_draw_line(surf, b.x, b.y, b.x + b.w - 1, b.y, LO_COLOR_SEPARATOR, 1);

    /* Dynamic divider calculations */
    int div2_x = b.w - 230;
    int div1_x = 240;
    if (sb->stats_text[0]) {
        int st_w = ui_measure_text(sb->stats_text, 11, LO_VCL_FONT_SANS);
        if (b.x + st_w + 24 > div1_x) {
            div1_x = b.x + st_w + 24;
        }
    }
    if (div1_x > div2_x - 20) {
        div1_x = div2_x - 20;
    }

    /* Left stats text (with truncation/ellipsizing safety if extremely long) */
    const char *draw_stats = sb->stats_text;
    char stats_buf[128];
    int max_stats_w = div1_x - b.x - 16;
    if (sb->stats_text[0] && max_stats_w > 10) {
        int st_w = ui_measure_text(sb->stats_text, 11, LO_VCL_FONT_SANS);
        if (st_w > max_stats_w) {
            size_t len = strlen(sb->stats_text);
            if (len >= sizeof(stats_buf) - 4) len = sizeof(stats_buf) - 5;
            while (len > 0) {
                memcpy(stats_buf, sb->stats_text, len);
                stats_buf[len] = '\0';
                strcat(stats_buf, "...");
                if (ui_measure_text(stats_buf, 11, LO_VCL_FONT_SANS) <= max_stats_w) break;
                len--;
            }
            draw_stats = stats_buf;
        }
    }
    ui_draw_text(surf, b.x + 12, b.y + 4, draw_stats, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

    /* Dividers and center mode text */
    if (div1_x < div2_x) {
        lo_vcl_draw_line(surf, div1_x, b.y + 3, div1_x, b.y + b.h - 3, LO_COLOR_SEPARATOR, 1);

        const char *draw_mode = sb->mode_text;
        char mode_buf[128];
        int max_mode_w = div2_x - div1_x - 20;
        if (sb->mode_text[0] && max_mode_w > 10) {
            int mw = ui_measure_text(sb->mode_text, 11, LO_VCL_FONT_SANS);
            if (mw > max_mode_w) {
                size_t len = strlen(sb->mode_text);
                if (len >= sizeof(mode_buf) - 4) len = sizeof(mode_buf) - 5;
                while (len > 0) {
                    memcpy(mode_buf, sb->mode_text, len);
                    mode_buf[len] = '\0';
                    strcat(mode_buf, "...");
                    if (ui_measure_text(mode_buf, 11, LO_VCL_FONT_SANS) <= max_mode_w) break;
                    len--;
                }
                draw_mode = mode_buf;
            }
        }
        ui_draw_text(surf, div1_x + 10, b.y + 4, draw_mode, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_SECONDARY);
    }
    if (div2_x > div1_x) {
        lo_vcl_draw_line(surf, div2_x, b.y + 3, div2_x, b.y + b.h - 3, LO_COLOR_SEPARATOR, 1);
    }

    /* Zoom slider section */
    /* '-' button */
    lo_ui_rect_t mr = sb->zoom_minus_rect;
    lo_vcl_fill_rounded_rect(surf, mr.x, mr.y, mr.w, mr.h, 2, LO_COLOR_HOVER_PILL);
    lo_vcl_draw_line(surf, mr.x + 3, mr.y + 7, mr.x + 10, mr.y + 7, LO_COLOR_TEXT_PRIMARY, 1);

    /* Track */
    lo_ui_rect_t tr = sb->zoom_track_rect;
    lo_vcl_fill_rounded_rect(surf, tr.x, tr.y, tr.w, tr.h, 2, LO_COLOR_SEPARATOR);
    /* Center tick at 100% */
    int center_tick_x = tr.x + tr.w * (100 - sb->zoom_min) / (sb->zoom_max - sb->zoom_min);
    lo_vcl_draw_line(surf, center_tick_x, tr.y - 2, center_tick_x, tr.y + tr.h + 2, LO_COLOR_TEXT_SECONDARY, 1);

    /* Draggable thumb pill */
    int clamped_zoom = sb->zoom_percent;
    if (clamped_zoom < sb->zoom_min) clamped_zoom = sb->zoom_min;
    if (clamped_zoom > sb->zoom_max) clamped_zoom = sb->zoom_max;
    int thumb_x = tr.x + tr.w * (clamped_zoom - sb->zoom_min) / (sb->zoom_max - sb->zoom_min);
    lo_vcl_fill_rounded_rect(surf, thumb_x - 3, tr.y - 4, 6, 12, 2, LO_COLOR_SELECT_ACCENT);

    /* '+' button */
    lo_ui_rect_t pr = sb->zoom_plus_rect;
    lo_vcl_fill_rounded_rect(surf, pr.x, pr.y, pr.w, pr.h, 2, LO_COLOR_HOVER_PILL);
    lo_vcl_draw_line(surf, pr.x + 3, pr.y + 7, pr.x + 10, pr.y + 7, LO_COLOR_TEXT_PRIMARY, 1);
    lo_vcl_draw_line(surf, pr.x + 7, pr.y + 3, pr.x + 7, pr.y + 10, LO_COLOR_TEXT_PRIMARY, 1);

    /* Zoom percent text */
    char zbuf[16];
    snprintf(zbuf, sizeof(zbuf), "%d%%", sb->zoom_percent);
    ui_draw_text(surf, pr.x + pr.w + 6, b.y + 4, zbuf, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
}

void lo_ui_render_popups(lo_ui_state_t *ui, lo_vcl_surface_t *surf) {
    if (!ui || !surf) return;

    /* 1. Menu Bar Dropdown */
    if (ui->menu_bar.active_menu >= 0 && ui->menu_bar.active_menu < ui->menu_bar.menu_count) {
        lo_ui_menu_t *m = &ui->menu_bar.menus[ui->menu_bar.active_menu];
        int popup_x = m->bounds.x;
        int popup_y = 24;
        int popup_w = m->dropdown_w > 0 ? m->dropdown_w : 220;
        int item_h = 24;
        int total_h = 6;
        for (int i = 0; i < m->item_count; i++) {
            total_h += m->items[i].is_separator ? 7 : item_h;
        }

        if (popup_x + popup_w > ui->window_width) {
            popup_x = ui->window_width - popup_w - 4;
        }

        /* Drop shadow */
        lo_vcl_draw_shadow(surf, popup_x, popup_y, popup_w, total_h, 3, 4, LO_COLOR_SHADOW);

        /* White background & border */
        lo_vcl_fill_rounded_rect(surf, popup_x, popup_y, popup_w, total_h, 3, LO_COLOR_MENU_BG);
        lo_vcl_draw_rounded_rect(surf, popup_x, popup_y, popup_w, total_h, 3, LO_COLOR_MENU_BORDER, 1);

        int cur_iy = popup_y + 4;
        for (int i = 0; i < m->item_count; i++) {
            lo_ui_menu_item_t *it = &m->items[i];
            if (it->is_separator) {
                lo_vcl_draw_line(surf, popup_x + 8, cur_iy + 3, popup_x + popup_w - 8, cur_iy + 3, LO_COLOR_HOVER_PILL, 1);
                cur_iy += 7;
                continue;
            }

            bool is_hover = (i == ui->menu_bar.hover_item);
            if (is_hover) {
                lo_vcl_draw_rect(surf, popup_x + 2, cur_iy, popup_w - 4, item_h, LO_COLOR_SELECT_ACCENT, true);
            }

            uint32_t text_col = is_hover ? LO_COLOR_SELECT_TEXT : (it->enabled ? LO_COLOR_TEXT_PRIMARY : LO_COLOR_TEXT_DISABLED);
            uint32_t sc_col   = is_hover ? LO_COLOR_SELECT_TEXT : LO_COLOR_TEXT_SECONDARY;

            /* Icon */
            if (it->icon_id != LO_ICON_NONE) {
                lo_ui_draw_icon(surf, popup_x + 6, cur_iy + 4, it->icon_id, 16);
            }

            /* Label */
            ui_draw_text(surf, popup_x + 30, cur_iy + 5, it->text, 12, LO_VCL_FONT_SANS, text_col);

            /* Shortcut hint */
            if (it->shortcut && it->shortcut[0]) {
                int sc_w = ui_measure_text(it->shortcut, 11, LO_VCL_FONT_SANS);
                ui_draw_text(surf, popup_x + popup_w - sc_w - 12, cur_iy + 5, it->shortcut, 11, LO_VCL_FONT_SANS, sc_col);
            }

            cur_iy += item_h;
        }
    }

    /* 2. Combobox Dropdown */
    if (ui->active_combo_row >= 0 && ui->active_combo_item >= 0) {
        lo_ui_tool_item_t *ti = &ui->toolbars[ui->active_combo_row].items[ui->active_combo_item];
        lo_ui_combo_t *cb = &ti->combo;
        int px = cb->bounds.x;
        int py = cb->bounds.y + cb->bounds.h + 2;
        int pw = cb->popup_w > 0 ? cb->popup_w : cb->bounds.w;
        int ph = cb->item_count * 22 + 6;

        lo_vcl_draw_shadow(surf, px, py, pw, ph, 3, 4, LO_COLOR_SHADOW);
        lo_vcl_fill_rounded_rect(surf, px, py, pw, ph, 3, LO_COLOR_MENU_BG);
        lo_vcl_draw_rounded_rect(surf, px, py, pw, ph, 3, LO_COLOR_MENU_BORDER, 1);

        for (int i = 0; i < cb->item_count; i++) {
            int iy = py + 3 + i * 22;
            bool is_hover = (i == cb->hover_index);
            if (is_hover) {
                lo_vcl_draw_rect(surf, px + 2, iy, pw - 4, 20, LO_COLOR_SELECT_ACCENT, true);
            }
            uint32_t tc = is_hover ? LO_COLOR_SELECT_TEXT : LO_COLOR_TEXT_PRIMARY;
            ui_draw_text(surf, px + 8, iy + 4, cb->items[i], 11, LO_VCL_FONT_SANS, tc);
        }
    }

    /* 3. Modal Dialog Overlay */
    if (ui->modal.type != LO_UI_MODAL_NONE) {
        lo_ui_modal_t *m = &ui->modal;
        int sw = ui->window_width > 0 ? ui->window_width : lo_vcl_surface_get_width(surf);
        int sh = ui->window_height > 0 ? ui->window_height : lo_vcl_surface_get_height(surf);

        /* Scrim: semi-transparent dark backdrop */
        lo_vcl_draw_rect(surf, 0, 0, sw, sh, 0x66000000, true);

        int dw = 420;
        int dh = (m->type == LO_UI_MODAL_ABOUT || m->type == LO_UI_MODAL_PRINT || m->type == LO_UI_MODAL_INFO) ? 220 : 180;
        int dx = (sw - dw) / 2;
        int dy = (sh - dh) / 2;
        m->dialog_rect.x = dx;
        m->dialog_rect.y = dy;
        m->dialog_rect.w = dw;
        m->dialog_rect.h = dh;

        /* Drop shadow & Card background */
        lo_vcl_draw_shadow(surf, dx, dy, dw, dh, 6, 8, LO_COLOR_SHADOW);
        lo_vcl_fill_rounded_rect(surf, dx, dy, dw, dh, 6, LO_COLOR_MENU_BG);
        lo_vcl_draw_rounded_rect(surf, dx, dy, dw, dh, 6, LO_COLOR_MENU_BORDER, 1);

        /* Header bar */
        lo_vcl_fill_rounded_rect(surf, dx + 1, dy + 1, dw - 2, 34, 5, LO_COLOR_BG_CHROME);
        lo_vcl_draw_line(surf, dx, dy + 35, dx + dw, dy + 35, LO_COLOR_MENU_BORDER, 1);
        ui_draw_text(surf, dx + 16, dy + 9, m->title, 13, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

        /* Content area depending on type */
        if (m->type == LO_UI_MODAL_ABOUT) {
            char body_copy[512];
            snprintf(body_copy, sizeof(body_copy), "%s", m->info_body);
            char *line = body_copy;
            int ly = dy + 48;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                ui_draw_text(surf, dx + 20, ly, line, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
                ly += 20;
                if (!nl) break;
                line = nl + 1;
            }
            int btn_w = 80;
            int btn_h = 28;
            int btn_x = dx + (dw - btn_w) / 2;
            int btn_y = dy + dh - 42;
            m->ok_rect = (lo_ui_rect_t){ btn_x, btn_y, btn_w, btn_h };
            m->cancel_rect = (lo_ui_rect_t){ 0, 0, 0, 0 };
            lo_vcl_fill_rounded_rect(surf, btn_x, btn_y, btn_w, btn_h, 3, LO_COLOR_SELECT_ACCENT);
            ui_draw_text(surf, btn_x + 28, btn_y + 6, "OK", 12, LO_VCL_FONT_SANS, LO_COLOR_SELECT_TEXT);
        } else if (m->type == LO_UI_MODAL_INFO) {
            char body_copy[512];
            snprintf(body_copy, sizeof(body_copy), "%s", m->info_body);
            char *line = body_copy;
            int ly = dy + 48;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                ui_draw_text(surf, dx + 20, ly, line, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
                ly += 20;
                if (!nl) break;
                line = nl + 1;
            }
            int btn_w = 80;
            int btn_h = 28;
            int btn_x = dx + (dw - btn_w) / 2;
            int btn_y = dy + dh - 42;
            m->ok_rect = (lo_ui_rect_t){ btn_x, btn_y, btn_w, btn_h };
            m->cancel_rect = (lo_ui_rect_t){ 0, 0, 0, 0 };
            lo_vcl_fill_rounded_rect(surf, btn_x, btn_y, btn_w, btn_h, 3, LO_COLOR_SELECT_ACCENT);
            ui_draw_text(surf, btn_x + 28, btn_y + 6, "OK", 12, LO_VCL_FONT_SANS, LO_COLOR_SELECT_TEXT);
        } else if (m->type == LO_UI_MODAL_PRINT) {
            char pinfo[256];
            snprintf(pinfo, sizeof(pinfo), "Document: %s", m->print_doc_name[0] ? m->print_doc_name : "Untitled");
            ui_draw_text(surf, dx + 20, dy + 48, pinfo, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
            snprintf(pinfo, sizeof(pinfo), "Pages / Sheets / Slides: %d", m->print_page_count > 0 ? m->print_page_count : 1);
            ui_draw_text(surf, dx + 20, dy + 70, pinfo, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
            snprintf(pinfo, sizeof(pinfo), "Output Destination: %s", m->print_spool_path[0] ? m->print_spool_path : "/home/PRINT.TXT");
            ui_draw_text(surf, dx + 20, dy + 92, pinfo, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_SECONDARY);
            ui_draw_text(surf, dx + 20, dy + 116, "Click Print to spool document to destination.", 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

            int btn_w = 80;
            int btn_h = 28;
            int btn_y = dy + dh - 42;
            int cancel_x = dx + dw - 180;
            int ok_x = dx + dw - 95;
            m->cancel_rect = (lo_ui_rect_t){ cancel_x, btn_y, btn_w, btn_h };
            m->ok_rect = (lo_ui_rect_t){ ok_x, btn_y, btn_w, btn_h };

            lo_vcl_fill_rounded_rect(surf, cancel_x, btn_y, btn_w, btn_h, 3, LO_COLOR_HOVER_PILL);
            ui_draw_text(surf, cancel_x + 18, btn_y + 6, "Cancel", 12, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

            lo_vcl_fill_rounded_rect(surf, ok_x, btn_y, btn_w, btn_h, 3, LO_COLOR_SELECT_ACCENT);
            ui_draw_text(surf, ok_x + 24, btn_y + 6, "Print", 12, LO_VCL_FONT_SANS, LO_COLOR_SELECT_TEXT);
        } else if (m->type == LO_UI_MODAL_PROMPT) {
            ui_draw_text(surf, dx + 20, dy + 48, m->label, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

            int in_x = dx + 20;
            int in_y = dy + 72;
            int in_w = dw - 40;
            int in_h = 28;
            m->input_rect = (lo_ui_rect_t){ in_x, in_y, in_w, in_h };

            lo_vcl_fill_rounded_rect(surf, in_x, in_y, in_w, in_h, 3, LO_COLOR_MENU_BG);
            lo_vcl_draw_rounded_rect(surf, in_x, in_y, in_w, in_h, 3, LO_COLOR_SELECT_ACCENT, 1);
            ui_draw_text(surf, in_x + 8, in_y + 6, m->text_input, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

            int cw = ui_measure_text(m->text_input, 11, LO_VCL_FONT_SANS);
            lo_vcl_draw_line(surf, in_x + 8 + cw + 1, in_y + 5, in_x + 8 + cw + 1, in_y + in_h - 5, LO_COLOR_TEXT_PRIMARY, 1);

            int btn_w = 80;
            int btn_h = 28;
            int btn_y = dy + dh - 42;
            int cancel_x = dx + dw - 180;
            int ok_x = dx + dw - 95;
            m->cancel_rect = (lo_ui_rect_t){ cancel_x, btn_y, btn_w, btn_h };
            m->ok_rect = (lo_ui_rect_t){ ok_x, btn_y, btn_w, btn_h };

            lo_vcl_fill_rounded_rect(surf, cancel_x, btn_y, btn_w, btn_h, 3, LO_COLOR_HOVER_PILL);
            ui_draw_text(surf, cancel_x + 18, btn_y + 6, "Cancel", 12, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);

            lo_vcl_fill_rounded_rect(surf, ok_x, btn_y, btn_w, btn_h, 3, LO_COLOR_SELECT_ACCENT);
            ui_draw_text(surf, ok_x + 30, btn_y + 6, "OK", 12, LO_VCL_FONT_SANS, LO_COLOR_SELECT_TEXT);
        }
    }
}

void lo_ui_render_all(lo_ui_state_t *ui, lo_vcl_surface_t *surf) {
    if (!ui || !surf) return;
    lo_ui_render_menu_bar(ui, surf);
    lo_ui_render_toolbars(ui, surf);
    lo_ui_render_status_bar(ui, surf);
    lo_ui_render_popups(ui, surf);
}

/* =========================================================================
 * Event Dispatching
 * ========================================================================= */
static bool point_in_rect(int px, int py, lo_ui_rect_t r) {
    return (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);
}

int lo_ui_handle_event(lo_ui_state_t *ui, const lo_vcl_event_t *ev, lo_vcl_surface_t *surf) {
    (void)surf;
    if (!ui || !ev) return -1;

    /* 0. Modal Dialog Event Interception */
    if (ui->modal.type != LO_UI_MODAL_NONE) {
        lo_ui_modal_t *m = &ui->modal;
        if (ev->type == LO_VCL_EVENT_MOUSE_DOWN) {
            int mx = ev->mouse.x;
            int my = ev->mouse.y;
            if (m->cancel_rect.w > 0 && point_in_rect(mx, my, m->cancel_rect)) {
                lo_ui_close_modal(ui);
                return 0;
            }
            if (m->ok_rect.w > 0 && point_in_rect(mx, my, m->ok_rect)) {
                if (m->type == LO_UI_MODAL_PROMPT) {
                    snprintf(ui->prompt_value, sizeof(ui->prompt_value), "%s", m->text_input);
                }
                int act = m->completion_action_id;
                lo_ui_close_modal(ui);
                return act > 0 ? act : 0;
            }
            return 0; /* Consume clicks on modal */
        }
        if (ev->type == LO_VCL_EVENT_KEY_PRESS) {
            uint32_t kc = ev->key.keycode;
            if (kc == LO_VCL_KEY_ESCAPE) {
                lo_ui_close_modal(ui);
                return 0;
            }
            if (kc == LO_VCL_KEY_RETURN) {
                if (m->type == LO_UI_MODAL_PROMPT) {
                    snprintf(ui->prompt_value, sizeof(ui->prompt_value), "%s", m->text_input);
                }
                int act = m->completion_action_id;
                lo_ui_close_modal(ui);
                return act > 0 ? act : 0;
            }
            if (m->type == LO_UI_MODAL_PROMPT) {
                int len = (int)strlen(m->text_input);
                if (kc == LO_VCL_KEY_BACKSPACE) {
                    if (len > 0) {
                        m->text_input[len - 1] = '\0';
                        m->cursor_pos = len - 1;
                    }
                    return 0;
                }
                char ch = ev->key.text[0];
                if (ch >= 32 && ch <= 126 && len + 1 < (int)sizeof(m->text_input)) {
                    m->text_input[len] = ch;
                    m->text_input[len + 1] = '\0';
                    m->cursor_pos = len + 1;
                    return 0;
                }
            }
            return 0;
        }
        return 0;
    }

    /* 1. Window Resize */
    if (ev->type == LO_VCL_EVENT_RESIZE) {
        lo_ui_layout(ui, ev->resize.width, ev->resize.height);
        return 0;
    }

    /* 2. Mouse Motion */
    if (ev->type == LO_VCL_EVENT_MOUSE_MOVE) {
        int mx = ev->mouse.x;
        int my = ev->mouse.y;

        /* Dragging zoom slider */
        if (ui->status_bar.is_dragging_zoom) {
            lo_ui_rect_t tr = ui->status_bar.zoom_track_rect;
            int rel = mx - tr.x;
            if (rel < 0) rel = 0;
            if (rel > tr.w) rel = tr.w;
            ui->status_bar.zoom_percent = ui->status_bar.zoom_min + (rel * (ui->status_bar.zoom_max - ui->status_bar.zoom_min)) / tr.w;
            return LO_ACTION_ZOOM_CHANGE;
        }

        /* Hovering menu headers */
        int prev_hover = ui->menu_bar.hover_menu;
        ui->menu_bar.hover_menu = -1;
        for (int i = 0; i < ui->menu_bar.menu_count; i++) {
            if (point_in_rect(mx, my, ui->menu_bar.menus[i].bounds)) {
                ui->menu_bar.hover_menu = i;
                /* If menu is already open, hover switches active dropdown! */
                if (ui->menu_bar.active_menu >= 0 && ui->menu_bar.active_menu != i) {
                    ui->menu_bar.active_menu = i;
                    ui->menu_bar.hover_item = -1;
                    return 0;
                }
                break;
            }
        }

        /* Hovering dropdown items */
        if (ui->menu_bar.active_menu >= 0) {
            lo_ui_menu_t *m = &ui->menu_bar.menus[ui->menu_bar.active_menu];
            int px = m->bounds.x;
            int py = 24;
            int pw = m->dropdown_w;
            int cur_iy = py + 4;
            ui->menu_bar.hover_item = -1;
            for (int i = 0; i < m->item_count; i++) {
                int ih = m->items[i].is_separator ? 7 : 24;
                if (!m->items[i].is_separator && mx >= px && mx < px + pw && my >= cur_iy && my < cur_iy + ih) {
                    ui->menu_bar.hover_item = i;
                    break;
                }
                cur_iy += ih;
            }
            return 0;
        }

        /* Hovering combobox dropdown */
        if (ui->active_combo_row >= 0 && ui->active_combo_item >= 0) {
            lo_ui_combo_t *cb = &ui->toolbars[ui->active_combo_row].items[ui->active_combo_item].combo;
            int px = cb->bounds.x;
            int py = cb->bounds.y + cb->bounds.h + 2;
            int pw = cb->popup_w > 0 ? cb->popup_w : cb->bounds.w;
            cb->hover_index = -1;
            for (int i = 0; i < cb->item_count; i++) {
                int iy = py + 3 + i * 22;
                if (mx >= px && mx < px + pw && my >= iy && my < iy + 22) {
                    cb->hover_index = i;
                    break;
                }
            }
            return 0;
        }

        /* Hovering toolbars */
        for (int t = 0; t < ui->toolbar_count; t++) {
            lo_ui_toolbar_t *tb = &ui->toolbars[t];
            if (point_in_rect(mx, my, tb->bounds)) {
                for (int i = 0; i < tb->item_count; i++) {
                    tb->items[i].is_hovered = point_in_rect(mx, my, tb->items[i].bounds);
                }
                return 0;
            } else {
                for (int i = 0; i < tb->item_count; i++) {
                    tb->items[i].is_hovered = false;
                }
            }
        }

        if (prev_hover != ui->menu_bar.hover_menu) return 0;
        return -1;
    }

    /* 3. Mouse Release */
    if (ev->type == LO_VCL_EVENT_MOUSE_UP) {
        if (ui->status_bar.is_dragging_zoom) {
            ui->status_bar.is_dragging_zoom = false;
            return 0;
        }
    }

    /* 4. Mouse Down */
    if (ev->type == LO_VCL_EVENT_MOUSE_DOWN) {
        int mx = ev->mouse.x;
        int my = ev->mouse.y;

        /* Click inside open Menu Bar Dropdown */
        if (ui->menu_bar.active_menu >= 0) {
            lo_ui_menu_t *m = &ui->menu_bar.menus[ui->menu_bar.active_menu];
            int px = m->bounds.x;
            int py = 24;
            int pw = m->dropdown_w;
            int cur_iy = py + 4;

            for (int i = 0; i < m->item_count; i++) {
                int ih = m->items[i].is_separator ? 7 : 24;
                if (!m->items[i].is_separator && mx >= px && mx < px + pw && my >= cur_iy && my < cur_iy + ih) {
                    int act = m->items[i].action_id;
                    ui->menu_bar.active_menu = -1;
                    ui->menu_bar.hover_item = -1;
                    if (act == LO_ACTION_ZOOM_IN) {
                        ui->status_bar.zoom_percent += 10;
                        if (ui->status_bar.zoom_percent > 200) ui->status_bar.zoom_percent = 200;
                    } else if (act == LO_ACTION_ZOOM_OUT) {
                        ui->status_bar.zoom_percent -= 10;
                        if (ui->status_bar.zoom_percent < 50) ui->status_bar.zoom_percent = 50;
                    } else if (act == LO_ACTION_ZOOM_100 || act == LO_ACTION_ZOOM_FIT) {
                        ui->status_bar.zoom_percent = 100;
                    }
                    return act > 0 ? act : 0;
                }
                cur_iy += ih;
            }

            /* Clicked on another header? Handled in header click */
            bool on_header = false;
            for (int i = 0; i < ui->menu_bar.menu_count; i++) {
                if (point_in_rect(mx, my, ui->menu_bar.menus[i].bounds)) {
                    on_header = true;
                    break;
                }
            }
            if (!on_header) {
                ui->menu_bar.active_menu = -1;
                ui->menu_bar.hover_item = -1;
                return 0;
            }
        }

        /* Click on Menu Bar header */
        for (int i = 0; i < ui->menu_bar.menu_count; i++) {
            if (point_in_rect(mx, my, ui->menu_bar.menus[i].bounds)) {
                if (ui->menu_bar.active_menu == i) {
                    ui->menu_bar.active_menu = -1;
                } else {
                    ui->menu_bar.active_menu = i;
                    ui->menu_bar.hover_item = -1;
                }
                return 0;
            }
        }

        /* Click inside open Combobox Dropdown */
        if (ui->active_combo_row >= 0 && ui->active_combo_item >= 0) {
            lo_ui_tool_item_t *item = &ui->toolbars[ui->active_combo_row].items[ui->active_combo_item];
            lo_ui_combo_t *cb = &item->combo;
            int px = cb->bounds.x;
            int py = cb->bounds.y + cb->bounds.h + 2;
            int pw = cb->popup_w > 0 ? cb->popup_w : cb->bounds.w;
            for (int i = 0; i < cb->item_count; i++) {
                int iy = py + 3 + i * 22;
                if (mx >= px && mx < px + pw && my >= iy && my < iy + 22) {
                    cb->selected_index = i;
                    if (cb->items && i >= 0 && i < cb->item_count && cb->items[i]) {
                        snprintf(cb->text, sizeof(cb->text), "%s", cb->items[i]);
                    }
                    int act = item->action_id;
                    ui->active_combo_row = -1;
                    ui->active_combo_item = -1;
                    return act;
                }
            }
            ui->active_combo_row = -1;
            ui->active_combo_item = -1;
            return 0;
        }

        /* Click on Toolbars */
        for (int t = 0; t < ui->toolbar_count; t++) {
            lo_ui_toolbar_t *tb = &ui->toolbars[t];
            if (point_in_rect(mx, my, tb->bounds)) {
                for (int i = 0; i < tb->item_count; i++) {
                    lo_ui_tool_item_t *item = &tb->items[i];
                    if (point_in_rect(mx, my, item->bounds)) {
                        if (item->type == LO_UI_WIDGET_COMBO) {
                            ui->active_combo_row = t;
                            ui->active_combo_item = i;
                            item->combo.hover_index = item->combo.selected_index;
                            return 0;
                        } else if (item->type == LO_UI_WIDGET_TOGGLE) {
                            if (item->action_id == LO_ACTION_ALIGN_LEFT ||
                                item->action_id == LO_ACTION_ALIGN_CENTER ||
                                item->action_id == LO_ACTION_ALIGN_RIGHT ||
                                item->action_id == LO_ACTION_ALIGN_JUSTIFY) {
                                for (int t2 = 0; t2 < ui->toolbar_count; t2++) {
                                    lo_ui_toolbar_t *tb2 = &ui->toolbars[t2];
                                    for (int j = 0; j < tb2->item_count; j++) {
                                        lo_ui_tool_item_t *other = &tb2->items[j];
                                        if (other->action_id == LO_ACTION_ALIGN_LEFT ||
                                            other->action_id == LO_ACTION_ALIGN_CENTER ||
                                            other->action_id == LO_ACTION_ALIGN_RIGHT ||
                                            other->action_id == LO_ACTION_ALIGN_JUSTIFY) {
                                            other->is_active = (other == item);
                                        }
                                    }
                                }
                                return item->action_id;
                            } else {
                                item->is_active = !item->is_active;
                                return item->action_id;
                            }
                        } else if (item->type == LO_UI_WIDGET_BUTTON) {
                            return item->action_id;
                        }
                    }
                }
                return 0;
            }
        }

        /* Click on Status Bar */
        if (point_in_rect(mx, my, ui->status_bar.bounds)) {
            lo_ui_status_bar_t *sb = &ui->status_bar;
            if (point_in_rect(mx, my, sb->zoom_minus_rect)) {
                sb->zoom_percent -= 10;
                if (sb->zoom_percent < sb->zoom_min) sb->zoom_percent = sb->zoom_min;
                return LO_ACTION_ZOOM_CHANGE;
            }
            if (point_in_rect(mx, my, sb->zoom_plus_rect)) {
                sb->zoom_percent += 10;
                if (sb->zoom_percent > sb->zoom_max) sb->zoom_percent = sb->zoom_max;
                return LO_ACTION_ZOOM_CHANGE;
            }
            if (point_in_rect(mx, my, sb->zoom_track_rect)) {
                sb->is_dragging_zoom = true;
                int rel = mx - sb->zoom_track_rect.x;
                sb->zoom_percent = sb->zoom_min + (rel * (sb->zoom_max - sb->zoom_min)) / sb->zoom_track_rect.w;
                return LO_ACTION_ZOOM_CHANGE;
            }
            return 0;
        }

        return -1;
    }

    /* 5. Key Press */
    if (ev->type == LO_VCL_EVENT_KEY_PRESS) {
        uint32_t kc = ev->key.keycode;
        uint32_t mods = ev->key.modifiers;

        /* Escape dismisses open menus / popups */
        if (kc == LO_VCL_KEY_ESCAPE) {
            if (ui->menu_bar.active_menu >= 0) {
                ui->menu_bar.active_menu = -1;
                ui->menu_bar.hover_item = -1;
                return 0;
            }
            if (ui->active_combo_row >= 0) {
                ui->active_combo_row = -1;
                ui->active_combo_item = -1;
                return 0;
            }
        }

        /* Menu keyboard navigation */
        if (ui->menu_bar.active_menu >= 0) {
            lo_ui_menu_t *m = &ui->menu_bar.menus[ui->menu_bar.active_menu];
            if (kc == LO_VCL_KEY_LEFT) {
                ui->menu_bar.active_menu = (ui->menu_bar.active_menu + ui->menu_bar.menu_count - 1) % ui->menu_bar.menu_count;
                ui->menu_bar.hover_item = -1;
                return 0;
            }
            if (kc == LO_VCL_KEY_RIGHT) {
                ui->menu_bar.active_menu = (ui->menu_bar.active_menu + 1) % ui->menu_bar.menu_count;
                ui->menu_bar.hover_item = -1;
                return 0;
            }
            if (kc == LO_VCL_KEY_DOWN) {
                ui->menu_bar.hover_item = (ui->menu_bar.hover_item + 1) % m->item_count;
                if (m->items[ui->menu_bar.hover_item].is_separator) {
                    ui->menu_bar.hover_item = (ui->menu_bar.hover_item + 1) % m->item_count;
                }
                return 0;
            }
            if (kc == LO_VCL_KEY_UP) {
                ui->menu_bar.hover_item = (ui->menu_bar.hover_item + m->item_count - 1) % m->item_count;
                if (m->items[ui->menu_bar.hover_item].is_separator) {
                    ui->menu_bar.hover_item = (ui->menu_bar.hover_item + m->item_count - 1) % m->item_count;
                }
                return 0;
            }
            if (kc == LO_VCL_KEY_RETURN) {
                if (ui->menu_bar.hover_item >= 0 && ui->menu_bar.hover_item < m->item_count) {
                    int act = m->items[ui->menu_bar.hover_item].action_id;
                    ui->menu_bar.active_menu = -1;
                    ui->menu_bar.hover_item = -1;
                    if (act == LO_ACTION_ZOOM_IN) {
                        ui->status_bar.zoom_percent += 10;
                        if (ui->status_bar.zoom_percent > 200) ui->status_bar.zoom_percent = 200;
                    } else if (act == LO_ACTION_ZOOM_OUT) {
                        ui->status_bar.zoom_percent -= 10;
                        if (ui->status_bar.zoom_percent < 50) ui->status_bar.zoom_percent = 50;
                    } else if (act == LO_ACTION_ZOOM_100 || act == LO_ACTION_ZOOM_FIT) {
                        ui->status_bar.zoom_percent = 100;
                    }
                    return act > 0 ? act : 0;
                }
            }
        }

        /* Standard Shortcuts */
        if (mods & LO_VCL_MOD_CTRL) {
            char ch = ev->key.text[0];
            if (mods & LO_VCL_MOD_SHIFT) {
                if (ch == 's' || ch == 'S') return LO_ACTION_SAVE_AS;
                if (ch == '+' || ch == '=') {
                    ui->status_bar.zoom_percent += 10;
                    if (ui->status_bar.zoom_percent > 200) ui->status_bar.zoom_percent = 200;
                    return LO_ACTION_ZOOM_IN;
                }
                if (ch == '-' || ch == '_') {
                    ui->status_bar.zoom_percent -= 10;
                    if (ui->status_bar.zoom_percent < 50) ui->status_bar.zoom_percent = 50;
                    return LO_ACTION_ZOOM_OUT;
                }
            }
            if (ch == 'n' || ch == 'N') return LO_ACTION_NEW;
            if (ch == 'o' || ch == 'O') return LO_ACTION_OPEN;
            if (ch == 's' || ch == 'S') return LO_ACTION_SAVE;
            if (ch == 'p' || ch == 'P') return LO_ACTION_PRINT;
            if (ch == 'q' || ch == 'Q') return LO_ACTION_QUIT;
            if (ch == 'w' || ch == 'W') return LO_ACTION_CLOSE;
            if (ch == 'z' || ch == 'Z') return LO_ACTION_UNDO;
            if (ch == 'y' || ch == 'Y') return LO_ACTION_REDO;
            if (ch == 'x' || ch == 'X') return LO_ACTION_CUT;
            if (ch == 'c' || ch == 'C') return LO_ACTION_COPY;
            if (ch == 'v' || ch == 'V') return LO_ACTION_PASTE;
            if (ch == 'a' || ch == 'A') return LO_ACTION_SELECT_ALL;
            if (ch == 'f' || ch == 'F') return LO_ACTION_FIND;
            if (ch == 'b' || ch == 'B') return LO_ACTION_BOLD;
            if (ch == 'i' || ch == 'I') return LO_ACTION_ITALIC;
            if (ch == 'u' || ch == 'U') return LO_ACTION_UNDERLINE;
            if (ch == 'm' || ch == 'M') return LO_ACTION_CLEAR_FORMATTING;
            if (ch == '1') return LO_ACTION_STYLE_H1;
            if (ch == '2') return LO_ACTION_STYLE_H2;
            if (ch == '0') {
                ui->status_bar.zoom_percent = 100;
                return LO_ACTION_ZOOM_100;
            }
            if (ch == '=' || ch == '+') {
                ui->status_bar.zoom_percent += 10;
                if (ui->status_bar.zoom_percent > 200) ui->status_bar.zoom_percent = 200;
                return LO_ACTION_ZOOM_IN;
            }
            if (ch == '-' || ch == '_') {
                ui->status_bar.zoom_percent -= 10;
                if (ui->status_bar.zoom_percent < 50) ui->status_bar.zoom_percent = 50;
                return LO_ACTION_ZOOM_OUT;
            }
            if (kc == LO_VCL_KEY_F12) return LO_ACTION_INSERT_TABLE;
            if (kc == LO_VCL_KEY_F2) return LO_ACTION_INSERT_FUNCTION;
        }

        if (kc == LO_VCL_KEY_F4) return LO_ACTION_POSITION_SIZE;
        if (kc == LO_VCL_KEY_F5) return LO_ACTION_SLIDESHOW;
        if (kc == LO_VCL_KEY_F7) return LO_ACTION_SPELLING;
        if (kc == LO_VCL_KEY_F9) return LO_ACTION_RECALCULATE;

        return -1;
    }

    return -1;
}

/* =========================================================================
 * Menu & Toolbar Builders
 * ========================================================================= */
static lo_ui_menu_item_t g_std_file_items[] = {
    { "New", 'N', LO_ICON_NEW, "Ctrl+N", LO_ACTION_NEW, false, false, false, true },
    { "Open...", 'O', LO_ICON_OPEN, "Ctrl+O", LO_ACTION_OPEN, false, false, false, true },
    { "Save", 'S', LO_ICON_SAVE, "Ctrl+S", LO_ACTION_SAVE, false, false, false, true },
    { "Save As...", 'A', LO_ICON_SAVE_AS, "Ctrl+Shift+S", LO_ACTION_SAVE_AS, false, false, false, true },
    { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
    { "Export as PDF...", 'D', LO_ICON_PDF, NULL, LO_ACTION_EXPORT_PDF, false, false, false, true },
    { "Print...", 'P', LO_ICON_PRINT, "Ctrl+P", LO_ACTION_PRINT, false, false, false, true },
    { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
    { "Exit", 'x', LO_ICON_NONE, "Ctrl+Q", LO_ACTION_QUIT, false, false, false, true }
};

void lo_ui_build_standard_file_menu(lo_ui_menu_t *menu, const char *app_ext) {
    (void)app_ext;
    if (!menu) return;
    menu->title = "File";
    menu->mnemonic = 'F';
    menu->items = g_std_file_items;
    menu->item_count = sizeof(g_std_file_items) / sizeof(g_std_file_items[0]);
    menu->dropdown_w = 220;
}

static lo_ui_menu_item_t g_std_edit_items[] = {
    { "Undo", 'U', LO_ICON_UNDO, "Ctrl+Z", LO_ACTION_UNDO, false, false, false, true },
    { "Redo", 'R', LO_ICON_REDO, "Ctrl+Y", LO_ACTION_REDO, false, false, false, true },
    { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
    { "Cut", 't', LO_ICON_CUT, "Ctrl+X", LO_ACTION_CUT, false, false, false, true },
    { "Copy", 'C', LO_ICON_COPY, "Ctrl+C", LO_ACTION_COPY, false, false, false, true },
    { "Paste", 'P', LO_ICON_PASTE, "Ctrl+V", LO_ACTION_PASTE, false, false, false, true },
    { "Select All", 'A', LO_ICON_NONE, "Ctrl+A", LO_ACTION_SELECT_ALL, false, false, false, true },
    { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
    { "Find...", 'F', LO_ICON_FIND, "Ctrl+F", LO_ACTION_FIND, false, false, false, true }
};

void lo_ui_build_standard_edit_menu(lo_ui_menu_t *menu) {
    if (!menu) return;
    menu->title = "Edit";
    menu->mnemonic = 'E';
    menu->items = g_std_edit_items;
    menu->item_count = sizeof(g_std_edit_items) / sizeof(g_std_edit_items[0]);
    menu->dropdown_w = 200;
}

static lo_ui_menu_item_t g_std_view_items[] = {
    { "Zoom In", 'I', LO_ICON_ZOOM_IN, "Ctrl++", LO_ACTION_ZOOM_IN, false, false, false, true },
    { "Zoom Out", 'O', LO_ICON_ZOOM_OUT, "Ctrl+-", LO_ACTION_ZOOM_OUT, false, false, false, true },
    { "100%", '1', LO_ICON_NONE, NULL, LO_ACTION_ZOOM_100, false, false, false, true },
    { "Zoom to Fit", 'F', LO_ICON_NONE, NULL, LO_ACTION_ZOOM_FIT, false, false, false, true }
};

void lo_ui_build_standard_view_menu(lo_ui_menu_t *menu) {
    if (!menu) return;
    menu->title = "View";
    menu->mnemonic = 'V';
    menu->items = g_std_view_items;
    menu->item_count = sizeof(g_std_view_items) / sizeof(g_std_view_items[0]);
    menu->dropdown_w = 180;
}

static lo_ui_menu_item_t g_std_help_items[] = {
    { "About LibreOffice", 'A', LO_ICON_APP_SOFFICE, NULL, LO_ACTION_ABOUT, false, false, false, true }
};

void lo_ui_build_standard_help_menu(lo_ui_menu_t *menu) {
    if (!menu) return;
    menu->title = "Help";
    menu->mnemonic = 'H';
    menu->items = g_std_help_items;
    menu->item_count = sizeof(g_std_help_items) / sizeof(g_std_help_items[0]);
    menu->dropdown_w = 190;
}

/* Standard Toolbar: Row 0 */
static lo_ui_tool_item_t g_std_tb_items[16] = {
    { LO_UI_WIDGET_BUTTON, LO_ACTION_NEW, LO_ICON_NEW, "New Document", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_OPEN, LO_ICON_OPEN, "Open...", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_SAVE, LO_ICON_SAVE, "Save", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_EXPORT_PDF, LO_ICON_PDF, "Export as PDF", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_PRINT, LO_ICON_PRINT, "Print", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_SEPARATOR, 0, LO_ICON_NONE, NULL, false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_CUT, LO_ICON_CUT, "Cut", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_COPY, LO_ICON_COPY, "Copy", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_PASTE, LO_ICON_PASTE, "Paste", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_SEPARATOR, 0, LO_ICON_NONE, NULL, false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_UNDO, LO_ICON_UNDO, "Undo", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_REDO, LO_ICON_REDO, "Redo", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_SEPARATOR, 0, LO_ICON_NONE, NULL, false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_FIND, LO_ICON_FIND, "Find...", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_INSERT_TABLE, LO_ICON_TABLE, "Insert Table", false, true, false, false, {0,0,0,0}, {0} },
    { LO_UI_WIDGET_BUTTON, LO_ACTION_INSERT_IMAGE, LO_ICON_IMAGE, "Insert Image", false, true, false, false, {0,0,0,0}, {0} }
};

void lo_ui_build_standard_toolbar(lo_ui_toolbar_t *tb, int app_action_save) {
    if (!tb) return;
    tb->has_grip = true;
    tb->bg_color = LO_COLOR_BG_CHROME;
    tb->items = g_std_tb_items;
    tb->item_count = sizeof(g_std_tb_items) / sizeof(g_std_tb_items[0]);
    for (int i = 0; i < tb->item_count; i++) {
        tb->items[i].is_active = false;
        tb->items[i].is_hovered = false;
        tb->items[i].is_pressed = false;
    }
    if (app_action_save > 0) {
        tb->items[2].action_id = app_action_save;
    }
}

/* Formatting Toolbar: Row 1 */
static const char *g_styles[] = { "Default Paragraph Style", "Heading 1", "Heading 2", "Heading 3", "Text Body" };
static const char *g_fonts[] = { "DejaVu Serif", "DejaVu Sans", "DejaVu Sans Mono" };
static const char *g_sizes[] = { "10 pt", "12 pt", "14 pt", "18 pt", "24 pt" };

static lo_ui_tool_item_t g_fmt_tb_items[16];

void lo_ui_build_formatting_toolbar(lo_ui_toolbar_t *tb) {
    if (!tb) return;
    tb->has_grip = true;
    tb->bg_color = LO_COLOR_BG_SECONDARY;

    memset(g_fmt_tb_items, 0, sizeof(g_fmt_tb_items));

    /* 0: Style combobox */
    g_fmt_tb_items[0].type = LO_UI_WIDGET_COMBO;
    g_fmt_tb_items[0].action_id = LO_ACTION_STYLE_CHANGE;
    g_fmt_tb_items[0].combo.items = g_styles;
    g_fmt_tb_items[0].combo.item_count = 5;
    g_fmt_tb_items[0].combo.selected_index = 0;
    snprintf(g_fmt_tb_items[0].combo.text, sizeof(g_fmt_tb_items[0].combo.text), "%s", g_styles[0]);
    g_fmt_tb_items[0].combo.bounds.w = 172;

    /* 1: Font combobox */
    g_fmt_tb_items[1].type = LO_UI_WIDGET_COMBO;
    g_fmt_tb_items[1].action_id = LO_ACTION_FONT_CHANGE;
    g_fmt_tb_items[1].combo.items = g_fonts;
    g_fmt_tb_items[1].combo.item_count = 3;
    g_fmt_tb_items[1].combo.selected_index = 0;
    snprintf(g_fmt_tb_items[1].combo.text, sizeof(g_fmt_tb_items[1].combo.text), "%s", g_fonts[0]);
    g_fmt_tb_items[1].combo.bounds.w = 110;

    /* 2: Size combobox */
    g_fmt_tb_items[2].type = LO_UI_WIDGET_COMBO;
    g_fmt_tb_items[2].action_id = LO_ACTION_SIZE_CHANGE;
    g_fmt_tb_items[2].combo.items = g_sizes;
    g_fmt_tb_items[2].combo.item_count = 5;
    g_fmt_tb_items[2].combo.selected_index = 1;
    snprintf(g_fmt_tb_items[2].combo.text, sizeof(g_fmt_tb_items[2].combo.text), "%s", g_sizes[1]);
    g_fmt_tb_items[2].combo.bounds.w = 55;

    /* 3: Separator */
    g_fmt_tb_items[3].type = LO_UI_WIDGET_SEPARATOR;

    /* 4: Bold */
    g_fmt_tb_items[4].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[4].action_id = LO_ACTION_BOLD;
    g_fmt_tb_items[4].icon_id = LO_ICON_BOLD;
    g_fmt_tb_items[4].tooltip = "Bold (Ctrl+B)";

    /* 5: Italic */
    g_fmt_tb_items[5].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[5].action_id = LO_ACTION_ITALIC;
    g_fmt_tb_items[5].icon_id = LO_ICON_ITALIC;
    g_fmt_tb_items[5].tooltip = "Italic (Ctrl+I)";

    /* 6: Underline */
    g_fmt_tb_items[6].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[6].action_id = LO_ACTION_UNDERLINE;
    g_fmt_tb_items[6].icon_id = LO_ICON_UNDERLINE;
    g_fmt_tb_items[6].tooltip = "Underline (Ctrl+U)";

    /* 7: Separator */
    g_fmt_tb_items[7].type = LO_UI_WIDGET_SEPARATOR;

    /* 8: Font Color */
    g_fmt_tb_items[8].type = LO_UI_WIDGET_BUTTON;
    g_fmt_tb_items[8].action_id = LO_ACTION_FONT_COLOR;
    g_fmt_tb_items[8].icon_id = LO_ICON_FONT_COLOR;

    /* 9: Highlight Color */
    g_fmt_tb_items[9].type = LO_UI_WIDGET_BUTTON;
    g_fmt_tb_items[9].action_id = LO_ACTION_HIGHLIGHT_COLOR;
    g_fmt_tb_items[9].icon_id = LO_ICON_HIGHLIGHT_COLOR;

    /* 10: Separator */
    g_fmt_tb_items[10].type = LO_UI_WIDGET_SEPARATOR;

    /* 11: Align Left */
    g_fmt_tb_items[11].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[11].action_id = LO_ACTION_ALIGN_LEFT;
    g_fmt_tb_items[11].icon_id = LO_ICON_ALIGN_LEFT;
    g_fmt_tb_items[11].is_active = true;

    /* 12: Align Center */
    g_fmt_tb_items[12].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[12].action_id = LO_ACTION_ALIGN_CENTER;
    g_fmt_tb_items[12].icon_id = LO_ICON_ALIGN_CENTER;

    /* 13: Align Right */
    g_fmt_tb_items[13].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[13].action_id = LO_ACTION_ALIGN_RIGHT;
    g_fmt_tb_items[13].icon_id = LO_ICON_ALIGN_RIGHT;

    /* 14: Align Justify */
    g_fmt_tb_items[14].type = LO_UI_WIDGET_TOGGLE;
    g_fmt_tb_items[14].action_id = LO_ACTION_ALIGN_JUSTIFY;
    g_fmt_tb_items[14].icon_id = LO_ICON_ALIGN_JUSTIFY;

    /* 15: Bullets */
    g_fmt_tb_items[15].type = LO_UI_WIDGET_BUTTON;
    g_fmt_tb_items[15].action_id = LO_ACTION_BULLETS;
    g_fmt_tb_items[15].icon_id = LO_ICON_BULLETS;

    tb->items = g_fmt_tb_items;
    tb->item_count = 16;
}

/* =========================================================================
 * Combobox & Toolbar Helpers
 * ========================================================================= */
int lo_ui_get_combo_selected(const lo_ui_state_t *ui, int action_id, const char **out_text) {
    if (!ui) return -1;
    for (int t = 0; t < ui->toolbar_count; t++) {
        const lo_ui_toolbar_t *tb = &ui->toolbars[t];
        for (int i = 0; i < tb->item_count; i++) {
            const lo_ui_tool_item_t *item = &tb->items[i];
            if (item->type == LO_UI_WIDGET_COMBO && item->action_id == action_id) {
                int idx = item->combo.selected_index;
                if (out_text) {
                    if (item->combo.items && idx >= 0 && idx < item->combo.item_count && item->combo.items[idx]) {
                        *out_text = item->combo.items[idx];
                    } else if (item->combo.text[0] != '\0') {
                        *out_text = item->combo.text;
                    } else {
                        *out_text = "";
                    }
                }
                return idx;
            }
        }
    }
    return -1;
}

void lo_ui_sync_combo(lo_ui_state_t *ui, int action_id, int selected_index, const char *text) {
    if (!ui) return;
    for (int t = 0; t < ui->toolbar_count; t++) {
        lo_ui_toolbar_t *tb = &ui->toolbars[t];
        for (int i = 0; i < tb->item_count; i++) {
            lo_ui_tool_item_t *item = &tb->items[i];
            if (item->type == LO_UI_WIDGET_COMBO && item->action_id == action_id) {
                if (selected_index >= 0) {
                    item->combo.selected_index = selected_index;
                    if (item->combo.items && selected_index < item->combo.item_count && item->combo.items[selected_index]) {
                        snprintf(item->combo.text, sizeof(item->combo.text), "%s", item->combo.items[selected_index]);
                    }
                } else if (text && item->combo.items) {
                    for (int k = 0; k < item->combo.item_count; k++) {
                        if (item->combo.items[k] && strcmp(item->combo.items[k], text) == 0) {
                            item->combo.selected_index = k;
                            break;
                        }
                    }
                    snprintf(item->combo.text, sizeof(item->combo.text), "%s", text);
                }
                return;
            }
        }
    }
}

void lo_ui_init_standard_toolbars(lo_ui_state_t *ui, int app_action_save) {
    if (!ui) return;
    ui->toolbar_count = 2;
    lo_ui_build_standard_toolbar(&ui->toolbars[0], app_action_save);
    lo_ui_build_formatting_toolbar(&ui->toolbars[1]);
}

void lo_ui_sync_toggle(lo_ui_state_t *ui, int action_id, int toggled) {
    if (!ui) return;
    bool is_align = (action_id == LO_ACTION_ALIGN_LEFT ||
                     action_id == LO_ACTION_ALIGN_CENTER ||
                     action_id == LO_ACTION_ALIGN_RIGHT ||
                     action_id == LO_ACTION_ALIGN_JUSTIFY);
    for (int t = 0; t < ui->toolbar_count; t++) {
        lo_ui_toolbar_t *tb = &ui->toolbars[t];
        for (int i = 0; i < tb->item_count; i++) {
            lo_ui_tool_item_t *item = &tb->items[i];
            if (item->action_id == action_id) {
                item->is_active = (toggled != 0);
            } else if (is_align && toggled) {
                if (item->action_id == LO_ACTION_ALIGN_LEFT ||
                    item->action_id == LO_ACTION_ALIGN_CENTER ||
                    item->action_id == LO_ACTION_ALIGN_RIGHT ||
                    item->action_id == LO_ACTION_ALIGN_JUSTIFY) {
                    item->is_active = false;
                }
            }
        }
    }
}

/* =========================================================================
 * Modal Dialog APIs
 * ========================================================================= */
static void lo_ui_modal_layout(lo_ui_state_t *ui) {
    if (!ui || ui->modal.type == LO_UI_MODAL_NONE) return;
    lo_ui_modal_t *m = &ui->modal;
    int sw = ui->window_width > 0 ? ui->window_width : 800;
    int sh = ui->window_height > 0 ? ui->window_height : 600;

    int dw = 420;
    int dh = (m->type == LO_UI_MODAL_ABOUT || m->type == LO_UI_MODAL_PRINT || m->type == LO_UI_MODAL_INFO) ? 220 : 180;
    int dx = (sw - dw) / 2;
    int dy = (sh - dh) / 2;
    m->dialog_rect = (lo_ui_rect_t){ dx, dy, dw, dh };

    int btn_w = 80;
    int btn_h = 28;
    int btn_y = dy + dh - 42;

    if (m->type == LO_UI_MODAL_ABOUT || m->type == LO_UI_MODAL_INFO) {
        int btn_x = dx + (dw - btn_w) / 2;
        m->ok_rect = (lo_ui_rect_t){ btn_x, btn_y, btn_w, btn_h };
        m->cancel_rect = (lo_ui_rect_t){ 0, 0, 0, 0 };
    } else {
        int cancel_x = dx + dw - 180;
        int ok_x = dx + dw - 95;
        m->cancel_rect = (lo_ui_rect_t){ cancel_x, btn_y, btn_w, btn_h };
        m->ok_rect = (lo_ui_rect_t){ ok_x, btn_y, btn_w, btn_h };
    }
}

void lo_ui_open_about(lo_ui_state_t *ui, const char *app_name) {
    if (!ui) return;
    ui->modal.type = LO_UI_MODAL_ABOUT;
    snprintf(ui->modal.title, sizeof(ui->modal.title), "About LibreOffice");
    snprintf(ui->modal.info_body, sizeof(ui->modal.info_body),
             "LibreOffice 24.8 HobbyOS Edition\n%s\nVCL Display Subsystem + FreeType2 / X11\nAArch64 / x86_64 Multi-Processing (SMP >= 4)\nCopyright (C) 2026 The Document Foundation / HobbyOS",
             app_name ? app_name : (ui->app_name[0] ? ui->app_name : "LibreOffice"));
    ui->modal.completion_action_id = 0;
    lo_ui_modal_layout(ui);
}

void lo_ui_open_prompt(lo_ui_state_t *ui, const char *title, const char *label, const char *initial_text, int completion_action_id) {
    if (!ui) return;
    ui->modal.type = LO_UI_MODAL_PROMPT;
    snprintf(ui->modal.title, sizeof(ui->modal.title), "%s", title ? title : "Prompt");
    snprintf(ui->modal.label, sizeof(ui->modal.label), "%s", label ? label : "Enter value:");
    snprintf(ui->modal.text_input, sizeof(ui->modal.text_input), "%s", initial_text ? initial_text : "");
    ui->modal.cursor_pos = (int)strlen(ui->modal.text_input);
    ui->modal.completion_action_id = completion_action_id;
    lo_ui_modal_layout(ui);
}

void lo_ui_open_print_dialog(lo_ui_state_t *ui, const char *doc_title, int page_count, const char *spool_file, int completion_action_id) {
    if (!ui) return;
    ui->modal.type = LO_UI_MODAL_PRINT;
    snprintf(ui->modal.title, sizeof(ui->modal.title), "Print Document");
    snprintf(ui->modal.print_doc_name, sizeof(ui->modal.print_doc_name), "%s", doc_title ? doc_title : "Untitled");
    ui->modal.print_page_count = (page_count > 0) ? page_count : 1;
    snprintf(ui->modal.print_spool_path, sizeof(ui->modal.print_spool_path), "%s", spool_file ? spool_file : "/home/PRINT.TXT");
    ui->modal.completion_action_id = (completion_action_id > 0) ? completion_action_id : LO_ACTION_PRINT_CONFIRM;
    lo_ui_modal_layout(ui);
}

void lo_ui_open_info_modal(lo_ui_state_t *ui, const char *title, const char *body) {
    if (!ui) return;
    ui->modal.type = LO_UI_MODAL_INFO;
    snprintf(ui->modal.title, sizeof(ui->modal.title), "%s", title ? title : "Information");
    snprintf(ui->modal.info_body, sizeof(ui->modal.info_body), "%s", body ? body : "");
    ui->modal.completion_action_id = 0;
    lo_ui_modal_layout(ui);
}

void lo_ui_close_modal(lo_ui_state_t *ui) {
    if (!ui) return;
    ui->modal.type = LO_UI_MODAL_NONE;
    ui->modal.ok_rect = (lo_ui_rect_t){ 0, 0, 0, 0 };
    ui->modal.cancel_rect = (lo_ui_rect_t){ 0, 0, 0, 0 };
}

/* =========================================================================
 * Shared Clipboard APIs
 * ========================================================================= */
static char s_lo_clipboard[4096] = {0};
static int s_lo_clipboard_len = 0;

void lo_ui_clipboard_set(const char *text, int len) {
    if (!text) {
        s_lo_clipboard[0] = '\0';
        s_lo_clipboard_len = 0;
        return;
    }
    if (len < 0) len = (int)strlen(text);
    if (len >= (int)sizeof(s_lo_clipboard)) len = (int)sizeof(s_lo_clipboard) - 1;
    memcpy(s_lo_clipboard, text, len);
    s_lo_clipboard[len] = '\0';
    s_lo_clipboard_len = len;

    FILE *fp = fopen("/tmp/CLIPBRD.TMP", "wb");
    if (!fp) fp = fopen("/home/CLIPBRD.TMP", "wb");
    if (fp) {
        fwrite(s_lo_clipboard, 1, s_lo_clipboard_len, fp);
        fclose(fp);
    }
}

const char *lo_ui_clipboard_get(int *out_len) {
    if (s_lo_clipboard_len == 0) {
        FILE *fp = fopen("/tmp/CLIPBRD.TMP", "rb");
        if (!fp) fp = fopen("/home/CLIPBRD.TMP", "rb");
        if (fp) {
            s_lo_clipboard_len = (int)fread(s_lo_clipboard, 1, sizeof(s_lo_clipboard) - 1, fp);
            s_lo_clipboard[s_lo_clipboard_len] = '\0';
            fclose(fp);
        }
    }
    if (out_len) *out_len = s_lo_clipboard_len;
    return s_lo_clipboard;
}

bool lo_ui_clipboard_has_text(void) {
    if (s_lo_clipboard_len > 0) return true;
    return (lo_ui_clipboard_get(NULL) != NULL && s_lo_clipboard_len > 0);
}

void lo_ui_clipboard_copy(const char *text) {
    lo_ui_clipboard_set(text, -1);
}

int lo_ui_clipboard_paste(char *out_buf, size_t cap) {
    if (!out_buf || cap == 0) return 0;
    int len = 0;
    const char *clip = lo_ui_clipboard_get(&len);
    if (!clip || len <= 0) {
        out_buf[0] = '\0';
        return 0;
    }
    if ((size_t)len >= cap) len = (int)cap - 1;
    memcpy(out_buf, clip, len);
    out_buf[len] = '\0';
    return len;
}
