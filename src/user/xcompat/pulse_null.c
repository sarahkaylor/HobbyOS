/*
 * pulse_null.c - libpulse-simple.so.0 and libpulse.so.0: the "there is no
 * PulseAudio server here" answer for the compat class (RimWorld plan AD-4).
 *
 * Measured on the host (docs/rimworld-evidence.md §2.4, §4): Unity's FMOD
 * output plugin is fmod_output_pulseaudio.cpp and SDL's audio backend is the
 * PulseAudio one, so the client library is tried **before** libasound.so.2.
 * The only name either of them opens is "libpulse-simple.so.0"; libpulse.so.0
 * and libpulsecommon-17.0.so arrive as that DSO's DT_NEEDED chain.  On this
 * host no server is reachable, FMOD logs
 *
 *   FMOD failed to initialize any audio devices, running on emulated
 *   software output with no sound
 *
 * and the game runs.  HobbyOS has no audio stack at all (M5), so this shim
 * reproduces exactly that situation: the sonames exist, every entry point
 * answers "connection refused" *immediately*, and the engine takes its silent
 * path instead of racing a missing DSO or stalling in a connect timeout.
 *
 * Why not just omit the DSO: a missing libpulse-simple.so.0 leaves FMOD's
 * probe to whatever it does on ENOENT, which is not the path its authors
 * tested; worse, an ld.so that resolves the name and then finds a half-filled
 * symbol table can leave the audio thread parked.  Answering the ABI is the
 * deterministic option, and this file is small enough to be reviewed.
 *
 * The rules this file keeps (same as alsa_null.c):
 *   - no libc, no allocation, no dtors: -nostdlib, no DT_NEEDED, so the
 *     compat loader can map it before ld.so finishes;
 *   - **nothing blocks**: no sleep, no poll, no retry loop.  Every call is a
 *     few instructions and a return.  That is the whole point -- FMOD's
 *     probe gets its answer in microseconds and moves on;
 *   - constructors return a *valid* handle where the real library would have
 *     one (pa_mainloop_new / pa_context_new do not talk to the server; only
 *     connect does), so a caller that checks the return value sees the same
 *     shape it sees against a real libpulse with no server.  A NULL context
 *     would be a legal answer too, but a caller that does not check it would
 *     dereference NULL; a sentinel cannot crash anyone.
 *
 * Exported set: the union of what the game's string table asks for (21
 * names), the rest of libpulse-simple's public surface (pa_simple_drain /
 * pa_simple_flush -- SDL's loader resolves the whole simple block at once and
 * gives up on the driver if one is missing), pa_get_library_version (FMOD
 * logs it) and pa_context_errno (the usual follow-up to a failed connect).
 * Both DSOs export all of it on purpose: the real libpulse-simple re-exports
 * nothing, but dlsym(handle, "pa_context_new") *does* find it through
 * libpulse-simple's DT_NEEDED on libpulse.so.0, and a shim with no DT_NEEDED
 * can only reproduce that by exporting the names itself.
 *
 * Built twice from this one source: -soname libpulse-simple.so.0 and
 * -soname libpulse.so.0 (see Makefile.inc).
 */

#include "pulse_abi.h"

/* ABI self-check: these are the four structures a caller allocates.  If the
 * header and this TU ever disagree, the build fails here rather than at
 * runtime inside the game. */
_Static_assert(sizeof(pulse_channel_map) == PULSE_ABI_CHANNEL_MAP_SIZE,
               "pa_channel_map layout drifted");
_Static_assert(__builtin_offsetof(pulse_channel_map, map) ==
               PULSE_ABI_CHANNEL_MAP_MAP_OFF, "pa_channel_map.map offset");
_Static_assert(sizeof(pulse_sample_spec) == PULSE_ABI_SAMPLE_SPEC_SIZE,
               "pa_sample_spec layout drifted");
_Static_assert(__builtin_offsetof(pulse_sample_spec, rate) ==
               PULSE_ABI_SAMPLE_SPEC_RATE_OFF, "pa_sample_spec.rate offset");
_Static_assert(__builtin_offsetof(pulse_sample_spec, channels) ==
               PULSE_ABI_SAMPLE_SPEC_CH_OFF, "pa_sample_spec.channels offset");
_Static_assert(sizeof(pulse_buffer_attr) == PULSE_ABI_BUFFER_ATTR_SIZE,
               "pa_buffer_attr layout drifted");
_Static_assert(sizeof(pulse_mainloop_api) == PULSE_ABI_MAINLOOP_API_SIZE,
               "pa_mainloop_api layout drifted");
_Static_assert(__builtin_offsetof(pulse_mainloop_api, io_new) ==
               PULSE_ABI_MAINLOOP_API_IO_OFF, "pa_mainloop_api.io_new offset");
_Static_assert(__builtin_offsetof(pulse_mainloop_api, quit) ==
               PULSE_ABI_MAINLOOP_API_QUIT_OFF, "pa_mainloop_api.quit offset");

/* ---- sentinels -------------------------------------------------------- *
 * Never dereferenced, never written: they only have to be distinct,
 * non-NULL addresses so a caller can hold, compare and pass them back. */
static char pulse_null_mainloop_tag;
static char pulse_null_context_tag;

/* ====================================================================== *
 * libpulse-simple: the blocking API FMOD's output uses
 * ====================================================================== */

/* pa_simple_new() is the probe that decides whether audio is available.  The
 * real library checks the sample spec first (PA_ERR_INVALID) and only then
 * connects; with no server the connect fails with PA_ERR_CONNECTIONREFUSED.
 * Both outcomes are returned in *error and both are instant. */
PULSEAPI pulse_simple *pa_simple_new(const char *server, const char *name,
                                     int dir, const char *dev,
                                     const char *stream_name,
                                     const pulse_sample_spec *ss,
                                     const pulse_channel_map *map,
                                     const pulse_buffer_attr *attr,
                                     int *error) {
  (void)server; (void)name; (void)dir; (void)dev; (void)stream_name;
  (void)map; (void)attr;

  if (error) {
    int ok = ss && ss->rate != 0 && ss->channels != 0;
    *error = ok ? PA_ERR_CONNECTIONREFUSED_ : PA_ERR_INVALID_;
  }
  return 0;                       /* no stream, ever */
}

PULSEAPI void pa_simple_free(pulse_simple *s) {
  (void)s;                        /* idempotent and NULL-safe, like the real
                                   * one: FMOD's cleanup path calls it even
                                   * when new() failed */
}

PULSEAPI int pa_simple_read(pulse_simple *s, void *data, unsigned long bytes,
                            int *error) {
  (void)s; (void)data; (void)bytes;
  if (error) *error = PA_ERR_INVALID_;
  return -PA_ERR_INVALID_;
}

PULSEAPI int pa_simple_write(pulse_simple *s, const void *data,
                             unsigned long bytes, int *error) {
  (void)s; (void)data; (void)bytes;
  if (error) *error = PA_ERR_INVALID_;
  return -PA_ERR_INVALID_;
}

PULSEAPI int pa_simple_drain(pulse_simple *s, int *error) {
  (void)s;
  if (error) *error = PA_ERR_INVALID_;
  return -PA_ERR_INVALID_;
}

PULSEAPI int pa_simple_flush(pulse_simple *s, int *error) {
  (void)s;
  if (error) *error = PA_ERR_INVALID_;
  return -PA_ERR_INVALID_;
}

PULSEAPI pulse_u64 pa_simple_get_latency(pulse_simple *s, int *error) {
  (void)s;
  if (error) *error = PA_ERR_INVALID_;
  return (pulse_u64)-1;           /* the documented "unknown" value */
}

/* ====================================================================== *
 * libpulse: the asynchronous API SDL's PulseAudio backend uses
 * ====================================================================== */

/* ---- main loop ------------------------------------------------------- */

static pulse_io_event *pulse_io_new(pulse_mainloop_api *a, int fd,
                                    int events, void *cb, void *ud) {
  (void)a; (void)fd; (void)events; (void)cb; (void)ud;
  return 0;
}
static void pulse_io_enable(pulse_io_event *e, int events) {
  (void)e; (void)events;
}
static void pulse_io_free(pulse_io_event *e) { (void)e; }
static void pulse_io_set_destroy(pulse_io_event *e, void *cb) {
  (void)e; (void)cb;
}
static pulse_time_event *pulse_time_new(pulse_mainloop_api *a,
                                        const void *tv, void *cb, void *ud) {
  (void)a; (void)tv; (void)cb; (void)ud;
  return 0;
}
static void pulse_time_restart(pulse_time_event *e, const void *tv) {
  (void)e; (void)tv;
}
static void pulse_time_free(pulse_time_event *e) { (void)e; }
static void pulse_time_set_destroy(pulse_time_event *e, void *cb) {
  (void)e; (void)cb;
}
static pulse_defer_event *pulse_defer_new(pulse_mainloop_api *a, void *cb,
                                          void *ud) {
  (void)a; (void)cb; (void)ud;
  return 0;
}
static void pulse_defer_enable(pulse_defer_event *e, int b) { (void)e; (void)b; }
static void pulse_defer_free(pulse_defer_event *e) { (void)e; }
static void pulse_defer_set_destroy(pulse_defer_event *e, void *cb) {
  (void)e; (void)cb;
}
static void pulse_quit(pulse_mainloop_api *a, int retval) {
  (void)a; (void)retval;
}

/* A complete, inert vtable.  Returned non-NULL because pa_mainloop_get_api
 * never fails in the real library; every callback below exists so that a
 * caller which walks the vtable (pa_context_new does not, but a host test or
 * a future FMOD could) never jumps to NULL. */
static pulse_mainloop_api pulse_null_api = {
  0,
  pulse_io_new, pulse_io_enable, pulse_io_free, pulse_io_set_destroy,
  pulse_time_new, pulse_time_restart, pulse_time_free, pulse_time_set_destroy,
  pulse_defer_new, pulse_defer_enable, pulse_defer_free,
  pulse_defer_set_destroy,
  pulse_quit
};

PULSEAPI pulse_mainloop *pa_mainloop_new(void) {
  return (pulse_mainloop *)&pulse_null_mainloop_tag;
}

PULSEAPI pulse_mainloop_api *pa_mainloop_get_api(pulse_mainloop *m) {
  (void)m;
  return &pulse_null_api;
}

/* Real libpulse returns 0 from a successful iteration (and 1 if the loop was
 * quit).  This shim returns -1 instead, deliberately: a caller whose wait
 * loop only tests `pa_context_get_state() != PA_CONTEXT_READY` would spin
 * forever against a state that can never become READY, and -1 is the one
 * signal every libpulse caller treats as "stop".  It also returns instantly,
 * so `block=1` never blocks. */
PULSEAPI int pa_mainloop_iterate(pulse_mainloop *m, int block, int *retval) {
  (void)m; (void)block;
  if (retval) *retval = 0;
  return -1;
}

PULSEAPI void pa_mainloop_free(pulse_mainloop *m) { (void)m; }

/* ---- context --------------------------------------------------------- */

PULSEAPI pulse_context *pa_context_new(pulse_mainloop_api *api,
                                       const char *name) {
  (void)api; (void)name;
  /* Non-NULL: constructing a context does not contact the server in the real
   * library either (the socket is opened by pa_context_connect), so this is
   * the faithful answer and the safe one for unchecked callers. */
  return (pulse_context *)&pulse_null_context_tag;
}

PULSEAPI int pa_context_connect(pulse_context *c, const char *server,
                                int flags, const void *api) {
  (void)c; (void)server; (void)flags; (void)api;
  return -PA_ERR_CONNECTIONREFUSED_;   /* the answer with no daemon running */
}

PULSEAPI void pa_context_disconnect(pulse_context *c) { (void)c; }

PULSEAPI int pa_context_get_state(pulse_context *c) {
  (void)c;
  /* Stateless by design: the only state this shim can ever be in is FAILED,
   * which is exactly what a real context reports after a refused connect. */
  return PA_CONTEXT_FAILED_;
}

PULSEAPI int pa_context_errno(pulse_context *c) {
  (void)c;
  return PA_ERR_CONNECTIONREFUSED_;
}

PULSEAPI void pa_context_unref(pulse_context *c) { (void)c; }

/* No operation object exists, so every "start an operation" entry point
 * returns NULL -- the real library's failure value, which callers already
 * handle because a NULL operation is also what they get from a disconnected
 * context. */
PULSEAPI pulse_operation *pa_context_get_server_info(pulse_context *c,
                                                     void *cb, void *ud) {
  (void)c; (void)cb; (void)ud;
  return 0;
}

PULSEAPI pulse_operation *pa_context_get_sink_info_list(pulse_context *c,
                                                        void *cb, void *ud) {
  (void)c; (void)cb; (void)ud;
  return 0;
}

PULSEAPI pulse_operation *pa_context_get_source_info_list(pulse_context *c,
                                                          void *cb, void *ud) {
  (void)c; (void)cb; (void)ud;
  return 0;
}

/* ---- operations ------------------------------------------------------ */

PULSEAPI int pa_operation_get_state(pulse_operation *o) {
  (void)o;
  /* NULL-safe: a caller that kept the NULL from a failed *_info_list and
   * polls it must not crash.  CANCELLED is the state that makes it stop. */
  return PA_OPERATION_CANCELLED_;
}

PULSEAPI void pa_operation_unref(pulse_operation *o) { (void)o; }

/* ---- helpers --------------------------------------------------------- */

/* pa_channel_map_init_auto() is the one function here that *writes* into a
 * caller-allocated structure, so its bounds are the ABI.  Fill only the
 * channels the caller asked for; leave the tail of map[] alone rather than
 * assuming the caller's header had 32 slots. */
PULSEAPI pulse_channel_map *pa_channel_map_init_auto(pulse_channel_map *m,
                                                     unsigned channels,
                                                     int def) {
  (void)def;
  if (!m || channels == 0 || channels > PULSE_CHANNELS_MAX) return 0;
  m->channels = (pulse_u8)channels;
  for (unsigned i = 0; i < channels; i++) m->map[i] = PA_CHANNEL_POSITION_INVALID_;
  switch (channels) {
  case 1:
    m->map[0] = PA_CHANNEL_POSITION_MONO_;
    break;
  default:
    /* The default layouts pulseaudio uses for the common channel counts;
     * FMOD only probes this, so anything self-consistent is enough. */
    if (channels >= 2) {
      m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT_;
      m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT_;
    }
    if (channels >= 3) m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER_;
    if (channels >= 4) m->map[3] = PA_CHANNEL_POSITION_LFE_;
    if (channels >= 5) m->map[4] = PA_CHANNEL_POSITION_REAR_LEFT_;
    if (channels >= 6) m->map[5] = PA_CHANNEL_POSITION_REAR_RIGHT_;
    for (unsigned i = 6; i < channels; i++) m->map[i] = PA_CHANNEL_POSITION_INVALID_;
    break;
  }
  return m;
}

/* pa_strerror() must return static storage: callers keep the pointer and some
 * (FMOD's log line) print it after the call.  The text matches the real
 * libpulse for every code this shim can produce. */
PULSEAPI const char *pa_strerror(int err) {
  switch (err) {
  case PA_OK_: return "OK";
  case PA_ERR_ACCESS_: return "Access denied";
  case PA_ERR_COMMAND_: return "Unknown command";
  case PA_ERR_INVALID_: return "Invalid argument";
  case PA_ERR_EXIST_: return "Entity exists";
  case PA_ERR_NOENTITY_: return "No such entity";
  case PA_ERR_CONNECTIONREFUSED_: return "Connection refused";
  case PA_ERR_PROTOCOL_: return "Protocol error";
  case PA_ERR_TIMEOUT_: return "Timeout";
  case PA_ERR_AUTHKEY_: return "No authentication key";
  case PA_ERR_INTERNAL_: return "Internal error";
  case PA_ERR_CONNECTIONTERMINATED_: return "Connection terminated";
  case PA_ERR_KILLED_: return "Entity killed";
  case PA_ERR_INVALIDSERVER_: return "Invalid server";
  case PA_ERR_MODINITFAILED_: return "Module initialization failed";
  case PA_ERR_BADSTATE_: return "Bad state";
  case PA_ERR_NODATA_: return "No data";
  case PA_ERR_VERSION_: return "Incompatible protocol version";
  case PA_ERR_TOOLARGE_: return "Too large";
  case PA_ERR_NOTSUPPORTED_: return "Not supported";
  case PA_ERR_UNKNOWN_: return "Unknown error code";
  case PA_ERR_NOEXTENSION_: return "No extension";
  case PA_ERR_OBSOLETE_: return "Obsolete functionality";
  case PA_ERR_NOTIMPLEMENTED_: return "Missing implementation";
  case PA_ERR_FORKED_: return "Client forked";
  case PA_ERR_IO_: return "Input/output error";
  case PA_ERR_BUSY_: return "Device or resource busy";
  default: return "Invalid error code";
  }
}

/* FMOD logs this at startup and some builds gate features on the major
 * version, so report the era this ABI was measured against (17.0, the host's
 * libpulse 17.0.0) rather than something obviously fake. */
PULSEAPI const char *pa_get_library_version(void) { return "17.0.0"; }
