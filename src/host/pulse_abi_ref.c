/* pulse_abi_ref.c - the PulseAudio layout rows, emitted from the HOST's real
 * <pulse/...>.
 *
 * The other half of the differential in src/host/xcompat_test.c.  The shim
 * carries its own ABI header (src/user/xcompat/pulse_abi.h) because the DSO
 * build must not require libpulse-dev; this TU is what proves that header
 * right whenever the real headers are installed.  If they are not, the
 * makefile compiles this file out and the test reports the differential as
 * skipped -- the shim's own _Static_asserts still run.
 *
 * The row order is the protocol in src/host/pulse_abi_rows.h. */

#include <stddef.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include "pulse_abi_rows.h"

const unsigned long xcompat_pulse_ref_values[XCOMPAT_PULSE_REF_ROWS] = {
    (unsigned long)sizeof(pa_channel_map),
    (unsigned long)offsetof(pa_channel_map, map),
    (unsigned long)sizeof(pa_sample_spec),
    (unsigned long)offsetof(pa_sample_spec, rate),
    (unsigned long)offsetof(pa_sample_spec, channels),
    (unsigned long)sizeof(pa_buffer_attr),
    (unsigned long)sizeof(pa_mainloop_api),
    (unsigned long)offsetof(pa_mainloop_api, io_new),
    (unsigned long)offsetof(pa_mainloop_api, quit),
    (unsigned long)PA_ERR_CONNECTIONREFUSED,
    (unsigned long)PA_CONTEXT_FAILED,
    (unsigned long)PA_OPERATION_CANCELLED,
    (unsigned long)PA_CHANNELS_MAX,
};
