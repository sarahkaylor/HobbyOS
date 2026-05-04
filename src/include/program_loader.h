#ifndef PROGRAM_LOADER_H
#define PROGRAM_LOADER_H
#include <stdint.h>

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
int load_and_run_program_in_scheduler(const char* filename, int stdin_fd, int stdout_fd);

#endif // PROGRAM_LOADER_H