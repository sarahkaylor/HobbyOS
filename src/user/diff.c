/*
 * diff.c - Diff for HobbyOS.
 *
 * File comparer: line-by-line diff of two text files.
 *
 * !! THIS IS A STUB TEMPLATE. Replace the body with the real application.
 * Keep the entry-point pattern below (HOST_TEST/else _start) intact: the
 * host unit test includes this file with `#define main diff_app_main`.
 *
 * Usage contract (see gui.h):
 *   - Draw with print() after gui_clear() ("\f" clears the window)
 *   - Read input with gui_read_event() / gui_read_event_timeout()
 *   - Register menus with gui_add_menu() (libc.h)
 *   - Pressing 'q' is NOT a general convention; use your menu/Exit items.
 */

#include "libc.h"
#include "gui.h"

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    gui_set_title("Diff");
    gui_clear();
    print("= DIFF =\n");
    print("[STUB] implement me\n");

    for (;;) {
        struct gui_event ev;
        if (gui_read_event(&ev)) {
            if (ev.type == GUI_EV_CHAR && ev.ch == 'q') exit(0);
        }
    }
}
