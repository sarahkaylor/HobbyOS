/* src/host/lsnd_host.h -- the kernel services src/kernel/linux_snd.c uses,
 * for the host harness that runs it under an unmodified alsa-lib
 * (tools/cp/audio/host-alsa-test.sh, docs/cp-audio.md §5).  Single-process,
 * single-threaded callers: locks are no-ops, a park is a 1 ms sleep in the
 * LD_PRELOAD shim (src/host/lsnd_preload.c). */
#ifndef LSND_HOST_H
#define LSND_HOST_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#ifndef ENOTTY
#define ENOTTY 25
#endif
typedef struct { int x; } spinlock_t;
typedef struct { int x; } ylock_t;
static inline void spinlock_init(spinlock_t *l) { (void)l; }
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *l) { (void)l; return 0; }
static inline void spinlock_release_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; }
static inline void ylock_init(ylock_t *l) { (void)l; }
static inline void ylock_acquire(ylock_t *l) { (void)l; }
static inline void ylock_release(ylock_t *l) { (void)l; }
static inline uint64_t kimg_pa(const volatile void *p) { return (uint64_t)(uintptr_t)p; }
#define MAX_PROCESSES 512
#define PROC_STATE_BLOCKED 3
struct process { int pid; int state; };
extern spinlock_t proc_lock;
static inline int process_park_ok_locked(const struct process *p) { (void)p; return 1; }
static inline void proc_set_deadline_us(struct process *p, uint64_t us) { (void)p; (void)us; }
int process_try_wake_locked(int pid);
void sched_wake_idle_cpu(void);
void poll_notify_obj(const void *obj);
struct process *current_process(void);
uint64_t timer_get_us(void);
int vm_user_read(struct process *p, uint64_t va, void *dst, int len);
int vm_kwrite(struct process *p, uint64_t va, const void *src, int len);
#endif
