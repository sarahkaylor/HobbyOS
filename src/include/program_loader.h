#ifndef PROGRAM_LOADER_H
#define PROGRAM_LOADER_H
#include <stdint.h>

// Load a user program from the disk and execute it
// Returns 0 on success, -1 on failure
int load_and_run_program(const char* filename);

// Load a program into memory without executing it (for debugging/analysis)
int load_program_to_memory(const char* filename, void** buffer);

#endif