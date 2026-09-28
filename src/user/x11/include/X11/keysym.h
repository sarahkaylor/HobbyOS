/*
 * keysym.h - the key symbols the HobbyOS X11 library can deliver.
 *
 * Values match X11's keysymdef.h, so ported code that compares against
 * XK_Return and friends keeps working.  The library's XLookupString()
 * produces these from the bytes the desktop forwards: printable keys
 * arrive as their ASCII byte, special keys as the escape sequences the
 * desktop sends (ESC [ A/B/C/D arrows, ESC [ H/F home/end, ESC [ n ~
 * for insert/delete/pgup/pgdn/F1-F12).
 */
#ifndef HOBBYOS_X11_KEYSYM_H
#define HOBBYOS_X11_KEYSYM_H

#define XK_VoidSymbol 0xFFFFFF
#define XK_BackSpace  0xFF08
#define XK_Tab        0xFF09
#define XK_Return     0xFF0D
#define XK_Escape     0xFF1B
#define XK_BackTab    0xFE20
#define XK_Delete     0xFFFF
#define XK_Insert     0xFF63
#define XK_Home       0xFF50
#define XK_End        0xFF57
#define XK_Page_Up    0xFF55
#define XK_Page_Down  0xFF56
#define XK_Left       0xFF51
#define XK_Up         0xFF52
#define XK_Right      0xFF53
#define XK_Down       0xFF54

#define XK_F1         0xFFBE
#define XK_F2         0xFFBF
#define XK_F3         0xFFC0
#define XK_F4         0xFFC1
#define XK_F5         0xFFC2
#define XK_F6         0xFFC3
#define XK_F7         0xFFC4
#define XK_F8         0xFFC5
#define XK_F9         0xFFC6
#define XK_F10        0xFFC7
#define XK_F11        0xFFC8
#define XK_F12        0xFFC9

#define XK_space      0x0020
#define XK_apostrophe 0x0027
#define XK_asterisk   0x002A
#define XK_plus       0x002B
#define XK_comma      0x002C
#define XK_minus      0x002D
#define XK_period     0x002E
#define XK_slash      0x002F
#define XK_0          0x0030
#define XK_1          0x0031
#define XK_2          0x0032
#define XK_3          0x0033
#define XK_4          0x0034
#define XK_5          0x0035
#define XK_6          0x0036
#define XK_7          0x0037
#define XK_8          0x0038
#define XK_9          0x0039
#define XK_equal      0x003D
#define XK_A          0x0041
#define XK_a          0x0061

#endif
