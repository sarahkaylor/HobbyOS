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
        buf[0] = 'N'; buf[1] = 'E'; buf[2] = 'T'; buf[3] = 'T';
        buf[4] = 'E'; buf[5] = 'S'; buf[6] = 'T'; buf[7] = '.';
        buf[8] = 'B'; buf[9] = 'I'; buf[10] = 'N'; buf[11] = '\0';
        return 0;
    } else if (index == 1) {
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
    if (code > 0) {
        inject_mock_event(EV_KEY, code, 1); // press
        inject_mock_event(EV_KEY, code, 0); // release
    }
}



extern struct window windows[];
static int flush_count = 0;

extern struct window windows[];
extern int num_windows;
static int test_state = 0;

#define STATE_WAIT_NETTEST_LAUNCH 1
#define STATE_WAIT_NETTEST_EXIT 2
#define STATE_WAIT_EDITOR_LAUNCH 3
#define STATE_CLICK_FILE_MENU 4
#define STATE_WAIT_FINISH 6

void flush_fb(void) {
    // Actually call the real syscall
    syscall(10 /* SYS_FLUSH_FB */, 0, 0, 0, 0);

    print_console("[TEST] flush_fb: test_state=");
    char buf[16];
    int st = test_state;
    if (st == 0) print_console("0");
    else if (st == 1) print_console("1");
    else if (st == 2) print_console("2");
    else if (st == 3) print_console("3");
    else print_console("other");
    print_console(" num_windows=");
    if (num_windows == 0) print_console("0\n");
    else if (num_windows == 1) print_console("1\n");
    else print_console("other\n");

    if (test_state == 0) {
        // Right click to open menu
        inject_mock_event(EV_KEY, 0x111, 1);
        inject_mock_event(EV_KEY, 0x111, 0);
        
        // Move mouse down to hit NETTEST.BIN (index 0 -> y = 384 + 0 * 20 + 10 = 394)
        inject_mock_event(EV_ABS, ABS_Y, (394 * 0x7FFF) / 768);
        
        // Left click to select NETTEST.BIN
        inject_mock_event(EV_KEY, 0x110, 1);
        inject_mock_event(EV_KEY, 0x110, 0);

        test_state = STATE_WAIT_NETTEST_LAUNCH;
    }
    if (test_state == STATE_WAIT_NETTEST_LAUNCH) {
        if (num_windows == 1) {
            print_console("[TEST] nettest launched.\n");
            test_state = STATE_WAIT_NETTEST_EXIT;
        }
    }
    if (test_state == STATE_WAIT_NETTEST_EXIT) {
        if (num_windows == 0) {
            print_console("[TEST] nettest exited.\n");
            // Right click to open menu
            inject_mock_event(EV_KEY, 0x111, 1);
            inject_mock_event(EV_KEY, 0x111, 0);
            
            // Move mouse down to hit EDITOR.BIN (index 1 -> y = 384 + 1 * 20 + 10 = 414)
            inject_mock_event(EV_ABS, ABS_Y, (414 * 0x7FFF) / 768);
            
            // Left click to select EDITOR.BIN
            inject_mock_event(EV_KEY, 0x110, 1);
            inject_mock_event(EV_KEY, 0x110, 0);
            
            test_state = STATE_WAIT_EDITOR_LAUNCH;
        }
    }
    if (test_state == STATE_WAIT_EDITOR_LAUNCH) {
        if (num_windows == 1) {
            print_console("[TEST] editor launched.\n");
            print_console("[TEST] Injecting 'h' 'e' 'l' 'l' 'o'...\n");
            send_key('h');
            send_key('e');
            send_key('l');
            send_key('l');
            send_key('o');
            test_state = STATE_CLICK_FILE_MENU;
        }
    }
    if (test_state == STATE_CLICK_FILE_MENU) {
        int found = 0;
        char *text = windows[0].text;
        for (int i = 0; text[i] != '\0'; i++) {
            if (text[i] == 'h' && text[i+1] == 'e' && text[i+2] == 'l' && text[i+3] == 'l' && text[i+4] == 'o') {
                found = 1;
                break;
            }
        }
        if (found) {
            print_console("[TEST] Moving mouse to File menu...\n");
            inject_mock_event(EV_ABS, ABS_X, (20 * 0x7FFF) / 1024);
            inject_mock_event(EV_ABS, ABS_Y, (26 * 0x7FFF) / 768);
            test_state = 100;
        }
    }
    if (test_state == 100) {
        inject_mock_event(EV_KEY, 0x110, 1);
        inject_mock_event(EV_KEY, 0x110, 0);
        test_state = 101;
    }
    if (test_state == 101) {
        print_console("[TEST] Moving mouse to Save item...\n");
        inject_mock_event(EV_ABS, ABS_X, (20 * 0x7FFF) / 1024);
        inject_mock_event(EV_ABS, ABS_Y, (64 * 0x7FFF) / 768);
        test_state = 102;
    }
    if (test_state == 102) {
        print_console("[TEST] Clicking Save item...\n");
        inject_mock_event(EV_KEY, 0x110, 1);
        inject_mock_event(EV_KEY, 0x110, 0);
        test_state = STATE_WAIT_FINISH;
    }
    if (test_state == STATE_WAIT_FINISH) {
        int found = 0;
        char *text = windows[0].text;
        for (int i = 0; text[i] != '\0'; i++) {
            if (text[i] == 'w' && text[i+1] == ' ') {
                found = 1;
                break;
            }
        }
        if (found) {
            print_console("[TEST] Found 'w ' in window 0! SCREENSHOT_READY\n");
            while(1);
        }
    }
}

__attribute__((section(".text._start")))
void _start(void) {
    print_console("[TEST] Starting editor integration test...\n");
    desktop_main();
    exit(0);
}
