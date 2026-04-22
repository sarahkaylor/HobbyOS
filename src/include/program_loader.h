#ifndef PROGRAM_LOADER_H
#define PROGRAM_LOADER_H
#include <stdint.h>

// Load a user program from the disk and execute it (legacy sequential mode)
// Returns 0 on success, -1 on failure
int load_and_run_program(const char* filename);

// Load a program into memory without executing it (for debugging/analysis)
int load_program_to_memory(const char* filename, void** buffer);

// Load a user program and register it with the scheduler.
// The program won't execute until start_scheduler() is called.
// Returns the assigned PID, or -1 on failure.
int load_and_run_program_in_scheduler(const char* filename);

#endif