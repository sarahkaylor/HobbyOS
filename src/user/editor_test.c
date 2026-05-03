#include "libc.h"
#include "graphics/graphics.h"
#include "graphics/window.h"

extern int desktop_main(void);

static long syscall(long num, long a0, long a1, long a2, long a3) {
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
}

// Event queue for get_events
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

int read_dir(int index, char *buf) {
    if (index == 0) {
        // We copy manually because strcpy isn't in libc.h yet
        buf[0] = 'E'; buf[1] = 'D'; buf[2] = 'I'; buf[3] = 'T';
        buf[4] = 'O'; buf[5] = 'R'; buf[6] = '.'; buf[7] = 'B';
        buf[8] = 'I'; buf[9] = 'N'; buf[10] = '\0';
        return 0;
    }
    return -1;
}

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

void print_console(const char *s) {
  int len = 0;
  while (s[len])
    len++;
  syscall(1 /* SYS_WRITE_CONSOLE */, (long)s, len, 0, 0);
}

extern struct window windows[];
static int flush_count = 0;

void flush_fb(void) {
    // Actually call the real syscall
    syscall(10 /* SYS_FLUSH_FB */, 0, 0, 0, 0);

    flush_count++;
    
    // DEBUG
    print_console("[TEST] flush_fb called, count=");
    char cbuf[2] = {'0' + (flush_count % 10), '\0'};
    if (flush_count >= 10) { cbuf[0] = '0' + (flush_count/10); print_console(cbuf); cbuf[0] = '0' + (flush_count%10); }
    print_console(cbuf);
    print_console("\n");
    
    if (flush_count == 2) {
        print_console("[TEST] Injecting 'h' 'e' 'l' 'l' 'o'...\n");
        send_key('h');
        send_key('e');
        send_key('l');
        send_key('l');
        send_key('o');
    }
    
    if (flush_count >= 2) {
        int found = 0;
        extern int num_windows;
        for (int w = 0; w < num_windows; w++) {
            char *text = windows[w].text;
            for (int i = 0; text[i] != '\0'; i++) {
                if (text[i] == 'h' && text[i+1] == 'e' && text[i+2] == 'l' && text[i+3] == 'l' && text[i+4] == 'o') {
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
        
        if (found) {
            print_console("[TEST] Found 'hello' in window 0! SCREENSHOT_READY\n");
            // Wait for host to kill QEMU
            while(1);
        }
    }
    
    if (flush_count > 20) {
        print_console("[TEST] Timeout waiting for text!\n");
        exit(1);
    }
}

__attribute__((section(".text._start")))
void _start(void) {
    print_console("[TEST] Starting editor integration test...\n");
    
    // Simulate right click to open menu (mouse starts at center screen)
    inject_mock_event(EV_KEY, 0x111, 1);
    inject_mock_event(EV_KEY, 0x111, 0);
    
    // Simulate left click immediately at the same position, which will hit the first menu item
    inject_mock_event(EV_KEY, 0x110, 1);
    inject_mock_event(EV_KEY, 0x110, 0);
    
    desktop_main();
    exit(0);
}
