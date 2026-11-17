#ifndef LO_UI_H
#define LO_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lo_vcl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Linux Adwaita / GTK3 Theme Palette (32-bit ARGB/XRGB: 0x00RRGGBB)
 * ========================================================================= */
#define LO_COLOR_BG_CHROME        0x00F6F5F4  /* Adwaita light main chrome */
#define LO_COLOR_BG_SECONDARY     0x00EFEFEF  /* Formatting bar, status bar, docks */
#define LO_COLOR_BG_CANVAS        0x00DCDAD5  /* Document workspace backdrop */
#define LO_COLOR_BG_WHITE         0x00FFFFFF  /* Document page, popup background */
#define LO_COLOR_HOVER_PILL       0x00E4E4E4  /* Button hover pill background */
#define LO_COLOR_ACTIVE_PILL      0x00D0D0D0  /* Button active/toggled background */
#define LO_COLOR_ACTIVE_BORDER    0x009A9A9A  /* 1px inner border for active buttons */
#define LO_COLOR_SEPARATOR        0x00CFCFCF  /* 1px dividers between groups */
#define LO_COLOR_MENU_BG          0x00FFFFFF  /* Menu popup background */
#define LO_COLOR_MENU_BORDER      0x00BDBDBD  /* Menu popup 1px outer border */
#define LO_COLOR_SELECT_ACCENT    0x003584E4  /* Selection / hover row highlight */
#define LO_COLOR_SELECT_TEXT      0x00FFFFFF  /* Text on selection */
#define LO_COLOR_TEXT_PRIMARY     0x002E3436  /* Primary dark charcoal text */
#define LO_COLOR_TEXT_SECONDARY   0x005E5C64  /* Secondary / shortcut text */
#define LO_COLOR_TEXT_DISABLED    0x009A9996  /* Disabled item text */
#define LO_COLOR_GRIP             0x009A9A9A  /* Toolbar drag grip dots */
#define LO_COLOR_SHADOW           0x40000000  /* Soft drop shadow (ARGB alpha) */

/* Brand App Colors */
#define LO_COLOR_BRAND_WRITER     0x001C71D8  /* Blue */
#define LO_COLOR_BRAND_CALC       0x0026A269  /* Green */
#define LO_COLOR_BRAND_IMPRESS    0x00E66100  /* Orange */
#define LO_COLOR_BRAND_DRAW       0x00E5A50A  /* Amber / Gold */
#define LO_COLOR_BRAND_MATH       0x009141AC  /* Magenta / Slate */
#define LO_COLOR_BRAND_BASE       0x00613583  /* Purple */

/* Text Formatting Styles */
#ifndef LO_TEXT_STYLE_BOLD
#define LO_TEXT_STYLE_NORMAL     0
#define LO_TEXT_STYLE_BOLD       (1 << 0)
#define LO_TEXT_STYLE_ITALIC     (1 << 1)
#define LO_TEXT_STYLE_UNDERLINE  (1 << 2)
#endif
#define LO_COLOR_BRAND_SOFFICE    0x0033393B  /* Dark Charcoal */

/* =========================================================================
 * 2. Icon Identifiers (Colibre / Breeze Procedural Icons)
 * ========================================================================= */
typedef enum {
    LO_ICON_NONE = 0,
    LO_ICON_NEW,
    LO_ICON_OPEN,
    LO_ICON_SAVE,
    LO_ICON_SAVE_AS,
    LO_ICON_PRINT,
    LO_ICON_PDF,
    LO_ICON_CUT,
    LO_ICON_COPY,
    LO_ICON_PASTE,
    LO_ICON_UNDO,
    LO_ICON_REDO,
    LO_ICON_BOLD,
    LO_ICON_ITALIC,
    LO_ICON_UNDERLINE,
    LO_ICON_FONT_COLOR,
    LO_ICON_HIGHLIGHT_COLOR,
    LO_ICON_ALIGN_LEFT,
    LO_ICON_ALIGN_CENTER,
    LO_ICON_ALIGN_RIGHT,
    LO_ICON_ALIGN_JUSTIFY,
    LO_ICON_BULLETS,
    LO_ICON_NUMBERS,
    LO_ICON_TABLE,
    LO_ICON_IMAGE,
    LO_ICON_FIND,
    LO_ICON_ZOOM_IN,
    LO_ICON_ZOOM_OUT,
    LO_ICON_FUNCTION,
    LO_ICON_SUM,
    LO_ICON_SHAPES,
    LO_ICON_SLIDESHOW,
    LO_ICON_DATABASE,
    /* Brand Module Icons */
    LO_ICON_APP_SOFFICE,
    LO_ICON_APP_WRITER,
    LO_ICON_APP_CALC,
    LO_ICON_APP_IMPRESS,
    LO_ICON_APP_DRAW,
    LO_ICON_APP_MATH,
    LO_ICON_APP_BASE,
    /* Specialized Tools */
    LO_ICON_DRAW_SELECT,
    LO_ICON_DRAW_RECT,
    LO_ICON_DRAW_ROUND_RECT,
    LO_ICON_DRAW_ELLIPSE,
    LO_ICON_DRAW_LINE,
    LO_ICON_DRAW_POLYGON,
    LO_ICON_DRAW_BEZIER,
    LO_ICON_DRAW_TEXT,
    LO_ICON_DRAW_CONNECTOR,
    LO_ICON_CALC_FX,
    LO_ICON_CALC_SIGMA,
    LO_ICON_CALC_EQUAL,
    LO_ICON_CALC_CURRENCY,
    LO_ICON_CALC_PERCENT,
    LO_ICON_CALC_DECIMAL,
    LO_ICON_BASE_TABLES,
    LO_ICON_BASE_QUERIES,
    LO_ICON_BASE_FORMS,
    LO_ICON_BASE_REPORTS,
    LO_ICON_COUNT
} lo_ui_icon_id_t;

/* =========================================================================
 * 3. Action Identifiers
 * ========================================================================= */
typedef enum {
    LO_ACTION_NONE = 0,
    LO_ACTION_NEW,
    LO_ACTION_OPEN,
    LO_ACTION_SAVE,
    LO_ACTION_SAVE_AS,
    LO_ACTION_EXPORT_PDF,
    LO_ACTION_PRINT,
    LO_ACTION_CLOSE,
    LO_ACTION_QUIT,
    LO_ACTION_UNDO,
    LO_ACTION_REDO,
    LO_ACTION_CUT,
    LO_ACTION_COPY,
    LO_ACTION_PASTE,
    LO_ACTION_SELECT_ALL,
    LO_ACTION_FIND,
    LO_ACTION_ZOOM_IN,
    LO_ACTION_ZOOM_OUT,
    LO_ACTION_ZOOM_100,
    LO_ACTION_ZOOM_FIT,
    LO_ACTION_ZOOM_CHANGE,
    LO_ACTION_SLIDESHOW,
    LO_ACTION_INSERT_IMAGE,
    LO_ACTION_INSERT_TABLE,
    LO_ACTION_INSERT_SHAPE,
    LO_ACTION_INSERT_SLIDE,
    LO_ACTION_INSERT_SHEET,
    LO_ACTION_BOLD,
    LO_ACTION_ITALIC,
    LO_ACTION_UNDERLINE,
    LO_ACTION_ALIGN_LEFT,
    LO_ACTION_ALIGN_CENTER,
    LO_ACTION_ALIGN_RIGHT,
    LO_ACTION_ALIGN_JUSTIFY,
    LO_ACTION_BULLETS,
    LO_ACTION_NUMBERS,
    LO_ACTION_FONT_COLOR,
    LO_ACTION_HIGHLIGHT_COLOR,
    LO_ACTION_STYLE_CHANGE,
    LO_ACTION_FONT_CHANGE,
    LO_ACTION_SIZE_CHANGE,

    /* Extended Standard Actions (Milestone M11) */
    LO_ACTION_ABOUT,
    LO_ACTION_NEW_WINDOW,
    LO_ACTION_CLEAR_FORMATTING,
    LO_ACTION_WORD_COUNT,
    LO_ACTION_SPELLING,

    /* Calc Specific Actions */
    LO_ACTION_INSERT_CELLS,
    LO_ACTION_CALC_INSERT_CELLS = LO_ACTION_INSERT_CELLS,
    LO_ACTION_INSERT_ROWS,
    LO_ACTION_CALC_INSERT_ROWS = LO_ACTION_INSERT_ROWS,
    LO_ACTION_INSERT_COLS,
    LO_ACTION_CALC_INSERT_COLS = LO_ACTION_INSERT_COLS,
    LO_ACTION_INSERT_FUNCTION,
    LO_ACTION_CALC_INSERT_FUNC = LO_ACTION_INSERT_FUNCTION,
    LO_ACTION_FORMAT_CELLS,
    LO_ACTION_CALC_FORMAT_CELLS = LO_ACTION_FORMAT_CELLS,
    LO_ACTION_FORMAT_ROWS,
    LO_ACTION_CALC_FORMAT_ROWS = LO_ACTION_FORMAT_ROWS,
    LO_ACTION_FORMAT_COLS,
    LO_ACTION_CALC_FORMAT_COLS = LO_ACTION_FORMAT_COLS,
    LO_ACTION_PAGE_STYLE,
    LO_ACTION_CALC_PAGE_STYLE = LO_ACTION_PAGE_STYLE,
    LO_ACTION_STYLE_DEFAULT,
    LO_ACTION_CALC_STYLE_DEFAULT = LO_ACTION_STYLE_DEFAULT,
    LO_ACTION_STYLE_H1,
    LO_ACTION_CALC_STYLE_HEADING1 = LO_ACTION_STYLE_H1,
    LO_ACTION_STYLE_H2,
    LO_ACTION_STYLE_H3,
    LO_ACTION_STYLE_RESULT,
    LO_ACTION_CALC_STYLE_RESULT = LO_ACTION_STYLE_RESULT,
    LO_ACTION_DELETE_SHEET,
    LO_ACTION_CALC_DELETE_SHEET = LO_ACTION_DELETE_SHEET,
    LO_ACTION_RENAME_SHEET,
    LO_ACTION_CALC_RENAME_SHEET = LO_ACTION_RENAME_SHEET,
    LO_ACTION_SORT_ASC,
    LO_ACTION_CALC_SORT_ASC = LO_ACTION_SORT_ASC,
    LO_ACTION_SORT_DESC,
    LO_ACTION_CALC_SORT_DESC = LO_ACTION_SORT_DESC,
    LO_ACTION_AUTOFILTER,
    LO_ACTION_CALC_AUTOFILTER = LO_ACTION_AUTOFILTER,
    LO_ACTION_VALIDITY,
    LO_ACTION_CALC_VALIDITY = LO_ACTION_VALIDITY,
    LO_ACTION_RECALCULATE,
    LO_ACTION_CALC_RECALCULATE = LO_ACTION_RECALCULATE,
    LO_ACTION_GOAL_SEEK,
    LO_ACTION_CALC_GOAL_SEEK = LO_ACTION_GOAL_SEEK,
    LO_ACTION_SOLVER,
    LO_ACTION_CALC_SOLVER = LO_ACTION_SOLVER,

    /* Impress Specific Actions */
    LO_ACTION_INSERT_TEXT_BOX,
    LO_ACTION_IMPRESS_TEXT_BOX = LO_ACTION_INSERT_TEXT_BOX,
    LO_ACTION_FORMAT_TEXT,
    LO_ACTION_IMPRESS_FORMAT_TEXT = LO_ACTION_FORMAT_TEXT,
    LO_ACTION_SLIDE_PROPERTIES,
    LO_ACTION_IMPRESS_SLIDE_PROPS = LO_ACTION_SLIDE_PROPERTIES,
    LO_ACTION_DELETE_SLIDE,
    LO_ACTION_IMPRESS_DELETE_SLIDE = LO_ACTION_DELETE_SLIDE,
    LO_ACTION_DUPLICATE_SLIDE,
    LO_ACTION_IMPRESS_DUPLICATE_SLIDE = LO_ACTION_DUPLICATE_SLIDE,
    LO_ACTION_IMPRESS_SPELLING = LO_ACTION_SPELLING,

    /* Draw Specific Actions */
    LO_ACTION_POSITION_SIZE,
    LO_ACTION_DRAW_POS_SIZE = LO_ACTION_POSITION_SIZE,
    LO_ACTION_LINE_PROPERTIES,
    LO_ACTION_DRAW_LINE_PROPS = LO_ACTION_LINE_PROPERTIES,
    LO_ACTION_AREA_PROPERTIES,
    LO_ACTION_DRAW_AREA_PROPS = LO_ACTION_AREA_PROPERTIES,
    LO_ACTION_PAGE_NEW,
    LO_ACTION_DRAW_NEW_PAGE = LO_ACTION_PAGE_NEW,
    LO_ACTION_PAGE_DELETE,
    LO_ACTION_DRAW_DELETE_PAGE = LO_ACTION_PAGE_DELETE,
    LO_ACTION_SHAPE_FRONT,
    LO_ACTION_DRAW_BRING_FRONT = LO_ACTION_SHAPE_FRONT,
    LO_ACTION_SHAPE_BACK,
    LO_ACTION_DRAW_SEND_BACK = LO_ACTION_SHAPE_BACK,
    LO_ACTION_SHAPE_GROUP,
    LO_ACTION_DRAW_GROUP = LO_ACTION_SHAPE_GROUP,
    LO_ACTION_COLOR_REPLACER,
    LO_ACTION_DRAW_COLOR_REPLACER = LO_ACTION_COLOR_REPLACER,

    /* Math Specific Actions */
    LO_ACTION_MATH_OPERATORS,
    LO_ACTION_MATH_INSERT_OPERATORS = LO_ACTION_MATH_OPERATORS,
    LO_ACTION_MATH_RELATIONS,
    LO_ACTION_MATH_INSERT_RELATIONS = LO_ACTION_MATH_RELATIONS,
    LO_ACTION_MATH_FUNCTIONS,
    LO_ACTION_MATH_INSERT_FUNCTIONS = LO_ACTION_MATH_FUNCTIONS,
    LO_ACTION_MATH_FONT_SIZE,
    LO_ACTION_MATH_FONTS,
    LO_ACTION_MATH_ALIGNMENT,
    LO_ACTION_MATH_CATALOG,

    /* Base Specific Actions */
    LO_ACTION_BASE_TABLE,
    LO_ACTION_BASE_INSERT_TABLE = LO_ACTION_BASE_TABLE,
    LO_ACTION_BASE_QUERY,
    LO_ACTION_BASE_INSERT_QUERY = LO_ACTION_BASE_QUERY,
    LO_ACTION_BASE_FORM,
    LO_ACTION_BASE_INSERT_FORM = LO_ACTION_BASE_FORM,
    LO_ACTION_BASE_REPORT,
    LO_ACTION_BASE_INSERT_REPORT = LO_ACTION_BASE_REPORT,
    LO_ACTION_RELATIONSHIPS,
    LO_ACTION_BASE_RELATIONSHIPS = LO_ACTION_RELATIONSHIPS,
    LO_ACTION_USER_ADMIN,
    LO_ACTION_BASE_USER_ADMIN = LO_ACTION_USER_ADMIN,

    /* App-specific actions */
    LO_ACTION_APP_WRITER,
    LO_ACTION_APP_CALC,
    LO_ACTION_APP_IMPRESS,
    LO_ACTION_APP_DRAW,
    LO_ACTION_APP_MATH,
    LO_ACTION_APP_BASE,
    LO_ACTION_BASE_RUN_SQL,

    /* Modal Confirmation Action Identifiers (Milestone M12) */
    LO_ACTION_OPEN_CONFIRM,
    LO_ACTION_SAVE_AS_CONFIRM,
    LO_ACTION_PRINT_CONFIRM,
    LO_ACTION_FIND_CONFIRM,
    LO_ACTION_RENAME_SHEET_CONFIRM,
    LO_ACTION_CALC_RENAME_SHEET_CONFIRM = LO_ACTION_RENAME_SHEET_CONFIRM,
    LO_ACTION_MATH_FONT_SIZE_CONFIRM,
    LO_ACTION_CUSTOM_BASE = 1000
} lo_ui_action_t;

/* =========================================================================
 * 4. Data Structures
 * ========================================================================= */
typedef struct {
    int x, y, w, h;
} lo_ui_rect_t;

typedef enum {
    LO_UI_WIDGET_BUTTON,
    LO_UI_WIDGET_TOGGLE,
    LO_UI_WIDGET_SEPARATOR,
    LO_UI_WIDGET_COMBO,
    LO_UI_WIDGET_CUSTOM
} lo_ui_widget_type_t;

/* Menu Item */
typedef struct {
    const char *text;          /* e.g. "Save _As..." */
    char mnemonic;             /* e.g. 'A' */
    lo_ui_icon_id_t icon_id;   /* 16x16 icon or LO_ICON_NONE */
    const char *shortcut;      /* e.g. "Ctrl+S" */
    int action_id;             /* Action ID triggered on activation */
    bool is_separator;         /* True for divider rule */
    bool is_check;             /* True for checkable item */
    bool checked;              /* Check state */
    bool enabled;              /* Enabled state */
} lo_ui_menu_item_t;

/* Top Menu Header */
typedef struct {
    const char *title;         /* e.g. "File" */
    char mnemonic;             /* e.g. 'F' */
    lo_ui_menu_item_t *items;  /* Array of menu items */
    int item_count;            /* Count of items */
    lo_ui_rect_t bounds;       /* Calculated header bounds in menu bar */
    int dropdown_w;            /* Calculated dropdown popup width (~220px) */
} lo_ui_menu_t;

/* Menu Bar */
typedef struct {
    lo_ui_menu_t *menus;
    int menu_count;
    int active_menu;           /* Index of open menu (-1 if closed) */
    int hover_menu;            /* Index of hovered menu header (-1 if none) */
    int hover_item;            /* Index of hovered item inside open menu (-1 if none) */
    lo_ui_rect_t bounds;       /* (0, 0, width, 24) */
} lo_ui_menu_bar_t;

/* Combobox Widget */
typedef struct {
    const char **items;        /* Options array */
    int item_count;
    int selected_index;
    bool is_open;
    int hover_index;
    char text[64];             /* Explicit buffer for current selection text */
    lo_ui_rect_t bounds;
    int popup_w;
} lo_ui_combo_t;

/* Toolbar Item */
typedef struct {
    lo_ui_widget_type_t type;
    int action_id;
    lo_ui_icon_id_t icon_id;
    const char *tooltip;
    bool is_active;            /* Toggle active/pressed state */
    bool is_enabled;
    bool is_hovered;
    bool is_pressed;
    lo_ui_rect_t bounds;
    lo_ui_combo_t combo;       /* Used if type == LO_UI_WIDGET_COMBO */
} lo_ui_tool_item_t;

/* Toolbar Row */
typedef struct {
    lo_ui_tool_item_t *items;
    int item_count;
    lo_ui_rect_t bounds;       /* e.g. (0, 24, width, 28) */
    bool has_grip;             /* Show left ⋮ drag grip */
    uint32_t bg_color;         /* Row background color */
} lo_ui_toolbar_t;

/* Status Bar */
typedef struct {
    char stats_text[128];      /* Left stats text */
    char mode_text[128];       /* Center mode/selection text */
    int zoom_percent;          /* Current zoom percentage (e.g. 100) */
    int zoom_min;              /* Minimum zoom (20) */
    int zoom_max;              /* Maximum zoom (400) */
    bool is_dragging_zoom;     /* Tracking slider drag */
    lo_ui_rect_t bounds;       /* (0, height - 22, width, 22) */
    lo_ui_rect_t zoom_minus_rect;
    lo_ui_rect_t zoom_plus_rect;
    lo_ui_rect_t zoom_track_rect;
} lo_ui_status_bar_t;

/* Modal Dialog Types */
typedef enum {
    LO_UI_MODAL_NONE = 0,
    LO_UI_MODAL_ABOUT,
    LO_UI_MODAL_PROMPT,
    LO_UI_MODAL_PRINT,
    LO_UI_MODAL_INFO
} lo_ui_modal_type_t;

typedef struct {
    lo_ui_modal_type_t type;
    char title[64];
    char label[128];
    char text_input[256];
    int cursor_pos;
    int completion_action_id;
    char info_body[512];
    char print_doc_name[64];
    char print_spool_path[128];
    int print_page_count;
    lo_ui_rect_t dialog_rect;
    lo_ui_rect_t ok_rect;
    lo_ui_rect_t cancel_rect;
    lo_ui_rect_t input_rect;
} lo_ui_modal_t;

/* Unified UI State */
typedef struct {
    lo_ui_menu_bar_t menu_bar;
    lo_ui_toolbar_t toolbars[3]; /* 0: Standard, 1: Formatting, 2: Custom */
    int toolbar_count;
    lo_ui_status_bar_t status_bar;
    int active_combo_row;      /* Row containing open combobox (-1 if none) */
    int active_combo_item;     /* Item containing open combobox (-1 if none) */
    int window_width;
    int window_height;
    char app_name[64];         /* Module name e.g. "LibreOffice Writer" */
    char prompt_value[256];    /* Retained value after prompt confirmation */
    lo_ui_modal_t modal;       /* Active modal dialog */
} lo_ui_state_t;

/* =========================================================================
 * 5. Core API Functions
 * ========================================================================= */
void lo_ui_init(lo_ui_state_t *ui, int w, int h);
void lo_ui_layout(lo_ui_state_t *ui, int w, int h);
void lo_ui_get_doc_bounds(const lo_ui_state_t *ui, int *out_x, int *out_y, int *out_w, int *out_h);

void lo_ui_draw_icon(lo_vcl_surface_t *surf, int x, int y, lo_ui_icon_id_t icon_id, int size);
void lo_ui_render_menu_bar(lo_ui_state_t *ui, lo_vcl_surface_t *surf);
void lo_ui_render_toolbars(lo_ui_state_t *ui, lo_vcl_surface_t *surf);
void lo_ui_render_status_bar(lo_ui_state_t *ui, lo_vcl_surface_t *surf);
void lo_ui_render_popups(lo_ui_state_t *ui, lo_vcl_surface_t *surf);
void lo_ui_render_all(lo_ui_state_t *ui, lo_vcl_surface_t *surf);

/* Event Dispatcher:
 *   > 0 : Triggered Action ID
 *     0 : Consumed by UI (state changed, redraw required)
 *    -1 : Unhandled (passes through to document workspace)
 */
int lo_ui_handle_event(lo_ui_state_t *ui, const lo_vcl_event_t *ev, lo_vcl_surface_t *surf);

/* Menu & Toolbar Builders */
void lo_ui_build_standard_file_menu(lo_ui_menu_t *menu, const char *app_ext);
void lo_ui_build_standard_edit_menu(lo_ui_menu_t *menu);
void lo_ui_build_standard_view_menu(lo_ui_menu_t *menu);
void lo_ui_build_standard_help_menu(lo_ui_menu_t *menu);
void lo_ui_build_standard_toolbar(lo_ui_toolbar_t *tb, int app_action_save);
void lo_ui_build_formatting_toolbar(lo_ui_toolbar_t *tb);

/* Combobox Helpers */
int lo_ui_get_combo_selected(const lo_ui_state_t *ui, int action_id, const char **out_text);
void lo_ui_sync_combo(lo_ui_state_t *ui, int action_id, int selected_index, const char *text);

/* Toolbar Reset & Toggle Sync */
void lo_ui_init_standard_toolbars(lo_ui_state_t *ui, int app_action_save);
void lo_ui_sync_toggle(lo_ui_state_t *ui, int action_id, int toggled);

/* Modal Dialog APIs */
void lo_ui_open_about(lo_ui_state_t *ui, const char *app_name);
void lo_ui_open_prompt(lo_ui_state_t *ui, const char *title, const char *label, const char *initial_text, int completion_action_id);
void lo_ui_open_print_dialog(lo_ui_state_t *ui, const char *doc_title, int page_count, const char *spool_file, int completion_action_id);
void lo_ui_open_info_modal(lo_ui_state_t *ui, const char *title, const char *body);
void lo_ui_close_modal(lo_ui_state_t *ui);

/* Shared Clipboard APIs */
void lo_ui_clipboard_set(const char *text, int len);
const char *lo_ui_clipboard_get(int *out_len);
bool lo_ui_clipboard_has_text(void);
void lo_ui_clipboard_copy(const char *text);
int lo_ui_clipboard_paste(char *out_buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LO_UI_H */
