/*
 * src/user/libreoffice/soffice/lo_soffice.c
 *
 * Unified CLI Dispatcher & Interactive Start Center for the LibreOffice suite on HobbyOS.
 * Routes command-line arguments to individual application engines:
 * Writer, Calc, Impress, Draw, Math, and Base, and presents the Start Center GUI.
 */

#include "soffice.h"
#include "lo_vcl.h"
#include "lo_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <X11/keysym.h>

enum {
    LO_APP_NONE = 0,
    LO_APP_WRITER,
    LO_APP_CALC,
    LO_APP_IMPRESS,
    LO_APP_DRAW,
    LO_APP_MATH,
    LO_APP_BASE
};

static int detect_app_from_name(const char *name) {
    if (!name) return LO_APP_NONE;
    const char *base = strrchr(name, '/');
    if (base) base++;
    else base = name;

    if (strncasecmp(base, "swriter", 7) == 0) return LO_APP_WRITER;
    if (strncasecmp(base, "scalc", 5) == 0) return LO_APP_CALC;
    if (strncasecmp(base, "simpress", 8) == 0) return LO_APP_IMPRESS;
    if (strncasecmp(base, "sdraw", 5) == 0) return LO_APP_DRAW;
    if (strncasecmp(base, "smath", 5) == 0) return LO_APP_MATH;
    if (strncasecmp(base, "sbase", 5) == 0) return LO_APP_BASE;
    return LO_APP_NONE;
}

static int detect_app_from_ext(const char *path) {
    if (!path) return LO_APP_NONE;
    const char *dot = strrchr(path, '.');
    if (!dot) return LO_APP_NONE;
    const char *ext = dot + 1;

    if (strcasecmp(ext, "odt") == 0 || strcasecmp(ext, "fodt") == 0 ||
        strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "html") == 0) {
        return LO_APP_WRITER;
    }
    if (strcasecmp(ext, "ods") == 0 || strcasecmp(ext, "fods") == 0 ||
        strcasecmp(ext, "csv") == 0) {
        return LO_APP_CALC;
    }
    if (strcasecmp(ext, "odp") == 0 || strcasecmp(ext, "fodp") == 0) {
        return LO_APP_IMPRESS;
    }
    if (strcasecmp(ext, "odg") == 0 || strcasecmp(ext, "fodg") == 0 ||
        strcasecmp(ext, "svg") == 0) {
        return LO_APP_DRAW;
    }
    if (strcasecmp(ext, "odf") == 0 || strcasecmp(ext, "mml") == 0 ||
        strcasecmp(ext, "xml") == 0) {
        return LO_APP_MATH;
    }
    if (strcasecmp(ext, "odb") == 0) {
        return LO_APP_BASE;
    }
    return LO_APP_NONE;
}

static int detect_app_from_fmt(const char *fmt) {
    if (!fmt) return LO_APP_NONE;
    if (strcasecmp(fmt, "txt") == 0 || strcasecmp(fmt, "html") == 0 ||
        strcasecmp(fmt, "pdf") == 0) {
        return LO_APP_WRITER;
    }
    if (strcasecmp(fmt, "csv") == 0) {
        return LO_APP_CALC;
    }
    if (strcasecmp(fmt, "svg") == 0) {
        return LO_APP_DRAW;
    }
    if (strcasecmp(fmt, "mml") == 0 || strcasecmp(fmt, "xml") == 0 ||
        strcasecmp(fmt, "mathml") == 0) {
        return LO_APP_MATH;
    }
    return LO_APP_NONE;
}

enum {
    VIEW_START_CARDS = 0,
    VIEW_REMOTE = 1,
    VIEW_RECENT_DOCS = 2,
    VIEW_TEMPLATES = 3
};
static int s_soffice_view_mode = VIEW_START_CARDS;

static void render_start_center(lo_ui_state_t *ui, lo_vcl_surface_t *surf) {
    if (!surf || !lo_vcl_surface_is_x11(surf)) return;
    int w = lo_vcl_surface_get_width(surf);
    int h = lo_vcl_surface_get_height(surf);
    if (ui) lo_ui_layout(ui, w, h);

    /* Background */
    lo_vcl_clear(surf, 0x00FAFAFA);

    /* 1. Left Dark-Slate Sidebar: x = 0..210, y = 24..h-22 */
    lo_vcl_draw_rect(surf, 0, 24, 210, h - 46, 0x0033393B, true);
    lo_vcl_draw_line(surf, 209, 24, 209, h - 23, 0x002B2E30, 1);

    /* Action buttons in sidebar */
    struct {
        int y;
        const char *label;
        lo_ui_icon_id_t icon;
        int mode;
    } side_actions[] = {
        { 34, "Open File...", LO_ICON_OPEN, -1 },
        { 64, "Remote Files", LO_ICON_NONE, VIEW_REMOTE },
        { 94, "Recent Documents", LO_ICON_NONE, VIEW_RECENT_DOCS },
        { 124, "Templates ▾", LO_ICON_NONE, VIEW_TEMPLATES }
    };
    for (int i = 0; i < 4; i++) {
        uint32_t bg = (side_actions[i].mode == s_soffice_view_mode && s_soffice_view_mode != VIEW_START_CARDS)
                      ? LO_COLOR_SELECT_ACCENT : 0x004A4F52;
        lo_vcl_fill_rounded_rect(surf, 12, side_actions[i].y, 186, 26, 3, bg);
        if (side_actions[i].icon != LO_ICON_NONE) {
            lo_ui_draw_icon(surf, 18, side_actions[i].y + 5, side_actions[i].icon, 16);
            lo_vcl_draw_text_aa(surf, 40, side_actions[i].y + 6, side_actions[i].label, 12, LO_VCL_FONT_SANS, 0x00FFFFFF);
        } else {
            lo_vcl_draw_text_aa(surf, 20, side_actions[i].y + 6, side_actions[i].label, 12, LO_VCL_FONT_SANS, 0x00FFFFFF);
        }
    }

    /* Divider */
    lo_vcl_draw_line(surf, 12, 158, 198, 158, 0x004A4F52, 1);
    lo_vcl_draw_text_aa(surf, 16, 168, "CREATE:", 11, LO_VCL_FONT_SANS_BOLD, 0x009A9996);

    /* 6 Module launch buttons */
    struct {
        int y;
        const char *name;
        lo_ui_icon_id_t icon;
        uint32_t brand_color;
    } side_modules[] = {
        { 186, "Writer Document",    LO_ICON_APP_WRITER,  0x001C71D8 },
        { 222, "Calc Spreadsheet",   LO_ICON_APP_CALC,    0x0026A269 },
        { 258, "Impress Presentation",LO_ICON_APP_IMPRESS, 0x00E66100 },
        { 294, "Draw Drawing",        LO_ICON_APP_DRAW,    0x00E5A50A },
        { 330, "Math Formula",        LO_ICON_APP_MATH,    0x009141AC },
        { 366, "Base Database",       LO_ICON_APP_BASE,    0x00613583 }
    };

    for (int i = 0; i < 6; i++) {
        int sy = side_modules[i].y;
        lo_vcl_fill_rounded_rect(surf, 12, sy, 186, 30, 4, 0x003D4346);
        lo_ui_draw_icon(surf, 18, sy + 7, side_modules[i].icon, 16);
        lo_vcl_draw_text_aa(surf, 42, sy + 8, side_modules[i].name, 12, LO_VCL_FONT_SANS, 0x00FFFFFF);
    }

    /* 2. Right Pane: depends on s_soffice_view_mode */
    if (s_soffice_view_mode == VIEW_RECENT_DOCS) {
        lo_vcl_draw_text_aa(surf, 240, 45, "Recent Documents", 24, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
        lo_vcl_draw_text_aa(surf, 240, 75, "Click a recent document to resume editing.", 12, LO_VCL_FONT_SANS, 0x005E5C64);

        struct {
            int col, row;
            const char *file;
            const char *type;
            lo_ui_icon_id_t icon;
            uint32_t color;
        } recents[] = {
            { 0, 0, "document.odt", "Writer Text Document", LO_ICON_APP_WRITER, 0x001C71D8 },
            { 1, 0, "sheet.ods", "Calc Spreadsheet", LO_ICON_APP_CALC, 0x0026A269 },
            { 2, 0, "slides.odp", "Impress Presentation", LO_ICON_APP_IMPRESS, 0x00E66100 },
            { 0, 1, "drawing.odg", "Draw Vector Drawing", LO_ICON_APP_DRAW, 0x00E5A50A },
            { 1, 1, "formula.odf", "Math Formula AST", LO_ICON_APP_MATH, 0x009141AC },
            { 2, 1, "database.odb", "Base SQLite Database", LO_ICON_APP_BASE, 0x00613583 }
        };
        int card_w = 175, card_h = 120, card_base_x = 240, card_gap_x = 12;
        for (int i = 0; i < 6; i++) {
            int cx = card_base_x + recents[i].col * (card_w + card_gap_x);
            int cy = 115 + recents[i].row * (card_h + 20);
            lo_vcl_draw_shadow(surf, cx, cy, card_w, card_h, 4, 3, LO_COLOR_SHADOW);
            lo_vcl_fill_rounded_rect(surf, cx, cy, card_w, card_h, 4, 0x00FFFFFF);
            lo_vcl_draw_rounded_rect(surf, cx, cy, card_w, card_h, 4, 0x00CFCFCF, 1);
            lo_vcl_fill_rounded_rect(surf, cx, cy, card_w, 6, 4, recents[i].color);
            lo_vcl_draw_rect(surf, cx, cy + 3, card_w, 3, recents[i].color, true);
            lo_ui_draw_icon(surf, cx + 12, cy + 16, recents[i].icon, 20);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 44, recents[i].file, 12, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 66, recents[i].type, 10, LO_VCL_FONT_SANS, 0x005E5C64);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 96, "Click to open", 10, LO_VCL_FONT_SANS, 0x009A9996);
        }
    } else if (s_soffice_view_mode == VIEW_TEMPLATES) {
        lo_vcl_draw_text_aa(surf, 240, 45, "Templates & Starters", 24, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
        lo_vcl_draw_text_aa(surf, 240, 75, "Select a starter template to create a new document.", 12, LO_VCL_FONT_SANS, 0x005E5C64);

        struct {
            int col, row;
            const char *title;
            const char *desc;
            lo_ui_icon_id_t icon;
            uint32_t color;
        } tmpls[] = {
            { 0, 0, "Modern Resume", "Pre-formatted Writer CV", LO_ICON_APP_WRITER, 0x001C71D8 },
            { 1, 0, "Annual Budget", "Calc Formulas & Totals", LO_ICON_APP_CALC, 0x0026A269 },
            { 2, 0, "Pitch Deck", "Impress 16:9 Slide Deck", LO_ICON_APP_IMPRESS, 0x00E66100 },
            { 0, 1, "Flowchart Diagram", "Draw Shapes & Connectors", LO_ICON_APP_DRAW, 0x00E5A50A },
            { 1, 1, "Scientific Paper", "Math & StarMath Symbols", LO_ICON_APP_MATH, 0x009141AC },
            { 2, 1, "Customer CRM", "Base Tables & Queries", LO_ICON_APP_BASE, 0x00613583 }
        };
        int card_w = 175, card_h = 120, card_base_x = 240, card_gap_x = 12;
        for (int i = 0; i < 6; i++) {
            int cx = card_base_x + tmpls[i].col * (card_w + card_gap_x);
            int cy = 115 + tmpls[i].row * (card_h + 20);
            lo_vcl_draw_shadow(surf, cx, cy, card_w, card_h, 4, 3, LO_COLOR_SHADOW);
            lo_vcl_fill_rounded_rect(surf, cx, cy, card_w, card_h, 4, 0x00FFFFFF);
            lo_vcl_draw_rounded_rect(surf, cx, cy, card_w, card_h, 4, 0x00CFCFCF, 1);
            lo_vcl_fill_rounded_rect(surf, cx, cy, card_w, 6, 4, tmpls[i].color);
            lo_vcl_draw_rect(surf, cx, cy + 3, card_w, 3, tmpls[i].color, true);
            lo_ui_draw_icon(surf, cx + 12, cy + 16, tmpls[i].icon, 20);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 44, tmpls[i].title, 12, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 66, tmpls[i].desc, 10, LO_VCL_FONT_SANS, 0x005E5C64);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 96, "Click to create", 10, LO_VCL_FONT_SANS, 0x009A9996);
        }
    } else if (s_soffice_view_mode == VIEW_REMOTE) {
        lo_vcl_draw_text_aa(surf, 240, 45, "Remote Files & Cloud Services", 24, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
        lo_vcl_draw_text_aa(surf, 240, 75, "Connect to remote filesystems, WebDAV, or network shares.", 12, LO_VCL_FONT_SANS, 0x005E5C64);

        int rx = 240, ry = 115, rw = 520, rh = 160;
        lo_vcl_fill_rounded_rect(surf, rx, ry, rw, rh, 4, 0x00FFFFFF);
        lo_vcl_draw_rounded_rect(surf, rx, ry, rw, rh, 4, 0x00CFCFCF, 1);
        lo_vcl_draw_text_aa(surf, rx + 20, ry + 20, "HobbyOS Network VFS: Connected", 13, LO_VCL_FONT_SANS_BOLD, 0x0026A269);
        lo_vcl_draw_text_aa(surf, rx + 20, ry + 50, "Mount path: /home (Local FAT-16 storage)", 11, LO_VCL_FONT_SANS, 0x002E3436);
        lo_vcl_draw_text_aa(surf, rx + 20, ry + 75, "Remote share: //hobbyos/share (Ready)", 11, LO_VCL_FONT_SANS, 0x002E3436);
        lo_vcl_draw_text_aa(surf, rx + 20, ry + 110, "Click 'Open File...' on the left or press Ctrl+O to open remote documents.", 11, LO_VCL_FONT_SANS, 0x005E5C64);
    } else {
        /* Standard 6 Cards in 3x2 grid */
        lo_vcl_draw_text_aa(surf, 240, 45, "LibreOffice", 24, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
        lo_vcl_draw_text_aa(surf, 240, 75, "Open a document or select an application on the left to get started.", 12, LO_VCL_FONT_SANS, 0x005E5C64);

        struct {
            int col, row;
            const char *key;
            const char *title;
            const char *desc;
            uint32_t color;
            lo_ui_icon_id_t icon;
        } cards[] = {
            { 0, 0, "[1]", "Writer Document",    "Word processing (.odt)",            0x001C71D8, LO_ICON_APP_WRITER },
            { 1, 0, "[2]", "Calc Spreadsheet",   "Spreadsheet & formulas (.ods)",      0x0026A269, LO_ICON_APP_CALC },
            { 2, 0, "[3]", "Impress Presentation","Slides & presentations (.odp)",     0x00E66100, LO_ICON_APP_IMPRESS },
            { 0, 1, "[4]", "Draw Drawing",        "Vector illustration (.odg)",        0x00E5A50A, LO_ICON_APP_DRAW },
            { 1, 1, "[5]", "Math Formula",        "2D formula typesetter (.odf)",      0x009141AC, LO_ICON_APP_MATH },
            { 2, 1, "[6]", "Base Database",       "SQLite relational database (.odb)", 0x00613583, LO_ICON_APP_BASE }
        };

        int card_w = 175;
        int card_h = 120;
        int card_base_x = 240;
        int card_gap_x = 12;

        for (int i = 0; i < 6; i++) {
            int cx = card_base_x + cards[i].col * (card_w + card_gap_x);
            int cy = 115 + cards[i].row * (card_h + 20);

            lo_vcl_draw_shadow(surf, cx, cy, card_w, card_h, 4, 3, LO_COLOR_SHADOW);
            lo_vcl_fill_rounded_rect(surf, cx, cy, card_w, card_h, 4, 0x00FFFFFF);
            lo_vcl_draw_rounded_rect(surf, cx, cy, card_w, card_h, 4, 0x00CFCFCF, 1);

            lo_vcl_fill_rounded_rect(surf, cx, cy, card_w, 6, 4, cards[i].color);
            lo_vcl_draw_rect(surf, cx, cy + 3, card_w, 3, cards[i].color, true);

            lo_ui_draw_icon(surf, cx + 12, cy + 16, cards[i].icon, 20);
            lo_vcl_fill_rounded_rect(surf, cx + card_w - 32, cy + 14, 22, 16, 2, cards[i].color);
            lo_vcl_draw_text_aa(surf, cx + card_w - 29, cy + 16, cards[i].key, 10, LO_VCL_FONT_SANS_BOLD, 0x00FFFFFF);

            lo_vcl_draw_text_aa(surf, cx + 12, cy + 44, cards[i].title, 12, LO_VCL_FONT_SANS_BOLD, 0x002E3436);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 66, cards[i].desc, 10, LO_VCL_FONT_SANS, 0x005E5C64);
            lo_vcl_draw_text_aa(surf, cx + 12, cy + 96, "Click or press key", 10, LO_VCL_FONT_SANS, 0x009A9996);
        }
    }

    /* 3. Render Menu Bar, Status Bar, and Popups */
    if (ui) {
        lo_ui_render_menu_bar(ui, surf);
        lo_ui_render_status_bar(ui, surf);
        lo_ui_render_popups(ui, surf);
    }
}

void lo_soffice_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);
    snprintf(ui->app_name, sizeof(ui->app_name), "LibreOffice Start Center");

    static lo_ui_menu_item_t file_items[] = {
        { "Open...", 'O', LO_ICON_OPEN, "Ctrl+O", LO_ACTION_OPEN, false, false, false, true },
        { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
        { "Exit", 'x', LO_ICON_NONE, "Ctrl+Q", LO_ACTION_QUIT, false, false, false, true }
    };
    static lo_ui_menu_item_t help_items[] = {
        { "About LibreOffice", 'A', LO_ICON_APP_SOFFICE, NULL, LO_ACTION_ABOUT, false, false, false, true }
    };
    static lo_ui_menu_t top_menus[2];
    top_menus[0].title = "File";
    top_menus[0].mnemonic = 'F';
    top_menus[0].items = file_items;
    top_menus[0].item_count = 3;
    top_menus[0].dropdown_w = 180;

    top_menus[1].title = "Help";
    top_menus[1].mnemonic = 'H';
    top_menus[1].items = help_items;
    top_menus[1].item_count = 1;
    top_menus[1].dropdown_w = 180;

    ui->menu_bar.menus = top_menus;
    ui->menu_bar.menu_count = 2;
    ui->toolbar_count = 0;
    strcpy(ui->status_bar.stats_text, "LibreOffice 24.8 (HobbyOS Edition)");
    strcpy(ui->status_bar.mode_text, "Press 1-6 or click to start. Ctrl+Q to exit.");
    lo_ui_layout(ui, 800, 600);
}

static int run_start_center(int argc, char **argv, bool headless) {
    (void)argc;
    (void)argv;
    if (headless) {
        return 0;
    }
    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice", "soffice", 800, 600);
    if (!surf) return LO_ERR_GENERIC;
    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        return 0;
    }

    lo_ui_state_t ui;
    lo_soffice_init_ui(&ui);

    render_start_center(&ui, surf);
    lo_vcl_present(surf);

    bool running = true;
    int launch_app = LO_APP_NONE;
    const char *launch_file_buf = NULL;
    char path_storage[256] = {0};

    while (running) {
        lo_vcl_event_t ev;
        if (lo_vcl_wait_event(surf, &ev) != 0) break;

        int ui_act = lo_ui_handle_event(&ui, &ev, surf);
        if (ui_act == LO_ACTION_QUIT) {
            running = false;
            break;
        } else if (ui_act == LO_ACTION_ABOUT) {
            lo_ui_open_about(&ui, "LibreOffice Start Center");
            render_start_center(&ui, surf);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN) {
            lo_ui_open_prompt(&ui, "Open Document", "Enter document path (.odt, .ods, .odp, .odg, .odf, .odb):", "/home/", LO_ACTION_OPEN_CONFIRM);
            render_start_center(&ui, surf);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                snprintf(path_storage, sizeof(path_storage), "%s", ui.prompt_value);
                ui.prompt_value[0] = '\0';
                int det = detect_app_from_ext(path_storage);
                if (det != LO_APP_NONE) {
                    launch_app = det;
                    launch_file_buf = path_storage;
                    running = false;
                    break;
                }
            }
            ui.prompt_value[0] = '\0';
            render_start_center(&ui, surf);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == 0) {
            render_start_center(&ui, surf);
            lo_vcl_present(surf);
            continue;
        }

        switch (ev.type) {
            case LO_VCL_EVENT_EXPOSE:
            case LO_VCL_EVENT_RESIZE:
                render_start_center(&ui, surf);
                lo_vcl_present(surf);
                break;
            case LO_VCL_EVENT_CLOSE:
                running = false;
                break;
            case LO_VCL_EVENT_KEY_PRESS:
                if ((ev.key.modifiers & LO_VCL_MOD_CTRL) &&
                    (ev.key.keysym == XK_q || ev.key.keysym == XK_Q || ev.key.text[0] == 17)) {
                    running = false;
                    break;
                }
                if (ev.key.text[0] == '1' || ev.key.keysym == XK_1 || ev.key.keysym == XK_KP_1) {
                    launch_app = LO_APP_WRITER; running = false;
                } else if (ev.key.text[0] == '2' || ev.key.keysym == XK_2 || ev.key.keysym == XK_KP_2) {
                    launch_app = LO_APP_CALC; running = false;
                } else if (ev.key.text[0] == '3' || ev.key.keysym == XK_3 || ev.key.keysym == XK_KP_3) {
                    launch_app = LO_APP_IMPRESS; running = false;
                } else if (ev.key.text[0] == '4' || ev.key.keysym == XK_4 || ev.key.keysym == XK_KP_4) {
                    launch_app = LO_APP_DRAW; running = false;
                } else if (ev.key.text[0] == '5' || ev.key.keysym == XK_5 || ev.key.keysym == XK_KP_5) {
                    launch_app = LO_APP_MATH; running = false;
                } else if (ev.key.text[0] == '6' || ev.key.keysym == XK_6 || ev.key.keysym == XK_KP_6) {
                    launch_app = LO_APP_BASE; running = false;
                }
                break;
            case LO_VCL_EVENT_MOUSE_DOWN: {
                int mx = ev.mouse.x;
                int my = ev.mouse.y;

                /* Click on Left Sidebar buttons */
                if (mx >= 12 && mx <= 198) {
                    if (my >= 34 && my < 60) {
                        lo_ui_open_prompt(&ui, "Open Document", "Enter document path (.odt, .ods, .odp, .odg, .odf, .odb):", "/home/", LO_ACTION_OPEN_CONFIRM);
                        render_start_center(&ui, surf);
                        lo_vcl_present(surf);
                        break;
                    } else if (my >= 64 && my < 90) {
                        s_soffice_view_mode = (s_soffice_view_mode == VIEW_REMOTE) ? VIEW_START_CARDS : VIEW_REMOTE;
                    } else if (my >= 94 && my < 120) {
                        s_soffice_view_mode = (s_soffice_view_mode == VIEW_RECENT_DOCS) ? VIEW_START_CARDS : VIEW_RECENT_DOCS;
                    } else if (my >= 124 && my < 150) {
                        s_soffice_view_mode = (s_soffice_view_mode == VIEW_TEMPLATES) ? VIEW_START_CARDS : VIEW_TEMPLATES;
                    } else if (my >= 186 && my < 216) { launch_app = LO_APP_WRITER; running = false; }
                    else if (my >= 222 && my < 252) { launch_app = LO_APP_CALC; running = false; }
                    else if (my >= 258 && my < 288) { launch_app = LO_APP_IMPRESS; running = false; }
                    else if (my >= 294 && my < 324) { launch_app = LO_APP_DRAW; running = false; }
                    else if (my >= 330 && my < 360) { launch_app = LO_APP_MATH; running = false; }
                    else if (my >= 366 && my < 396) { launch_app = LO_APP_BASE; running = false; }
                }

                /* Click on Right Cards / Views */
                if (mx >= 240 && mx < 800) {
                    int card_w = 175;
                    int card_h = 120;
                    int card_base_x = 240;
                    int card_gap_x = 12;
                    for (int i = 0; i < 6; i++) {
                        int col = i % 3;
                        int row = i / 3;
                        int cx = card_base_x + col * (card_w + card_gap_x);
                        int cy = 115 + row * (card_h + 20);
                        if (mx >= cx && mx <= cx + card_w && my >= cy && my <= cy + card_h) {
                            if (s_soffice_view_mode == VIEW_RECENT_DOCS) {
                                const char *rec_files[] = {
                                    "/home/document.odt", "/home/sheet.ods", "/home/slides.odp",
                                    "/home/drawing.odg", "/home/formula.odf", "/home/database.odb"
                                };
                                launch_app = i + 1;
                                launch_file_buf = rec_files[i];
                                running = false;
                                break;
                            } else if (s_soffice_view_mode == VIEW_TEMPLATES || s_soffice_view_mode == VIEW_START_CARDS) {
                                launch_app = i + 1;
                                running = false;
                                break;
                            }
                        }
                    }
                }
                render_start_center(&ui, surf);
                lo_vcl_present(surf);
                break;
            }
            case LO_VCL_EVENT_MOUSE_MOVE:
                render_start_center(&ui, surf);
                lo_vcl_present(surf);
                break;
            default:
                break;
        }
    }

    lo_vcl_surface_destroy(surf);

    if (launch_app != LO_APP_NONE) {
        char *sub_argv[3] = {NULL, NULL, NULL};
        int sub_argc = 1;
        if (launch_file_buf) {
            sub_argv[1] = (char*)launch_file_buf;
            sub_argc = 2;
        }
        switch (launch_app) {
            case LO_APP_WRITER: sub_argv[0] = "swriter"; return run_writer_main(sub_argc, sub_argv, false);
            case LO_APP_CALC: sub_argv[0] = "scalc"; return run_calc_main(sub_argc, sub_argv, false);
            case LO_APP_IMPRESS: sub_argv[0] = "simpress"; return run_impress_main(sub_argc, sub_argv, false);
            case LO_APP_DRAW: sub_argv[0] = "sdraw"; return run_draw_main(sub_argc, sub_argv, false);
            case LO_APP_MATH: sub_argv[0] = "smath"; return run_math_main(sub_argc, sub_argv, false);
            case LO_APP_BASE: sub_argv[0] = "sbase"; return run_base_main(sub_argc, sub_argv, false);
            default: break;
        }
    }

    return 0;
}

int run_soffice_main(int argc, char **argv) {
    if (argc <= 0 || !argv) return 0;

    bool headless = false;
    int target_app = LO_APP_NONE;
    const char *convert_fmt = NULL;
    const char *input_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--writer") == 0) {
            target_app = LO_APP_WRITER;
        } else if (strcmp(argv[i], "--calc") == 0) {
            target_app = LO_APP_CALC;
        } else if (strcmp(argv[i], "--impress") == 0) {
            target_app = LO_APP_IMPRESS;
        } else if (strcmp(argv[i], "--draw") == 0) {
            target_app = LO_APP_DRAW;
        } else if (strcmp(argv[i], "--math") == 0) {
            target_app = LO_APP_MATH;
        } else if (strcmp(argv[i], "--base") == 0) {
            target_app = LO_APP_BASE;
        } else if (strcmp(argv[i], "--convert-to") == 0) {
            if (i + 1 < argc) {
                convert_fmt = argv[++i];
                headless = true;
            } else {
                return LO_ERR_NULL_ARG;
            }
        } else if (argv[i][0] != '-') {
            if (!input_file) {
                input_file = argv[i];
            }
        }
    }

    /* Resolution hierarchy:
     * 1. Explicit CLI application flag
     * 2. Input file extension
     * 3. Target conversion format (if --convert-to used)
     * 4. Multi-call binary name from argv[0]
     * 5. Start Center (if interactive with no args) or fallback to Writer
     */
    if (target_app == LO_APP_NONE && input_file) {
        target_app = detect_app_from_ext(input_file);
    }
    if (target_app == LO_APP_NONE && convert_fmt) {
        target_app = detect_app_from_fmt(convert_fmt);
    }
    if (target_app == LO_APP_NONE && argv[0]) {
        target_app = detect_app_from_name(argv[0]);
    }
    if (target_app == LO_APP_NONE) {
        if (convert_fmt) {
            return LO_ERR_NOT_FOUND;
        }
        return run_start_center(argc, argv, headless);
    }

    switch (target_app) {
        case LO_APP_CALC:
            return run_calc_main(argc, argv, headless);
        case LO_APP_IMPRESS:
            return run_impress_main(argc, argv, headless);
        case LO_APP_DRAW:
            return run_draw_main(argc, argv, headless);
        case LO_APP_MATH:
            return run_math_main(argc, argv, headless);
        case LO_APP_BASE:
            return run_base_main(argc, argv, headless);
        case LO_APP_WRITER:
        default:
            return run_writer_main(argc, argv, headless);
    }
}
