/* HobbyOS/linux-doom shim for i_sound.c.
 *
 * linuxdoom-1.10 #includes <linux/soundcard.h> unconditionally, but the OSS
 * API header left the kernel UAPI long ago and musl ships no copy at all.
 * doomdef.h defines SNDSERV, so the only code that would touch OSS
 * (SNDCTL_DSP_*, /dev/dsp) is compiled out; nothing here is referenced.
 * An empty placeholder keeps the include satisfiable without touching the
 * source.  Result: DOOM runs without sound (it popen()s an optional
 * "sndserver" helper, which is absent on this host and non-fatal).
 */
#ifndef HOBBYOS_LINUX_SOUNDCARD_SHIM_H
#define HOBBYOS_LINUX_SOUNDCARD_SHIM_H
#endif
