/*
 * pulse_abi.h - the fragment of the PulseAudio client ABI the compat shim
 * needs, written out by hand.
 *
 * Why a private header instead of <pulse/...>: src/user/xcompat/ are
 * freestanding DSOs (-nostdlib, no DT_NEEDED) built with HOST_CC, and the
 * guest-side build must not depend on libpulse-dev being installed on the
 * build host.  Only four public structures cross the ABI boundary and the
 * shim never allocates any of them itself, so the layouts below are the whole
 * contract.  Every number was measured against the real 17.0 headers on
 * 2026-11-04 (see tools/gen_exports.py's pulse section and the host test's
 * REF rows):
 *
 *   sizeof(pa_channel_map)      132   channels @0 (u8), map[32] @4 (int)
 *   sizeof(pa_sample_spec)       12   format @0, rate @4, channels @8
 *   sizeof(pa_buffer_attr)       20   five u32
 *   sizeof(pa_mainloop_api)     112   userdata @0, io_new @8, quit @104
 *
 * A caller compiled against the real headers writes those structures on its
 * own stack and passes a pointer; if any of the four drifts, the shim reads
 * (pa_simple_new's sample spec) or writes (pa_channel_map_init_auto's map)
 * the wrong bytes.  The host test pins the sizes and compares them with the
 * host's real headers when libpulse-dev is present (src/host/pulse_abi_ref.c).
 *
 * Plain C types only: no <stdint.h>, so this compiles equally in the
 * freestanding DSO and in the hosted test.
 */

#ifndef HOBBYOS_XCOMPAT_PULSE_ABI_H
#define HOBBYOS_XCOMPAT_PULSE_ABI_H

#define PULSEAPI __attribute__((visibility("default")))

typedef unsigned char pulse_u8;
typedef unsigned int pulse_u32;      /* ILP32-in-LP64: 4 bytes on x86-64 */
typedef unsigned long pulse_u64;     /* pa_usec_t: always 64 bit */
typedef int pulse_bool;

/* ---- enums (the int-sized public spellings) --------------------------- */

/* pulse/def.h: pa_error_code */
enum {
  PA_OK_ = 0,
  PA_ERR_ACCESS_ = 1,
  PA_ERR_COMMAND_ = 2,
  PA_ERR_INVALID_ = 3,
  PA_ERR_EXIST_ = 4,
  PA_ERR_NOENTITY_ = 5,
  PA_ERR_CONNECTIONREFUSED_ = 6,
  PA_ERR_PROTOCOL_ = 7,
  PA_ERR_TIMEOUT_ = 8,
  PA_ERR_AUTHKEY_ = 9,
  PA_ERR_INTERNAL_ = 10,
  PA_ERR_CONNECTIONTERMINATED_ = 11,
  PA_ERR_KILLED_ = 12,
  PA_ERR_INVALIDSERVER_ = 13,
  PA_ERR_MODINITFAILED_ = 14,
  PA_ERR_BADSTATE_ = 15,
  PA_ERR_NODATA_ = 16,
  PA_ERR_VERSION_ = 17,
  PA_ERR_TOOLARGE_ = 18,
  PA_ERR_NOTSUPPORTED_ = 19,
  PA_ERR_UNKNOWN_ = 20,
  PA_ERR_NOEXTENSION_ = 21,
  PA_ERR_OBSOLETE_ = 22,
  PA_ERR_NOTIMPLEMENTED_ = 23,
  PA_ERR_FORKED_ = 24,
  PA_ERR_IO_ = 25,
  PA_ERR_BUSY_ = 26,
  PA_ERR_MAX_ = 27
};

/* pulse/def.h: pa_context_state */
enum {
  PA_CONTEXT_UNCONNECTED_ = 0,
  PA_CONTEXT_CONNECTING_ = 1,
  PA_CONTEXT_AUTHORIZING_ = 2,
  PA_CONTEXT_SETTING_NAME_ = 3,
  PA_CONTEXT_READY_ = 4,
  PA_CONTEXT_FAILED_ = 5,
  PA_CONTEXT_TERMINATED_ = 6
};

/* pulse/def.h: pa_operation_state */
enum {
  PA_OPERATION_RUNNING_ = 0,
  PA_OPERATION_DONE_ = 1,
  PA_OPERATION_CANCELLED_ = 2
};

/* pulse/def.h: pa_stream_direction */
enum {
  PA_STREAM_NODIRECTION_ = 0,
  PA_STREAM_PLAYBACK_ = 1,
  PA_STREAM_RECORD_ = 2,
  PA_STREAM_UPLOAD_ = 3
};

/* pulse/channelmap.h: pa_channel_position (the ones the default map uses) */
enum {
  PA_CHANNEL_POSITION_INVALID_ = -1,
  PA_CHANNEL_POSITION_MONO_ = 0,
  PA_CHANNEL_POSITION_FRONT_LEFT_ = 1,
  PA_CHANNEL_POSITION_FRONT_RIGHT_ = 2,
  PA_CHANNEL_POSITION_FRONT_CENTER_ = 3,
  PA_CHANNEL_POSITION_REAR_CENTER_ = 4,
  PA_CHANNEL_POSITION_REAR_LEFT_ = 5,
  PA_CHANNEL_POSITION_REAR_RIGHT_ = 6,
  PA_CHANNEL_POSITION_LFE_ = 7
};

#define PULSE_CHANNELS_MAX 32

/* ---- public structures (see the table at the top) --------------------- */

typedef struct pulse_channel_map {
  pulse_u8 channels;
  int map[PULSE_CHANNELS_MAX];
} pulse_channel_map;

typedef struct pulse_sample_spec {
  int format;             /* pa_sample_format_t */
  pulse_u32 rate;
  pulse_u8 channels;
} pulse_sample_spec;

typedef struct pulse_buffer_attr {
  pulse_u32 maxlength;
  pulse_u32 tlength;
  pulse_u32 prebuf;
  pulse_u32 minreq;
  pulse_u32 fragsize;
} pulse_buffer_attr;

/* Opaque in the public API: only ever a pointer here. */
typedef struct pulse_mainloop pulse_mainloop;
typedef struct pulse_context pulse_context;
typedef struct pulse_operation pulse_operation;
typedef struct pulse_simple pulse_simple;
typedef struct pulse_io_event pulse_io_event;
typedef struct pulse_time_event pulse_time_event;
typedef struct pulse_defer_event pulse_defer_event;

/* pulse/mainloop-api.h: struct pa_mainloop_api, a vtable of 12 callbacks. */
typedef struct pulse_mainloop_api {
  void *userdata;
  pulse_io_event *(*io_new)(struct pulse_mainloop_api *a, int fd, int events,
                            void *cb, void *userdata);
  void (*io_enable)(pulse_io_event *e, int events);
  void (*io_free)(pulse_io_event *e);
  void (*io_set_destroy)(pulse_io_event *e, void *cb);
  pulse_time_event *(*time_new)(struct pulse_mainloop_api *a, const void *tv,
                                void *cb, void *userdata);
  void (*time_restart)(pulse_time_event *e, const void *tv);
  void (*time_free)(pulse_time_event *e);
  void (*time_set_destroy)(pulse_time_event *e, void *cb);
  pulse_defer_event *(*defer_new)(struct pulse_mainloop_api *a, void *cb,
                                  void *userdata);
  void (*defer_enable)(pulse_defer_event *e, int b);
  void (*defer_free)(pulse_defer_event *e);
  void (*defer_set_destroy)(pulse_defer_event *e, void *cb);
  void (*quit)(struct pulse_mainloop_api *a, int retval);
} pulse_mainloop_api;

/* The pinned numbers, spelled once.  The host test asserts these against
 * sizeof/offsetof from this header *and* (when available) against the real
 * <pulse/...>.  x86-64 only, like the whole compat class (plan AD-8). */
#define PULSE_ABI_CHANNEL_MAP_SIZE     132
#define PULSE_ABI_CHANNEL_MAP_MAP_OFF    4
#define PULSE_ABI_SAMPLE_SPEC_SIZE      12
#define PULSE_ABI_SAMPLE_SPEC_RATE_OFF   4
#define PULSE_ABI_SAMPLE_SPEC_CH_OFF     8
#define PULSE_ABI_BUFFER_ATTR_SIZE      20
#define PULSE_ABI_MAINLOOP_API_SIZE    112
#define PULSE_ABI_MAINLOOP_API_IO_OFF    8
#define PULSE_ABI_MAINLOOP_API_QUIT_OFF 104

#endif /* HOBBYOS_XCOMPAT_PULSE_ABI_H */
