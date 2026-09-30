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
int load_and_run_program_v2(const char* filename, int stdin_fd,
                            int stdout_fd, int stderr_fd, int caller_pid,
                            const char *args) {
  (void)stdin_fd;
  (void)stdout_fd;
  (void)stderr_fd;
  if (!filename) return -1;
  uart_puts("Loading program for scheduler (v2 AS): ");
  uart_puts(filename);
  uart_puts("\n");

  int pid = -1;
  for (int attempt = 0; attempt < 18000; attempt++) {
    uint64_t t0 = timer_get_ms();
    for (volatile int spin = 0; spin < 4000000; spin++) {
      if (timer_get_ms() - t0 >= 100u) break;
    }
    /* Same headroom rationale as the v1 loader, in frames now: a wave
       load must leave WAVE_LOAD_RESERVE blocks' worth of frames free so
       child spawns always find memory (design section 2.2). */
    if (caller_pid < 0 &&
        frame_free_count() <= (int)WAVE_LOAD_RESERVE * FRAME_BLOCK_FRAMES)
      continue;
    pid = process_create_v2();
    if (pid >= 0)
      break;
  }
  if (pid < 0) {
    uart_puts("Loader (v2) starved: ");
    uart_puts(filename);
    uart_puts("\n");
    return -1;
  }

  struct process *child = process_get_pcb(pid);
  struct addr_space *as = child ? child->as : 0;
  if (!child || !as) {
    process_free(pid);
    return -1;
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
  } else {
    child->cwd[0] = '/';
    child->cwd[1] = '\0';
  }
  int ai = 0;
  if (args) {
    while (args[ai] && ai < 255) {
      child->args[ai] = args[ai];
      ai++;
    }
  }
  child->args[ai] = '\0';
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

  struct file f;
  if (fat16_open(filename, &f) != 0) {
    uart_puts("Failed to open file: ");
    uart_puts(filename);
    uart_puts("\n");
    process_free(pid);
    return -1;
  }
  uint32_t fsize = f.fat16.entry.file_size;
  if (fsize == 0 || fsize > USER_IMG_SIZE) {
    uart_puts("v2 loader: bad image size ");
    print_int((int)fsize);
    uart_puts("\n");
    fat16_close(&f);
    process_free(pid);
    return -1;
  }
  uint64_t img_len = ((uint64_t)fsize + 0xFFF) & ~0xFFFULL;

  /* IMAGE region + one mapped frame per 4 KiB page (read path). */
  if (vm_region_insert(as, USER_IMG_BASE, img_len,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC, VMK_IMG,
                       VM_MAP_PRIVATE, 0, 0) != 0) {
    uart_puts("v2 loader: image region insert failed\n");
    fat16_close(&f);
    process_free(pid);
    return -1;
  }
  uint64_t off = 0;
  int total = 0;
  while (off < img_len) {
    uint64_t fr = frame_alloc_zeroed();
    if (!fr)
      goto fail;
    int n = fat16_read(&f, (void *)fr, (int)FRAME_SIZE);
    if (n <= 0) {
      frame_free(fr);
      break;
    }
    if (vm_map_page(as, USER_IMG_BASE + off, fr,
                    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC, VMK_IMG) != 0) {
      frame_free(fr);
      goto fail;
    }
    /* Same cache discipline as the v1 loader: clean by the identity VA,
       invalidate the I-cache shareably (the user VA is not mapped in the
       kernel's current context, so it cannot be used here). */
    __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);
    total += n;
    off += FRAME_SIZE;
  }
  fat16_close(&f);
  uart_puts("v2 loader: image mapped, bytes=");
  print_int(total);
  uart_puts(" pages=");
  print_int((int)(off / FRAME_SIZE));
  uart_puts("\n");

  /* Main-stack region (the 64 KiB guard below stays a hole).  No demand
     in S2: commit the top 64 KiB eagerly; S3 grows the rest on fault. */
  if (vm_region_insert(as, USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_STACK,
                       VM_MAP_PRIVATE, 0, 0) != 0) {
    uart_puts("v2 loader: stack region insert failed\n");
    process_free(pid);
    return -1;
  }
  for (uint64_t a = USER_MAIN_STK_TOP_V2 - 64 * 1024; a < USER_MAIN_STK_TOP_V2;
       a += FRAME_SIZE) {
    uint64_t fr = frame_alloc_zeroed();
    if (!fr)
      goto fail;
    if (vm_map_page(as, a, fr, VM_PROT_READ | VM_PROT_WRITE, VMK_STACK) != 0) {
      frame_free(fr);
      goto fail;
    }
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

fail:
  uart_puts("v2 loader: out of frames / map failure for ");
  uart_puts(filename);
  uart_puts("\n");
  fat16_close(&f);
  process_free(pid);
  return -1;
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

  /* P1 (D7): cwd and address space are group state; exec replaces the
     group's image.  (Exec from a secondary thread is a documented P1
     divergence: siblings would keep running the old image.) */
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
    cur->name[i] = new_name[i];
  cur->name[i] = '\0';
  for (i = 0; args && args[i] && i < 255; i++)
    cur->args[i] = args[i];
  cur->args[i] = '\0';

  /* Exec resets the address-space state: fresh heap top, no anonymous
     mappings (the new image's data/bss start at the load cap). */
  grp->heap_brk = USER_HEAP_BASE;
  grp->anon_map_count = 0;
  for (int i = 0; i < USER_ANON_MAX_REGS; i++) {
    grp->anon_maps[i].addr = 0;
    grp->anon_maps[i].len = 0;
  }

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

/* Copy a NUL-terminated user string into kernel memory with full bounds
 * checking (mirrors the static u_strcpy in each trap.c). Returns 1 on
 * success (dst NUL-terminated), 0 on bad/out-of-range pointer. */
static int pl_strcpy(const char *src, char *dst, int cap) {
  uint64_t base = (uint64_t)src;
  if (!src || base < USER_VIRT_BASE ||
      base >= USER_VIRT_BASE + USER_REGION_SIZE)
    return 0;
  if (base + cap - 1 >= USER_VIRT_BASE + USER_REGION_SIZE)
    return 0;
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
  return 1;
}

/* Copy a caller-space argv[] array into the process's argv blob at exec
 * time.  Unlike the space-joined flat args string, each element keeps its
 * own length, so quoted words that contain spaces round-trip exactly.
 * argv may be NULL (leaves eargc == 0 so crt0 falls back to name+args).
 * Elements are truncated at 63 chars (matching the flat-args path). */
int proc_set_argv_array(struct process *p, char *const *argv) {
  p = process_group(p); /* P1 (D7): the argv blob is group state */
  p->eargc = 0;
  p->eargv[0] = '\0';
  if (!argv)
    return 0;
  int pos = 0;
  for (int ai = 0; ai < HO_EXEC_MAX_ARGS; ai++) {
    if (argv[ai] == 0)
      break;
    char one[64];
    if (!pl_strcpy((const char *)argv[ai], one, sizeof one))
      break;
    if (pos >= HO_EXEC_ARGV_LEN)
      break;
    for (int k = 0; one[k] && pos < HO_EXEC_ARGV_LEN - 1; k++)
      p->eargv[pos++] = one[k];
    if (pos < HO_EXEC_ARGV_LEN)
      p->eargv[pos++] = '\0';
    p->eargc++;
  }
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
