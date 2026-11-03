#ifndef PROGRAM_LOADER_H
#define PROGRAM_LOADER_H
#include <stdint.h>

/* GB-013 (ELF loader mapper).  Declared here (not in a new header) because the
 * only caller is load_v2_internal in program_loader.c; the parser's structs
 * live in elf_image.h.  See docs/elf-loader.md §2 and docs/shared-libs.md §4
 * (lane B: aarch64 mapper, PT_INTERP, startup block).
 *
 * The loader used to report (entry, phdr, phnum) through three out-parameters;
 * the M0 freeze added the interpreter base, the executable base, phent and the
 * user stack pointer, so they travel as one struct.  Everything zero/-1 on a
 * refusal — the caller only reads it on rc == 0. */
struct addr_space;

struct elf_load_info {
  uint64_t entry;       /* runtime entry to start at (interp_base + interp e_entry
                           when interpreted, else base + e_entry)              */
  uint64_t exe_entry;   /* runtime entry of the EXECUTABLE (base + e_entry), even
                           when `entry` is the interpreter's: the compat auxv's
                           AT_ENTRY is this value, never the interpreter's      */
  uint64_t base;        /* runtime base of the executable image                */
  uint64_t phdr;        /* runtime VA of the executable's program headers      */
  uint32_t phnum;       /* number of program headers                           */
  uint32_t phent;       /* sizeof(Elf64_Phdr) == 56                            */
  uint64_t interp_base; /* runtime base of the interpreter (0 if none)         */
  uint64_t sp;          /* user stack pointer the process should start with    */
  int interpreted;      /* 1 when a PT_INTERP interpreter was mapped           */
  int native;           /* 1 = native track (no PROC_FLAG_LINUX_ABI), 0 = compat */
  int parse_rc;         /* ELF_OK, or the ELF_ERR_* that ended the load        */
};

/* Loader flags.  ELF_LOAD_NATIVE is the native track: PT_INTERP is honoured
 * (only when it names /LDHOBBY.SO) and the startup block is written.  The
 * default (0) is the Linux-ABI compat track, which keeps GB-013's behaviour
 * byte for byte — including its ELF_ERR_INTERP refusal of dynamic binaries.
 *
 * ELF_LOAD_COMPAT_INTERP is the explicit LINUX opt-in: PT_INTERP is honoured
 * for ANY absolute path (glibc's /lib64/ld-linux-x86-64.so.2 included), the
 * interpreter is mapped in a USER_LIB slot with its own p_flags and WITHOUT
 * the kernel's relocation pass (a real ld.so relocates itself), and the
 * caller is responsible for the Linux SysV startup stack
 * (docs/rw-ldso-design.md).  flags == 0 still refuses PT_INTERP, so no
 * existing caller changes behaviour by accident. */
#define ELF_LOAD_NATIVE 0x1u
#define ELF_LOAD_COMPAT_INTERP 0x2u

int elf_loader_load(struct addr_space *as, uint16_t cluster, uint32_t fsize,
                    uint32_t flags, struct elf_load_info *info);

/* The same load, for an image that is already in kernel memory (the VFS/NFS
 * exec path: no FAT16 cluster exists for the file).  The PT_INTERP image is
 * still resolved from the FAT16 root.  elf_loader_load is the FAT16 wrapper
 * around this. */
int elf_loader_load_image(struct addr_space *as, const uint8_t *exe,
                          uint32_t fsize, uint32_t flags,
                          struct elf_load_info *info);

/* The mapping half without a filesystem: both images are already in kernel
 * memory.  Used by elf_loader_test.c to exercise the real mapper (plan,
 * regions, p_flags, relocations, USER_LIB placement, startup block) on both
 * arches; elf_loader_load is the FS-facing wrapper around the same code. */
int elf_loader_map_buffers(struct addr_space *as, const uint8_t *exe,
                           uint64_t exe_len, const uint8_t *interp,
                           uint64_t interp_len, uint32_t flags,
                           struct elf_load_info *info);

/* 1 when the file at (cluster, fsize) carries a PT_INTERP naming the native
 * interpreter, 0 when it does not (or its header is unreadable), -1 when the
 * file cannot be read.  The x86_64 dispatch uses it to pick the track. */
int elf_loader_probe_native(uint16_t cluster, uint32_t fsize);

/* Buffer form of elf_loader_probe_native(), for the VFS exec path. */
int elf_loader_probe_native_buf(const uint8_t *exe, uint32_t fsize);

/**
 * Loads a user program from the disk and executes it in the current context.
 * Bypasses the scheduler.
 *
 * Returns:
 *   0 on success, -1 on failure.
 */
int load_and_run_program(const char* filename);

/**
 * Loads a program file from the filesystem into a newly allocated memory buffer.
 *
 * Parameters:
 *   filename - Name of the file on disk.
 *   buffer   - Output pointer for the memory location.
 *
 * Returns:
 *   The size of the loaded program on success, or -1 on failure.
 */
int load_program_to_memory(const char* filename, void** buffer);

/**
 * Loads a user program from disk and registers it with the kernel scheduler.
 *
 * Returns:
 *   The PID of the new process, or -1 on failure.
 */
int load_and_run_program_in_scheduler(const char* filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid);
int load_and_run_program_in_scheduler_args(const char* filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid, const char *args);

/**
 * P2.2 (S2): loader v2 — the AS_V2 opt-in path.  Reads the flat .bin into
 * fresh 4 KiB frames and maps them in the process's v2 IMAGE region at
 * USER_IMG_BASE (no 32 MiB block).  Used by the MMTEST wiring in main.c.
 *
 * Returns the PID of the new process, or -1 on failure.
 */
int load_and_run_program_v2(const char* filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid, const char *args);

#endif // PROGRAM_LOADER_H
