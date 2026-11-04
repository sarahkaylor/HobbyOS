/*
 * pulse_abi_rows.h - the row protocol for the PulseAudio ABI differential.
 *
 * src/host/pulse_abi_ref.c compiles the same row list against the host's real
 * <pulse/...>; src/host/xcompat_test.c compares the values with the ones
 * src/user/xcompat/pulse_abi.h promises.  The two TUs cannot include both
 * headers (they define the same typedefs), so the ordering has to live here.
 *
 * Keep this list in step with xcompat_pulse_row_names[] in xcompat_test.c and
 * with the row array in pulse_abi_ref.c.
 */
#ifndef HOBBYOS_XCOMPAT_PULSE_ABI_ROWS_H
#define HOBBYOS_XCOMPAT_PULSE_ABI_ROWS_H

#define XCOMPAT_PULSE_REF_ROWS 13

#endif
