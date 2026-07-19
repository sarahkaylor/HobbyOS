/*
 * HobbyOS Notepad - A Notepad-like text editor for HobbyOS
 *
 * Features replicated from Windows Notepad:
 *   - File menu: New, Open, Save, Save As, Exit
 *   - Edit menu: Undo, Find, Find Next, Replace, Go To, Select All, Time/Date
 *   - Word wrap toggle
 *   - Insert date/time (F5 equivalent)
 *   - Status bar with filename, modified indicator, line:col
 *   - Single-level undo
 *   - Find and replace with wrap-around
 *
 * HobbyOS constraints:
 *   - No clipboard (copy/paste) - intentionally omitted
 *   - Text output via print() to stdout, captured by desktop window manager
 *   - Input via stdin (fd 0) - desktop sends key events as characters
 *   - Arrow keys sent as ESC sequences: ESC [ A/B/C/D
 *   - Menu selections sent as: ESC [ M <menu> ; <item> ~
 *   - No Ctrl key detection (desktop only handles Shift), so ESC enters command mode
 *   - No F-keys (desktop keymap doesn't map them), so commands use ESC + letter
 *   - Time from sysinfo(1,...) returns uptime in ms (no RTC)
 */

#include "libc.h"
#include "dialog.h"
#include "filedialog.h"

/* ---- Constants ---- */

#define MAX_TEXT       16384   /* Maximum document size */
#define DISPLAY_LINES   18     /* Lines visible in viewport */
#define LINE_WIDTH       78     /* Characters per line (word wrap) */
#define MAX_FILENAME     64     /* Maximum filename length */
#define MAX_SEARCH      128     /* Maximum search/replace string */
#define MAX_CMD         128     /* Command input buffer */
#define MAX_PROMPT      256     /* Prompt display buffer */

/* ---- Global State ---- */

char text[MAX_TEXT];            /* The document */
int  text_len = 0;              /* Current length */
int  cursor_pos = 0;            /* Cursor position in text */
int  modified = 0;              /* Modified flag (0 = clean, 1 = dirty) */
int  word_wrap = 1;             /* Word wrap on/off */
char filename[MAX_FILENAME] = "UNTITLED.TXT";

/* Scroll position - first visible line number */
int  top_line = 0;

/* Undo (single-level, like classic Notepad) */
char undo_buf[MAX_TEXT];
int  undo_len = 0;
int  has_undo = 0;

/* Search state */
char search_str[MAX_SEARCH];
int  search_len = 0;
int  last_found = -1;           /* Position of last match */

/* Command mode (ESC to toggle) */
int  in_command = 0;
char cmd_buf[MAX_CMD];
int  cmd_len = 0;

/* ---- String Utilities ---- */

static void my_strcpy(char *dest, const char *src) {
    int i = 0;
    while (src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

static void int_to_str(int val, char *buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int i = 0, neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* ---- Line Navigation ---- */

/* Find start of the logical line containing position pos */
static int line_start(int pos) {
    if (pos <= 0) return 0;
    int i = pos;
    /* If we're sitting on a newline, the line start is after the previous newline */
    if (i > 0 && text[i] == '\n') i--;
    while (i > 0 && text[i - 1] != '\n') i--;
    return i;
}

/* Find end of the logical line containing position pos (index of newline or text_len) */
static int line_end(int pos) {
    int i = pos;
    while (i < text_len && text[i] != '\n') i++;
    return i;
}

/* Find start of the previous logical line before the one containing pos */
static int prev_line_start(int pos) {
    int ls = line_start(pos);
    if (ls == 0) return 0;  /* Already at first line */
    /* ls is after a newline; go to the line before that newline */
    int i = ls - 1;  /* This is the newline character */
    if (i <= 0) return 0;
    i--;  /* Move before the newline */
    while (i > 0 && text[i - 1] != '\n') i--;
    return i;
}

/* Count line number of a position (0-based) */
static int line_num(int pos) {
    int line = 0;
    for (int i = 0; i < pos && i < text_len; i++) {
        if (text[i] == '\n') line++;
    }
    return line;
}

/* Find start of the Nth logical line (0-based) */
static int nth_line_start(int n) {
    if (n <= 0) return 0;
    int line = 0;
    for (int i = 0; i < text_len; i++) {
        if (text[i] == '\n') {
            line++;
            if (line == n) return i + 1;
        }
    }
    return text_len;
}

/* ---- Text Operations ---- */

static void save_undo(void) {
    for (int i = 0; i < text_len && i < MAX_TEXT; i++) {
        undo_buf[i] = text[i];
    }
    undo_len = text_len;
    has_undo = 1;
}

static void do_undo(void) {
    if (!has_undo) return;
    for (int i = 0; i < undo_len && i < MAX_TEXT; i++) {
        text[i] = undo_buf[i];
    }
    text_len = undo_len;
    text[text_len] = '\0';
    if (cursor_pos > text_len) cursor_pos = text_len;
    has_undo = 0;
    modified = 1;
}

/* Insert character c at cursor position */
static void insert_char(char c) {
    if (text_len >= MAX_TEXT - 1) return;
    save_undo();
    for (int i = text_len; i > cursor_pos; i--) {
        text[i] = text[i - 1];
    }
    text[cursor_pos] = c;
    cursor_pos++;
    text_len++;
    text[text_len] = '\0';
    modified = 1;
}

/* Delete character before cursor (backspace) */
static void delete_backspace(void) {
    if (cursor_pos <= 0) return;
    save_undo();
    for (int i = cursor_pos - 1; i < text_len - 1; i++) {
        text[i] = text[i + 1];
    }
    text_len--;
    cursor_pos--;
    text[text_len] = '\0';
    modified = 1;
}

/* Delete character at cursor (forward delete) */
static void delete_forward(void) {
    if (cursor_pos >= text_len) return;
    save_undo();
    for (int i = cursor_pos; i < text_len - 1; i++) {
        text[i] = text[i + 1];
    }
    text_len--;
    text[text_len] = '\0';
    modified = 1;
}

/* ---- Cursor Movement ---- */

static void cursor_left(void) {
    if (cursor_pos > 0) cursor_pos--;
}

static void cursor_right(void) {
    if (cursor_pos < text_len) cursor_pos++;
}

static void cursor_up(void) {
    int ls = line_start(cursor_pos);
    if (ls == 0) {
        cursor_pos = 0;
        return;
    }
    int prev_ls = prev_line_start(cursor_pos);
    int col = cursor_pos - ls;
    int prev_le = line_end(prev_ls);
    if (prev_ls + col <= prev_le) {
        cursor_pos = prev_ls + col;
    } else {
        cursor_pos = prev_le;
    }
}

static void cursor_down(void) {
    int ls = line_start(cursor_pos);
    int le = line_end(cursor_pos);
    if (le >= text_len) return;  /* Last line, no next line */
    int next_ls = le + 1;
    int next_le = line_end(next_ls);
    int col = cursor_pos - ls;
    if (next_ls + col <= next_le) {
        cursor_pos = next_ls + col;
    } else {
        cursor_pos = next_le;
    }
}

static void cursor_doc_start(void) {
    cursor_pos = 0;
}

/* ---- Display ---- */

/* Render the editor screen */
static void redraw(void) {
    print("\f");  /* Clear screen */

    /* Find the line number of the cursor */
    int cur_line = line_num(cursor_pos);

    /* Adjust scroll to keep cursor visible */
    if (cur_line < top_line) {
        top_line = cur_line;
    } else if (cur_line >= top_line + DISPLAY_LINES) {
        top_line = cur_line - DISPLAY_LINES + 1;
    }
    if (top_line < 0) top_line = 0;

    /* Find the start of the top_line-th line */
    int display_start = nth_line_start(top_line);

    /* Display lines from display_start */
    int pos = display_start;
    int lines_shown = 0;
    int cursor_shown = 0;

    while (pos <= text_len && lines_shown < DISPLAY_LINES) {
        /* Handle cursor at end of text */
        if (pos == cursor_pos && !cursor_shown) {
            print("_");
            cursor_shown = 1;
            if (pos < text_len && text[pos] == '\n') {
                print("\n");
                pos++;
                lines_shown++;
            }
            continue;
        }

        if (pos >= text_len) break;

        char c = text[pos];
        if (c == '\n') {
            print("\n");
            pos++;
            lines_shown++;
        } else {
            /* Build a line segment up to cursor or newline */
            char seg[LINE_WIDTH + 2];
            int seg_len = 0;
            int wrap_col = 0;
            while (pos < text_len && text[pos] != '\n' && wrap_col < LINE_WIDTH) {
                if (pos == cursor_pos && !cursor_shown) {
                    seg[seg_len++] = '_';
                    cursor_shown = 1;
                }
                seg[seg_len++] = text[pos];
                pos++;
                wrap_col++;
            }
            /* Check if cursor is at end of this segment */
            if (pos == cursor_pos && !cursor_shown) {
                seg[seg_len++] = '_';
                cursor_shown = 1;
            }
            seg[seg_len] = '\0';
            print(seg);

            if (pos < text_len && text[pos] == '\n') {
                print("\n");
                pos++;
                lines_shown++;
            } else if (pos < text_len && wrap_col >= LINE_WIDTH && word_wrap) {
                print("\n");
                lines_shown++;
            } else {
                /* End of logical line */
                print("\n");
                lines_shown++;
            }
        }

        if (lines_shown >= DISPLAY_LINES) break;
    }

    /* If cursor not yet shown (e.g., at very end) */
    if (!cursor_shown) {
        print("_");
    }

    /* Status bar */
    print("\n");
    print("--------------------------------\n");
    print("File: ");
    print(filename);
    if (modified) print(" *");
    print("  Line: ");
    char numbuf[16];
    int_to_str(cur_line + 1, numbuf);
    print(numbuf);
    print(" Col: ");
    int col = cursor_pos - line_start(cursor_pos);
    int_to_str(col + 1, numbuf);
    print(numbuf);
    print("  Wrap: ");
    print(word_wrap ? "ON" : "OFF");

    if (in_command) {
        print("\n: ");
        print(cmd_buf);
        print("_");
    }
}

/* ---- File Operations ---- */

static void file_new(void) {
    save_undo();
    text_len = 0;
    text[0] = '\0';
    cursor_pos = 0;
    modified = 0;
    my_strcpy(filename, "UNTITLED.TXT");
    top_line = 0;
}

static void file_open(const char *fname) {
    int fd = open(fname);
    if (fd < 0) {
        dialog_message("Error", "Cannot open file.");
        return;
    }
    save_undo();
    text_len = read(fd, text, MAX_TEXT - 1);
    if (text_len < 0) text_len = 0;
    text[text_len] = '\0';
    close(fd);
    cursor_pos = 0;
    top_line = 0;
    modified = 0;
    my_strcpy(filename, fname);
}

static void file_save(const char *fname) {
    int fd = open(fname);
    if (fd < 0) {
        dialog_message("Error", "Cannot create file.");
        return;
    }
    write(fd, text, text_len);
    close(fd);
    modified = 0;
    my_strcpy(filename, fname);
}

/* ---- Find / Replace ---- */

/* Search for search_str starting from pos. Returns match position or -1 */
static int find_from(int start) {
    if (search_len <= 0) return -1;
    for (int i = start; i <= text_len - search_len; i++) {
        int match = 1;
        for (int j = 0; j < search_len; j++) {
            if (text[i + j] != search_str[j]) { match = 0; break; }
        }
        if (match) return i;
    }
    /* Wrap around */
    for (int i = 0; i < start && i <= text_len - search_len; i++) {
        int match = 1;
        for (int j = 0; j < search_len; j++) {
            if (text[i + j] != search_str[j]) { match = 0; break; }
        }
        if (match) return i;
    }
    return -1;
}

static void do_find(void) {
    /* Prompt for search string using dialog library */
    if (!dialog_prompt("Find", "Search text", search_str, MAX_SEARCH)) return;
    search_len = 0;
    while (search_str[search_len]) search_len++;
    if (search_len <= 0) return;
    int pos = find_from(cursor_pos);
    if (pos >= 0) {
        cursor_pos = pos;
        last_found = pos;
    } else {
        dialog_message("Find", "Not found.");
    }
}

static void find_next(void) {
    if (search_len <= 0) {
        do_find();
        return;
    }
    int start = (last_found >= 0) ? last_found + 1 : cursor_pos;
    int pos = find_from(start);
    if (pos >= 0) {
        cursor_pos = pos;
        last_found = pos;
    } else {
        dialog_message("Find", "No more matches.");
    }
}

static void do_replace(void) {
    char replace_str[MAX_SEARCH];
    int replace_len = 0;

    /* Prompt for search string using dialog library */
    if (!dialog_prompt("Replace", "Find what", search_str, MAX_SEARCH)) return;
    search_len = 0;
    while (search_str[search_len]) search_len++;
    if (search_len <= 0) return;

    /* Prompt for replacement string using dialog library */
    replace_str[0] = '\0';
    if (!dialog_prompt("Replace", "Replace with", replace_str, MAX_SEARCH)) return;
    replace_len = 0;
    while (replace_str[replace_len]) replace_len++;

    /* Replace all occurrences */
    int count = 0;
    int i = 0;
    save_undo();
    while (i <= text_len - search_len) {
        int match = 1;
        for (int j = 0; j < search_len; j++) {
            if (text[i + j] != search_str[j]) { match = 0; break; }
        }
        if (match) {
            /* Delete search_len chars, insert replace_len chars */
            int diff = replace_len - search_len;
            if (text_len + diff >= MAX_TEXT) break;
            /* Shift text */
            if (diff > 0) {
                for (int k = text_len; k > i + search_len; k--) {
                    text[k + diff - 1] = text[k - 1];
                }
            } else if (diff < 0) {
                for (int k = i + search_len; k < text_len; k++) {
                    text[k + diff] = text[k];
                }
            }
            /* Insert replacement */
            for (int k = 0; k < replace_len; k++) {
                text[i + k] = replace_str[k];
            }
            text_len += diff;
            text[text_len] = '\0';
            i += replace_len;
            count++;
        } else {
            i++;
        }
    }
    if (cursor_pos > text_len) cursor_pos = text_len;
    if (count > 0) modified = 1;
    char nbuf[16];
    int_to_str(count, nbuf);
    char msg[40];
    /* Build "Replaced N occurrence(s)." message */
    my_strcpy(msg, "Replaced ");
    /* Append count */
    int ml = 9;
    /* Append number */
    if (count == 0) { msg[ml++] = '0'; }
    else {
        char tmp[16];
        int tl = 0;
        int v = count;
        while (v > 0) { tmp[tl++] = '0' + (v % 10); v /= 10; }
        while (tl > 0) msg[ml++] = tmp[--tl];
    }
    msg[ml] = '\0';
    dialog_message("Replace", msg);
}

/* ---- Date/Time ---- */

static void insert_datetime(void) {
    /* HobbyOS has no RTC; use system uptime from sysinfo */
    int ms = sysinfo(1, 0, 0);
    if (ms < 0) ms = 0;
    int total_sec = ms / 1000;
    int hours = (total_sec / 3600) % 24;
    int minutes = (total_sec / 60) % 60;
    int seconds = total_sec % 60;

    char dtstr[16];
    dtstr[0] = '[';
    dtstr[1] = '0' + (hours / 10);
    dtstr[2] = '0' + (hours % 10);
    dtstr[3] = ':';
    dtstr[4] = '0' + (minutes / 10);
    dtstr[5] = '0' + (minutes % 10);
    dtstr[6] = ':';
    dtstr[7] = '0' + (seconds / 10);
    dtstr[8] = '0' + (seconds % 10);
    dtstr[9] = ']';
    dtstr[10] = '\0';

    /* Insert the timestamp at cursor */
    for (int k = 0; dtstr[k]; k++) {
        insert_char(dtstr[k]);
    }
}

/* ---- Prompt Dialog ---- */

/* Show a prompt and read a string. Returns 1 on OK, 0 on cancel (ESC) */
/* ---- Go To Line ---- */

static void do_goto(void) {
    char buf[16];
    if (!dialog_prompt("Go To Line", "Line number", buf, sizeof(buf))) return;
    /* Parse line number */
    int line = 0;
    int i = 0;
    while (buf[i]) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            line = line * 10 + (buf[i] - '0');
        }
        i++;
    }
    if (line > 0) {
        cursor_pos = nth_line_start(line - 1);
    }
}

/* ---- Main Entry Point ---- */

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
#else
int main(void) {
#endif

    /* Register menus with the desktop window manager */
    gui_add_menu(0, "File", "New,Open,Save,Save As,Exit");
    gui_add_menu(1, "Edit", "Undo,Find,Find Next,Replace,Go To,Select All,Time/Date");

    /* Brief startup message */
    print("\fHobbyOS Notepad\n");
    print("ESC for commands, arrows to move\n");
    for (volatile int i = 0; i < 2000000; i++) {} /* brief delay */

    /* Try to load existing file */
    int fd = open(filename);
    if (fd >= 0) {
        text_len = read(fd, text, MAX_TEXT - 1);
        if (text_len < 0) text_len = 0;
        text[text_len] = '\0';
        close(fd);
    } else {
        text[0] = '\0';
        text_len = 0;
    }
    cursor_pos = 0;
    top_line = 0;

    redraw();

    /* ---- Main Event Loop ---- */

    while (1) {
        char c;
        if (read(0, &c, 1) > 0) {

            /* ---- ESC key: escape sequences or command mode ---- */
            if (c == 27) {
                /* Check if more data follows (escape sequence) */
                int avail = available(0);
                if (avail >= 2) {
                    char seq[8];
                    read(0, seq, 2);
                    if (seq[0] == '[') {
                        if (seq[1] == 'M') {
                            /* Menu selection: ESC [ M <menu> ; <item> ~ */
                            if (available(0) >= 4) {
                                read(0, seq + 2, 4);
                                if (seq[2] >= '0' && seq[3] == ';') {
                                    int menu = seq[2] - '0';
                                    int item = seq[4] - '0';
                                    if (seq[5] == '~' || seq[5] == '\0') {
                                        /* Handle File menu (0) */
                                        if (menu == 0) {
                                            switch (item) {
                                                case 0: { /* New */
                                                    file_new();
                                                    break;
                                                }
                                                case 1: { /* Open */
                                                    char fname[MAX_FILENAME];
                                                    if (file_open_dialog(fname, sizeof(fname))) {
                                                        file_open(fname);
                                                    }
                                                    break;
                                                }
                                                case 2: { /* Save */
                                                    file_save(filename);
                                                    break;
                                                }
                                                case 3: { /* Save As */
                                                    char fname[MAX_FILENAME];
                                                    if (file_save_dialog(fname, sizeof(fname))) {
                                                        file_save(fname);
                                                    }
                                                    break;
                                                }
                                                case 4: { /* Exit */
                                                    exit(0);
                                                }
                                            }
                                        } else if (menu == 1) {
                                            /* Handle Edit menu (1) */
                                            switch (item) {
                                                case 0: do_undo(); break;           /* Undo */
                                                case 1: do_find(); break;            /* Find */
                                                case 2: find_next(); break;          /* Find Next */
                                                case 3: do_replace(); break;         /* Replace */
                                                case 4: do_goto(); break;            /* Go To */
                                                case 5: cursor_doc_start(); break;   /* Select All -> cursor to start */
                                                case 6: insert_datetime(); break;    /* Time/Date */
                                            }
                                        }
                                        redraw();
                                        continue;
                                    }
                                }
                            }
                        } else {
                            /* Arrow key sequences: ESC [ A/B/C/D */
                            if (seq[1] == 'A') { cursor_up();   redraw(); continue; }
                            if (seq[1] == 'B') { cursor_down(); redraw(); continue; }
                            if (seq[1] == 'C') { cursor_right(); redraw(); continue; }
                            if (seq[1] == 'D') { cursor_left();  redraw(); continue; }
                        }
                    }
                } else {
                    /* ESC alone: toggle command mode */
                    if (in_command) {
                        in_command = 0;
                        cmd_len = 0;
                        cmd_buf[0] = '\0';
                    } else {
                        in_command = 1;
                        cmd_len = 0;
                        cmd_buf[0] = '\0';
                    }
                    redraw();
                    continue;
                }
            }

            /* ---- Command mode: single-letter commands ---- */
            if (in_command) {
                if (c == '\n') {
                    /* Execute command */
                    if (cmd_len > 0) {
                        char cmd = cmd_buf[0];
                        in_command = 0;
                        cmd_len = 0;
                        cmd_buf[0] = '\0';

                        switch (cmd) {
                            case 'n': case 'N': /* New */
                                file_new();
                                break;
                            case 'o': case 'O': { /* Open */
                                char fname[MAX_FILENAME];
                                if (file_open_dialog(fname, sizeof(fname))) {
                                    file_open(fname);
                                }
                                break;
                            }
                            case 's': /* Save */
                                file_save(filename);
                                break;
                            case 'S': { /* Save As */
                                char fname[MAX_FILENAME];
                                if (file_save_dialog(fname, sizeof(fname))) {
                                    file_save(fname);
                                }
                                break;
                            }
                            case 'q': case 'Q': /* Quit */
                                exit(0);
                            case 'u': case 'U': /* Undo */
                                do_undo();
                                break;
                            case 'f': case 'F': /* Find */
                                do_find();
                                break;
                            case 'r': case 'R': /* Replace */
                                do_replace();
                                break;
                            case 'g': case 'G': /* Go To */
                                do_goto();
                                break;
                            case 't': case 'T': /* Time/Date */
                                insert_datetime();
                                break;
                            case 'a': case 'A': /* Select All -> cursor to start */
                                cursor_doc_start();
                                break;
                            case 'w': case 'W': /* Toggle word wrap */
                                word_wrap = !word_wrap;
                                break;
                            case 'h': case 'H': /* Help */
                                print("\nCommands: n=New o=Open s=Save S=SaveAs\n");
                                print("          q=Quit u=Undo f=Find r=Replace\n");
                                print("          g=GoTo t=Time a=SelectAll w=Wrap\n");
                                break;
                        }
                    } else {
                        in_command = 0;
                    }
                    redraw();
                    continue;
                } else if (c == '\b') {
                    if (cmd_len > 0) {
                        cmd_len--;
                        cmd_buf[cmd_len] = '\0';
                    }
                    redraw();
                    continue;
                } else if (c >= 32 && c <= 126) {
                    if (cmd_len < MAX_CMD - 1) {
                        cmd_buf[cmd_len++] = c;
                        cmd_buf[cmd_len] = '\0';
                    }
                    redraw();
                    continue;
                }
                continue;
            }

            /* ---- Normal editing mode ---- */
            if (c == '\b') {
                delete_backspace();
            } else if (c == '\n') {
                insert_char('\n');
            } else if (c == 127) {
                /* Forward delete (DEL key) */
                delete_forward();
            } else if (c >= 32 && c <= 126) {
                insert_char(c);
            }
            redraw();
        }
    }

#ifdef HOST_TEST
    return 0;
#endif
}