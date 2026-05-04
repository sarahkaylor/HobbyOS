#include "graphics/window.h"
extern struct window windows[];
void print_console(const char *s);

void debug_print_text() {
    print_console("[DEBUG] Window 0 text: ");
    print_console(windows[0].text);
    print_console("\n");
}
