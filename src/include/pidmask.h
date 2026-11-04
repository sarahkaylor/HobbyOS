#ifndef PIDMASK_H
#define PIDMASK_H

#include <stdint.h>

/*
 * A PID bitmap with a fixed capacity, independent of MAX_PROCESSES.
 *
 * Several kernel structures (pipe ends, AF_UNIX wait channels, the
 * name-space connect/accept queues) record parked pids as a bitmask and used
 * a bare uint64_t.  That was only correct while MAX_PROCESSES was 64:
 * `1ull << pid` is undefined for pid >= 64, so a pid above 63 would corrupt
 * an adjacent word or vanish.  L12 raised MAX_PROCESSES to 192 for the
 * RimWorld thread count, so those masks became word arrays.
 *
 * The capacity is fixed at 256 bits (4 words) rather than derived from
 * MAX_PROCESSES so that this header is standalone: pipe.h/unix.h do not have
 * to include process.h.  process.h asserts MAX_PROCESSES <= PID_MASK_BITS, so
 * a future table growth cannot silently outrun the masks again.
 *
 * Bit 0 is never used (pid 0 is the reserved kernel slot), which keeps
 * "mask is empty" and "no pid" unambiguous.
 */
#define PID_MASK_BITS 256
#define PID_MASK_WORDS (PID_MASK_BITS / 64)

struct pid_mask {
  uint64_t w[PID_MASK_WORDS];
};

static inline void pid_mask_zero(struct pid_mask *m) {
  for (int i = 0; i < PID_MASK_WORDS; i++)
    m->w[i] = 0;
}

static inline void pid_mask_set(struct pid_mask *m, int pid) {
  if (pid > 0 && pid < PID_MASK_BITS)
    m->w[pid >> 6] |= 1ull << (pid & 63);
}

static inline int pid_mask_test(const struct pid_mask *m, int pid) {
  if (pid <= 0 || pid >= PID_MASK_BITS)
    return 0;
  return (int)((m->w[pid >> 6] >> (pid & 63)) & 1);
}

static inline int pid_mask_empty(const struct pid_mask *m) {
  for (int i = 0; i < PID_MASK_WORDS; i++)
    if (m->w[i])
      return 0;
  return 1;
}

static inline void pid_mask_or(struct pid_mask *dst, const struct pid_mask *src) {
  for (int i = 0; i < PID_MASK_WORDS; i++)
    dst->w[i] |= src->w[i];
}

#endif /* PIDMASK_H */
