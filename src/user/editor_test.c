#include "libc.h"
#include "graphics/graphics.h"
#include "graphics/window.h"

extern int desktop_main(void);

static long ho_syscall4(long num, long a0, long a1, long a2, long a3) {
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

/* Fake root directory: the two launch targets the state machine clicks, in the
 * order it expects (index 0 = NETTEST, 1 = EDITOR -- the menu renders read_dir
 * order).  Both are real names on the boot disk, so the menu's launch actually
 * resolves; the editor's own Open dialog reads the REAL disk, because the menu
 * spawns EDITOR.BIN (the production binary) rather than this harness.
 * `path` is honoured rather than ignored: the GB-039 launcher also scans
 * "/GAMES", which this fake disk does not have (and the real FAT-16 driver
 * answers -1 for a path that does not resolve).  Ignoring the path made that
 * scan "find" NETTEST.BIN/EDITOR.BIN, mark them /GAMES-resident and launch
 * "/GAMES/NETTEST.BIN" -- a name no disk carries.  No window ever opened,
 * num_windows stayed 0, and the run died on "Timeout waiting for
 * SCREENSHOT_READY" (CI desktop tier, 2026-10-26). */
static const char *const fake_root[] = {
  "NETTEST.BIN", "EDITOR.BIN",
};
#define FAKE_ROOT_COUNT ((int)(sizeof(fake_root) / sizeof(fake_root[0])))

int read_dir(const char *path, int index, struct sys_dirent *ent) {
  if (!path || path[0] != '/' || path[1] != '\0') return -1;
  if (index < 0 || index >= FAKE_ROOT_COUNT) return -1;
  const char *src = fake_root[index];
  int i = 0;
  while (src[i] && i < (int)sizeof(ent->name) - 1) { ent->name[i] = src[i]; i++; }
  ent->name[i] = '\0';
  ent->attr = 0x20;   /* ATTR_ARCHIVE — a regular file, as mcopy writes it */
  ent->size = 0;
  return 0;
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

/* Substring search over the window text.  The state machine used to spell each
 * assertion out as a chain of byte compares; a helper keeps them readable and
 * makes "look for this text" vs "look for that text" a one-line change. */
static int text_has(const char *text, const char *needle) {
  for (int i = 0; text[i]; i++) {
    int k = 0;
    while (needle[k] && text[i + k] == needle[k]) k++;
    if (!needle[k]) return 1;
  }
  return 0;
}

/* One presented frame ticks the test state machine.  The desktop presents
 * whole-scene frames with SYS_FLUSH_FB (10) and damage frames with the GX
 * rect flush (SYS_FLUSH_FB_RECTS 89), so both wrappers land here -- a
 * rect-present frame must keep the state machine moving. */
static void editor_test_tick(void) {
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

    // Move mouse down to hit NETTEST.BIN (index 0 -> y = menu_y + 10, and the
    // menu opens at the pointer = screen centre: SCREEN_HEIGHT/2 + 10)
    inject_mock_event(EV_ABS, ABS_Y, ((SCREEN_HEIGHT / 2 + 10) * 0x7FFF) / SCREEN_HEIGHT);

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

      // Move mouse down to hit EDITOR.BIN (index 1 -> y = menu_y + 30)
      inject_mock_event(EV_ABS, ABS_Y, ((SCREEN_HEIGHT / 2 + 30) * 0x7FFF) / SCREEN_HEIGHT);

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
      inject_mock_event(EV_ABS, ABS_X, (20 * 0x7FFF) / SCREEN_WIDTH);
      inject_mock_event(EV_ABS, ABS_Y, (26 * 0x7FFF) / SCREEN_HEIGHT);
      test_state = 100;
    }
  }
  if (test_state == 100) {
    inject_mock_event(EV_KEY, 0x110, 1);
    inject_mock_event(EV_KEY, 0x110, 0);
    test_state = 101;
  }
  if (test_state == 101) {
    print_console("[TEST] Moving mouse to Open item...\n");
    inject_mock_event(EV_ABS, ABS_X, (20 * 0x7FFF) / SCREEN_WIDTH);
    inject_mock_event(EV_ABS, ABS_Y, (64 * 0x7FFF) / SCREEN_HEIGHT);
    test_state = 102;
  }
  if (test_state == 102) {
    print_console("[TEST] Clicking Open item...\n");
    inject_mock_event(EV_KEY, 0x110, 1);
    inject_mock_event(EV_KEY, 0x110, 0);
    test_state = STATE_WAIT_FINISH;
  }
  if (test_state == STATE_WAIT_FINISH) {
    /* Clicking File->Open must make the editor render its file open
     * dialog, listing real entries read from the FAT-16 disk.
     *
     * What proves it: the title, the listing header -- "Files in root:" is
     * printed only on the branch that got entries (the empty branch prints
     * "No files found. Type name:") -- and EFI, the directory the disk recipe
     * creates first, so read_dir lists it first.
     *
     * This used to look for "MEMTES" (MEMTEST.BIN).  That name rotted: the
     * boot disk grew eleven directories at the head of its root, the dialog
     * renders ten rows, and MEMTEST.BIN fell past the visible page -- so the
     * assertion could never succeed again and the run sat here until the
     * harness timed out (CI desktop tier, 2026-10-26).  Assert on the listing,
     * not on a position inside it. */
    char *text = windows[0].text;
    int found_title = text_has(text, "Open File");
    int found_listing = text_has(text, "Files in root:");
    int found_entry = text_has(text, "EFI");
    if (found_title && found_listing && found_entry) {
      print_console("[TEST] Open File dialog rendered with disk entries! SCREENSHOT_READY\n");
      while(1);
    }
  }
}

void flush_fb(void) {
  // Actually call the real syscall
  ho_syscall4(10 /* SYS_FLUSH_FB */, 0, 0, 0, 0);
  editor_test_tick();
}

int flush_fb_rects(const struct fb_rect *rects, int count) {
  ho_syscall4(89 /* SYS_FLUSH_FB_RECTS */, (long)rects, (long)count, 0, 0);
  editor_test_tick();
  return 0;
}

__attribute__((section(".text._start")))
void _start(void) {
  print_console("[TEST] Starting editor integration test...\n");
  desktop_main();
  exit(0);
}
