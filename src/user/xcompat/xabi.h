/*
 * xabi.h - the ABI-exact Xlib surface that libX11.so.6 must export.
 *
 * This header is NOT src/user/x11/include/X11/Xlib.h.  That one is the
 * native apps' fake Xlib: it defines the subset HobbyOS programs use and
 * lays XEvent out in its own (smaller) shape.  A compat DSO is different:
 * SDL2 is compiled against the REAL Xlib headers, allocates XEvent on its
 * stack and reads fields at the real offsets.  So every structure here is
 * byte-for-byte the X11R7 layout (x86-64: XEvent = 192 B, XKeyEvent = 96 B,
 * XWindowAttributes = 136 B, ...).
 *
 * That claim is not from memory: src/host/xcompat_test.c includes this
 * header and <X11/Xlib.h> in two translation units and compares sizeof()
 * and offsetof() for every field.  A layout drift fails `make host_tests`.
 *
 * Only what the compat track needs is declared here; anything missing is
 * declared in xcore.h's own vocabulary instead of reaching for real Xlib.
 */
#ifndef HOBBYOS_XCOMPAT_XABI_H
#define HOBBYOS_XCOMPAT_XABI_H

#include <stdint.h>
#include <stddef.h>

/* ---- base types (X11R7, LP64) --------------------------------------- */

typedef int Bool;
typedef int Status;
typedef unsigned long XID;
typedef XID Window;
typedef XID Drawable;
typedef XID Pixmap;
typedef XID Cursor;
typedef XID Font;
typedef XID Colormap;
typedef XID GContext;
typedef XID KeySym;
typedef unsigned char KeyCode;
typedef unsigned long Atom;
typedef unsigned long Time;
typedef unsigned long VisualID;
typedef unsigned long Mask;

typedef struct _XDisplay Display;
typedef struct _XScreen Screen;

/* Visual: the REAL Xlib layout, defined here rather than privately in
 * libx11.c because it is client-visible ABI.  XVisualInfo.visual points at
 * one and a client reads vinfo.visual->visualid / ->class through it.  Xlib
 * puts XExtData *ext_data first, so visualid lands at offset 8; a private
 * struct starting with VisualID would answer garbage to that read (the SDL
 * window probe prints the value it gets back). */
struct _XVisual {
  void *ext_data;                 /*   0   XExtData *            */
  VisualID visualid;              /*   8                         */
  int c_class;                    /*  16   (`class` in C)        */
  unsigned long red_mask;         /*  24                         */
  unsigned long green_mask;       /*  32                         */
  unsigned long blue_mask;        /*  40                         */
  int bits_per_rgb;               /*  48                         */
  int map_entries;                /*  52                         */
};
typedef struct _XVisual Visual;
typedef struct _XGC *GC;
typedef struct _XImage XImage;
typedef struct _XErrorHandlerStruct *XErrorHandler;
typedef int (*XIOErrorHandler)(Display *);
typedef int (*XErrorHandlerFn)(Display *, void *);
typedef struct _XIC *XIC;
typedef struct _XIM *XIM;
typedef struct _XOM *XOM;
typedef struct _XFontStruct XFontStruct;
typedef struct _XFontSetRec *XFontSet;

#define True 1
#define False 0

/* Grab return codes (X11: 0..4) and pointer/keyboard modes. */
#define GrabSuccess 0
#define AlreadyGrabbed 1
#define GrabInvalidTime 2
#define GrabNotViewable 3
#define GrabFrozen 4
#define GrabModeSync 0
#define GrabModeAsync 1

/* A rectangle in X11's own layout (signed origin, unsigned extent). */
typedef struct {
  short x, y;
  unsigned short width, height;
} XRectangle;

/* XColor: pixel(8) then three 16-bit channels, flags, pad. */
typedef struct {
  unsigned long pixel;
  unsigned short red, green, blue;
  char flags;
  char pad;
} XColor;
#define None 0L
#define CopyFromParent 0L
#define PointerRoot 1L
#define ParentRelative 1L
#define AnyPropertyType 0L
#define AllPlanes ((unsigned long)~0L)

/* ---- event types ----------------------------------------------------- */
#define KeyPress 2
#define KeyRelease 3
#define ButtonPress 4
#define ButtonRelease 5
#define MotionNotify 6
#define EnterNotify 7
#define LeaveNotify 8
#define FocusIn 9
#define FocusOut 10
#define KeymapNotify 11
#define Expose 12
#define GraphicsExpose 13
#define NoExpose 14
#define VisibilityNotify 15
#define CreateNotify 16
#define DestroyNotify 17
#define UnmapNotify 18
#define MapNotify 19
#define MapRequest 20
#define ReparentNotify 21
#define ConfigureNotify 22
#define ConfigureRequest 23
#define GravityNotify 24
#define ResizeRequest 25
#define CirculateNotify 26
#define CirculateRequest 27
#define PropertyNotify 28
#define SelectionClear 29
#define SelectionRequest 30
#define SelectionNotify 31
#define ColormapNotify 32
#define ClientMessage 33
#define MappingNotify 34
#define GenericEvent 35

/* ---- event masks ----------------------------------------------------- */
#define NoEventMask 0L
#define KeyPressMask (1L << 0)
#define KeyReleaseMask (1L << 1)
#define ButtonPressMask (1L << 2)
#define ButtonReleaseMask (1L << 3)
#define EnterWindowMask (1L << 4)
#define LeaveWindowMask (1L << 5)
#define PointerMotionMask (1L << 6)
#define PointerMotionHintMask (1L << 7)
#define Button1MotionMask (1L << 8)
#define Button2MotionMask (1L << 9)
#define Button3MotionMask (1L << 10)
#define Button4MotionMask (1L << 11)
#define Button5MotionMask (1L << 12)
#define ButtonMotionMask (1L << 13)
#define KeymapStateMask (1L << 14)
#define ExposureMask (1L << 15)
#define VisibilityChangeMask (1L << 16)
#define StructureNotifyMask (1L << 17)
#define ResizeRedirectMask (1L << 18)
#define SubstructureNotifyMask (1L << 19)
#define SubstructureRedirectMask (1L << 20)
#define FocusChangeMask (1L << 21)
#define PropertyChangeMask (1L << 22)
#define ColormapChangeMask (1L << 23)
#define OwnerGrabButtonMask (1L << 24)

/* ---- modifier / button state ---------------------------------------- */
#define ShiftMask (1 << 0)
#define LockMask (1 << 1)
#define ControlMask (1 << 2)
#define Mod1Mask (1 << 3)
#define Mod2Mask (1 << 4)
#define Mod3Mask (1 << 5)
#define Mod4Mask (1 << 6)
#define Mod5Mask (1 << 7)
#define Button1Mask (1 << 8)
#define Button2Mask (1 << 9)
#define Button3Mask (1 << 10)
#define Button4Mask (1 << 11)
#define Button5Mask (1 << 12)
#define AnyModifier (1 << 15)

/* ---- visual classes (XVisualInfo.class / Visual.class) ----------------
 * These are NOT the window classes below: X11 numbers visuals
 * StaticGray..DirectColor from 0, and Mesa's xmesa_convert_from_x_visual_type
 * switches on them. */
#define StaticGray 0
#define GrayScale 1
#define StaticColor 2
#define PseudoColor 3
#define TrueColor 4
#define DirectColor 5

/* ---- XGetVisualInfo masks (Xlib.h) ---------------------------------- */
#define VisualNoMask           0x0
#define VisualIDMask           0x1
#define VisualScreenMask       0x2
#define VisualDepthMask        0x4
#define VisualClassMask        0x8
#define VisualRedMaskMask      0x10
#define VisualGreenMaskMask    0x20
#define VisualBlueMaskMask     0x40
#define VisualColormapSizeMask 0x80
#define VisualBitsPerRGBMask   0x100
#define VisualAllMask          0x1FF

/* ---- window classes / map states ------------------------------------ */
#define InputOutput 1
#define InputOnly 2
#define IsUnmapped 0
#define IsUnviewable 1
#define IsViewable 2
#define NotUseful 0
#define WhenMapped 1
#define Always 2
#define Unsorted 0
#define YSorted 1
#define YXSorted 2
#define YXBanded 3
#define PropModeReplace 0
#define PropModePrepend 1
#define PropModeAppend  2

/* PropertyNotify state (X.h) */
#define PropertyNewValue 0
#define PropertyDelete   1
#define PropModePrepend 1
#define PropModeAppend 2
#define Success 0
#define BadRequest 1
#define BadValue 2
#define BadWindow 3
#define BadPixmap 4
#define BadAtom 5
#define BadDrawable 9
#define BadMatch 8
#define BadAlloc 11
#define BadGC 13

/* ---- graphics context ------------------------------------------------- */
#define GCFunction (1L << 0)
#define GCPlaneMask (1L << 1)
#define GCForeground (1L << 2)
#define GCBackground (1L << 3)
#define GCLineWidth (1L << 4)
#define GCLineStyle (1L << 5)
#define GCCapStyle (1L << 6)
#define GCJoinStyle (1L << 7)
#define GCFillStyle (1L << 8)
#define GCFillRule (1L << 9)
#define GCTile (1L << 10)
#define GCStipple (1L << 11)
#define GCTileStipXOrigin (1L << 12)
#define GCTileStipYOrigin (1L << 13)
#define GCFont (1L << 14)
#define GCSubwindowMode (1L << 15)
#define GCGraphicsExposures (1L << 16)
#define GCClipXOrigin (1L << 17)
#define GCClipYOrigin (1L << 18)
#define GCClipMask (1L << 19)
#define GCDashOffset (1L << 20)
#define GCDashList (1L << 21)
#define GCArcMode (1L << 22)

#define GXcopy 3
#define GXxor 6
#define LineSolid 0
#define FillSolid 0
#define FillTiled 1
#define FillStippled 2
#define FillOpaqueStippled 3
#define CoordModeOrigin 0
#define CoordModePrevious 1

/* ---- atoms ----------------------------------------------------------- */
#define XA_PRIMARY 1L
#define XA_SECONDARY 2L
#define XA_ARC 3L
#define XA_ATOM 4L
#define XA_BITMAP 5L
#define XA_CARDINAL 6L
#define XA_COLORMAP 7L
#define XA_CURSOR 8L
#define XA_CUT_BUFFER0 9L
#define XA_CUT_BUFFER1 10L
#define XA_CUT_BUFFER2 11L
#define XA_CUT_BUFFER3 12L
#define XA_CUT_BUFFER4 13L
#define XA_CUT_BUFFER5 14L
#define XA_CUT_BUFFER6 15L
#define XA_CUT_BUFFER7 16L
#define XA_DRAWABLE 17L
#define XA_FONT 18L
#define XA_INTEGER 19L
#define XA_PIXMAP 20L
#define XA_POINT 21L
#define XA_RECTANGLE 22L
#define XA_RESOURCE_MANAGER 23L
#define XA_RGB_COLOR_MAP 24L
#define XA_RGB_BEST_MAP 25L
#define XA_RGB_BLUE_MAP 26L
#define XA_RGB_DEFAULT_MAP 27L
#define XA_RGB_GRAY_MAP 28L
#define XA_RGB_GREEN_MAP 29L
#define XA_RGB_RED_MAP 30L
#define XA_STRING 31L
#define XA_VISUALID 32L
#define XA_WINDOW 33L
#define XA_WM_COMMAND 34L
#define XA_WM_HINTS 35L
#define XA_WM_CLIENT_MACHINE 36L
#define XA_WM_ICON_NAME 37L
#define XA_WM_ICON_SIZE 38L
#define XA_WM_NAME 39L
#define XA_WM_NORMAL_HINTS 40L
#define XA_WM_SIZE_HINTS 41L
#define XA_WM_ZOOM_HINTS 42L
#define XA_MIN_SPACE 43L
#define XA_NORM_SPACE 44L
#define XA_MAX_SPACE 45L
#define XA_END_SPACE 46L
#define XA_SUPERSCRIPT_X 47L
#define XA_SUPERSCRIPT_Y 48L
#define XA_SUBSCRIPT_X 49L
#define XA_SUBSCRIPT_Y 50L
#define XA_UNDERLINE_POSITION 51L
#define XA_UNDERLINE_THICKNESS 52L
#define XA_STRIKEOUT_ASCENT 53L
#define XA_STRIKEOUT_DESCENT 54L
#define XA_ITALIC_ANGLE 55L
#define XA_X_HEIGHT 56L
#define XA_QUAD_WIDTH 57L
#define XA_WEIGHT 58L
#define XA_POINT_SIZE 59L
#define XA_RESOLUTION 60L
#define XA_COPYRIGHT 61L
#define XA_NOTICE 62L
#define XA_FONT_NAME 63L
#define XA_FAMILY_NAME 64L
#define XA_FULL_NAME 65L
#define XA_CAP_HEIGHT 66L
#define XA_WM_CLASS 67L
#define XA_WM_TRANSIENT_FOR 68L
#define XA_LAST_PREDEFINED 68L

/* ---- keysyms used by the shim's keymap ------------------------------- */
#define XK_VoidSymbol 0xffffffL
#define XK_BackSpace 0xff08L
#define XK_Tab 0xff09L
#define XK_Linefeed 0xff0aL
#define XK_Clear 0xff0bL
#define XK_Return 0xff0dL
#define XK_Pause 0xff13L
#define XK_Scroll_Lock 0xff14L
#define XK_Sys_Req 0xff15L
#define XK_Escape 0xff1bL
#define XK_Delete 0xffffL
#define XK_Home 0xff50L
#define XK_Left 0xff51L
#define XK_Up 0xff52L
#define XK_Right 0xff53L
#define XK_Down 0xff54L
#define XK_Prior 0xff55L
#define XK_Page_Up 0xff55L
#define XK_Next 0xff56L
#define XK_Page_Down 0xff56L
#define XK_End 0xff57L
#define XK_Begin 0xff58L
#define XK_Select 0xff60L
#define XK_Print 0xff61L
#define XK_Insert 0xff63L
#define XK_Menu 0xff67L
#define XK_Num_Lock 0xff7fL
#define XK_KP_Enter 0xff8dL
#define XK_Shift_L 0xffe1L
#define XK_Shift_R 0xffe2L
#define XK_Control_L 0xffe3L
#define XK_Control_R 0xffe4L
#define XK_Caps_Lock 0xffe5L
#define XK_Shift_Lock 0xffe6L
#define XK_Meta_L 0xffe7L
#define XK_Meta_R 0xffe8L
#define XK_Alt_L 0xffe9L
#define XK_Alt_R 0xffeaL
#define XK_Super_L 0xffebL
#define XK_Super_R 0xffecL
#define XK_F1 0xffbeL
#define XK_F2 0xffbfL
#define XK_F3 0xffc0L
#define XK_F4 0xffc1L
#define XK_F5 0xffc2L
#define XK_F6 0xffc3L
#define XK_F7 0xffc4L
#define XK_F8 0xffc5L
#define XK_F9 0xffc6L
#define XK_F10 0xffc7L
#define XK_F11 0xffc8L
#define XK_F12 0xffc9L
#define XK_space 0x020L
#define XK_0 0x030L
#define XK_9 0x039L
#define XK_A 0x041L
#define XK_Z 0x05aL
#define XK_a 0x061L
#define XK_z 0x07aL
#define XK_ISO_Left_Tab 0xfe20L

/* ---- structures ------------------------------------------------------ */

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  int x, y;
  int width, height;
  int count;
} XExposeEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Drawable drawable;
  int x, y;
  int width, height;
  int count;
  int major_code;
  int minor_code;
} XGraphicsExposeEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Drawable drawable;
  int major_code;
  int minor_code;
} XNoExposeEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  int state;
} XVisibilityEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window parent;
  Window window;
  int x, y;
  int width, height;
  int border_width;
  Bool override_redirect;
} XCreateWindowEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
} XDestroyWindowEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
  Bool from_configure;
} XUnmapEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
  Bool override_redirect;
} XMapEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window parent;
  Window window;
} XMapRequestEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
  Window parent;
  int x, y;
  Bool override_redirect;
} XReparentEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
  int x, y;
  int width, height;
  int border_width;
  Window above;
  Bool override_redirect;
} XConfigureEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
  int x, y;
} XGravityEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  int width, height;
} XResizeRequestEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window parent;
  Window window;
  int x, y;
  int width, height;
  int border_width;
  Window above;
  int detail;
  unsigned long value_mask;
} XConfigureRequestEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window event;
  Window window;
  int place;
} XCirculateEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window parent;
  Window window;
  int place;
} XCirculateRequestEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Atom atom;
  Time time;
  int state;
} XPropertyEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Atom selection;
  Time time;
} XSelectionClearEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window owner;
  Window requestor;
  Atom selection;
  Atom target;
  Atom property;
  Time time;
} XSelectionRequestEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window requestor;
  Atom selection;
  Atom target;
  Atom property;
  Time time;
} XSelectionEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Colormap colormap;
  Bool new_;
  int state;
} XColormapEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Atom message_type;
  int format;
  union {
    char b[20];
    short s[10];
    long l[5];
  } data;
} XClientMessageEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  int request;
  int first_keycode;
  int count;
} XMappingEvent;

typedef struct {
  int type;
  Display *display;
  XID resourceid;
  unsigned long serial;
  unsigned char error_code;
  unsigned char request_code;
  unsigned char minor_code;
} XErrorEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
} XAnyEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  int extension;
  int evtype;
} XGenericEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  int extension;
  int evtype;
  unsigned int cookie;
  void *data;
} XGenericEventCookie;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  unsigned int state;
  unsigned int keycode;
  Bool same_screen;
} XKeyEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  unsigned int state;
  unsigned int button;
  Bool same_screen;
} XButtonEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  unsigned int state;
  char is_hint;
  Bool same_screen;
} XMotionEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root;
  Window subwindow;
  Time time;
  int x, y;
  int x_root, y_root;
  int mode;
  int detail;
  Window focus;
  int state;
  Bool same_screen;
} XCrossingEvent;

typedef union _XEvent {
  int type;
  XAnyEvent xany;
  XKeyEvent xkey;
  XButtonEvent xbutton;
  XMotionEvent xmotion;
  XCrossingEvent xcrossing;
  XExposeEvent xexpose;
  XGraphicsExposeEvent xgraphicsexpose;
  XNoExposeEvent xnoexpose;
  XVisibilityEvent xvisibility;
  XCreateWindowEvent xcreatewindow;
  XDestroyWindowEvent xdestroywindow;
  XUnmapEvent xunmap;
  XMapEvent xmap;
  XMapRequestEvent xmaprequest;
  XReparentEvent xreparent;
  XConfigureEvent xconfigure;
  XGravityEvent xgravity;
  XResizeRequestEvent xresizerequest;
  XConfigureRequestEvent xconfigurerequest;
  XCirculateEvent xcirculate;
  XCirculateRequestEvent xcirculaterequest;
  XPropertyEvent xproperty;
  XSelectionClearEvent xselectionclear;
  XSelectionRequestEvent xselectionrequest;
  XSelectionEvent xselection;
  XColormapEvent xcolormap;
  XClientMessageEvent xclient;
  XMappingEvent xmapping;
  XErrorEvent xerror;
  XGenericEvent xgeneric;
  XGenericEventCookie xcookie;
  long pad[24];
} XEvent;

typedef struct {
  Pixmap background_pixmap;
  unsigned long background_pixel;
  Pixmap border_pixmap;
  unsigned long border_pixel;
  int bit_gravity;
  int win_gravity;
  int backing_store;
  unsigned long backing_planes;
  unsigned long backing_pixel;
  Bool save_under;
  long event_mask;
  long do_not_propagate_mask;
  Bool override_redirect;
  Colormap colormap;
  Cursor cursor;
} XSetWindowAttributes;

typedef struct {
  int x, y;
  int width, height;
  int border_width;
  int depth;
  Visual *visual;
  Window root;
  int c_class;
  int bit_gravity;
  int win_gravity;
  int backing_store;
  unsigned long backing_planes;
  unsigned long backing_pixel;
  Bool save_under;
  Colormap colormap;
  Bool map_installed;
  int map_state;
  long all_event_masks;
  long your_event_mask;
  long do_not_propagate_mask;
  Bool override_redirect;
  Screen *screen;
} XWindowAttributes;

typedef struct {
  Visual *visual;
  VisualID visualid;
  int screen;
  int depth;
  int c_class;
  unsigned long red_mask, green_mask, blue_mask;
  int colormap_size;
  int bits_per_rgb;
} XVisualInfo;

typedef struct {
  int function;
  unsigned long plane_mask;
  unsigned long foreground;
  unsigned long background;
  int line_width;
  int line_style;
  int cap_style;
  int join_style;
  int fill_style;
  int fill_rule;
  int arc_mode;
  Pixmap tile;
  Pixmap stipple;
  int ts_x_origin;
  int ts_y_origin;
  Font font;
  int subwindow_mode;
  Bool graphics_exposures;
  int clip_x_origin;
  int clip_y_origin;
  Pixmap clip_mask;
  int dash_offset;
  char dashes;
} XGCValues;

typedef struct {
  long flags;
  Bool input;
  int initial_state;
  Pixmap icon_pixmap;
  Window icon_window;
  int icon_x, icon_y;
  Pixmap icon_mask;
  XID window_group;
} XWMHints;

typedef struct {
  long flags;
  int x, y;
  int width, height;
  int min_width, min_height;
  int max_width, max_height;
  int width_inc, height_inc;
  struct {
    int x;
    int y;
  } min_aspect, max_aspect;
  int base_width, base_height;
  int win_gravity;
} XSizeHints;

typedef struct {
  char *res_name;
  char *res_class;
} XClassHint;

typedef struct {
  unsigned char *value;
  Atom encoding;
  int format;
  unsigned long nitems;
} XTextProperty;

typedef struct {
  int depth;
  int bits_per_pixel;
  int scanline_pad;
} XPixmapFormatValues;

/* ---- the Screen a client's macros dereference ------------------------
 * `Screen` in <X11/Xlib.h> is an anonymous struct with exactly this layout
 * (root at 0x10, width at 0x18, root_visual at 0x40, white_pixel at 0x58;
 * sizeof 128 on x86-64).  Clients reach it through ScreenOfDisplay(), and
 * every pixel/dimension macro in Xlib.h reads it. */
typedef struct _XScreen {
  void *ext_data;                 /*   0  XExtData *          */
  Display *display;               /*   8  back pointer        */
  Window root;                    /*  16                      */
  int width, height;              /*  24, 28                  */
  int mwidth, mheight;            /*  32, 36  millimetres     */
  int ndepths;                    /*  40                      */
  void *depths;                   /*  48  Depth * (opaque)    */
  int root_depth;                 /*  56                      */
  Visual *root_visual;            /*  64                      */
  GC default_gc;                  /*  72                      */
  Colormap cmap;                  /*  80                      */
  unsigned long white_pixel;      /*  88                      */
  unsigned long black_pixel;      /*  96                      */
  int max_maps, min_maps;         /* 104, 108                 */
  int backing_store;              /* 112                      */
  Bool save_unders;               /* 116                      */
  long root_input_mask;           /* 120                      */
} Screen;

/* ---- the Display / Screen objects a CLIENT's macros walk --------------
 *
 * Xlib's own headers make a client read the display object directly:
 *
 *     #define DefaultScreen(dpy)  (((_XPrivDisplay)(dpy))->default_screen)
 *     #define ScreenOfDisplay(dpy,scr) (&((dpy)->screens[scr]))
 *     #define RootWindow(dpy,scr)  (ScreenOfDisplay(dpy,scr)->root)
 *     #define DisplayWidth(dpy,scr)(ScreenOfDisplay(dpy,scr)->width)
 *     #define BlackPixel(dpy,scr)  (ScreenOfDisplay(dpy,scr)->black_pixel)
 *
 * so a compat libX11 cannot hand back a private struct: the object it returns
 * from XOpenDisplay must START with the real `_XPrivDisplay` layout, and its
 * `screens` pointer must address a real `Screen` array.  Measured on the M3
 * gate: a client compiled against the real headers faulted at VA=0x10 in
 * `DefaultScreen(dpy)+screens` before it ever made a call, because our
 * display had no such fields.
 *
 * `x_abi_display_prefix` is the public prefix of Xlib's Display (everything
 * up to `xdefaults`; Xlib's own comment says "there is more to this structure,
 * but it is private to Xlib").  The DSO embeds it as the first member of its
 * display and hangs its private state off the end.  Offsets are pinned by
 * `_Static_assert` below and by the host ABI differential
 * (src/host/xcompat_abi_probe.h).
 *
 * Fields below `screens` are opaque pointers (`Depth *`, `ScreenFormat *`,
 * `struct _XPrivate *`, `XrmHashBucketRec *`): a client never dereferences
 * them through these offsets, only stores the pointers. */
typedef struct {
  void *ext_data;                 /*   0  XExtData *           */
  void *private1;                 /*   8  struct _XPrivate *   */
  int fd;                         /*  16  network socket       */
  int private2;                   /*  20                       */
  int proto_major_version;        /*  24                       */
  int proto_minor_version;        /*  28                       */
  char *vendor;                   /*  32                       */
  XID private3;                   /*  40                       */
  XID private4;                   /*  48                       */
  XID private5;                   /*  56                       */
  int private6;                   /*  64                       */
  void *resource_alloc;           /*  72  XID (*)(Display*)    */
  int byte_order;                 /*  80                       */
  int bitmap_unit;                /*  84                       */
  int bitmap_pad;                 /*  88                       */
  int bitmap_bit_order;           /*  92                       */
  int nformats;                   /*  96                       */
  void *pixmap_format;            /* 104  ScreenFormat *       */
  int private8;                   /* 112                       */
  int release;                    /* 116                       */
  void *private9;                 /* 120                       */
  void *private10;                /* 128                       */
  int qlen;                       /* 136                       */
  unsigned long last_request_read;/* 144                       */
  unsigned long request;          /* 152                       */
  void *private11;                /* 160                       */
  void *private12;                /* 168                       */
  void *private13;                /* 176                       */
  void *private14;                /* 184                       */
  unsigned max_request_size;      /* 192                       */
  void *db;                       /* 200  XrmHashBucketRec *   */
  void *private15;                /* 208                       */
  char *display_name;             /* 216                       */
  int default_screen;             /* 224  0xe0                  */
  int nscreens;                   /* 228  0xe4                  */
  Screen *screens;                /* 232  0xe8                  */
  unsigned long motion_buffer;    /* 240                       */
  unsigned long private16;        /* 248                       */
  int min_keycode;                /* 256                       */
  int max_keycode;                /* 260                       */
  void *private17;                /* 264                       */
  void *private18;                /* 272                       */
  int private19;                  /* 280                       */
  char *xdefaults;                /* 288                       */
} x_abi_display_prefix;           /* sizeof 296 */

_Static_assert(sizeof(x_abi_display_prefix) == 296, "Display prefix size");
_Static_assert(offsetof(x_abi_display_prefix, fd) == 0x10, "Display.fd");
_Static_assert(offsetof(x_abi_display_prefix, vendor) == 0x20, "Display.vendor");
_Static_assert(offsetof(x_abi_display_prefix, release) == 0x74, "Display.release");
_Static_assert(offsetof(x_abi_display_prefix, display_name) == 0xd8,
               "Display.display_name");
_Static_assert(offsetof(x_abi_display_prefix, default_screen) == 0xe0,
               "Display.default_screen");
_Static_assert(offsetof(x_abi_display_prefix, screens) == 0xe8, "Display.screens");
_Static_assert(sizeof(Screen) == 128, "Screen size");
_Static_assert(offsetof(Screen, root) == 0x10, "Screen.root");
_Static_assert(offsetof(Screen, width) == 0x18, "Screen.width");
_Static_assert(offsetof(Screen, root_visual) == 0x40, "Screen.root_visual");
_Static_assert(offsetof(Screen, default_gc) == 0x48, "Screen.default_gc");
_Static_assert(offsetof(Screen, white_pixel) == 0x58, "Screen.white_pixel");
_Static_assert(offsetof(Screen, black_pixel) == 0x60, "Screen.black_pixel");

typedef struct {
  int screen;
  int depth;
  int c_class;
  int visualid;
} XFakeVisualInfoQuery;

/* ---- the handful of functions the compat track implements ----------- */
/* (the full exported set is regenerated into EXPORTS.md; this list is the
 *  compile-time contract libx11.c is checked against) */

#endif
