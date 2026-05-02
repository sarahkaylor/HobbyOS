#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"

extern int desktop_main(void);
extern void set_flush_callback(void (*cb)(void));
extern void inject_mock_event(uint16_t type, uint16_t code, uint32_t value);
extern void *map_fb(void);

// Basic key mapping for US keyboard reverse lookup
int char_to_keycode(char c) {
    char keymap[128] = {0,    27,  '1', '2',  '3',  '4',  '5', '6', '7',  '8',
                        '9',  '0', '-', '=',  '\b', '\t', 'q', 'w', 'e',  'r',
                        't',  'y', 'u', 'i',  'o',  'p',  '[', ']', '\n', 0,
                        'a',  's', 'd', 'f',  'g',  'h',  'j', 'k', 'l',  ';',
                        '\'', '`', 0,   '\\', 'z',  'x',  'c', 'v', 'b',  'n',
                        'm',  ',', '.', '/',  0,    '*',  0,   ' ', 0};
    for (int i = 0; i < 128; i++) {
        if (keymap[i] == c) return i;
    }
    return 0;
}

void send_key(char c) {
    int code = char_to_keycode(c);
    if (code) {
        inject_mock_event(EV_KEY, code, 1); // Press
        inject_mock_event(EV_KEY, code, 0); // Release
    }
}

#include "../user_include/graphics/window.h"

extern struct window windows[];

static int flush_count = 0;

void on_flush(void) {
    flush_count++;
    
    // At flush 2, the window is created and initial text is drawn
    if (flush_count == 2) {
        printf("[TEST] Injecting 'h' 'e' 'l' 'l' 'o'...\n");
        send_key('h');
        send_key('e');
        send_key('l');
        send_key('l');
        send_key('o');
    }
    
    if (flush_count >= 2) {
        if (strstr(windows[0].text, "hello")) {
            printf("[TEST] Found 'hello' in window 0! Test completed successfully!\n");
            exit(0);
        }
    }
    
    if (flush_count > 20) {
        printf("[TEST] Timeout waiting for text!\n");
        exit(1);
    }
}

int main() {
    printf("[TEST] Starting editor integration test...\n");
    
    set_flush_callback(on_flush);
    
    // Simulate right click to open menu (mouse starts at center screen)
    inject_mock_event(EV_KEY, 0x111, 1);
    inject_mock_event(EV_KEY, 0x111, 0);
    
    // Simulate left click immediately at the same position, which will hit the first menu item
    inject_mock_event(EV_KEY, 0x110, 1);
    inject_mock_event(EV_KEY, 0x110, 0);
    
    desktop_main();
    
    return 0;
}
