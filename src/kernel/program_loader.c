#include "program_loader.h"
#include "fs.h"
#include "setjmp.h"
#include "process.h"
#include "arch/cpu.h"
#include "errno.h"
#include "timer.h"


extern struct process *process_get_pcb(int pid);
extern void uart_puts(const char* s);
extern void print_int(int val);

jmp_buf user_exit_context;

/* Cap on the program image the loader copies into user memory.  A process
 * region is 32MB with its first USER_INITIAL_CLEAR_SIZE (1MB) bytes zeroed
 * at creation, so sizing the cap to match keeps "zeroed" and "loadable" the
 * same 1MB.  This used to be 64KB and the loader silently truncated the
 * image: the tail of .text/.rodata never arrived and the process died with
 * a mystery data abort.  Oversized programs are now refused with an
 * explicit error instead. */
#define MAX_PROGRAM_SIZE  USER_INITIAL_CLEAR_SIZE

/* Blocks the boot-wave loader keeps free so child processes can spawn
 * while the wave is loaded.  Every test that spawns a tool (SHTEST's
 * shells, SEDTEST's per-case runs, TACTEST's tool runs, ...) needs a
 * block of its own; without headroom the fully-loaded wave pins the pool
 * and those tests wait forever.  Six covers the deepest concurrent
 * child demand seen in the suite. */
#define WAVE_LOAD_RESERVE  6

/* Is the opened program too big for MAX_PROGRAM_SIZE?  (Call after
 * fat16_open, before reading.) */
static int program_too_large(const struct file *f) {
  return f->fat16.entry.file_size > (uint32_t)MAX_PROGRAM_SIZE;
}

/**
 * Loads a program from the FAT16 filesystem directly into user memory and executes it.
 * This function bypasses the scheduler and is used for early boot testing.
 *
 * Returns:
 *   0 on successful completion (via longjmp), or -1 on failure.
 */
int load_and_run_program(const char* filename) {
  uart_puts("Loading program: ");
  uart_puts(filename);
  uart_puts("\n");

  struct file f;
  if (fat16_open(filename, &f) != 0) {
    uart_puts("Failed to locate ");
    uart_puts(filename);
    uart_puts(" on disk image!\n");
    return -1;
  }

  if (program_too_large(&f)) {
    uart_puts("Program too large (");
    print_int((int)f.fat16.entry.file_size);
    uart_puts(" bytes > ");
    print_int((int)MAX_PROGRAM_SIZE);
    uart_puts("): ");
    uart_puts(filename);
    uart_puts("\n");
    fat16_close(&f);
    return -1;
  }

  int bytes_read = fat16_read(&f, (void*)USER_VIRT_BASE, MAX_PROGRAM_SIZE);
  if (bytes_read <= 0) {
    uart_puts("Failed to read ");
    uart_puts(filename);
    uart_puts(" from disk!\n");
    fat16_close(&f);
    return -1;
  }

  // Clean D-cache and invalidate I-cache so the loaded program executes correctly
  __builtin___clear_cache((char*)USER_VIRT_BASE, (char*)USER_VIRT_BASE + bytes_read);

  fat16_close(&f);

  if (setjmp(user_exit_context) != 0) {
    interrupts_enable();
    return 0;
  }

  arch_enter_user_mode((uint64_t)USER_VIRT_BASE, (uint64_t)(USER_VIRT_BASE + USER_REGION_SIZE));

  return -1;
}

/**
 * Loads a program from the FAT16 filesystem into a new process's memory and
 * registers it with the scheduler.
 *
 * Parameters:
 *   filename - The name of the binary to load.
 *
 * Returns:
 *   The PID of the new process, or -1 on failure.
 */
int load_and_run_program_in_scheduler_args(const char* filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid, const char *args) {
  if (!filename) return -1;
  uart_puts("Loading program for scheduler: ");
  uart_puts(filename);
  uart_puts("\n");

  /* Boot-wave loads (caller_pid < 0) must leave headroom: with ~40 wave
     programs and 40 physical blocks the wave itself can pin the whole
     pool, and every test that spawns a child (SHTEST's shells, SEDTEST's
     per-case spawns, TACTEST's 17 tool runs) then waits forever for a
     block.  Wave loads therefore only take a block while more than
     WAVE_LOAD_RESERVE blocks stay free; child spawns (called from the
     spawn worker with a real caller_pid) take any block.  The physical
     pool can also be momentarily exhausted by heavy tests; in both cases
     wait and retry instead of silently dropping the program.  This CPU is
     pre-scheduler and cannot sleep, so the wait is a bounded read of the
     free-running counter (~100 ms per attempt) also capped by an
     iteration count so it terminates even if the counter is
     uncalibrated. */
  extern int phys_block_free_count(void);
  int pid = -1;
  for (int attempt = 0; attempt < 18000; attempt++) {
    uint64_t t0 = timer_get_ms();
    for (volatile int spin = 0; spin < 4000000; spin++) {
      if (timer_get_ms() - t0 >= 100u) break;
    }
    if (caller_pid < 0 && phys_block_free_count() <= WAVE_LOAD_RESERVE)
      continue; /* keep headroom for child spawns */
    pid = process_create();
    if (pid >= 0)
      break;
  }
  if (pid < 0) {
    uart_puts("Loader starved: ");
    uart_puts(filename);
    uart_puts(" never got a physical block.\n");
    uart_puts("Failed to create process for ");
    uart_puts(filename);
    uart_puts("!\n");
    return -1;
  }

  struct process *child = process_get_pcb(pid);
  if (child) {
    for (int i = 0; i < 31 && filename[i] != '\0'; i++) {
      child->name[i] = filename[i];
      child->name[i + 1] = '\0';
    }
    struct process *parent = process_get_pcb(caller_pid);
    if (parent) {
      /* Record the parent so waitpid() can reap this child and so the
         zombie reclaimers never free it while the parent is alive. */
      child->parent_pid = parent->pid;
      for (int i = 0; i < 128; i++) {
        child->cwd[i] = parent->cwd[i];
      }
    } else {
      child->cwd[0] = '/';
      child->cwd[1] = '\0';
    }
    /* Store the command line BEFORE process_set_entry() makes the child
       runnable: on multicore the child can start on another CPU the moment
       it is scheduled, and crt0 reads its args through its very first
       syscall, so a post-hoc copy from the spawn worker is too late. */
    int ai = 0;
    if (args) {
      while (args[ai] && ai < 255) {
        child->args[ai] = args[ai];
        ai++;
      }
    }
    child->args[ai] = '\0';
  }

  struct file f;
  uart_puts("Calling fat16_open...\n");
  if (fat16_open(filename, &f) != 0) {
    uart_puts("Failed to open file: ");
    uart_puts(filename);
    uart_puts("\n");
    process_free(pid);
    return -1;
  }

  if (program_too_large(&f)) {
    uart_puts("Program too large (");
    print_int((int)f.fat16.entry.file_size);
    uart_puts(" bytes > ");
    print_int((int)MAX_PROGRAM_SIZE);
    uart_puts("): ");
    uart_puts(filename);
    uart_puts("\n");
    fat16_close(&f);
    process_free(pid);
    return -1;
  }

  uint64_t phys_base = process_get_phys_base(pid);

  int bytes_read = fat16_read_direct(&f, phys_base, MAX_PROGRAM_SIZE);
  if (bytes_read <= 0) {
    uart_puts("Failed to read ");
    uart_puts(filename);
    uart_puts(" from disk!\n");
    fat16_close(&f);
    process_free(pid);
    return -1;
  }

  uart_puts("Read ");
  print_int(bytes_read);
  uart_puts(" bytes for PID=");
  print_int(pid);
  uart_puts("\n");

  // Clean D-cache and invalidate I-cache so the loaded program executes correctly
  __builtin___clear_cache((char*)phys_base, (char*)phys_base + bytes_read);
  uart_puts("Cache cleared.\n");

  fat16_close(&f);
  uart_puts("fat16_close finished.\n");

  struct process *parent = process_get_pcb(caller_pid);
  // child is already defined above
  if (parent && child) {
    if (stdin_fd >= 0 && stdin_fd < MAX_OPEN_FDS && parent->open_fds[stdin_fd] != -1) {
      child->open_fds[0] = parent->open_fds[stdin_fd];
      fs_reopen(child->open_fds[0]);
      child->num_open_fds++;
    }
    if (stdout_fd >= 0 && stdout_fd < MAX_OPEN_FDS && parent->open_fds[stdout_fd] != -1) {
      child->open_fds[1] = parent->open_fds[stdout_fd];
      fs_reopen(child->open_fds[1]);
      child->num_open_fds++;
    }
    if (stderr_fd >= 0 && stderr_fd < MAX_OPEN_FDS && parent->open_fds[stderr_fd] != -1) {
      child->open_fds[2] = parent->open_fds[stderr_fd];
      fs_reopen(child->open_fds[2]);
      child->num_open_fds++;
    } else {
      /* Default: inherit the parent's fd 2 only when it is NOT a pipe.
         Handing a child a duplicate pipe end here is how it used to go
         wrong: the duplicate keeps the end's reader/writer count alive
         forever (e.g. a leaked read end of the pipe the child itself
         writes to), so after the real drainer exits no writer ever sees
         EPIPE/EOF and a full pipe self-deadlocks (observed: SH.BIN
         wedged on a full stdout pipe whose only remaining reader was
         its own inherited fd 2). */
      int gf = parent->open_fds[2];
      if (gf != -1 && !file_gfd_is_pipe(gf)) {
        child->open_fds[2] = gf;
        fs_reopen(gf);
        child->num_open_fds++;
      }
    }
  }

  process_set_entry(pid, USER_VIRT_BASE, USER_VIRT_BASE + USER_REGION_SIZE);
  return pid;
}

int load_and_run_program_in_scheduler(const char* filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid) {
  return load_and_run_program_in_scheduler_args(filename, stdin_fd, stdout_fd, stderr_fd, caller_pid, 0);
}

/**
 * SYS_EXEC: replace the CURRENT process's image with a program loaded from
 * disk, preserving its pid, fd table, cwd and stack (POSIX exec semantics).
 *
 * The process is running when this runs, so the new image is read over the
 * old one IN PLACE in the process's existing physical region (nothing needs
 * re-mapping); the trap frame's ELR is redirected to USER_VIRT_BASE so the
 * syscall return enters the new program. On success this never returns to
 * the caller; on failure the caller continues running with errno set.
 *
 * Returns 0 on success (i.e. the new image was installed and the saved
 * frame now points at it), or a negative errno (-ENOENT, -ENOEXEC) leaving
 * the current program intact.
 */
int process_exec_current(struct trap_frame *tf, const char *path,
                         const char *args, const char *new_name) {
  struct process *cur = current_process();
  if (!cur)
    return -EINVAL;
  if (!path)
    return -EINVAL;

  /* Resolve a relative path against the process cwd (which the shell
     keeps as e.g. "/" or "/subdir" — no trailing slash). */
  char abs[128];
  int al = 0;
  if (path[0] == '/') {
    for (al = 0; path[al] && al < 126; al++) abs[al] = path[al];
  } else {
    int cl = 0;
    for (; cur->cwd[cl] && cl < 96; cl++) abs[cl] = cur->cwd[cl];
    if (cl > 0 && abs[cl - 1] != '/') abs[cl++] = '/';
    for (int pi = 0; path[pi] && cl < 126; pi++, cl++) abs[cl] = path[pi];
    al = cl;
  }
  abs[al] = '\0';

  struct file f;
  if (fat16_open(abs, &f) != 0)
    return -ENOENT;
  if (program_too_large(&f)) {
    fat16_close(&f);
    return -ENOEXEC;
  }

  uint64_t base = cur->user_phys_base;

  /* Zero image + bss [0, MAX_PROGRAM_SIZE) so the new program starts
     with clean bss (spawn gets a freshly-allocated region; exec reuses).
     The stack lives near the TOP of the 32MB region, untouched here. */
  volatile uint8_t *zp = (volatile uint8_t *)base;
  for (uint32_t z = 0; z < MAX_PROGRAM_SIZE; z++)
    zp[z] = 0;

  int n = fat16_read(&f, (void *)base, MAX_PROGRAM_SIZE);
  fat16_close(&f);
  if (n <= 0)
    return -ENOENT;
  __builtin___clear_cache((char *)base, (char *)base + n);

  int i;
  for (i = 0; new_name && new_name[i] && i < 31; i++)
    cur->name[i] = new_name[i];
  cur->name[i] = '\0';
  for (i = 0; args && args[i] && i < 255; i++)
    cur->args[i] = args[i];
  cur->args[i] = '\0';

  /* Exec resets the address-space state: fresh heap top, no anonymous
     mappings (the new image's data/bss start at the load cap). */
  cur->heap_brk = USER_HEAP_BASE;
  cur->anon_map_count = 0;
  for (int i = 0; i < USER_ANON_MAX_REGS; i++) {
    cur->anon_maps[i].addr = 0;
    cur->anon_maps[i].len = 0;
  }

  /* Redirect the running process into the fresh image. regs[0]=0 is the
     exec() success return that the new program never actually reads. */
  tf->elr = USER_VIRT_BASE;
  tf->regs[0] = 0;
  return 0;
}
