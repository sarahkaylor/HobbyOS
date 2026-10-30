/*
 * hw/hobbyos/hbinput.c -- HobbyOS DDX input: the kernel's evdev node
 * (SYS_INPUT_READ, Linux 24-byte struct input_event records) turned into
 * dix keyboard + pointer events.  Derived from hw/vfb/InitInput.c.
 *
 * The node has no file descriptor (read_input_events() / get_events() are
 * non-blocking syscalls), so it cannot sit in the server's ospoll set.  Instead a block
 * handler drains it every time the server is about to sleep and bounds the
 * sleep to hbInputPollMs (default 10 ms) -- or 0 when it just queued
 * events, so they are processed before the next poll.
 *
 * Mapping:
 *   EV_KEY code < 0x100  -> key, X keycode = evdev code + 8 (evdev rules
 *                           keymap /X11/US.XKM); value 2 (autorepeat) is
 *                           dropped, XKB repeats on its own.
 *   BTN_LEFT/MIDDLE/RIGHT/SIDE/EXTRA (+BTN_TOUCH) -> buttons 1/2/3/8/9
 *   REL_WHEEL +/- -> button 4/5 click, REL_HWHEEL -/+ -> 6/7
 *   ABS_X/ABS_Y (0..32767: x86 PS/2 + vmmouse via the kernel decoder, ARM
 *   virtio-tablet) -> absolute screen position; REL_X/REL_Y (a virtio
 *   mouse) -> relative motion of the same position.  Motion is coalesced
 *   per EV_SYN packet and flushed before any button so clicks land at the
 *   packet's position.
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdint.h>
#include <string.h>
#include <X11/X.h>
#include "mi.h"
#include <X11/Xproto.h>
#include "scrnintstr.h"
#include "inputstr.h"
#include <X11/Xos.h>
#include "mipointer.h"
#include "xkbsrv.h"
#include <X11/keysym.h>
#include "xserver-properties.h"
#include "exevents.h"
#include "extinit.h"
#include "inpututils.h"
#include "hobbyos.h"

extern int read_input_events(void *buf, int max_events);   /* SYS_INPUT_READ */
extern int get_events(void *buf, int max_events);          /* SYS_GET_EVENTS */

/* SYS_GET_EVENTS record: the kernel ring's raw triple (8 bytes). */
struct hb_raw_event {
    uint16_t type, code;
    uint32_t value;
};

struct hb_input_event {         /* Linux ABI, 24 bytes */
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define HB_EV_SYN 0x00
#define HB_EV_KEY 0x01
#define HB_EV_REL 0x02
#define HB_EV_ABS 0x03
#define HB_REL_X 0x00
#define HB_REL_Y 0x01
#define HB_REL_HWHEEL 0x06
#define HB_REL_WHEEL 0x08
#define HB_ABS_X 0x00
#define HB_ABS_Y 0x01
#define HB_ABS_MAX 32767
#define HB_BTN_LEFT 0x110
#define HB_BTN_RIGHT 0x111
#define HB_BTN_MIDDLE 0x112
#define HB_BTN_SIDE 0x113
#define HB_BTN_EXTRA 0x114
#define HB_BTN_TOUCH 0x14a

#define HB_MIN_KEYCODE 8
#define HB_MAX_KEYCODE 255
#define HB_NBUTTONS 9
#define HB_NAXES 2

int hbInputPollMs = HB_INPUT_POLL_MS_DEFAULT;

static DeviceIntPtr hbPointer, hbKeyboard;
static int hbPx = -1, hbPy = -1;        /* current pointer position */
static Bool hbMotionPending;
static Bool hbBlockRegistered;
unsigned long hbInputEvents;            /* stats (debugger aid) */

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
}

/* ---- devices ------------------------------------------------------------ */
static int
hbKeybdProc(DeviceIntPtr pDevice, int onoff)
{
    DevicePtr pDev = (DevicePtr) pDevice;

    switch (onoff) {
    case DEVICE_INIT:
        /* NULL RMLVO = the built-in defaults (evdev/pc105/us); the patched
         * xkb/ddxLoad.c answers with /X11/US.XKM. */
        if (!InitKeyboardDeviceStruct(pDevice, NULL, NULL, NULL))
            return BadAlloc;
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    }
    return Success;
}

static int
hbMouseProc(DeviceIntPtr pDevice, int onoff)
{
    BYTE map[HB_NBUTTONS + 1];
    DevicePtr pDev = (DevicePtr) pDevice;
    Atom btn_labels[HB_NBUTTONS] = { 0 };
    Atom axes_labels[HB_NAXES] = { 0 };
    int i;

    switch (onoff) {
    case DEVICE_INIT:
        for (i = 0; i <= HB_NBUTTONS; i++)
            map[i] = i;
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
        btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
        btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
        btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
        btn_labels[7] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_SIDE);
        btn_labels[8] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_EXTRA);
        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y);
        if (!InitPointerDeviceStruct(pDev, map, HB_NBUTTONS, btn_labels,
                                     (PtrCtrlProcPtr) NoopDDA,
                                     GetMotionHistorySize(), HB_NAXES,
                                     axes_labels))
            return BadAlloc;
        /* Absolute axes in screen coordinates. */
        InitValuatorAxisStruct(pDevice, 0, axes_labels[0], 0,
                               hbScreenWidth() - 1, 1, 0, 1, Absolute);
        InitValuatorAxisStruct(pDevice, 1, axes_labels[1], 0,
                               hbScreenHeight() - 1, 1, 0, 1, Absolute);
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    }
    return Success;
}

/* ---- event translation -------------------------------------------------- */
static void
hbFlushMotion(void)
{
    ValuatorMask mask;

    if (!hbMotionPending || !hbPointer || !hbPointer->public.on)
        return;
    hbMotionPending = FALSE;
    valuator_mask_zero(&mask);
    valuator_mask_set(&mask, 0, hbPx);
    valuator_mask_set(&mask, 1, hbPy);
    QueuePointerEvents(hbPointer, MotionNotify, 0,
                       POINTER_ABSOLUTE | POINTER_SCREEN, &mask);
}

static void
hbButton(int button, Bool down)
{
    ValuatorMask mask;

    if (!hbPointer || !hbPointer->public.on)
        return;
    hbFlushMotion();
    valuator_mask_zero(&mask);
    QueuePointerEvents(hbPointer, down ? ButtonPress : ButtonRelease, button,
                       POINTER_RELATIVE, &mask);
}

static void
hbClick(int button, int count)
{
    if (count > 8)
        count = 8;
    while (count-- > 0) {
        hbButton(button, TRUE);
        hbButton(button, FALSE);
    }
}

static int
hbClamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void
hbSetPos(int x, int y)
{
    x = hbClamp(x, 0, hbScreenWidth() - 1);
    y = hbClamp(y, 0, hbScreenHeight() - 1);
    if (x != hbPx || y != hbPy) {
        hbPx = x;
        hbPy = y;
        hbMotionPending = TRUE;
    }
}

static void
hbHandleEvent(const struct hb_input_event *ev)
{
    switch (ev->type) {
    case HB_EV_SYN:
        if (ev->code == 0)
            hbFlushMotion();
        break;
    case HB_EV_KEY:
        switch (ev->code) {
        case HB_BTN_LEFT:
        case HB_BTN_TOUCH:  hbButton(1, ev->value != 0); return;
        case HB_BTN_MIDDLE: hbButton(2, ev->value != 0); return;
        case HB_BTN_RIGHT:  hbButton(3, ev->value != 0); return;
        case HB_BTN_SIDE:   hbButton(8, ev->value != 0); return;
        case HB_BTN_EXTRA:  hbButton(9, ev->value != 0); return;
        }
        if (ev->code > 0 && ev->code + HB_MIN_KEYCODE <= HB_MAX_KEYCODE &&
            ev->value != 2 && hbKeyboard && hbKeyboard->public.on) {
            hbFlushMotion();
            QueueKeyboardEvents(hbKeyboard, ev->value ? KeyPress : KeyRelease,
                                ev->code + HB_MIN_KEYCODE);
        }
        break;
    case HB_EV_REL:
        switch (ev->code) {
        case HB_REL_X:
            hbSetPos((hbPx < 0 ? 0 : hbPx) + ev->value, hbPy < 0 ? 0 : hbPy);
            break;
        case HB_REL_Y:
            hbSetPos(hbPx < 0 ? 0 : hbPx, (hbPy < 0 ? 0 : hbPy) + ev->value);
            break;
        case HB_REL_WHEEL:
            if (ev->value > 0) hbClick(4, ev->value);
            else if (ev->value < 0) hbClick(5, -ev->value);
            break;
        case HB_REL_HWHEEL:
            if (ev->value < 0) hbClick(6, -ev->value);
            else if (ev->value > 0) hbClick(7, ev->value);
            break;
        }
        break;
    case HB_EV_ABS: {
        int v = hbClamp(ev->value, 0, HB_ABS_MAX);
        if (ev->code == HB_ABS_X)
            hbSetPos((int) (((int64_t) v * (hbScreenWidth() - 1) + HB_ABS_MAX / 2) / HB_ABS_MAX),
                     hbPy < 0 ? hbScreenHeight() / 2 : hbPy);
        else if (ev->code == HB_ABS_Y)
            hbSetPos(hbPx < 0 ? hbScreenWidth() / 2 : hbPx,
                     (int) (((int64_t) v * (hbScreenHeight() - 1) + HB_ABS_MAX / 2) / HB_ABS_MAX));
        break;
    }
    }
}

/*
 * Both syscalls drain the same kernel ring.  SYS_INPUT_READ (Linux 24-byte
 * records) is preferred; as of 2026-10-30 its range check is inverted on
 * both arches (src/kernel/arch/{x64,arm}/trap.c sys_input_read: a VALID
 * buffer gets -EFAULT), so on -EFAULT the DDX switches for good to
 * SYS_GET_EVENTS, the desktop's call, which returns the raw triples.
 */
static Bool hbUseGetEvents;

static int
hbReadBatch(struct hb_input_event *evs, int max)
{
    struct hb_raw_event raw[64];
    int n, i;

    if (!hbUseGetEvents) {
        n = read_input_events(evs, max);
        if (n != -14 /* -EFAULT */)
            return n;
        hbUseGetEvents = TRUE;
        LogMessage(X_WARNING, "hobbyos: SYS_INPUT_READ returned -EFAULT for a "
                   "valid buffer; using SYS_GET_EVENTS\n");
    }
    if (max > 64)
        max = 64;
    n = get_events(raw, max);
    for (i = 0; i < n; i++) {
        evs[i].sec = evs[i].usec = 0;
        evs[i].type = raw[i].type;
        evs[i].code = raw[i].code;
        evs[i].value = (int32_t) raw[i].value;
    }
    return n;
}

/* Drain the ring; returns how many records were handled. */
static int
hbDrainInput(void)
{
    struct hb_input_event evs[64];
    int total = 0, n, i;

    do {
        n = hbReadBatch(evs, 64);
        if (n <= 0)
            break;
        if (n > 64)
            n = 64;
        for (i = 0; i < n; i++)
            hbHandleEvent(&evs[i]);
        total += n;
    } while (n == 64 && total < 4096);
    hbFlushMotion();
    hbInputEvents += total;
    return total;
}

static void
hbBlockHandler(void *blockData, void *timeout)
{
    if (hbDrainInput() > 0)
        AdjustWaitForDelay(timeout, 0);
    else
        AdjustWaitForDelay(timeout, hbInputPollMs);
}

static void
hbWakeupHandler(void *blockData, int result)
{
}

void
InitInput(int argc, char *argv[])
{
    Atom xiclass;

    hbPointer = AddInputDevice(serverClient, hbMouseProc, TRUE);
    hbKeyboard = AddInputDevice(serverClient, hbKeybdProc, TRUE);
    xiclass = MakeAtom(XI_MOUSE, sizeof(XI_MOUSE) - 1, TRUE);
    AssignTypeAndName(hbPointer, xiclass, "HobbyOS pointer");
    xiclass = MakeAtom(XI_KEYBOARD, sizeof(XI_KEYBOARD) - 1, TRUE);
    AssignTypeAndName(hbKeyboard, xiclass, "HobbyOS keyboard");
    (void) mieqInit();
    if (!hbBlockRegistered)
        hbBlockRegistered = RegisterBlockAndWakeupHandlers(hbBlockHandler,
                                                           hbWakeupHandler,
                                                           NULL);
}

void
CloseInput(void)
{
    if (hbBlockRegistered) {
        RemoveBlockAndWakeupHandlers(hbBlockHandler, hbWakeupHandler, NULL);
        hbBlockRegistered = FALSE;
    }
    hbPointer = hbKeyboard = NULL;
    mieqFini();
}
