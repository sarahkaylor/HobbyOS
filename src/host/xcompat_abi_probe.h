/*
 * xcompat_abi_probe.h - one list of layout facts, compiled twice.
 *
 * The X11 compat DSO only works if its structures are byte-for-byte the
 * real X11R7 ones: SDL allocates XEvent on its own stack and reads
 * ev.xkey.keycode at the offset this header promises.  So the test builds
 * two translation units from this file -- one against src/user/xcompat/
 * xabi.h, one against the host's real <X11/Xlib.h> -- and compares the
 * emitted rows at runtime.  A drift in any size, field offset or constant
 * fails `make host_tests` with the exact row printed.
 *
 * The values are evaluated expressions (sizeof/offsetof/number), not
 * stringified tokens: a stringified sizeof would compare "sizeof(XEvent)"
 * with "sizeof(XEvent)" and pass no matter what the layout was.
 *
 * The including TU defines:
 *   ABI_HDR   the header to include
 *   ABI_FN    the name of the function returning the row array
 *   ABI_REF   set for the reference (real Xlib) side
 */
#include <stddef.h>
#include ABI_HDR
#ifdef ABI_REF
#include <X11/keysym.h>       /* XK_* live here, not in Xlib.h */
#include <X11/Xutil.h>        /* XVisualInfo / XSizeHints / XWMHints */
#include <X11/Xatom.h>        /* XA_* predefined atoms */
#endif
#include "xcompat_abi_row.h"

/* Xlib's C spelling of two fields differs from C++'s; both sides of the
 * diff use the same label and their own spelling. */
#ifdef ABI_REF
#define ABIF_CLASS class
#define ABIF_NEW new
#else
#define ABIF_CLASS c_class
#define ABIF_NEW new_
#endif

#define ABI_SIZE(t) { #t ".sizeof", (unsigned long)sizeof(t) },
#define ABI_FIELD(t, f) { #t "." #f, (unsigned long)offsetof(t, f) },
#define ABI_CONST(n) { #n, (unsigned long)(n) },
/* Field name spelled by the side (class/c_class). */
#define ABI_FIELD_AS(t, label, f) { #t "." #label, (unsigned long)offsetof(t, f) },

static const struct abi_row abi_rows[] = {
  ABI_SIZE(XEvent)
  ABI_SIZE(XKeyEvent)
  ABI_SIZE(XButtonEvent)
  ABI_SIZE(XMotionEvent)
  ABI_SIZE(XExposeEvent)
  ABI_SIZE(XConfigureEvent)
  ABI_SIZE(XClientMessageEvent)
  ABI_SIZE(XPropertyEvent)
  ABI_SIZE(XCrossingEvent)
  ABI_SIZE(XErrorEvent)
  ABI_SIZE(XWindowAttributes)
  ABI_SIZE(XSetWindowAttributes)
  ABI_SIZE(XVisualInfo)
  ABI_SIZE(XGCValues)
  ABI_SIZE(XWMHints)
  ABI_SIZE(XSizeHints)
  ABI_SIZE(XTextProperty)
  ABI_SIZE(XRectangle)

  ABI_FIELD(XKeyEvent, type)
  ABI_FIELD(XKeyEvent, serial)
  ABI_FIELD(XKeyEvent, send_event)
  ABI_FIELD(XKeyEvent, display)
  ABI_FIELD(XKeyEvent, window)
  ABI_FIELD(XKeyEvent, root)
  ABI_FIELD(XKeyEvent, subwindow)
  ABI_FIELD(XKeyEvent, time)
  ABI_FIELD(XKeyEvent, x)
  ABI_FIELD(XKeyEvent, y)
  ABI_FIELD(XKeyEvent, x_root)
  ABI_FIELD(XKeyEvent, y_root)
  ABI_FIELD(XKeyEvent, state)
  ABI_FIELD(XKeyEvent, keycode)
  ABI_FIELD(XKeyEvent, same_screen)

  ABI_FIELD(XButtonEvent, button)
  ABI_FIELD(XButtonEvent, state)
  ABI_FIELD(XButtonEvent, same_screen)

  ABI_FIELD(XMotionEvent, is_hint)
  ABI_FIELD(XMotionEvent, same_screen)

  ABI_FIELD(XExposeEvent, window)
  ABI_FIELD(XExposeEvent, x)
  ABI_FIELD(XExposeEvent, width)
  ABI_FIELD(XExposeEvent, count)

  ABI_FIELD(XConfigureEvent, window)
  ABI_FIELD(XConfigureEvent, above)
  ABI_FIELD(XConfigureEvent, override_redirect)

  ABI_FIELD(XClientMessageEvent, message_type)
  ABI_FIELD(XClientMessageEvent, format)
  ABI_FIELD(XClientMessageEvent, data)

  ABI_FIELD(XPropertyEvent, atom)
  ABI_FIELD(XPropertyEvent, time)
  ABI_FIELD(XPropertyEvent, state)

  ABI_FIELD(XErrorEvent, display)
  ABI_FIELD(XErrorEvent, resourceid)
  ABI_FIELD(XErrorEvent, serial)
  ABI_FIELD(XErrorEvent, error_code)

  ABI_FIELD(XWindowAttributes, x)
  ABI_FIELD(XWindowAttributes, y)
  ABI_FIELD(XWindowAttributes, width)
  ABI_FIELD(XWindowAttributes, height)
  ABI_FIELD(XWindowAttributes, border_width)
  ABI_FIELD(XWindowAttributes, depth)
  ABI_FIELD(XWindowAttributes, visual)
  ABI_FIELD(XWindowAttributes, root)
  ABI_FIELD_AS(XWindowAttributes, class, ABIF_CLASS)
  ABI_FIELD(XWindowAttributes, backing_store)
  ABI_FIELD(XWindowAttributes, save_under)
  ABI_FIELD(XWindowAttributes, colormap)
  ABI_FIELD(XWindowAttributes, map_state)
  ABI_FIELD(XWindowAttributes, all_event_masks)
  ABI_FIELD(XWindowAttributes, your_event_mask)
  ABI_FIELD(XWindowAttributes, override_redirect)
  ABI_FIELD(XWindowAttributes, screen)

  ABI_FIELD(XSetWindowAttributes, background_pixel)
  ABI_FIELD(XSetWindowAttributes, border_pixel)
  ABI_FIELD(XSetWindowAttributes, event_mask)
  ABI_FIELD(XSetWindowAttributes, override_redirect)
  ABI_FIELD(XSetWindowAttributes, colormap)
  ABI_FIELD(XSetWindowAttributes, cursor)

  ABI_FIELD(XVisualInfo, visual)
  ABI_FIELD(XVisualInfo, visualid)
  ABI_FIELD(XVisualInfo, depth)
  ABI_FIELD_AS(XVisualInfo, class, ABIF_CLASS)
  ABI_FIELD(XVisualInfo, red_mask)
  ABI_FIELD(XVisualInfo, bits_per_rgb)

  ABI_FIELD(XGCValues, foreground)
  ABI_FIELD(XGCValues, background)
  ABI_FIELD(XGCValues, fill_style)
  ABI_FIELD(XGCValues, tile)
  ABI_FIELD(XGCValues, clip_mask)

  ABI_FIELD(XWMHints, flags)
  ABI_FIELD(XWMHints, input)
  ABI_FIELD(XWMHints, icon_window)
  ABI_FIELD(XWMHints, window_group)

  ABI_FIELD(XSizeHints, flags)
  ABI_FIELD(XSizeHints, min_width)
  ABI_FIELD(XSizeHints, width_inc)
  ABI_FIELD(XSizeHints, base_width)
  ABI_FIELD(XSizeHints, win_gravity)

  ABI_FIELD(XTextProperty, value)
  ABI_FIELD(XTextProperty, encoding)
  ABI_FIELD(XTextProperty, format)
  ABI_FIELD(XTextProperty, nitems)

  /* Event type numbers */
  ABI_CONST(KeyPress)
  ABI_CONST(KeyRelease)
  ABI_CONST(ButtonPress)
  ABI_CONST(ButtonRelease)
  ABI_CONST(MotionNotify)
  ABI_CONST(Expose)
  ABI_CONST(DestroyNotify)
  ABI_CONST(MapNotify)
  ABI_CONST(ConfigureNotify)
  ABI_CONST(PropertyNotify)
  ABI_CONST(ClientMessage)
  ABI_CONST(GenericEvent)

  /* Event masks */
  ABI_CONST(KeyPressMask)
  ABI_CONST(KeyReleaseMask)
  ABI_CONST(ButtonPressMask)
  ABI_CONST(ButtonReleaseMask)
  ABI_CONST(PointerMotionMask)
  ABI_CONST(ExposureMask)
  ABI_CONST(StructureNotifyMask)
  ABI_CONST(PropertyChangeMask)
  ABI_CONST(FocusChangeMask)
  ABI_CONST(SubstructureNotifyMask)
  ABI_CONST(ShiftMask)
  ABI_CONST(ControlMask)
  ABI_CONST(Mod1Mask)
  ABI_CONST(Button1Mask)

  /* Predefined atoms: the numbering is part of the wire contract */
  ABI_CONST(XA_PRIMARY)
  ABI_CONST(XA_SECONDARY)
  ABI_CONST(XA_ATOM)
  ABI_CONST(XA_CARDINAL)
  ABI_CONST(XA_CUT_BUFFER0)
  ABI_CONST(XA_CUT_BUFFER7)
  ABI_CONST(XA_DRAWABLE)
  ABI_CONST(XA_PIXMAP)
  ABI_CONST(XA_RESOURCE_MANAGER)
  ABI_CONST(XA_RGB_RED_MAP)
  ABI_CONST(XA_STRING)
  ABI_CONST(XA_WINDOW)
  ABI_CONST(XA_WM_NAME)
  ABI_CONST(XA_WM_CLASS)
  ABI_CONST(XA_WM_TRANSIENT_FOR)
  ABI_CONST(XA_LAST_PREDEFINED)

  /* Core keysyms the keymap hands out */
  ABI_CONST(XK_Return)
  ABI_CONST(XK_Escape)
  ABI_CONST(XK_BackSpace)
  ABI_CONST(XK_Tab)
  ABI_CONST(XK_Left)
  ABI_CONST(XK_Up)
  ABI_CONST(XK_F1)
  ABI_CONST(XK_Delete)
  ABI_CONST(XK_Shift_L)
  ABI_CONST(XK_Control_L)
  ABI_CONST(XK_Alt_L)
  ABI_CONST(XK_a)
  ABI_CONST(XK_space)
};

const struct abi_row *ABI_FN(int *count) {
  *count = (int)(sizeof abi_rows / sizeof abi_rows[0]);
  return abi_rows;
}
