/*
 * virgl_hobbyos_public.h -- the entry point xlib.c uses to select the virgl
 * driver at runtime (M2, docs/rimworld-virgl-plan.md §4.3).
 *
 * The DSO ships BOTH drivers.  Selection happens once per screen creation:
 *
 *   1. HOBBYOS_GL=<name>          -- the developer override ("virgl" or
 *                                    "softpipe"; anything else is ignored).
 *   2. /GLMODE, first line        -- the in-guest switch, because a desktop
 *                                    menu launch cannot pass an environment.
 *   3. default                    -- softpipe.  Until M4's gate the software
 *                                    rasteriser stays the correctness
 *                                    reference and the CI renderer, and a
 *                                    regression in virgl must never take the
 *                                    desktop down with it.
 *
 * virgl_hobbyos_probe() is cheap and side-effect free: it opens /dev/gpu,
 * asks for the capset and closes.  A NULL winsys (no /dev/gpu, no VIRGL
 * feature, no capset, context refused) always falls back to softpipe, so the
 * caller never has to handle a half-built virgl stack.
 */
#ifndef VIRGL_HOBBYOS_PUBLIC_H
#define VIRGL_HOBBYOS_PUBLIC_H

struct virgl_winsys;

/* 1 when /dev/gpu exists and the device negotiated VIRGL. */
int virgl_hobbyos_probe(void);

/* Build the winsys over /dev/gpu.  `display` is the Xlib Display* the GLX
 * frontend was created with; it is used only for the present path (the sw
 * xlib winsys owns the shadow and the XPutImage).  NULL on any failure. */
struct virgl_winsys *virgl_hobbyos_winsys_create(void *display);

/* Destroy a winsys that never became a screen (virgl_create_screen failed, or
 * the caller changed its mind).  Declared here so xlib.c needs no view of
 * struct virgl_winsys. */
void virgl_hobbyos_winsys_discard(struct virgl_winsys *vws);

#endif /* VIRGL_HOBBYOS_PUBLIC_H */
