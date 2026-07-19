/*
 * File Dialog Arrow Key Test - runs INSIDE HobbyOS as a desktop test.
 *
 * This test reproduces the bug where Up/Down arrow keys do not work
 * in the editor's File > Open dialog. It runs the real desktop, launches
 * the real editor, opens the file open dialog, sends arrow key events,
 * and checks the dialog's rendered output (the > selection marker).
 *
 * The test is as close to the real OS environment as possible: it uses
 * the real pipe I/O, the real scheduler, and the real dialog code.
 *
 * Build: linked as EDITOR_T.BIN (replaces editor_test.c for this test mode)
 * Run:   make desktop_test_run  (boots OS in QEMU, runs this as desktop)
 *
 * Test sequence:
 *   1. Right-click to open the launch menu, left-click EDITOR.BIN
 *   2. Wait for editor window to appear
 *   3. Move mouse to the "File" menu in the editor's title bar, click it
 *   4. Move mouse to "Open" item in the dropdown, click it
 *   5. Wait for the file open dialog to render (look for "Open File")
 *   6. Send DOWN arrow key events
 *   7. Check that the > selection marker moved down in the file list
 *   8. Send UP arrow key events
 *   9. Check that the > selection marker moved back up
 *  10. Print TEST_RESULT: PASS or FAIL
 */

#include "libc.h"
#include "graphics/graphics.h"
#include "graphics/window.h"

extern int desktop_main(void);

static long syscall(long num, long a0, long a1, long a2, long a3) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
#endif
}

/* ---- Mock event queue (same as editor_test.c) ---- */

#define MAX_MOCK_EVENTS 256
static struct virtio_input_event mock_events[MAX_MOCK_EVENTS];
static int mock_events_head = 0;
static int mock_events_tail = 0;

void inject_mock_event(uint16_t type, uint16_t code, uint32_t value) {
    int next = (mock_events_head + 1) % MAX_MOCK_EVENTS;
    if (next != mock_events_tail) {
        mock_events[mock_events_head].type = type;
        mock_events[mock_events_head].code = code;
        mock_events[mock_events_head].value = value;
        mock_events_head = next;
    }
}

int get_events(void *buf, int max_events) {
    struct virtio_input_event *events = (struct virtio_input_event *)buf;
    int count = 0;
    while (mock_events_tail != mock_events_head && count < max_events) {
        events[count++] = mock_events[mock_events_tail];
        mock_events_tail = (mock_events_tail + 1) % MAX_MOCK_EVENTS;
    }
    return count;
}

/* Mock read_dir: return different files depending on the phase.
 * Phase 0 (desktop menu loading): return EDITOR.BIN so the menu works.
 * Phase 1 (file dialog): return ALPHA.TXT, BETA.TXT, GAMMA.TXT for arrow key testing.
 */
int dialog_read_dir_phase = 0;  /* 0 = desktop menu, 1 = file dialog */

int read_dir(const char *path, int index, struct sys_dirent *ent) {
    (void)path;
    if (dialog_read_dir_phase == 0) {
        /* Desktop menu: return EDITOR.BIN */
        if (index == 0) {
            static const char name[] = "EDITOR.BIN";
            int k = 0;
            while (name[k] && k < 31) { ent->name[k] = name[k]; k++; }
            ent->name[k] = '\0';
            ent->attr = 0;
            ent->size = 0;
            return 0;
        }
        return -1;
    } else {
        /* File dialog: return test files */
        static const char *files[] = {"ALPHA.TXT", "BETA.TXT", "GAMMA.TXT"};
        if (index < 0 || index >= 3) return -1;
        int k = 0;
        while (files[index][k] && k < 31) {
            ent->name[k] = files[index][k];
            k++;
        }
        ent->name[k] = '\0';
        ent->attr = 0;
        ent->size = 0;
        return 0;
    }
}

/* ---- Test state machine ---- */

extern struct window windows[];
extern int num_windows;

static int test_state = 0;
static int flush_count = 0;
static int arrow_down_count = 0;
static int arrow_up_count = 0;
static int test_result = -1;  /* -1 = not done, 0 = fail, 1 = pass */
static int initial_row = -1;  /* marker row when the dialog first opened (sel 0) */
static int down_row = -1;     /* marker row after DOWN keys */
static int settle = 0;        /* flushes waited since the last injected key */

#define STATE_INIT              0
#define STATE_WAIT_EDITOR       1
#define STATE_CLICK_FILE_MENU   2
#define STATE_WAIT_DROPDOWN     3
#define STATE_CLICK_OPEN        4
#define STATE_WAIT_DIALOG       5
#define STATE_SEND_DOWN         6
#define STATE_CHECK_DOWN        7
#define STATE_SEND_UP           8
#define STATE_CHECK_UP          9
#define STATE_DONE              99

/* Helper: check if window text contains a substring */
static int text_contains(const char *text, const char *sub) {
    if (!text || !sub) return 0;
    int i, j;
    for (i = 0; text[i]; i++) {
        for (j = 0; sub[j]; j++) {
            if (text[i+j] != sub[j]) break;
        }
        if (!sub[j]) return 1;
    }
    return 0;
}

/* Helper: return the 0-based row (line number) that the " > " selection
 * marker sits on within the dialog text, or -1 if no marker is present yet.
 *
 * This is deliberately filename-independent. The file dialog runs inside the
 * real EDITOR.BIN process, which lists the REAL root directory via the read_dir
 * syscall (our mock read_dir only affects THIS desktop process). So the test
 * cannot know which filenames appear — but it can watch WHERE the selection is.
 * Arrow navigation is proven by the marker moving to a different row: DOWN must
 * increase the row, UP must decrease it. */
static int selected_row(const char *text) {
    if (!text) return -1;
    int row = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') { row++; continue; }
        /* " > " marks the selected file (a leading space, '>', trailing space). */
        if (text[i] == '>' && i > 0 && text[i-1] == ' ' && text[i+1] == ' ') {
            return row;
        }
    }
    return -1;
}

/* Debug helper: print "label=<int>\n" over the serial console. */
static void dbg_int(const char *label, int v) {
    char buf[48];
    int p = 0;
    while (*label && p < 32) buf[p++] = *label++;
    buf[p++] = '=';
    if (v < 0) { buf[p++] = '-'; v = -v; }
    char tmp[12];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 11) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0) buf[p++] = tmp[--t];
    buf[p++] = '\n';
    buf[p] = 0;
    print_console(buf);
}

void flush_fb(void) {
    syscall(10 /* SYS_FLUSH_FB */, 0, 0, 0, 0);

    flush_count++;

    /* State machine driven by flush callbacks */
    if (test_state == STATE_INIT) {
        /* Right-click to open the launch menu.
         * The mouse starts at (512, 384). Right-click opens the menu at (512, 384).
         * With our read_dir, the menu has EDITOR.BIN at index 0.
         * Item 0 is at y = 384 to y = 404. Click at y = 394 to select it.
         */
        inject_mock_event(EV_KEY, 0x111, 1);  /* BTN_RIGHT press */
        inject_mock_event(EV_KEY, 0x111, 0);  /* BTN_RIGHT release */
        /* Move mouse to EDITOR.BIN (index 0 -> y = 384 + 0*20 + 10 = 394) */
        inject_mock_event(EV_ABS, ABS_Y, (394 * 0x7FFF) / 768);
        /* Left-click to launch EDITOR.BIN */
        inject_mock_event(EV_KEY, 0x110, 1);  /* BTN_LEFT press */
        inject_mock_event(EV_KEY, 0x110, 0);  /* BTN_LEFT release */
        test_state = STATE_WAIT_EDITOR;
    }

    if (test_state == STATE_WAIT_EDITOR) {
        /* Wait until the editor window exists AND has registered its menus.
         * The editor sends its File/Edit menu definitions (OSC sequences)
         * shortly AFTER its window is created; clicking the File menu before
         * they arrive is a race that leaves win->num_menus == 0, so the
         * dropdown never opens and the file dialog is never triggered. */
        if (flush_count % 50 == 0) {
            print_console("[TEST] WAIT_EDITOR: num_windows=");
            char buf[16];
            int p = 0;
            int v = num_windows;
            if (v == 0) buf[p++] = '0';
            while (v > 0) { buf[p++] = '0' + v % 10; v /= 10; }
            buf[p] = 0;
            print_console(buf);
            print_console(" menus=");
            v = (num_windows >= 1) ? windows[0].num_menus : -1;
            p = 0;
            if (v < 0) { buf[p++] = '-'; v = -v; }
            if (v == 0) buf[p++] = '0';
            while (v > 0) { buf[p++] = '0' + v % 10; v /= 10; }
            buf[p] = 0;
            print_console(buf);
            print_console("\n");
        }
        if (num_windows >= 1 && windows[0].num_menus >= 2) {
            /* Editor launched. Now click on the "File" menu in its title bar.
             * The window is at (0, 0) with size (1024, 768).
             * The title bar menus are at y = 18 to y = 34.
             * "File" is the first menu, starting at x = 10, width = 48.
             * Click at x = 20, y = 26 (within the "File" menu area).
             */
            inject_mock_event(EV_ABS, ABS_X, (20 * 0x7FFF) / 1024);
            inject_mock_event(EV_ABS, ABS_Y, (26 * 0x7FFF) / 768);
            test_state = STATE_CLICK_FILE_MENU;
        }
    }

    if (test_state == STATE_CLICK_FILE_MENU) {
        /* Click to open the File dropdown menu */
        inject_mock_event(EV_KEY, 0x110, 1);  /* BTN_LEFT press */
        inject_mock_event(EV_KEY, 0x110, 0);  /* BTN_LEFT release */
        test_state = STATE_WAIT_DROPDOWN;
    }

    if (test_state == STATE_WAIT_DROPDOWN) {
        /* The dropdown should now be open. Move mouse to "Open" (item 1).
         * The dropdown appears at (app_menu_x, app_menu_y) = (10, 34).
         * Each item is 20px tall. Item 1 (Open) is at y = 54 to 74.
         * Click at x = 20, y = 64 (within the "Open" item area).
         */
        inject_mock_event(EV_ABS, ABS_X, (20 * 0x7FFF) / 1024);
        inject_mock_event(EV_ABS, ABS_Y, (64 * 0x7FFF) / 768);
        test_state = STATE_CLICK_OPEN;
    }

    if (test_state == STATE_CLICK_OPEN) {
        /* Click on the "Open" menu item. The editor receives the menu
         * selection (ESC [ M 0 ; 1 ~) and calls file_open_dialog(), which
         * lists the REAL root directory via the read_dir syscall (our mock
         * read_dir only affects this desktop process, not EDITOR.BIN). */
        inject_mock_event(EV_KEY, 0x110, 1);  /* BTN_LEFT press */
        inject_mock_event(EV_KEY, 0x110, 0);  /* BTN_LEFT release */
        test_state = STATE_WAIT_DIALOG;
    }

    if (test_state == STATE_WAIT_DIALOG) {
        /* Wait for the file open dialog to appear AND finish rendering its
         * file list. The dialog title is "Open File"; the selection marker
         * (" > ") only appears once the list has been drawn. Record the marker
         * row (selection starts on file 0) as our baseline, then inject exactly
         * ONE DOWN.
         *
         * We pace strictly: one key, then wait for the dialog to re-render and
         * the selection to move, before sending the next. The editor processes
         * roughly one key per full-screen redraw (which the desktop drains in
         * 63-byte chunks); injecting faster than it drains backs the editor's
         * stdin pipe up and later keys (e.g. UP) get stuck behind unprocessed
         * ones. Real users type at human pace, so this models real usage. */
        if (num_windows >= 1 && text_contains(windows[0].text, "Open File")) {
            int row = selected_row(windows[0].text);
            if (row >= 0) {
                initial_row = row;
                dbg_int("[TEST] dialog open, initial_row", initial_row);
                inject_mock_event(EV_KEY, 108, 1);  /* DOWN press */
                inject_mock_event(EV_KEY, 108, 0);  /* DOWN release */
                settle = 0;
                test_state = STATE_CHECK_DOWN;
            }
        }
        /* Timeout check */
        if (flush_count > 400) {
            print_console("[TEST] TIMEOUT waiting for dialog!\n");
            test_result = 0;
            test_state = STATE_DONE;
        }
    }

    if (test_state == STATE_CHECK_DOWN) {
        /* DOWN works if the selection marker moved to a LATER row than where it
         * started. Filename-independent: we only care that it moved down. */
        int row = selected_row(windows[0].text);
        if (row > initial_row) {
            down_row = row;
            dbg_int("[TEST] DOWN moved, down_row", down_row);
            print_console("[TEST] DOWN arrow: selection marker moved down. PASS\n");
            /* Now send exactly ONE UP and wait for it to move back up. */
            inject_mock_event(EV_KEY, 103, 1);  /* UP press */
            inject_mock_event(EV_KEY, 103, 0);  /* UP release */
            settle = 0;
            test_state = STATE_CHECK_UP;
        } else if (++settle > 25) {
            /* With the heartbeat below keeping flush_fb() ticking, a working
             * DOWN reflects in the render within a few flushes. Give it 25 to
             * absorb scheduling jitter, then re-nudge ONCE (one key, not a
             * flood), up to a couple of tries, before declaring failure. */
            if (arrow_down_count < 2) {
                inject_mock_event(EV_KEY, 108, 1);
                inject_mock_event(EV_KEY, 108, 0);
                arrow_down_count++;
                settle = 0;
            } else {
                print_console("[TEST] DOWN arrow: selection did NOT move\n");
                test_result = 0;
                test_state = STATE_DONE;
            }
        }
    }

    if (test_state == STATE_CHECK_UP) {
        /* UP works if the selection marker moved back to an EARLIER row than
         * where DOWN left it. (row >= 0 guards against a transient partial
         * render where the marker isn't in the buffer yet.) */
        int row = selected_row(windows[0].text);
        if (row >= 0 && row < down_row) {
            print_console("[TEST] UP arrow: selection marker moved up. PASS\n");
            test_result = 1;
            test_state = STATE_DONE;
        } else if (++settle > 25) {
            if (arrow_up_count < 2) {
                inject_mock_event(EV_KEY, 103, 1);
                inject_mock_event(EV_KEY, 103, 0);
                arrow_up_count++;
                settle = 0;
            } else {
                print_console("[TEST] UP arrow: selection did NOT move up\n");
                test_result = 0;
                test_state = STATE_DONE;
            }
        }
    }

    /* ---- Heartbeat ----
     * CRITICAL for reproducing the bug cleanly. When an arrow key is dropped
     * (the desktop fails to forward it), the editor never reads a key, never
     * redraws, and writes nothing to its stdout. The desktop then sees no
     * events and no redraw, takes its idle yield() branch, and STOPS calling
     * graphics_flush()/flush_fb() -- which freezes this state machine. The
     * result is a silent 60s harness timeout instead of a clean FAIL.
     *
     * To keep the desktop's event loop alive while we wait for the dialog to
     * react, inject a tiny mouse jiggle each tick. A mouse MOVE (never a click)
     * sets needs_redraw and guarantees flush_fb() is called again next loop, so
     * the settle counters advance and the FAIL verdict is reached in well under
     * a second. It is harmless: it changes nothing in the dialog, so it can
     * neither cause nor mask a real arrow-key movement. */
    if (test_state == STATE_CHECK_DOWN || test_state == STATE_CHECK_UP) {
        static int hb = 0;
        hb ^= 1;
        inject_mock_event(EV_ABS, ABS_X, ((500 + hb) * 0x7FFF) / 1024);
    }

    if (test_state == STATE_DONE) {
        if (test_result == 1) {
            print_console("SCREENSHOT_READY\n");
            print_console("FILEDIALOG_ARROW_TEST: PASS\n");
        } else {
            print_console("SCREENSHOT_READY\n");
            print_console("FILEDIALOG_ARROW_TEST: FAIL\n");
        }
        /* Spin to keep the result visible */
        while (1) {}
    }
}

__attribute__((section(".text._start")))
void _start(void) {
    print_console("[TEST] Starting file dialog arrow key test...\n");
    desktop_main();
    exit(0);
}