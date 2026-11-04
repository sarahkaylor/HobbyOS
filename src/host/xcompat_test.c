/*
 * xcompat_test.c - host tests for the X11 compat DSO's testable half.
 *
 * Tier 1 of docs/rimworld-plan.md §8.1: "xcompat symbol bookkeeping (pure
 * logic)".  Three groups:
 *
 *   1. ABI differential.  src/host/xcompat_abi_mine.c and
 *      xcompat_abi_ref.c are the SAME table of sizeof()/offsetof() facts,
 *      compiled once against src/user/xcompat/xabi.h and once against this
 *      host's real <X11/Xlib.h>.  Every row must be identical, because
 *      SDL reads the events this DSO produces at the real X11 offsets.  A
 *      mismatch prints the field and fails.
 *
 *   2. xcore: the atom table, the event ring, the geometry/clip maths and
 *      the keymap -- the parts of the DSO that involve no I/O.
 *
 *   3. The export contract: XCOMPAT_REQUIRED_SYMS (generated into
 *      required-libx11.h from the measured SDL import list) must all be
 *      present in the built DSO.  The makefile does that check with nm;
 *      here we only assert the list is non-empty and sorted, so a broken
 *      generation cannot silently pass.
 *
 * Build: `make host_tests` (rule in src/user/xcompat/Makefile.inc).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "xcore.h"
#include "pulse_abi.h"

#ifdef XCOMPAT_HAVE_REQUIRED
#include "required-libx11.h"
#include "required-alsa.h"
#include "required-pulse.h"
#endif
#ifdef XCOMPAT_DSO_PATH
#include <dlfcn.h>
#endif

#ifndef XCOMPAT_HAVE_X11_REF
#define XCOMPAT_HAVE_X11_REF 0
#endif

#include "xcompat_abi_row.h"

extern const struct abi_row *abi_rows_mine(int *count);
#if XCOMPAT_HAVE_X11_REF
extern const struct abi_row *abi_rows_ref(int *count);
#endif

#ifndef XCOMPAT_HAVE_PULSE_REF
#define XCOMPAT_HAVE_PULSE_REF 0
#endif
#if XCOMPAT_HAVE_PULSE_REF
#include "pulse_abi_rows.h"
extern const unsigned long xcompat_pulse_ref_values[XCOMPAT_PULSE_REF_ROWS];
#endif

static int checks, fails;

static void check(int cond, const char *what) {
  checks++;
  if (!cond) {
    fails++;
    printf("FAIL %s\n", what);
  }
}

/* ---- 1. ABI differential --------------------------------------------- */

static void test_abi(void) {
  int mn = 0;
  const struct abi_row *mine = abi_rows_mine(&mn);
#if !XCOMPAT_HAVE_X11_REF
  printf("SKIP ABI differential (host has no <X11/Xlib.h>)\n");
  (void)mine;
#else
  int rn = 0;
  const struct abi_row *ref = abi_rows_ref(&rn);
  int mismatches = 0;
  check(mn == rn, "abi row lists have the same length");
  int rows = mn < rn ? mn : rn;
  for (int i = 0; i < rows; i++) {
    if (strcmp(mine[i].name, ref[i].name) != 0) {
      printf("FAIL abi row %d name: mine=%s ref=%s\n", i, mine[i].name,
             ref[i].name);
      mismatches++;
      continue;
    }
    if (mine[i].value != ref[i].value) {
      printf("FAIL abi %s: mine=%lu ref=%lu\n", mine[i].name,
             mine[i].value, ref[i].value);
      mismatches++;
    }
  }
  /* The arrays are static, so this also needs a real floor: 149 rows is
   * every struct size, field offset, atom, mask and keysym above. */
  check(rows >= 140, "abi differential compared every row");
  check(mismatches == 0, "xabi.h matches the host <X11/Xlib.h> layout");
  printf("     abi: %d rows compared, %d mismatches\n", rows, mismatches);
#endif
}

/* ---- 2a. atom table --------------------------------------------------- */

static void test_atoms(void) {
  struct xc_core c;
  xc_init(&c);

  /* Predefined atoms: the numbering is a wire contract. */
  check(xc_intern(&c, "PRIMARY", 1) == XA_PRIMARY, "PRIMARY is atom 1");
  check(xc_intern(&c, "STRING", 1) == XA_STRING, "STRING is atom 31");
  check(xc_intern(&c, "WM_NAME", 1) == XA_WM_NAME, "WM_NAME is atom 39");
  check(xc_intern(&c, "WM_TRANSIENT_FOR", 1) == XA_WM_TRANSIENT_FOR,
        "WM_TRANSIENT_FOR is atom 68");
  check(xc_intern(&c, "NO_SUCH_ATOM_NAME_HERE", 1) == None,
        "only_if_exists misses");
  check(xc_atom_name(&c, XA_WM_NAME) != 0, "predefined atom has a name");
  check(strcmp(xc_atom_name(&c, XA_WM_NAME), "WM_NAME") == 0,
        "predefined atom name round-trips");

  /* Interning a new name allocates above the predefined range and is
   * stable on the second call (that stability is what SDL relies on when
   * it compares atoms it interned at different times). */
  Atom wm_protocols = xc_intern(&c, "WM_PROTOCOLS", 0);
  check(wm_protocols > XA_LAST_PREDEFINED, "new atom is above the predefined range");
  check(xc_intern(&c, "WM_PROTOCOLS", 0) == wm_protocols, "interning is idempotent");
  check(xc_intern(&c, "WM_PROTOCOLS", 1) == wm_protocols,
        "only_if_exists finds an interned atom");
  check(strcmp(xc_atom_name(&c, wm_protocols), "WM_PROTOCOLS") == 0,
        "interned name round-trips");

  /* None is never a valid atom name. */
  check(xc_atom_name(&c, None) == 0, "None has no name");
  check(xc_atom_name(&c, 99999) == 0, "unknown atom has no name");

  /* Long names are truncated to the table's width but still distinct up
   * to that width; a full table refuses new names instead of corrupting. */
  char longname[200];
  for (int i = 0; i < 199; i++) longname[i] = (char)('A' + (i % 26));
  longname[199] = 0;
  Atom a1 = xc_intern(&c, longname, 0);
  check(a1 != None, "a long name still interns");
  check(a1 > XA_LAST_PREDEFINED, "long name got a fresh id");

  struct xc_core full;
  xc_init(&full);
  int added = 0;
  char name[32];
  for (int i = 0; i < XC_MAX_ATOMS + 20; i++) {
    name[0] = 'A';
    name[1] = (char)('a' + (i % 26));
    name[2] = (char)('0' + (i / 26) % 10);
    name[3] = (char)('0' + (i / 260) % 10);
    name[4] = 0;
    if (xc_intern(&full, name, 0) != None) added++;
  }
  check(added == XC_MAX_ATOMS - (int)XA_LAST_PREDEFINED,
        "the atom table fills exactly to XC_MAX_ATOMS");
  check(xc_atom_count(&full) == XC_MAX_ATOMS, "atom high-water mark is the cap");
}

/* ---- 2b. event ring --------------------------------------------------- */

static void test_ring(void) {
  struct xc_core c;
  xc_init(&c);
  check(xc_pending(&c) == 0, "a fresh ring is empty");

  XEvent ev;
  memset(&ev, 0, sizeof ev);
  ev.type = Expose;
  ev.xexpose.window = 7;
  check(xc_queue(&c, &ev) == 1, "queueing an event succeeds");
  check(xc_pending(&c) == 1, "one event pending");
  check(xc_serial(&c) == 1, "serial advanced");

  XEvent out;
  check(xc_peek(&c, &out) == 1 && out.type == Expose, "peek returns the head");
  check(xc_pending(&c) == 1, "peek does not consume");
  check(xc_dequeue(&c, &out) == 1 && out.type == Expose, "dequeue returns the head");
  check(xc_pending(&c) == 0, "dequeue consumed it");
  check(xc_dequeue(&c, &out) == 0, "dequeueing an empty ring reports empty");

  /* FIFO order and the serial stamped by the ring (not by the caller). */
  for (int i = 0; i < 8; i++) {
    memset(&ev, 0, sizeof ev);
    ev.type = ButtonPress;
    ev.xbutton.button = (unsigned)i;
    xc_queue(&c, &ev);
  }
  check(xc_pending(&c) == 8, "eight events queued");
  int ordered = 1;
  unsigned last_serial = 0;
  for (int i = 0; i < 8; i++) {
    xc_dequeue(&c, &out);
    if ((int)out.xbutton.button != i) ordered = 0;
    if (out.xany.serial <= last_serial) ordered = 0;
    last_serial = out.xany.serial;
  }
  check(ordered, "events come out in order with increasing serials");

  /* Wrapping: fill past the ring size in several rounds. */
  int queued = 0;
  for (int round = 0; round < 4; round++) {
    for (int i = 0; i < XC_EVENT_RING - 1; i++) {
      memset(&ev, 0, sizeof ev);
      ev.type = MotionNotify;
      if (xc_queue(&c, &ev)) queued++;
    }
    for (int i = 0; i < XC_EVENT_RING - 1; i++) xc_dequeue(&c, &out);
  }
  check(queued == 4 * (XC_EVENT_RING - 1), "the ring wraps without losing slots");

  /* Overflow policy: the newest event is dropped and counted. */
  struct xc_core small;
  xc_init(&small);
  for (int i = 0; i < XC_EVENT_RING + 10; i++) {
    memset(&ev, 0, sizeof ev);
    ev.type = KeyPress;
    ev.xkey.keycode = (unsigned char)i;
    xc_queue(&small, &ev);
  }
  check(xc_pending(&small) == XC_EVENT_RING - 1, "the ring holds N-1 events");
  check(xc_dropped(&small) == 11, "overflow is counted, not silent");
  xc_dequeue(&small, &out);
  check(out.xkey.keycode == 0, "the OLDEST event survives a full ring");
}

/* ---- 2c. geometry and clip maths -------------------------------------- */

static void test_geometry(void) {
  struct xc_rect content = { 100, 50, 640, 480 };
  struct xc_rect r;

  r.x = 10; r.y = 10; r.w = 100; r.h = 100;
  check(xc_clip_to_content(&content, &r) == 1, "an inside rect clips to itself");
  check(r.w == 100 && r.h == 100, "inside rect unchanged");

  r.x = -20; r.y = -10; r.w = 100; r.h = 100;
  check(xc_clip_to_content(&content, &r) == 1, "a negative rect clips");
  check(r.x == 0 && r.y == 0 && r.w == 80 && r.h == 90,
        "negative origin is clipped to the content");

  r.x = 600; r.y = 400; r.w = 100; r.h = 100;
  check(xc_clip_to_content(&content, &r) == 1, "an overhanging rect clips");
  check(r.w == 40 && r.h == 80, "overhang is clipped to the content");

  r.x = 640; r.y = 10; r.w = 10; r.h = 10;
  check(xc_clip_to_content(&content, &r) == 0, "a rect past the edge clips away");

  r.x = 0; r.y = 0; r.w = 0; r.h = 10;
  check(xc_clip_to_content(&content, &r) == 0, "a zero-width rect is empty");

  /* content -> screen, including a window pushed partly off-screen. */
  r.x = 0; r.y = 0; r.w = 640; r.h = 480;
  check(xc_content_to_screen(&content, &r, 1920, 1080) == 1,
        "the whole content maps to the screen");
  check(r.x == 100 && r.y == 50, "content origin is added");

  struct xc_rect off = { -30, -20, 640, 480 };
  r.x = 0; r.y = 0; r.w = 640; r.h = 480;
  check(xc_content_to_screen(&off, &r, 1920, 1080) == 1,
        "a partly off-screen window clips");
  check(r.x == 0 && r.y == 0 && r.w == 610 && r.h == 460,
        "off-screen part is dropped");

  struct xc_rect big = { 1900, 1070, 640, 480 };
  r.x = 0; r.y = 0; r.w = 640; r.h = 480;
  check(xc_content_to_screen(&big, &r, 1920, 1080) == 1,
        "a window past the bottom-right clips");
  check(r.w == 20 && r.h == 10, "bottom-right overhang is clipped");

  /* Dirty bbox accumulation. */
  struct xc_core c;
  xc_init(&c);
  c.content = content;
  c.content_valid = 1;
  check(xc_dirty_take(&c, &r) == 0, "a fresh dirty bbox is empty");
  xc_dirty_add(&c, 10, 10, 20, 20);
  xc_dirty_add(&c, 30, 5, 40, 15);
  check(xc_dirty_take(&c, &r) == 1, "dirty bbox takes");
  check(r.x == 10 && r.y == 5 && r.w == 30 && r.h == 15,
        "dirty bbox is the union");
  check(xc_dirty_take(&c, &r) == 0, "taking the bbox resets it");
  xc_dirty_add(&c, -50, -50, 5, 5);
  check(xc_dirty_take(&c, &r) == 1 && r.x == 0 && r.y == 0,
        "dirty bbox clamps to the content");
  xc_dirty_add(&c, 700, 500, 800, 600);
  check(xc_dirty_take(&c, &r) == 0, "a fully outside rect dirties nothing");

  /* Pointer translation, both directions, including outside the window. */
  int x, y;
  xc_root_to_window(&content, 150, 80, &x, &y);
  check(x == 50 && y == 30, "root->window subtracts the content origin");
  xc_window_to_root(&content, 50, 30, &x, &y);
  check(x == 150 && y == 80, "window->root adds it back");
  xc_root_to_window(&content, 5, 5, &x, &y);
  check(x == -95 && y == -45, "a pointer outside the window is negative");
}

/* ---- 2d. keymap ------------------------------------------------------- */

static void test_keymap(void) {
  check(xc_keymap_len() > 70, "the keymap has the US layout");
  check(xc_keysym_to_keycode('a') == 38, "XK_a is keycode 38");
  check(xc_keysym_to_keycode('A') == 38, "XK_A folds to the same keycode");
  check(xc_keysym_to_keycode('q') == 24, "XK_q is keycode 24");
  check(xc_keysym_to_keycode('1') == 10, "XK_1 is keycode 10");
  check(xc_keysym_to_keycode(XK_Return) == 36, "Return is keycode 36");
  check(xc_keysym_to_keycode(XK_Escape) == 9, "Escape is keycode 9");
  check(xc_keysym_to_keycode(XK_Left) == 113, "Left is keycode 113");
  check(xc_keysym_to_keycode(XK_F1) == 67, "F1 is keycode 67");
  check(xc_keysym_to_keycode(0x1234) == 0, "an unknown keysym has no keycode");

  check(xc_keycode_to_keysym(38, 0) == 'a', "keycode 38 unshifted is 'a'");
  check(xc_keycode_to_keysym(38, 1) == 'A', "keycode 38 shifted is 'A'");
  check(xc_keycode_to_keysym(10, 1) == '!', "keycode 10 shifted is '!'");
  check(xc_keycode_to_keysym(23, 1) == XK_ISO_Left_Tab, "Tab shifted is BackTab");
  check(xc_keycode_to_keysym(200, 0) == XK_VoidSymbol,
        "an unknown keycode is VoidSymbol");

  /* Round-trip every key in the table (this is what keeps SDL's
   * keycode->scancode table landing on the right physical key). */
  const struct xc_key *k = xc_keymap();
  int rt = 1;
  for (int i = 0; k[i].keycode; i++) {
    if (xc_keysym_to_keycode(k[i].sym) != k[i].keycode) rt = 0;
    if (xc_keycode_to_keysym(k[i].keycode, 0) != k[i].sym) rt = 0;
  }
  check(rt, "every keymap entry round-trips keysym <-> keycode");

  /* XLookupString semantics. */
  XKeyEvent ev;
  char buf[8];
  KeySym sym;
  memset(&ev, 0, sizeof ev);
  memset(buf, 0, sizeof buf);

  ev.keycode = 38;                       /* 'a' */
  check(xc_lookup_string(&ev, buf, sizeof buf, &sym) == 1, "letter produces one byte");
  check(buf[0] == 'a' && sym == 'a', "plain letter is 'a'");

  ev.state = ShiftMask;
  xc_lookup_string(&ev, buf, sizeof buf, &sym);
  check(buf[0] == 'A' && sym == 'A', "Shift produces 'A'");

  ev.state = ControlMask;
  xc_lookup_string(&ev, buf, sizeof buf, &sym);
  check(buf[0] == 1, "Ctrl+A produces the control byte");
  check(sym == 'A', "Ctrl+A reports the letter keysym");

  ev.state = 0;
  ev.keycode = 36;                       /* Return */
  xc_lookup_string(&ev, buf, sizeof buf, &sym);
  check(buf[0] == '\r' && sym == XK_Return, "Return is CR");

  ev.keycode = 22;                       /* BackSpace */
  xc_lookup_string(&ev, buf, sizeof buf, &sym);
  check(buf[0] == '\b' && sym == XK_BackSpace, "BackSpace is BS");

  ev.keycode = 113;                      /* Left */
  check(xc_lookup_string(&ev, buf, sizeof buf, &sym) == 0, "Left has no text");
  check(sym == XK_Left, "Left reports its keysym");

  ev.keycode = 65;                       /* space */
  xc_lookup_string(&ev, buf, sizeof buf, &sym);
  check(buf[0] == ' ', "space produces a space");
}

/* ---- 3. the export contract ------------------------------------------ */

static void test_export_list(void) {
#ifdef XCOMPAT_REQUIRED_COUNT
  check(XCOMPAT_REQUIRED_COUNT > 100,
        "the required-symbol list is the measured one (>100)");
  check(XCOMPAT_REQUIRED_COUNT < 400, "the required-symbol list is bounded");
  int shaped = 1;
  for (int i = 0; i < XCOMPAT_REQUIRED_COUNT; i++) {
    const char *s = xcompat_required[i];
    if (!s || s[0] != 'X') shaped = 0;      /* every Xlib name starts with X */
  }
  check(shaped, "every required symbol looks like an Xlib entry point");
#else
  printf("SKIP export list (required-libx11.h not generated)\n");
#endif

#ifdef XCOMPAT_PULSE_COUNT
  check(XCOMPAT_PULSE_COUNT == 21,
        "the pulse list is the measured 21 (5 simple + 16 async)");
  int pshaped = 1, psorted = 1;
  for (int i = 0; i < XCOMPAT_PULSE_COUNT; i++) {
    const char *s = xcompat_pulse_required[i];
    if (!s || s[0] != 'p' || s[1] != 'a' || s[2] != '_') pshaped = 0;
    if (i && strcmp(xcompat_pulse_required[i - 1], s) >= 0) psorted = 0;
  }
  check(pshaped, "every required pulse symbol is a pa_* name");
  check(psorted, "the required pulse list is sorted and duplicate-free");
#endif

#ifdef XCOMPAT_DSO_PATH
  /* The strongest form of the F4 check: dlopen the artifact this tree just
   * built and resolve every required name through it, exactly the way
   * SDL's X11 driver does. */
  void *h = dlopen(XCOMPAT_DSO_PATH, RTLD_NOW | RTLD_LOCAL);
  check(h != 0, "the built libX11.so.6 dlopens");
  if (h) {
    int missing = 0;
#ifdef XCOMPAT_REQUIRED_COUNT
    for (int i = 0; i < XCOMPAT_REQUIRED_COUNT; i++) {
      if (!dlsym(h, xcompat_required[i])) {
        if (missing < 10)
          printf("FAIL dlsym(%s) is NULL\n", xcompat_required[i]);
        missing++;
      }
    }
    check(missing == 0, "every required symbol resolves through the built DSO");
    printf("     dso: %d required symbols dlsym'd from %s\n",
           XCOMPAT_REQUIRED_COUNT - missing, XCOMPAT_DSO_PATH);
#endif
    check(dlsym(h, "XOpenDisplay") != 0, "XOpenDisplay is exported");
    check(dlsym(h, "XNextEvent") != 0, "XNextEvent is exported");
    check(dlsym(h, "XInternAtom") != 0, "XInternAtom is exported");
    dlclose(h);
  }
#else
  printf("SKIP dso dlsym check (XCOMPAT_DSO_PATH not set)\n");
#endif

#ifdef XCOMPAT_ALSA_PATH
  void *a = dlopen(XCOMPAT_ALSA_PATH, RTLD_NOW | RTLD_LOCAL);
  check(a != 0, "the built libasound.so.2 dlopens");
  if (a) {
    int missing = 0;
    for (int i = 0; i < XCOMPAT_ALSA_COUNT; i++)
      if (!dlsym(a, xcompat_alsa_required[i])) missing++;
    check(missing == 0, "every required snd_* symbol resolves");
    /* The contract is "open refuses with -ENODEV, close is idempotent". */
    int (*pcm_open)(void **, const char *, int, int) =
        (int (*)(void **, const char *, int, int))dlsym(a, "snd_pcm_open");
    int (*pcm_close)(void *) = (int (*)(void *))dlsym(a, "snd_pcm_close");
    int (*hint)(int, const char *, void ***) =
        (int (*)(int, const char *, void ***))dlsym(a, "snd_device_name_hint");
    const char *(*strerror_)(int) =
        (const char *(*)(int))dlsym(a, "snd_strerror");
    void **hints = (void **)0x1;
    /* ALSA's shape is a NULL-terminated array, not a NULL pointer: FMOD
     * walks hints[i] without checking the array (see alsa_null.c). */
    check(hint && hint(0, "pcm", &hints) == 0 && hints != 0 && hints[0] == 0,
          "device enumeration returns a NULL-terminated empty array");
    void *pcm = (void *)0x1;
    check(pcm_open && pcm_open(&pcm, "default", 0, 0) == -19,
          "snd_pcm_open fails with -ENODEV");
    check(pcm == 0, "a failed open nulls the handle");
    check(pcm_close && pcm_close(0) == 0, "snd_pcm_close is idempotent");
    check(strerror_ && strcmp(strerror_(-19), "No such device") == 0,
          "snd_strerror(-ENODEV) reads like ALSA");
    printf("     alsa: %d snd_* symbols, open->-ENODEV\n", XCOMPAT_ALSA_COUNT);
    dlclose(a);
  }
#else
  printf("SKIP alsa dso check (XCOMPAT_ALSA_PATH not set)\n");
#endif
}

/* ---- 4. PulseAudio shim (libpulse-simple.so.0 / libpulse.so.0) -------- *
 * Two things are checked.  First the ABI: the shim reads pa_sample_spec and
 * writes pa_channel_map inside the caller's storage, so the four public
 * layouts and the enum values are pinned here, and -- when libpulse-dev is
 * installed -- compared row for row with the host's real <pulse/...>.
 *
 * Then the contract the game depends on: every required name resolves on
 * BOTH sonames, pa_simple_new answers "connection refused" without
 * allocating, and the whole probe is fast enough that FMOD's ladder cannot
 * stall on it (10k probes must finish in well under a second -- a single
 * sleep() anywhere in the shim would blow that). */
static void test_pulse_abi(void) {
  check(sizeof(pulse_channel_map) == PULSE_ABI_CHANNEL_MAP_SIZE,
        "sizeof(pa_channel_map) is 132");
  check(offsetof(pulse_channel_map, map) == PULSE_ABI_CHANNEL_MAP_MAP_OFF,
        "pa_channel_map.map is at offset 4");
  check(sizeof(pulse_sample_spec) == PULSE_ABI_SAMPLE_SPEC_SIZE,
        "sizeof(pa_sample_spec) is 12");
  check(offsetof(pulse_sample_spec, rate) == PULSE_ABI_SAMPLE_SPEC_RATE_OFF,
        "pa_sample_spec.rate is at offset 4");
  check(offsetof(pulse_sample_spec, channels) == PULSE_ABI_SAMPLE_SPEC_CH_OFF,
        "pa_sample_spec.channels is at offset 8");
  check(sizeof(pulse_buffer_attr) == PULSE_ABI_BUFFER_ATTR_SIZE,
        "sizeof(pa_buffer_attr) is 20");
  check(sizeof(pulse_mainloop_api) == PULSE_ABI_MAINLOOP_API_SIZE,
        "sizeof(pa_mainloop_api) is 112");
  check(offsetof(pulse_mainloop_api, io_new) ==
            PULSE_ABI_MAINLOOP_API_IO_OFF,
        "pa_mainloop_api.io_new is at offset 8");
  check(offsetof(pulse_mainloop_api, quit) ==
            PULSE_ABI_MAINLOOP_API_QUIT_OFF,
        "pa_mainloop_api.quit is at offset 104");
  check(PA_ERR_CONNECTIONREFUSED_ == 6, "PA_ERR_CONNECTIONREFUSED is 6");
  check(PA_ERR_INVALID_ == 3, "PA_ERR_INVALID is 3");
  check(PA_CONTEXT_FAILED_ == 5, "PA_CONTEXT_FAILED is 5");
  check(PA_OPERATION_CANCELLED_ == 2, "PA_OPERATION_CANCELLED is 2");
  check(PULSE_CHANNELS_MAX == 32, "PA_CHANNELS_MAX is 32");

#if XCOMPAT_HAVE_PULSE_REF
  static const char *const row_names[XCOMPAT_PULSE_REF_ROWS] = {
    "sizeof(pa_channel_map)", "offsetof(pa_channel_map, map)",
    "sizeof(pa_sample_spec)", "offsetof(pa_sample_spec, rate)",
    "offsetof(pa_sample_spec, channels)", "sizeof(pa_buffer_attr)",
    "sizeof(pa_mainloop_api)", "offsetof(pa_mainloop_api, io_new)",
    "offsetof(pa_mainloop_api, quit)", "PA_ERR_CONNECTIONREFUSED",
    "PA_CONTEXT_FAILED", "PA_OPERATION_CANCELLED", "PA_CHANNELS_MAX",
  };
  const unsigned long mine[XCOMPAT_PULSE_REF_ROWS] = {
    (unsigned long)sizeof(pulse_channel_map),
    (unsigned long)offsetof(pulse_channel_map, map),
    (unsigned long)sizeof(pulse_sample_spec),
    (unsigned long)offsetof(pulse_sample_spec, rate),
    (unsigned long)offsetof(pulse_sample_spec, channels),
    (unsigned long)sizeof(pulse_buffer_attr),
    (unsigned long)sizeof(pulse_mainloop_api),
    (unsigned long)offsetof(pulse_mainloop_api, io_new),
    (unsigned long)offsetof(pulse_mainloop_api, quit),
    (unsigned long)PA_ERR_CONNECTIONREFUSED_,
    (unsigned long)PA_CONTEXT_FAILED_,
    (unsigned long)PA_OPERATION_CANCELLED_,
    (unsigned long)PULSE_CHANNELS_MAX,
  };
  int drift = 0;
  for (int i = 0; i < XCOMPAT_PULSE_REF_ROWS; i++) {
    if (mine[i] != xcompat_pulse_ref_values[i]) {
      printf("FAIL pulse ABI row %s: shim says %lu, real <pulse/...> says "
             "%lu\n", row_names[i], mine[i], xcompat_pulse_ref_values[i]);
      drift++;
    }
  }
  check(drift == 0, "every pulse ABI row matches the host's real headers");
  printf("     pulse abi: %d rows identical to <pulse/...>\n",
         XCOMPAT_PULSE_REF_ROWS);
#else
  printf("SKIP pulse ABI differential (no libpulse-dev headers; the pinned "
         "sizes above still ran)\n");
#endif
}

static void test_pulse_dso(void) {
#ifdef XCOMPAT_PULSE_SIMPLE_PATH
  const char *paths[2] = { XCOMPAT_PULSE_SIMPLE_PATH, XCOMPAT_PULSE_PATH };
  const char *labels[2] = { "libpulse-simple.so.0", "libpulse.so.0" };
  void *h[2] = { 0, 0 };

  for (int k = 0; k < 2; k++) {
    h[k] = dlopen(paths[k], RTLD_NOW | RTLD_LOCAL);
    if (!h[k]) {
      printf("FAIL dlopen(%s): %s\n", labels[k], dlerror());
      fails++;
    }
    checks++;
  }

  for (int k = 0; k < 2; k++) {
    if (!h[k]) continue;
#ifdef XCOMPAT_PULSE_COUNT
    int missing = 0;
    for (int i = 0; i < XCOMPAT_PULSE_COUNT; i++) {
      if (!dlsym(h[k], xcompat_pulse_required[i])) {
        if (missing < 5)
          printf("FAIL %s: dlsym(%s) is NULL\n", labels[k],
                 xcompat_pulse_required[i]);
        missing++;
      }
    }
    check(missing == 0, "every required pa_* symbol resolves on both sonames");
    printf("     pulse: %s resolves %d/%d required symbols\n", labels[k],
           XCOMPAT_PULSE_COUNT - missing, XCOMPAT_PULSE_COUNT);
#endif
  }

  void *s = h[0] ? h[0] : h[1];
  if (!s) {
    printf("SKIP pulse dso checks (neither soname dlopened)\n");
    return;
  }

  /* ---- libpulse-simple: the probe FMOD actually makes ---- */
  typedef void *(*simple_new_fn)(const char *, const char *, int, const char *,
                                 const char *, const pulse_sample_spec *,
                                 const pulse_channel_map *,
                                 const pulse_buffer_attr *, int *);
  typedef void (*simple_free_fn)(void *);
  typedef int (*simple_io_fn)(void *, const void *, unsigned long, int *);
  typedef unsigned long (*simple_lat_fn)(void *, int *);
  typedef const char *(*strerror_fn)(int);
  typedef const char *(*version_fn)(void);

  simple_new_fn simple_new = (simple_new_fn)dlsym(s, "pa_simple_new");
  simple_free_fn simple_free = (simple_free_fn)dlsym(s, "pa_simple_free");
  simple_io_fn simple_write = (simple_io_fn)dlsym(s, "pa_simple_write");
  simple_io_fn simple_read = (simple_io_fn)dlsym(s, "pa_simple_read");
  simple_lat_fn simple_latency =
      (simple_lat_fn)dlsym(s, "pa_simple_get_latency");
  strerror_fn strerror_ = (strerror_fn)dlsym(s, "pa_strerror");
  version_fn version = (version_fn)dlsym(s, "pa_get_library_version");

  check(simple_new && simple_free && simple_write && simple_read &&
            simple_latency && strerror_ && version,
        "the simple-API block resolves as one unit (SDL loads it that way)");

  pulse_sample_spec ss = { 5 /* PA_SAMPLE_FLOAT32LE */, 48000, 2 };
  int err = 0;
  void *stream = simple_new ? simple_new(0, "xcompat-test", 1,
                                         0, "probe", &ss, 0, 0, &err) : 0;
  check(stream == 0, "pa_simple_new returns NULL (there is no server)");
  check(err == 6, "pa_simple_new reports PA_ERR_CONNECTIONREFUSED");
  if (strerror_)
    check(strcmp(strerror_(err), "Connection refused") == 0,
          "pa_strerror(6) reads \"Connection refused\"");

  pulse_sample_spec bad = { 5, 0, 0 };
  err = 0;
  if (simple_new) stream = simple_new(0, "xcompat-test", 1, 0, "probe", &bad,
                                      0, 0, &err);
  check(stream == 0 && err == 3,
        "a malformed sample spec is PA_ERR_INVALID, still NULL");

  simple_free(0);                       /* must not crash */
  err = 0;
  check(simple_write(0, 0, 0, &err) < 0, "pa_simple_write fails on a NULL handle");
  err = 0;
  check(simple_read(0, 0, 0, &err) < 0, "pa_simple_read fails on a NULL handle");
  err = 0;
  check(simple_latency(0, &err) == (pulse_u64)-1,
        "pa_simple_get_latency returns the unknown value");
  if (version)
    check(strncmp(version(), "17.", 3) == 0,
          "pa_get_library_version reports the measured 17.x era");
  if (strerror_) {
    check(strcmp(strerror_(0), "OK") == 0, "pa_strerror(0) is \"OK\"");
    check(strcmp(strerror_(3), "Invalid argument") == 0,
          "pa_strerror(3) is \"Invalid argument\"");
    check(strerror_(999) != 0 && strerror_(999)[0] != 0,
          "pa_strerror() never returns NULL");
  }

  /* The one entry point that writes into caller memory: bounds and layout. */
  typedef pulse_channel_map *(*map_fn)(pulse_channel_map *, unsigned, int);
  map_fn map_init = (map_fn)dlsym(s, "pa_channel_map_init_auto");
  check(map_init != 0, "pa_channel_map_init_auto is exported");
  if (map_init) {
    struct {
      pulse_channel_map m;
      unsigned char guard[16];
    } box;
    memset(&box, 0xAA, sizeof box);
    pulse_channel_map *r = map_init(&box.m, 2, 0);
    check(r == &box.m, "pa_channel_map_init_auto returns the caller's map");
    check(box.m.channels == 2 && box.m.map[0] == 1 && box.m.map[1] == 2,
          "stereo default layout is front-left/front-right");
    int guard_ok = 1;
    for (unsigned i = 0; i < sizeof box.guard; i++)
      if (box.guard[i] != 0xAA) guard_ok = 0;
    check(guard_ok, "pa_channel_map_init_auto writes only inside the map");
    check(map_init(&box.m, 0, 0) == 0 && map_init(&box.m, 33, 0) == 0,
          "0 and >PA_CHANNELS_MAX are refused");
  }

  /* ---- libpulse: the async shape SDL/FMOD probe ---- */
  typedef void *(*ml_new_fn)(void);
  typedef pulse_mainloop_api *(*ml_api_fn)(void *);
  typedef int (*ml_iter_fn)(void *, int, int *);
  typedef void *(*ctx_new_fn)(pulse_mainloop_api *, const char *);
  typedef int (*ctx_connect_fn)(void *, const char *, int, const void *);
  typedef int (*ctx_state_fn)(void *);
  typedef int (*ctx_errno_fn)(void *);
  typedef void *(*ctx_ops_fn)(void *, void *, void *);
  typedef int (*op_state_fn)(void *);

  ml_new_fn ml_new = (ml_new_fn)dlsym(s, "pa_mainloop_new");
  ml_api_fn ml_api = (ml_api_fn)dlsym(s, "pa_mainloop_get_api");
  ml_iter_fn ml_iter = (ml_iter_fn)dlsym(s, "pa_mainloop_iterate");
  ctx_new_fn ctx_new = (ctx_new_fn)dlsym(s, "pa_context_new");
  ctx_connect_fn ctx_connect = (ctx_connect_fn)dlsym(s, "pa_context_connect");
  ctx_state_fn ctx_state = (ctx_state_fn)dlsym(s, "pa_context_get_state");
  ctx_errno_fn ctx_errno = (ctx_errno_fn)dlsym(s, "pa_context_errno");
  ctx_ops_fn ctx_server = (ctx_ops_fn)dlsym(s, "pa_context_get_server_info");
  op_state_fn op_state = (op_state_fn)dlsym(s, "pa_operation_get_state");

  check(ml_new && ml_api && ml_iter && ctx_new && ctx_connect && ctx_state &&
            ctx_errno && ctx_server && op_state,
        "the async-API block resolves");

  void *ml = ml_new ? ml_new() : 0;
  check(ml != 0, "pa_mainloop_new returns a handle");
  pulse_mainloop_api *api = ml_api ? ml_api(ml) : 0;
  check(api != 0, "pa_mainloop_get_api returns a vtable");
  if (api) {
    /* A caller that walks the vtable must not jump to NULL. */
    int nulls = 0;
    nulls += api->io_new == 0;
    nulls += api->io_enable == 0;
    nulls += api->io_free == 0;
    nulls += api->io_set_destroy == 0;
    nulls += api->time_new == 0;
    nulls += api->time_restart == 0;
    nulls += api->time_free == 0;
    nulls += api->time_set_destroy == 0;
    nulls += api->defer_new == 0;
    nulls += api->defer_enable == 0;
    nulls += api->defer_free == 0;
    nulls += api->defer_set_destroy == 0;
    nulls += api->quit == 0;
    check(nulls == 0, "all 13 vtable slots are callable");
    check(api->io_new(api, 0, 0, 0, 0) == 0, "vtable io_new returns NULL");
    api->quit(api, 0);                  /* inert, must not crash */
  }
  if (ml_iter) {
    int ret = -123;
    check(ml_iter(ml, 1, &ret) == -1,
          "pa_mainloop_iterate returns -1 and never blocks");
  }

  void *ctx = ctx_new ? ctx_new(api, "xcompat-test") : 0;
  check(ctx != 0, "pa_context_new returns a handle (as libpulse does)");
  check(ctx_connect && ctx_connect(ctx, 0, 0, 0) == -6,
        "pa_context_connect fails with -PA_ERR_CONNECTIONREFUSED");
  check(ctx_state && ctx_state(ctx) == 5,
        "pa_context_get_state is PA_CONTEXT_FAILED");
  check(ctx_errno && ctx_errno(ctx) == 6,
        "pa_context_errno is PA_ERR_CONNECTIONREFUSED");
  check(ctx_server && ctx_server(ctx, 0, 0) == 0,
        "pa_context_get_server_info returns no operation");
  check(op_state && op_state(0) == 2,
        "pa_operation_get_state(NULL) is CANCELLED, not a crash");

  /* ---- the "fails fast" requirement ---- */
  if (simple_new) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 10000; i++) {
      err = 0;
      simple_new(0, "probe", 1, 0, "probe", &ss, 0, 0, &err);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    check(ms < 500.0,
          "10000 pa_simple_new probes finish in <500 ms (no sleep anywhere)");
    printf("     pulse: 10000 pa_simple_new probes in %.2f ms\n", ms);
  }

  for (int k = 0; k < 2; k++)
    if (h[k]) dlclose(h[k]);
#else
  printf("SKIP pulse dso check (XCOMPAT_PULSE_SIMPLE_PATH not set)\n");
#endif
}

int main(void) {
  printf("xcompat host tests\n");
  test_abi();
  test_atoms();
  test_ring();
  test_geometry();
  test_keymap();
  test_export_list();
  test_pulse_abi();
  test_pulse_dso();
  printf("[TEST] %d checks, %d failed\n", checks, fails);
  if (fails) {
    printf("SOME XCOMPAT TESTS FAILED\n");
    return 1;
  }
  printf("ALL XCOMPAT TESTS PASSED\n");
  return 0;
}
