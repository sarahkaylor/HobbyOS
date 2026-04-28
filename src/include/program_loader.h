#ifndef PROGRAM_LOADER_H
#define PROGRAM_LOADER_H
#include <stdint.h>

// Load a user program from the disk and execute it in the current context.
// Note: This is primarily for legacy sequential testing.
// Returns 0 on success, -1 on failure.
int load_and_run_program(const char* filename);

// Load a program file from the filesystem into a newly allocated memory buffer.
// buffer: Pointer to a void* that will receive the allocated buffer address.
// Returns the size of the loaded program on success, or -1 on failure.
int load_program_to_memory(const char* filename, void** buffer);

// Load a user program from disk and register it with the kernel scheduler.
// The program is initialized but won't run until the scheduler is started.
// Returns the PID of the new process, or -1 on failure.
int load_and_run_program_in_scheduler(const char* filename);

#endif // PROGRAM_LOADER_H