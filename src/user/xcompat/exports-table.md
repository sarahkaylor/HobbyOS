### (a) libX11.so.6 - the 129 symbols SDL requires

| symbol | status | detail |
|---|---|---|
| `XAllocClassHint` | STUB | returns `&g_class_hint_store` |
| `XAllocSizeHints` | STUB | returns `&g_size_hints_store` |
| `XAllocWMHints` | STUB | returns `&g_wm_hints_store` |
| `XAutoRepeatOff` | STUB | returns `1` |
| `XAutoRepeatOn` | STUB | returns `1` |
| `XChangePointerControl` | STUB | returns `0` |
| `XChangeProperty` | IMPL | stores name/type/format/data in a 12-slot property list (`_NET_WM_NAME`, `_NET_WM_PID`, `WM_PROTOCOLS`, `WM_CLASS` all round-trip); a title written this way also drives the desktop frame (`ESC ] T`). |
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
| `XDeleteProperty` | IMPL |  |
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
| `XGetWindowProperty` | IMPL | reads the property list back (malloc'd copy, as Xlib returns); only the first 512 bytes of a property are retained. |
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
| `XPutImage` | IMPL |  |
| `XQueryExtension` | STUB | returns `False` |
| `XQueryKeymap` | STUB | returns `0` |
| `XQueryPointer` | IMPL |  |
| `XQueryTree` | IMPL |  |
| `XRaiseWindow` | STUB | returns `0` |
| `XRefreshKeyboardMapping` | STUB | returns `0` |
| `XReparentWindow` | STUB | returns `0` |
| `XResetScreenSaver` | STUB | returns `0` |
| `XResizeWindow` | STUB | returns `0` |
| `XScreenNumberOfScreen` | STUB | returns `0` |
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
| `XSetWMProtocols` | IMPL | records the list, stores it as WM_PROTOCOLS/ATOM/32 and arms the WM_DELETE_WINDOW path: the desktop's in-band `ESC [ D ~` close becomes the ClientMessage SDL reads (see XNextEvent). |
| `XSetWindowBackground` | IMPL |  |
| `XStoreColors` | STUB | returns `0` |
| `XStoreName` | IMPL |  |
| `XStringListToTextProperty` | STUB | returns `0` |
| `XSupportsLocale` | STUB | returns `True` |
| `XSync` | IMPL |  |
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
| `XVisualIDFromVisual` | STUB | returns `v ? v->visualid : 0` |
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
| `XDrawImageString` | STUB | returns `XDrawString(d, dr, gc, x, y, s, len)` |
| `XDrawLine` | IMPL |  |
| `XDrawPoint` | IMPL |  |
| `XDrawString16` | STUB | returns `0` |
| `XESetCloseDisplay` | STUB | returns `0` |
| `XESetError` | STUB | returns `0` |
| `XFreeColormap` | STUB | returns `0` |
| `XFreeExtensionList` | STUB | returns `0` |
| `XFreeFontInfo` | STUB | returns `0` |
| `XFreeFontNames` | STUB | returns `0` |
| `XGetAtomNames` | IMPL |  |
| `XGetGeometry` | IMPL |  |
| `XGetIMValues` | STUB | returns `0` |
| `XGetImage` | STUB | returns `0` |
| `XGetKeyboardMapping` | PARTIAL | returns the shim's static 2-keysym-per-keycode map |
| `XGetKeyboardMappingSupported` | STUB | returns `True` |
| `XGetPointerMapping` | STUB | returns `1` |
| `XGetTextProperty` | IMPL |  |
| `XGetWMProtocols` | STUB | returns `1` |
| `XInternAtoms` | IMPL |  |
| `XListExtensions` | STUB | returns `0` |
| `XListFonts` | STUB | returns `0` |
| `XListProperties` | IMPL |  |
| `XLowerWindow` | STUB | returns `0` |
| `XMapWindow` | IMPL |  |
| `XMissingExtension` | STUB | returns `0` |
| `XMoveResizeWindow` | STUB | returns `0` |
| `XNextEvent` | IMPL | polls the desktop pipe, drains queued events, and turns the desktop's close into a WM_PROTOCOLS/WM_DELETE_WINDOW ClientMessage when the client registered one via XSetWMProtocols; a client that never did keeps the old exit-the-process close. |
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
| `XTextWidth` | STUB | returns `n * 8` |
| `XUnmapSubwindows` | STUB | returns `0` |
| `XUnmapWindow` | PARTIAL | the protocol cannot retract a pixel window: the surface stays up and stops receiving events |
| `XVendorRelease` | STUB | returns `1` |
| `XkbGetState` | STUB | returns `0` |
| `XkbKeycodeToKeysym` | IMPL |  |
| `XkbQueryExtension` | STUB | returns `False` |
| `XkbSetDetectableAutoRepeat` | STUB | returns `True` |
| `XmbLookupString` | STUB | returns `XLookupString(ev, buf, n, ks, 0)` |
| `XmbSetWMProperties` | STUB | returns `XSetWMProperties(d, w, n ? &tp : 0, 0, argv, argc, hints, wmh, ch)` |
| `XmbTextListToTextProperty` | STUB | returns `-1` |
| `Xutf8LookupString` | STUB | returns `XLookupString(ev, buf, n, ks, 0)` |
| `_XEatData` | IMPL |  |
| `_XFlush` | IMPL |  |
| `_XFlushGCCache` | STUB | returns `0` |
| `_XRead` | IMPL |  |
| `_XReadPad` | STUB | returns `0` |
| `_XReply` | STUB | returns `0` |
| `_XSend` | STUB | returns `0` |
| `_XSetLastRequestRead` | IMPL |  |

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

### (d) libpulse-simple.so.0 / libpulse.so.0 - the 21 symbols the audio path resolves

Both built sonames export this entire table (plus the extras in (e)):

| symbol | behaviour |
|---|---|
| `pa_channel_map_init_auto` | fills the caller's map with the default layout, returns it (NULL for 0 channels or >32) |
| `pa_context_connect` | returns `-PA_ERR_CONNECTIONREFUSED` |
| `pa_context_disconnect` | no-op |
| `pa_context_get_server_info` | returns NULL (no operation object) |
| `pa_context_get_sink_info_list` | returns NULL (no operation object) |
| `pa_context_get_source_info_list` | returns NULL (no operation object) |
| `pa_context_get_state` | returns `PA_CONTEXT_FAILED` (5) |
| `pa_context_new` | returns a sentinel handle (non-NULL: constructing a context does not contact the server in the real library) |
| `pa_context_unref` | no-op |
| `pa_mainloop_free` | no-op |
| `pa_mainloop_get_api` | returns a complete inert vtable of 12 callbacks |
| `pa_mainloop_iterate` | returns `-1` immediately (never blocks, even with `block=1`): the strongest stop signal for a READY-wait loop |
| `pa_mainloop_new` | returns a sentinel handle (non-NULL) |
| `pa_operation_get_state` | returns `PA_OPERATION_CANCELLED` (2), NULL-safe |
| `pa_operation_unref` | no-op (NULL-safe) |
| `pa_simple_free` | no-op (NULL-safe: FMOD's cleanup calls it after a failed new) |
| `pa_simple_get_latency` | returns `(pa_usec_t)-1` (the documented unknown) |
| `pa_simple_new` | returns NULL; `*error` = `PA_ERR_CONNECTIONREFUSED` (6), `PA_ERR_INVALID` (3) if the sample spec is malformed |
| `pa_simple_read` | returns `-PA_ERR_INVALID` |
| `pa_simple_write` | returns `-PA_ERR_INVALID` |
| `pa_strerror` | static text, `6` -> "Connection refused" |

### (e) libpulse - exported beyond the required set

| symbol | behaviour |
|---|---|
| `pa_context_errno` | returns `PA_ERR_CONNECTIONREFUSED` (6) |
| `pa_get_library_version` | returns `"17.0.0"` (the ABI's measured era) |
| `pa_simple_drain` | returns `-PA_ERR_INVALID` |
| `pa_simple_flush` | returns `-PA_ERR_INVALID` |

### (f) libudev.so.1 - the 26 symbols SDL's udev backend resolves

| symbol | behaviour |
|---|---|
| `udev_device_get_action` | returns NULL: no event |
| `udev_device_get_devnode` | returns NULL (NULL-safe) |
| `udev_device_get_devnum` | returns 0 (`dev_t` is 8 bytes on x86-64 glibc) |
| `udev_device_get_parent_with_subsystem_devtype` | returns NULL (NULL-safe) |
| `udev_device_get_property_value` | returns NULL (NULL-safe) |
| `udev_device_get_subsystem` | returns NULL (NULL-safe) |
| `udev_device_get_sysattr_value` | returns NULL (NULL-safe) |
| `udev_device_new_from_devnum` | returns NULL: no such device |
| `udev_device_new_from_syspath` | returns NULL: no such device |
| `udev_device_unref` | no-op, returns NULL, NULL-safe |
| `udev_enumerate_add_match_property` | returns 0 (filter accepted; nothing to match) |
| `udev_enumerate_add_match_subsystem` | returns 0 (filter accepted; nothing to match) |
| `udev_enumerate_get_list_entry` | returns NULL: the empty list |
| `udev_enumerate_new` | returns a non-NULL sentinel enumerator, even for a NULL udev handle |
| `udev_enumerate_scan_devices` | returns 0 — success with zero devices |
| `udev_enumerate_unref` | no-op, returns NULL, NULL-safe |
| `udev_list_entry_get_name` | returns NULL (NULL-safe) |
| `udev_list_entry_get_next` | returns NULL (NULL-safe) |
| `udev_monitor_enable_receiving` | returns 0 (enabled; nothing will arrive) |
| `udev_monitor_filter_add_match_subsystem_devtype` | returns 0 |
| `udev_monitor_get_fd` | the pipe read end (never readable), or -1 if `pipe2` failed |
| `udev_monitor_new_from_netlink` | returns a non-NULL sentinel monitor and creates the never-signalling pipe |
| `udev_monitor_receive_device` | returns NULL: the documented "nothing pending" answer, never an error |
| `udev_monitor_unref` | no-op, returns NULL; the pipe fds stay open |
| `udev_new` | returns a non-NULL sentinel handle (a caller treats NULL as "no udev available" and gives up) |
| `udev_unref` | no-op, returns NULL, NULL-safe |

### (g) libudev.so.1 - exported beyond the required set

Nothing in UnityPlayer.so names these; they exist so a caller that probes the wider public surface gets the same empty answer instead of a missing symbol. Every one is NULL-safe.

| symbol | behaviour |
|---|---|
| `udev_device_get_devtype` | no-op / NULL |
| `udev_device_get_driver` | no-op / NULL |
| `udev_device_get_is_initialized` | no-op / NULL |
| `udev_device_get_parent` | no-op / NULL |
| `udev_device_get_sysname` | no-op / NULL |
| `udev_device_get_syspath` | no-op / NULL |
| `udev_device_get_udev` | no-op / NULL |
| `udev_device_new_from_subsystem_sysname` | no-op / NULL |
| `udev_device_ref` | no-op / NULL |
| `udev_enumerate_add_match_sysattr` | no-op / NULL |
| `udev_enumerate_add_match_tag` | no-op / NULL |
| `udev_enumerate_add_nomatch_subsystem` | no-op / NULL |
| `udev_enumerate_add_nomatch_sysattr` | no-op / NULL |
| `udev_enumerate_add_syspath` | no-op / NULL |
| `udev_enumerate_get_udev` | no-op / NULL |
| `udev_list_entry_get_by_name` | no-op / NULL |
| `udev_list_entry_get_sysname` | no-op / NULL |
| `udev_list_entry_get_value` | no-op / NULL |
| `udev_monitor_filter_add_match_tag` | no-op / NULL |
| `udev_monitor_filter_remove` | no-op / NULL |
| `udev_monitor_filter_update` | no-op / NULL |
| `udev_monitor_get_udev` | no-op / NULL |
| `udev_monitor_set_receive_buffer_size` | no-op / NULL |
| `udev_ref` | no-op / NULL |

