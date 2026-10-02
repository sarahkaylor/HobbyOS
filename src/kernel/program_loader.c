#include "program_loader.h"
#include "fs.h"
#include "setjmp.h"
#include "process.h"
#include "arch/cpu.h"
#include "errno.h"
#include "timer.h"
#include "frame.h"
#include "vm.h"


extern struct process *process_get_pcb(int pid);
extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);

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

/* Famine-pacing budgets for the load retry loop (P8.2 soak class).  A
 * child spawn (caller_pid >= 0) that cannot get a process slot or AS
 * must give up fast — a full 63-slot table only drains as processes
 * exit, which the caller's own bounded respawn logic is the right place
 * to wait for; retrying in-kernel for minutes was the 28-minute soak
 * wedge (see load_v2_internal).  Boot loads (caller_pid < 0) keep a
 * longer budget: the boot loader has no user-side retry to fall back
 * on and must ride out the wave's frame-headroom wait. */
#define LOAD_RETRY_CHILD_MS  2000u
#define LOAD_RETRY_BOOT_MS   20000u

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
  /* P2.5 (S5 flip, design section 8/D4): the loader default is v2 -- every
     wave/spawn user program loads as an AS_V2 process at USER_IMG_BASE
     (kernel tasks never pass through here).  Rollback lever: this call is
     the AS_V2 selection line; returning to the 32 MiB v1 body below
     restores the pre-flip behavior. */
  if (1)
    return load_and_run_program_v2(filename, stdin_fd, stdout_fd, stderr_fd,
                                   caller_pid, args);
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
  /* Famine pacing (P8.2 soak): the old loop retried every 100 ms up to
     18 000 times (~30 min), and each failed attempt printed a flood
     line.  Under a full 63-slot table (the wave + TORTURE at once) that
     console storm saturated the UART and stalled the very exits that
     free slots — a self-sustaining wedge (observed: 138k flood lines
     at 1 soak round vs 800+ clean).  A transient drains in a tick or
     two: try immediately, park ~10 ms, and give up after a SHORT window
     so the caller's own bounded retry (the wave tests all respawn on -1)
     handles the rest.  Boot loads (caller_pid < 0) keep a longer budget:
     the boot loader has no user-side retry to fall back on and must ride
     out the frame-headroom wait. */
  extern int phys_block_free_count(void);
  int pid = -1;
  uint64_t t_start = timer_get_ms();
  uint32_t budget_ms = (caller_pid < 0) ? LOAD_RETRY_BOOT_MS
                                        : LOAD_RETRY_CHILD_MS;
  for (;;) {
    if (caller_pid < 0 && phys_block_free_count() <= WAVE_LOAD_RESERVE) {
      /* keep headroom for child spawns: wait, do not burn a reserve */;
    } else {
      pid = process_create();
      if (pid >= 0)
        break;
    }
    if (timer_get_ms() - t_start >= budget_ms)
      break;
    for (volatile int spin = 0; spin < 400000; spin++) { /* ~10 ms park */
      if (timer_get_ms() - t_start >= budget_ms)
        break;
    }
  }
  if (pid < 0) {
    uart_puts("Loader starved (slot pressure): ");
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
    /* P1 (D7): children are parented to the group ANCHOR, so a spawn from a
       thread stays reapable through the group's waitpid(). */
    if (parent)
      parent = process_group(parent);
    if (parent) {
      /* Record the parent so waitpid() can reap this child and so the
         zombie reclaimers never free it while the parent is alive. */
      child->parent_pid = parent->pid;
      for (int i = 0; i < 128; i++) {
        child->cwd[i] = parent->cwd[i];
      }
      /* P6.3: a spawned child inherits the group's environment blob (set
         by execve(envp); execv() forwards `environ`, so an exec'd parent
         hands its environment down).  The spawn contract carries no envp
         -- execve() stays the explicit form. */
      for (int i = 0; i < HO_ENV_LEN; i++) {
        child->env[i] = parent->env[i];
      }
      child->envc = parent->envc;
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
    /* Positional-parameter blob: argv[0] is the binary name (matching
       crt0's get_progname fallback), then the flat args, space-split. */
    {
      char flat[320];
      int fi = 0;
      for (int i = 0; child->name[i] && fi < 63; i++)
        flat[fi++] = child->name[i];
      if (fi)
        flat[fi++] = ' ';
      for (int i = 0; child->args[i] && fi < 315; i++)
        flat[fi++] = child->args[i];
      flat[fi] = '\0';
      proc_split_argv(child, flat);
    }
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
  if (parent)
    parent = process_group(parent); /* P1 (D7): fd table + cwd are group state */
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

  /* SysV AMD64 ABI (browser.md F1.5): at the first instruction of a
     function RSP must be 16n+8 — the caller's return address occupies the
     8 bytes at 16n.  The user entry (_start) is compiled C whose prologue
     makes the standard `push %rbp` / 16-byte-aligned-local assumption, so
     hand the process the ABI-shaped stack: one phantom "return address"
     slot below the stack top.  With SSE enabled for user code, an entry
     RSP of exactly USER_VIRT_STACK (16-aligned, ≡0) makes every aligned
     spill of a -N(%rbp) slot take #GP — observed as the x64 wave's
     Vector:13 storm and FPU_T dying at its first movaps (0x440001c9,
     -0x90(%rbp)).  ARM64 has no such requirement. */
#ifdef __x86_64__
  process_set_entry(pid, USER_VIRT_BASE, USER_VIRT_BASE + USER_REGION_SIZE - 8);
#else
  process_set_entry(pid, USER_VIRT_BASE, USER_VIRT_BASE + USER_REGION_SIZE);
#endif
  return pid;
}

int load_and_run_program_in_scheduler(const char* filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid) {
  return load_and_run_program_in_scheduler_args(filename, stdin_fd, stdout_fd, stderr_fd, caller_pid, 0);
}

/* P2.5 (S5 flip): reserve the whole v2 HEAP slot as one RW demand-zero
 * region.  The user allocator (src/user/malloc.c) carves its arena from
 * this span, so its handed-out memory is always backed -- pages materialize
 * on first touch (VMK_HEAP is demand-zero).  It also makes SYS_BRK grows
 * bookkeeping within an already-backed span (sys_brk skips the redundant
 * region insert).  Costs no frames at load.  Returns 0 / -1. */
static int v2_insert_heap(struct addr_space *as) {
  if (vm_region_insert(as, USER_HEAP_BASE_V2, USER_HEAP_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_HEAP,
                       VM_MAP_PRIVATE, 0, 0) != 0) {
    uart_puts("v2 loader: heap region insert failed\n");
    return -1;
  }
  return 0;
}

/* P2.5 (S5 flip): read a flat .bin image into fresh zeroed frames mapped at
 * USER_IMG_BASE in `as` and register the IMAGE region (loader v2 and v2
 * exec share this).  The flat .bin now covers the full image span through
 * _end (.bss included; linker.ld's .tls_meta pads it), so every page the
 * program needs is backed.  Returns 0 (file cursor consumed) or -1 -- the
 * caller tears `as` down. */
static int v2_map_image(struct addr_space *as, struct file *f, uint32_t fsize) {
  if (fsize == 0 || fsize > USER_IMG_SIZE) {
    uart_puts("v2 loader: bad image size ");
    print_int((int)fsize);
    uart_puts("\n");
    return -1;
  }
  uint64_t img_len = ((uint64_t)fsize + 0xFFF) & ~0xFFFULL;

  /* IMAGE region + one mapped frame per 4 KiB page (read path). */
  if (vm_region_insert(as, USER_IMG_BASE, img_len,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC, VMK_IMG,
                       VM_MAP_PRIVATE, 0, 0) != 0) {
    uart_puts("v2 loader: image region insert failed\n");
    return -1;
  }
  uint64_t off = 0;
  int total = 0;
  while (off < img_len) {
    uint64_t fr = frame_alloc_zeroed();
    if (!fr)
      return -1;
    int n = fat16_read(f, (void *)fr, (int)FRAME_SIZE);
    if (n <= 0) {
      frame_free(fr);
      break;
    }
    if (vm_map_page(as, USER_IMG_BASE + off, fr,
                    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC, VMK_IMG) != 0) {
      frame_free(fr);
      return -1;
    }
    /* Same cache discipline as the v1 loader: clean by the identity VA,
       invalidate the I-cache shareably (the user VA is not mapped in the
       kernel's current context, so it cannot be used here). */
    __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);
    total += n;
    off += FRAME_SIZE;
  }
  uart_puts("v2 loader: image mapped, bytes=");
  print_int(total);
  uart_puts(" pages=");
  print_int((int)(off / FRAME_SIZE));
  uart_puts("\n");
  return 0;
}

/**
 * P2.2 (S2): loader v2 — the AS_V2 opt-in path (design section 8, S2).
 *
 * Reads the flat .bin image into FRESH ZEROED FRAMES (one per 4 KiB page,
 * through the ordinary read path) and maps them into the process's v2
 * IMAGE region at USER_IMG_BASE; commits the top 64 KiB of the main-stack
 * reserve; registers the IMAGE + MAIN-STACK regions in the address space.
 * Everything else (demand commit, mmap, memfd) builds on this in S3+.
 *
 * fd inheritance is not wired here: S2's only v2 program (MMTEST) uses
 * no descriptors.  Returns the pid, or -1 (pid slot released) on failure.
 */
/* P5 S5 (D4, row 86): the spawn_ex payload + pool (shared by both arch
   trap layers, which marshal the caller's pointers into the caller's slot
   while still in the caller's context; the spawn worker consumes it). */
struct ho_spawn_ex spawn_ex_pool[MAX_PROCESSES];

/* D4.1: apply the spawn_ex fd semantics to a freshly created child:
   (1) child fd table = parent copy (fs_reopen per open slot -- this is
   what INHERIT_FDS means for WebKit), (2) fdmap pairs as dup2 in the
   child (dst CLOEXEC cleared; glib's source==target trick relies on it),
   (3) the CLOEXEC sweep (map first, sweep second -- gspawn's order).
   The whole map is validated before the first mutation, so an illegal
   src/dst fails atomically (-EBADF) with nothing half-applied. */
static int spawn_ex_apply_fds(struct process *child, struct process *parent,
                              const struct ho_spawn_ex *ex) {
  for (int k = 0; k < ex->fdmap_n; k++) {
    int src = ex->fdmap_src[k];
    int dst = ex->fdmap_dst[k];
    if (!parent || src < 0 || src >= MAX_OPEN_FDS || dst < 0 ||
        dst >= MAX_OPEN_FDS || parent->open_fds[src] == -1)
      return -EBADF;
  }
  if (!parent)
    return 0;
  child->num_open_fds = 0;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    child->open_fds[i] = parent->open_fds[i];
    if (child->open_fds[i] != -1) {
      fs_reopen(child->open_fds[i]);
      child->num_open_fds++;
    }
  }
  child->fd_cloexec = parent->fd_cloexec;
  for (int k = 0; k < ex->fdmap_n; k++) {
    int src = ex->fdmap_src[k];
    int dst = ex->fdmap_dst[k];
    if (src == dst) {
      /* file_dup2's oldfd==newfd early return skips the CLOEXEC clear;
         dup2 semantics still clear it (D4.1 / D3.2). */
      child->fd_cloexec &= ~(1u << dst);
      continue;
    }
    if (file_dup2(child, src, dst) < 0)
      return -EBADF; /* validated above; stay atomic if it still fails */
  }
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (child->open_fds[i] != -1 && (child->fd_cloexec & (1u << i)))
      file_close(child, i);
  }
  return 0;
}

/* The v2 loader body, shared by spawn2 (ex == 0) and spawn_ex (ex != 0).
 * Returns the child pid or -errno; the spawn2 wrapper below maps a
 * negative result back to the historical -1 so existing callers see the
 * unchanged contract. */
static int load_v2_internal(const char* filename, int stdin_fd, int stdout_fd,
                            int stderr_fd, int caller_pid, const char *args,
                            const struct ho_spawn_ex *ex) {
  if (!filename) return -EINVAL;
  uart_puts("Loading program for scheduler (v2 AS): ");
  uart_puts(filename);
  uart_puts("\n");

  int pid = -1;
  /* Famine pacing — same contract as the v1 loop above: try immediately,
     park ~10 ms, fail fast on slot pressure (a full 63-slot table is the
     soak wedge; the caller retries).  Boot loads keep the longer budget
     for the frame-headroom wait. */
  uint64_t t_start = timer_get_ms();
  uint32_t budget_ms = (caller_pid < 0) ? LOAD_RETRY_BOOT_MS
                                        : LOAD_RETRY_CHILD_MS;
  for (;;) {
    /* Same headroom rationale as the v1 loader, in frames now: a wave
       load must leave WAVE_LOAD_RESERVE blocks' worth of frames free so
       child spawns always find memory (design section 2.2). */
    if (caller_pid < 0 &&
        frame_free_count() <= (int)WAVE_LOAD_RESERVE * FRAME_BLOCK_FRAMES) {
      /* boot headroom: wait, do not burn a reserve */;
    } else {
      pid = process_create_v2();
      if (pid >= 0)
        break;
    }
    if (timer_get_ms() - t_start >= budget_ms)
      break;
    for (volatile int spin = 0; spin < 400000; spin++) { /* ~10 ms park */
      if (timer_get_ms() - t_start >= budget_ms)
        break;
    }
  }
  if (pid < 0) {
    uart_puts("Loader (v2) starved (slot pressure): ");
    uart_puts(filename);
    uart_puts("\n");
    return -EAGAIN; /* slot/AS pressure (D4.4's -EAGAIN class) */
  }

  struct process *child = process_get_pcb(pid);
  struct addr_space *as = child ? child->as : 0;
  if (!child || !as) {
    process_free(pid);
    return -ENOMEM;
  }
  for (int i = 0; i < 31 && filename[i] != '\0'; i++) {
    child->name[i] = filename[i];
    child->name[i + 1] = '\0';
  }
  struct process *parent = process_get_pcb(caller_pid);
  if (parent)
    parent = process_group(parent);
  if (parent) {
    child->parent_pid = parent->pid;
    for (int i = 0; i < 128; i++)
      child->cwd[i] = parent->cwd[i];
    /* P6.3: a spawned child inherits the group's environment blob (set
       by execve(envp); execv() forwards `environ`, so an exec'd parent
       hands its environment down).  The spawn contract carries no envp
       -- execve() stays the explicit form.  S5 flip: the v2 loader is
       the live path, so the copy must live here too (the v1 body below
       keeps its copy for the rollback lever).  spawn_ex carries its own
       envp blob (D4.5 / D3.1: NULL = empty), so the inheritance is
       skipped for it. */
    if (!ex) {
      for (int i = 0; i < HO_ENV_LEN; i++)
        child->env[i] = parent->env[i];
      child->envc = parent->envc;
    }
  } else {
    child->cwd[0] = '/';
    child->cwd[1] = '\0';
  }
  if (ex) {
    /* D4.5/D3.1: envp as given (NULL marshalled to empty); argv[0] stored
       as given (WebKit's aux main parses argv[1..]).  Dispositions are
       all-default by construction (fresh PCB; D4.2). */
    for (int i = 0; i < HO_ENV_LEN; i++)
      child->env[i] = ex->env[i];
    child->envc = ex->envc;
    proc_set_argv_array(child, ex->argvblob, ex->argc);
    /* Flat args string for get_args() consumers: the blob's entries
       space-joined (argv blob keeps spaces, the flat string cannot). */
    int pos = 0, fi = 0;
    for (int k = 0; k < ex->argc && fi < 255; k++) {
      while (pos < HO_EXEC_ARGV_LEN && ex->argvblob[pos] && fi < 255)
        child->args[fi++] = ex->argvblob[pos++];
      if (pos < HO_EXEC_ARGV_LEN && ex->argvblob[pos] == '\0')
        pos++;
      if (k + 1 < ex->argc && fi < 255)
        child->args[fi++] = ' ';
    }
    child->args[fi] = '\0';
  }
  if (!ex) {
    int ai = 0;
    if (args) {
      while (args[ai] && ai < 255) {
        child->args[ai] = args[ai];
        ai++;
      }
    }
    child->args[ai] = '\0';
    char flat[320];
    int fi = 0;
    for (int i = 0; child->name[i] && fi < 63; i++)
      flat[fi++] = child->name[i];
    if (fi)
      flat[fi++] = ' ';
    for (int i = 0; child->args[i] && fi < 315; i++)
      flat[fi++] = child->args[i];
    flat[fi] = '\0';
    proc_split_argv(child, flat);
  }

  /* P2.5 (S5 flip): spawn fd inheritance.  The v1 loader's wiring applies
     verbatim to a v2 PCB -- fds are group state, not AS state -- and the
     utility-test programs (CUTTEST/…) and PIPETEST/SHTEST spawn children
     with pipe ends on fd 0/1/2.  spawn_ex (ex != 0) replaces this block
     with the D4.1 copy+map+sweep, applied after the image load below. */
  if (ex) {
    ; /* spawn_ex: see the D4.1 block after the image/region setup */
  } else if (parent && child) {
    if (stdin_fd >= 0 && stdin_fd < MAX_OPEN_FDS &&
        parent->open_fds[stdin_fd] != -1) {
      child->open_fds[0] = parent->open_fds[stdin_fd];
      fs_reopen(child->open_fds[0]);
      child->num_open_fds++;
    }
    if (stdout_fd >= 0 && stdout_fd < MAX_OPEN_FDS &&
        parent->open_fds[stdout_fd] != -1) {
      child->open_fds[1] = parent->open_fds[stdout_fd];
      fs_reopen(child->open_fds[1]);
      child->num_open_fds++;
    }
    if (stderr_fd >= 0 && stderr_fd < MAX_OPEN_FDS &&
        parent->open_fds[stderr_fd] != -1) {
      child->open_fds[2] = parent->open_fds[stderr_fd];
      fs_reopen(child->open_fds[2]);
      child->num_open_fds++;
    } else {
      /* Default: inherit the parent's fd 2 only when it is NOT a pipe
         (same rule and rationale as the v1 loader: a duplicated pipe end
         keeps reader/writer counts alive and self-deadlocks full pipes). */
      int gf = parent->open_fds[2];
      if (gf != -1 && !file_gfd_is_pipe(gf)) {
        child->open_fds[2] = gf;
        fs_reopen(gf);
        child->num_open_fds++;
      }
    }
  }

  struct file f;
  if (fat16_open(filename, &f) != 0) {
    uart_puts("Failed to open file: ");
    uart_puts(filename);
    uart_puts("\n");
    process_free(pid);
    return -ENOENT;
  }
  uint32_t fsize = f.fat16.entry.file_size;
  if (fsize == 0) {
    /* D4.4: a missing image surfaces as a zero-size entry (fat16_open
       creates on miss), and an empty image can never be executed -- the
       documented -ENOENT.  The spawn2 wrapper below folds it back to -1. */
    fat16_close(&f);
    process_free(pid);
    return -ENOENT;
  }
  if (v2_map_image(as, &f, fsize) != 0) {
    fat16_close(&f);
    process_free(pid);
    return -ENOEXEC;
  }
  fat16_close(&f);

  /* Main-stack region (the 64 KiB guard below stays a hole).  S3: demand
     materializes every stack page on first touch, so the loader only
     creates the region -- the old eager top-64-KiB commit is gone (the
     design's "committed on load: IMAGE only"). */
  if (vm_region_insert(as, USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_STACK,
                       VM_MAP_PRIVATE, 0, 0) != 0) {
    uart_puts("v2 loader: stack region insert failed\n");
    process_free(pid);
    return -ENOMEM;
  }
  /* P2.5 (S5 flip): the heap slot, demand-zero (the user malloc's arena). */
  if (v2_insert_heap(as) != 0) {
    process_free(pid);
    return -ENOMEM;
  }

  /* P5 S5 (D4.1): spawn_ex fd semantics -- after a successful image load
     (a load failure must not leak the reopened fd references) and before
     the child is made runnable (process_set_entry below).  -EBADF fails
     the whole spawn atomically and releases the child slot. */
  if (ex && spawn_ex_apply_fds(child, parent, ex) != 0) {
    process_free(pid);
    return -EBADF;
  }

#ifdef __x86_64__
  process_set_entry(pid, USER_IMG_BASE, USER_MAIN_STK_TOP_V2 - 8);
#else
  process_set_entry(pid, USER_IMG_BASE, USER_MAIN_STK_TOP_V2);
#endif
  uart_puts("v2 loader: PID=");
  print_int(pid);
  uart_puts(" entry=");
  uart_print_hex(USER_IMG_BASE);
  uart_puts(" sp=");
  uart_print_hex(USER_MAIN_STK_TOP_V2);
  uart_puts(" resident=");
  print_int((int)as->resident_frames);
  uart_puts(" tables=");
  print_int((int)as->table_frames);
  uart_puts("\n");
  return pid;
}

/* The spawn2/v1-compat entry (spawn2's worker path): unchanged contract
 * (child pid or -1).  The v2 loader body above reports negative errnos
 * for spawn_ex; fold them back to the historical -1 here. */
int load_and_run_program_v2(const char* filename, int stdin_fd,
                            int stdout_fd, int stderr_fd, int caller_pid,
                            const char *args) {
  int r = load_v2_internal(filename, stdin_fd, stdout_fd, stderr_fd,
                           caller_pid, args, 0);
  return r < 0 ? -1 : r;
}

/* P5 S5 (D4, row 86): the spawn_ex loader (both arch spawn_ex workers call
 * this).  Returns the child pid or -errno: -ENOENT (no image), -ENOEXEC
 * (bad image), -EBADF (illegal fdmap, atomic), -EAGAIN (slot/AS pressure),
 * -ENOMEM (frames/regions).  The caller's parked WAIT_SPAWN context is
 * released by the caller's spawn worker with this value. */
int load_and_run_spawn_ex(const struct ho_spawn_ex *ex) {
  if (!ex)
    return -EINVAL;
  return load_v2_internal(ex->filename, -1, -1, -1, ex->caller_pid, 0, ex);
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
                         const char *args, const char *new_name,
                         const char *env, int envc,
                         const char *argvblob, int argc) {
  struct process *cur = current_process();
  if (!cur)
    return -EINVAL;
  if (!path)
    return -EINVAL;

  /* P1 (D7): cwd and address space are group state; exec replaces the
     group's image.  P5 (D3.4): from a secondary thread (or with live
     siblings) POSIX requires every other thread to die first -- the
     caller survives as the new single-threaded image. */
  struct process *grp = process_group(cur);

  /* Resolve a relative path against the process cwd (which the shell
     keeps as e.g. "/" or "/subdir" — no trailing slash). */
  char abs[128];
  int al = 0;
  if (path[0] == '/') {
    for (al = 0; path[al] && al < 126; al++) abs[al] = path[al];
  } else {
    int cl = 0;
    for (; grp->cwd[cl] && cl < 96; cl++) abs[cl] = grp->cwd[cl];
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

  /* D3.4: past this point the image replaces the old one, so the other
     threads must already be gone (their next tick would execute clobbered
     memory).  A later load failure leaves the caller single-threaded on
     the old image -- documented deviation from "no state change". */
  if (cur->is_thread || grp->live_threads > 1)
    process_exec_terminate_siblings(grp, cur);

  /* P2.5 (S5 flip): v2 exec -- replace the group's AS with a fresh one
     holding the new image (fresh frames at USER_IMG_BASE + the main-stack
     reserve) and redirect the live trap frame.  The AS pool is
     pid-indexed, so a second slot is not available while the old AS
     lives: restore the kernel table, tear the old AS down, rebuild in the
     same slot.  A failure past that point leaves the group AS-less, which
     kills it on the next user fault -- the same effective contract as the
     v1 path below, whose failed read leaves a zeroed image.  Sibling
     threads keep the old image (documented P1 divergence, as for v1). */
  if (grp->as) {
    vm_arch_restore_kernel();
    struct addr_space *old = grp->as;
    grp->as = 0;
    vm_as_teardown(old);
    struct addr_space *nas = vm_as_create((uint64_t)grp->pid);
    if (!nas) {
      fat16_close(&f);
      return -ENOMEM;
    }
    if (v2_map_image(nas, &f, f.fat16.entry.file_size) != 0) {
      fat16_close(&f);
      vm_as_teardown(nas);
      return -ENOEXEC;
    }
    fat16_close(&f);
    if (vm_region_insert(nas, USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_SIZE,
                         VM_PROT_READ | VM_PROT_WRITE, VMK_STACK,
                         VM_MAP_PRIVATE, 0, 0) != 0) {
      vm_as_teardown(nas);
      return -ENOMEM;
    }
    if (v2_insert_heap(nas) != 0) {
      vm_as_teardown(nas);
      return -ENOMEM;
    }
    int vi;
    for (vi = 0; new_name && new_name[vi] && vi < 31; vi++)
      cur->name[vi] = new_name[vi];
    cur->name[vi] = '\0';
    for (vi = 0; args && args[vi] && vi < 255; vi++)
      cur->args[vi] = args[vi];
    cur->args[vi] = '\0';
    grp->as = nas;
    vm_arch_switch(nas);
    grp->heap_brk = USER_HEAP_BASE_V2;
    grp->anon_map_count = 0;
    /* Exec resets the TLS register state: the incoming image has NOT
       installed a TLS block, and a fork-inherited foreign TP (the parent
       shell's image) must not leak into it -- otherwise the new program's
       lazy __errno_location() sees TP != 0, skips its install, and its
       first errno store lands at [old image's TLS + offset], a hole in
       the new image (observed: mkdir killed writing errno=EEXIST at
       0x100002C3D0 after fork+exec from the shell). */
    grp->tls_base = 0;
    /* P5 (D3.1-D3.3): the same success-path apply as the v1 tail --
       environment, signal-state reset, FD_CLOEXEC sweep. */
    process_exec_apply(grp, env, envc);
    /* P5 (D3-order): install the pre-load marshalled argv[] blob. */
    proc_set_argv_array(grp, argvblob, argc);
    tf->elr = USER_IMG_BASE;
#ifdef __x86_64__
    grp->context[33] = USER_MAIN_STK_TOP_V2 - 8; /* SysV: 16n+8 at entry */
#else
    grp->context[33] = USER_MAIN_STK_TOP_V2;
#endif
    arch_set_user_sp(grp->context[33]);
    tf->regs[0] = 0;
    return 0;
  }

  uint64_t base = grp->user_phys_base;

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
    grp->name[i] = new_name[i];
  grp->name[i] = '\0';
  for (i = 0; args && args[i] && i < 255; i++)
    grp->args[i] = args[i];
  grp->args[i] = '\0';

  /* Exec resets the address-space state: fresh heap top, no anonymous
     mappings (the new image's data/bss start at the load cap), and NO TLS
     register: a fork-inherited foreign TP would defeat the new image's
     lazy/crt0 TLS install (same 0x2C3D0-class errno-hole as the v2 path). */
  grp->tls_base = 0;
  grp->heap_brk = USER_HEAP_BASE;
  grp->anon_map_count = 0;
  for (int i = 0; i < USER_ANON_MAX_REGS; i++) {
    grp->anon_maps[i].addr = 0;
    grp->anon_maps[i].len = 0;
  }

  /* P5 (D3.1-D3.3): the new environment, the signal-state reset and the
     FD_CLOEXEC sweep -- all success-path only. */
  process_exec_apply(grp, env, envc);

  /* P5 (D3-order): install the pre-load marshalled argv[] blob; empty
     leaves crt0 on its name+flat-args fallback. */
  proc_set_argv_array(grp, argvblob, argc);

  /* Redirect the running process into the fresh image. regs[0]=0 is the
     exec() success return that the new program never actually reads. */
  tf->elr = USER_VIRT_BASE;
  tf->regs[0] = 0;
  return 0;
}

/* --- SYS_GETARGV plumbing (see process.h struct process.eargv) --- */

/* Split a flat, space-separated args string into the process's argv blob,
 * with the same whitespace semantics as user-space parse_args().  Used at
 * spawn time so every spawned program exposes a positional-parameter
 * array (argv[0] is prepended by the caller: the binary name). */
void proc_split_argv(struct process *p, const char *args) {
  p = process_group(p); /* P1 (D7): the argv blob is group state */
  int narg = 0;
  int pos = 0;
  p->eargc = 0;
  p->eargv[0] = '\0';
  const char *s = args ? args : "";
  while (*s && narg < HO_EXEC_MAX_ARGS) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
      s++;
    if (!*s)
      break;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r'
           && pos < HO_EXEC_ARGV_LEN - 1) {
      p->eargv[pos++] = *s++;
    }
    if (pos < HO_EXEC_ARGV_LEN)
      p->eargv[pos++] = '\0';
    narg++;
  }
  if (pos < HO_EXEC_ARGV_LEN)
    p->eargv[pos] = '\0';
  p->eargc = narg;
}

/* P2.5 (S5): is [addr, addr+len) readable (write=0) / writable (write=1)
 * caller user space?  A v2 process answers to the address-space walk --
 * vm_range_ok demand-commits the pages (S2 semantics), so a raw access
 * after this check is safe; v1 keeps the flat-window test. */
static int pl_user_ok(struct process *p, uint64_t addr, uint64_t len,
                      int write) {
  if (len == 0)
    len = 1;
  if (p && p->as)
    return vm_range_ok(p, addr, len, write) == 0;
  if (addr < USER_VIRT_BASE || addr + len - 1 >= USER_VIRT_BASE + USER_REGION_SIZE)
    return 0;
  return 1;
}

/* Copy a NUL-terminated user string into kernel memory with full bounds
 * checking (mirrors the static u_strcpy in each trap.c). Returns 1 on
 * success (dst NUL-terminated), 0 on bad/out-of-range pointer. */
static int pl_strcpy(struct process *p, const char *src, char *dst, int cap) {
  if (!src || cap <= 1)
    return 0;
  if (!pl_user_ok(p, (uint64_t)src, (uint64_t)cap, 0))
    return 0;
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
  return 1;
}

/* Copy a caller-space argv[] array into a kernel blob BEFORE exec replaces
 * the image (P5 D3-order fix: the elements live in the OLD image and are
 * clobbered by the load, so they must be read early).  Layout matches the
 * process argv blob (NUL-separated, 63-char elements, <= max entries).
 * argv may be NULL; a bad element stops the copy (the caller falls back to
 * name + flat args, as before).  Always returns 0; *count_out gets the
 * number of elements captured. */
int proc_marshal_argv(struct process *p, char *const *argv, char *dst, int cap,
                      int *count_out) {
  if (!dst || !count_out || cap <= 0)
    return -1;
  p = process_group(p);
  *count_out = 0;
  dst[0] = '\0';
  if (!argv)
    return 0;
  int pos = 0;
  for (int ai = 0; ai < HO_EXEC_MAX_ARGS; ai++) {
    /* Validate the array slot itself before the raw load below. */
    if (!pl_user_ok(p, (uint64_t)(argv + ai), sizeof(char *), 0))
      break;
    if (argv[ai] == 0)
      break;
    char one[64];
    if (!pl_strcpy(p, (const char *)argv[ai], one, sizeof one))
      break;
    if (pos >= cap - 1)
      break;
    for (int k = 0; one[k] && pos < cap - 1; k++)
      dst[pos++] = one[k];
    if (pos < cap)
      dst[pos++] = '\0';
    (*count_out)++;
  }
  if (pos < cap)
    dst[pos] = '\0';
  return 0;
}

/* Install a marshalled argv blob (proc_marshal_argv) as group state; runs
 * on the exec success path.  argv may be NULL/empty (leaves eargc == 0 so
 * crt0 falls back to name+args). */
int proc_set_argv_array(struct process *p, const char *blob, int count) {
  p = process_group(p); /* P1 (D7): the argv blob is group state */
  p->eargc = 0;
  p->eargv[0] = '\0';
  if (!blob || count <= 0)
    return 0;
  int pos = 0;
  for (int k = 0; k < count && pos < HO_EXEC_ARGV_LEN - 1; k++) {
    while (pos < HO_EXEC_ARGV_LEN - 1 && blob[pos]) {
      p->eargv[pos] = blob[pos];
      pos++;
    }
    if (pos < HO_EXEC_ARGV_LEN - 1) {
      p->eargv[pos] = '\0';
      pos++;
    }
  }
  p->eargv[pos] = '\0';
  p->eargc = count;
  return 0;
}

/* Syscall 64 (SYS_GETARGV): read the process's positional parameters.
 * idx == -1 returns the argument count (>= 0).  idx >= 0 copies the
 * idx-th argument (NUL-terminated) into buf (size bytes) and returns the
 * number of bytes copied (excluding the NUL), or -1 when idx is out of
 * range. */
int sys_readargv(struct process *p, int idx, char *buf, int size) {
  p = process_group(p); /* P1 (D7): the argv blob is group state */
  if (!p)
    return -1;
  if (idx == -1)
    return p->eargc;
  if (idx < 0 || idx >= p->eargc)
    return -1;
  int pos = 0;
  for (int ai = 0; ai < idx; ai++) {
    while (pos < HO_EXEC_ARGV_LEN && p->eargv[pos])
      pos++;
    if (pos < HO_EXEC_ARGV_LEN)
      pos++; /* skip the NUL */
  }
  int len = 0;
  while (pos + len < HO_EXEC_ARGV_LEN && p->eargv[pos + len])
    len++;
  if (len == 0)
    return -1;
  if (size <= 0)
    return len;
  int n = len < size - 1 ? len : size - 1;
  for (int i = 0; i < n; i++)
    buf[i] = p->eargv[pos + i];
  if (n > 0)
    buf[n] = '\0';
  return n;
}
