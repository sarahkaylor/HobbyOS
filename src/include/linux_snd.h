/* linux_snd.h — the ALSA kernel ABI subset over virtio-sound (lane C4-audio,
 * docs/cp-audio.md).  Nodes /dev/snd/controlC0 and /dev/snd/pcmC0D0p exist
 * only when a virtio-sound device was brought up; without one every open is
 * ENOENT (the pre-audio behaviour, so every gate without the device is
 * unchanged). */
#ifndef LINUX_SND_H
#define LINUX_SND_H

#include <stdint.h>

struct process;

#define LSND_MINOR_CONTROL 0   /* /dev/snd/controlC0, char 116:0 */
#define LSND_MINOR_PCM_PLAY 16 /* /dev/snd/pcmC0D0p,  char 116:16 */
#define LSND_PARKED (-100000)  /* parked: the route answers l_park (never -ENOENT, which is -2) */

/* "/dev/snd/..." -> minor, or -1 when the path is not an ALSA node. */
int linux_snd_path_minor(const char *path);
/* Open-time check: 0, -ENOENT (no device / no such node), -EBUSY (the PCM
 * is single-open). */
int linux_snd_open(int minor, uint32_t flags);
void linux_snd_release(int minor);
/* ioctl(2) on an ALSA node.  Returns >= 0, -errno, or LSND_PARKED. */
int linux_snd_ioctl(struct process *p, int minor, uint32_t open_flags, uint32_t req,
                    uint64_t arg);
/* poll/select readiness of the node (fs.c f1_probe_fd). */
void linux_snd_poll_probe(int minor, int *r, int *w, int *e);
const void *linux_snd_poll_key(void);

#endif
