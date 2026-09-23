#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include <stdint.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define ABS_X 0x00
#define ABS_Y 0x01

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

int virtio_input_init(void);

// Read up to max_events from the kernel buffer into the provided user buffer.
// Returns the number of events read.
int virtio_input_get_events(struct virtio_input_event *buf, int max_events);

#endif
