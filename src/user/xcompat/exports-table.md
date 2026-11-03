### (a) libX11.so.6 - the 129 symbols SDL requires

| symbol | status | detail |
|---|---|---|
| `XAllocClassHint` | STUB | returns `&g_class_hint_store` |
| `XAllocSizeHints` | STUB | returns `&g_size_hints_store` |
| `XAllocWMHints` | STUB | returns `&g_wm_hints_store` |
| `XAutoRepeatOff` | STUB | returns `1` |
| `XAutoRepeatOn` | STUB | returns `1` |
| `XChangePointerControl` | STUB | returns `0` |
| `XChangeProperty` | PARTIAL | accepted and dropped; no property list is kept |
| `XCheckIfEvent` | IMPL |  |
| `XClearWindow` | IMPL |  |
| `XCloseDisplay` | IMPL |  |
| `XCloseIM` | STUB | returns `0` |
| `XConvertSelection` | STUB | returns `0` |
| `XCreateBitmapFromData` | STUB | returns `0x51` |
| `XCreateColormap` | IMPL |  |
| `XCreateFontCursor` | STUB | returns `(Cursor)(0x40 + (shape & 0x3f))` |
| `XCreateFontSet` | STUB | returns `0` |
| `XCreateGC` | STUB | returns `0` |
| `XCreateIC` | STUB | returns `0` |
| `XCreateImage` | STUB | returns `0` |
| `XCreatePixmapCursor` | STUB | returns `0x41` |
| `XCreateWindow` | IMPL |  |
| `XDefineCursor` | IMPL |  |
| `XDeleteProperty` | STUB | returns `1` |
| `XDestroyIC` | STUB | returns `0` |
| `XDestroyWindow` | IMPL |  |
| `XDisplayKeycodes` | STUB | returns `0` |
| `XDisplayName` | STUB | returns `(char *)":0"` |
| `XDisplayString` | STUB | returns `(char *)":0"` |
| `XDrawRectangle` | IMPL |  |
| `XDrawString` | IMPL | the 8x8 ASCII font the native Xlib uses (`font8x8.h`, vendored from `src/user_include/graphics/font.h`): `y` is the baseline, advance 8 px, GXxor honoured through `put_px`. M3 gate renders legible text. |
| `XESetEventToWire` | STUB | returns `0` |
| `XESetWireToEvent` | STUB | returns `0` |
| `XEventsQueued` | IMPL |  |
| `XFillRectangle` | IMPL |  |
| `XFilterEvent` | STUB | returns `False` |
| `XFlush` | IMPL |  |
| `XFree` | STUB | returns `0` |
| `XFreeCursor` | STUB | returns `0` |
| `XFreeEventData` | STUB | returns `0` |
| `XFreeFont` | STUB | returns `0` |
| `XFreeFontSet` | STUB | returns `0` |
| `XFreeGC` | STUB | returns `0` |
| `XFreeModifiermap` | STUB | returns `0` |
| `XFreePixmap` | STUB | returns `0` |
| `XFreeStringList` | STUB | returns `0` |
| `XGetAtomName` | IMPL |  |
| `XGetDefault` | STUB | returns `0` |
| `XGetErrorDatabaseText` | STUB | returns `0` |
| `XGetErrorText` | STUB | returns `0` |
| `XGetEventData` | STUB | returns `False` |
| `XGetICValues` | STUB | returns `0` |
| `XGetInputFocus` | IMPL |  |
| `XGetKeyboardControl` | STUB | returns `0` |
| `XGetModifierMapping` | STUB | returns `0` |
| `XGetPointerControl` | STUB | returns `0` |
| `XGetSelectionOwner` | STUB | returns `None` |
| `XGetVisualInfo` | IMPL |  |
| `XGetWMHints` | IMPL |  |
| `XGetWMNormalHints` | IMPL |  |
| `XGetWindowAttributes` | IMPL |  |
| `XGetWindowProperty` | PARTIAL | answers None/Success; nothing round-trips except WM hints (XGetWMHints/XGetWMNormalHints) |
| `XGrabKeyboard` | IMPL |  |
| `XGrabPointer` | IMPL |  |
| `XGrabServer` | STUB | returns `0` |
| `XIconifyWindow` | IMPL |  |
| `XIfEvent` | IMPL |  |
| `XInitThreads` | STUB | returns `1` |
| `XInstallColormap` | STUB | returns `0` |
| `XInternAtom` | IMPL |  |
| `XKeycodeToKeysym` | IMPL |  |
| `XKeysymToKeycode` | IMPL |  |
| `XKeysymToString` | STUB | returns `(char *)"space"` |
| `XListPixmapFormats` | STUB | returns `0` |
| `XLoadQueryFont` | STUB | returns `0` |
| `XLookupKeysym` | IMPL |  |
| `XLookupString` | IMPL |  |
| `XMapRaised` | STUB | returns `XMapWindow(d, w)` |
| `XMatchVisualInfo` | IMPL |  |
| `XMoveWindow` | STUB | returns `0` |
| `XOpenDisplay` | IMPL |  |
| `XOpenIM` | STUB | returns `0` |
| `XPeekEvent` | IMPL |  |
| `XPending` | IMPL |  |
| `XPutImage` | STUB | returns `0` |
| `XQueryExtension` | STUB | returns `False` |
| `XQueryKeymap` | STUB | returns `0` |
| `XQueryPointer` | IMPL |  |
| `XQueryTree` | IMPL |  |
| `XRaiseWindow` | STUB | returns `0` |
| `XRefreshKeyboardMapping` | STUB | returns `0` |
| `XReparentWindow` | STUB | returns `0` |
| `XResetScreenSaver` | STUB | returns `0` |
| `XResizeWindow` | STUB | returns `0` |
| `XScreenNumberOfScreen` | STUB | returns `s ? s->scr : 0` |
| `XSelectInput` | IMPL |  |
| `XSendEvent` | STUB | returns `0` |
| `XSetErrorHandler` | STUB | returns `old` |
| `XSetForeground` | STUB | returns `0` |
| `XSetICFocus` | STUB | returns `0` |
| `XSetIOErrorHandler` | STUB | returns `0` |
| `XSetInputFocus` | PARTIAL | accepted; the desktop routes key input to the focused pixel window, there is no focus protocol |
| `XSetLocaleModifiers` | STUB | returns `(char *)""` |
| `XSetSelectionOwner` | STUB | returns `0` |
| `XSetTextProperty` | IMPL |  |
| `XSetTransientForHint` | STUB | returns `1` |
| `XSetWMHints` | IMPL |  |
| `XSetWMNormalHints` | IMPL |  |
| `XSetWMProperties` | IMPL |  |
| `XSetWMProtocols` | IMPL |  |
| `XSetWindowBackground` | IMPL |  |
| `XStoreColors` | STUB | returns `0` |
| `XStoreName` | IMPL |  |
| `XStringListToTextProperty` | STUB | returns `0` |
| `XSupportsLocale` | STUB | returns `True` |
| `XSync` | STUB | returns `XFlush(d)` |
| `XSynchronize` | IMPL |  |
| `XTextExtents` | STUB | returns `0` |
| `XTranslateCoordinates` | PARTIAL | window<->root is exact, window<->window is only approximate (there are no child windows) |
| `XUndefineCursor` | STUB | returns `0` |
| `XUngrabKeyboard` | IMPL |  |
| `XUngrabPointer` | IMPL |  |
| `XUngrabServer` | STUB | returns `0` |
| `XUninstallColormap` | STUB | returns `0` |
| `XUnloadFont` | STUB | returns `0` |
| `XUnsetICFocus` | STUB | returns `0` |
| `XVisualIDFromVisual` | STUB | returns `v ? v->id : 0` |
| `XWarpPointer` | STUB | returns `0` |
| `XWindowEvent` | IMPL |  |
| `XWithdrawWindow` | IMPL |  |

### (b) libX11.so.6 - exported beyond the required set

| symbol | status | detail |
|---|---|---|
| `BlackPixel` | STUB | returns `0x00000000L` |
| `BlackPixelOfScreen` | STUB | returns `0` |
| `DefaultColormap` | IMPL |  |
| `DefaultDepth` | STUB | returns `24` |
| `DefaultGC` | IMPL |  |
| `DefaultRootWindow` | IMPL |  |
| `DefaultScreen` | IMPL |  |
| `DefaultScreenOfDisplay` | IMPL |  |
| `DefaultVisual` | IMPL |  |
| `DisplayHeight` | IMPL |  |
| `DisplayHeightMM` | IMPL |  |
| `DisplayWidth` | IMPL |  |
| `DisplayWidthMM` | IMPL |  |
| `RootWindow` | IMPL |  |
| `ScreenCount` | STUB | returns `1` |
| `ScreenOfDisplay` | IMPL |  |
| `WhitePixel` | STUB | returns `0x00FFFFFFL` |
| `WhitePixelOfScreen` | STUB | returns `0x00FFFFFFL` |
| `XAllocColor` | STUB | returns `1` |
| `XAllocIconSize` | STUB | returns `g_icon_size_store` |
| `XAllocNamedColor` | STUB | returns `1` |
| `XAllocStandardColormap` | STUB | returns `0` |
| `XAllocTextProperty` | STUB | returns `&g_text_prop_store` |
| `XClearArea` | IMPL |  |
| `XConnectionNumber` | IMPL |  |
| `XCopyArea` | STUB | returns `0` |
| `XCreatePixmap` | STUB | returns `0x50` |
| `XCreatePixmapFromBitmapData` | STUB | returns `0x52` |
| `XCreateSimpleWindow` | IMPL |  |
| `XDefaultDepth` | STUB | returns `DefaultDepth(d, scr)` |
| `XDefaultScreen` | STUB | returns `DefaultScreen(d)` |
| `XDefaultScreenOfDisplay` | STUB | returns `DefaultScreenOfDisplay(d)` |
| `XDefaultVisual` | STUB | returns `DefaultVisual(d, scr)` |
| `XDestroyImage` | STUB | returns `0` |
| `XDestroySubwindows` | STUB | returns `0` |
| `XDisplayHeight` | STUB | returns `DisplayHeight(d, scr)` |
| `XDisplayHeightMM` | STUB | returns `DisplayHeightMM(d, scr)` |
| `XDisplayWidth` | STUB | returns `DisplayWidth(d, scr)` |
| `XDisplayWidthMM` | STUB | returns `DisplayWidthMM(d, scr)` |
| `XDrawImageString` | PARTIAL | returns `XDrawString(d, dr, gc, x, y, s, len)`: glyphs are drawn, but the background is not filled first (X11's ImageString semantics) |
| `XDrawLine` | IMPL |  |
| `XDrawPoint` | IMPL |  |
| `XESetCloseDisplay` | STUB | returns `0` |
| `XESetError` | STUB | returns `0` |
| `XFreeColormap` | STUB | returns `0` |
| `XFreeExtensionList` | STUB | returns `0` |
| `XFreeFontNames` | STUB | returns `0` |
| `XGetAtomNames` | IMPL |  |
| `XGetGeometry` | IMPL |  |
| `XGetIMValues` | STUB | returns `0` |
| `XGetKeyboardMapping` | PARTIAL | returns the shim's static 2-keysym-per-keycode map |
| `XGetKeyboardMappingSupported` | STUB | returns `True` |
| `XGetPointerMapping` | STUB | returns `1` |
| `XGetTextProperty` | IMPL |  |
| `XGetWMProtocols` | STUB | returns `1` |
| `XInternAtoms` | IMPL |  |
| `XListExtensions` | STUB | returns `0` |
| `XListFonts` | STUB | returns `0` |
| `XListProperties` | STUB | returns `0` |
| `XLowerWindow` | STUB | returns `0` |
| `XMapWindow` | IMPL |  |
| `XMoveResizeWindow` | STUB | returns `0` |
| `XNextEvent` | IMPL |  |
| `XParseColor` | STUB | returns `1` |
| `XProtocolRevision` | STUB | returns `0` |
| `XProtocolVersion` | STUB | returns `11` |
| `XQueryColor` | STUB | returns `1` |
| `XQueryColors` | STUB | returns `1` |
| `XQueryFont` | STUB | returns `0` |
| `XRecolorCursor` | STUB | returns `0` |
| `XResourceManagerString` | STUB | returns `0` |
| `XRootWindow` | STUB | returns `RootWindow(d, scr)` |
| `XScreenCount` | STUB | returns `ScreenCount(d)` |
| `XScreenResourceString` | STUB | returns `0` |
| `XSetAfterFunction` | STUB | returns `0` |
| `XSetBackground` | STUB | returns `0` |
| `XSetClassHint` | IMPL |  |
| `XSetClipRectangles` | STUB | returns `0` |
| `XSetFillStyle` | STUB | returns `0` |
| `XSetFont` | STUB | returns `0` |
| `XSetFunction` | STUB | returns `0` |
| `XSetGraphicsExposures` | STUB | returns `0` |
| `XSetICValues` | STUB | returns `0` |
| `XSetIconName` | STUB | returns `0` |
| `XSetKeyboardMapping` | STUB | returns `0` |
| `XSetLineAttributes` | STUB | returns `0` |
| `XSetModifierMapping` | STUB | returns `0` |
| `XSetPointerMapping` | STUB | returns `0` |
| `XSetWMName` | STUB | returns `XSetTextProperty(d, w, tp, XA_WM_NAME)` |
| `XSetWindowBackgroundPixmap` | STUB | returns `0` |
| `XSetWindowBorder` | STUB | returns `0` |
| `XSetWindowBorderWidth` | STUB | returns `0` |
| `XSetWindowColormap` | STUB | returns `0` |
| `XShapeCombineMask` | STUB | returns `0` |
| `XShapeQueryExtension` | STUB | returns `False` |
| `XShmAttach` | STUB | returns `0` |
| `XShmCreateImage` | STUB | returns `0` |
| `XShmDetach` | STUB | returns `0` |
| `XShmPutImage` | STUB | returns `0` |
| `XShmQueryExtension` | STUB | returns `False` |
| `XShmQueryVersion` | STUB | returns `False` |
| `XStoreColor` | STUB | returns `0` |
| `XTextPropertyToStringList` | STUB | returns `0` |
| `XTextWidth` | STUB | returns `n * 8` (the cell `XDrawString` advances) |
| `XUnmapSubwindows` | STUB | returns `0` |
| `XUnmapWindow` | PARTIAL | the protocol cannot retract a pixel window: the surface stays up and stops receiving events |
| `XVendorRelease` | STUB | returns `1` |
| `XkbGetState` | STUB | returns `0` |
| `XkbKeycodeToKeysym` | IMPL |  |
| `XkbQueryExtension` | STUB | returns `False` |
| `XkbSetDetectableAutoRepeat` | STUB | returns `True` |
| `XmbLookupString` | STUB | returns `XLookupString(ev, buf, n, ks, 0)` |
| `XmbSetWMProperties` | STUB | returns `XSetWMProperties(d, w, n ? &tp : 0, 0, argv, argc, hints, wmh, ch)` |
| `Xutf8LookupString` | STUB | returns `XLookupString(ev, buf, n, ks, 0)` |

### (c) libasound.so.2 - the 19 symbols FMOD requires

| symbol | behaviour |
|---|---|
| `snd_device_name_free_hint` | returns -ENODEV |
| `snd_device_name_get_hint` | returns -ENODEV |
| `snd_device_name_hint` | returns 0 with a NULL-*terminated* empty array |
| `snd_pcm_close` | returns 0 (idempotent) |
| `snd_pcm_hw_params` | returns -ENODEV |
| `snd_pcm_hw_params_any` | zeroes the block, returns 0 |
| `snd_pcm_hw_params_set_access` | returns -ENODEV |
| `snd_pcm_hw_params_set_buffer_size_near` | returns -ENODEV |
| `snd_pcm_hw_params_set_channels` | returns -ENODEV |
| `snd_pcm_hw_params_set_format` | returns -ENODEV |
| `snd_pcm_hw_params_set_period_size_near` | returns -ENODEV |
| `snd_pcm_hw_params_set_rate` | returns -ENODEV |
| `snd_pcm_hw_params_sizeof` | returns 256 (the opaque params block size) |
| `snd_pcm_nonblock` | returns -ENODEV |
| `snd_pcm_open` | returns -ENODEV, handle NULL |
| `snd_pcm_prepare` | returns -ENODEV |
| `snd_pcm_readi` | returns -ENODEV |
| `snd_pcm_writei` | returns -ENODEV |
| `snd_strerror` | static text, `-19` -> "No such device" |

