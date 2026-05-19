#include <stdio.h>
#include <stdint.h>

struct file {
    uint32_t type;
    int ref_count;
    char name[32];
    uint32_t size;
    uint32_t offset;
    int parent_dir_cluster;
    int start_cluster;
    union {
        struct {
            void* pcb;
            int connected;
        } socket;
        struct {
            int read_fd;
            int write_fd;
        } pipe;
    };
};

struct file global_file_table[256];

int main() {
    printf("global_file_table size: %lu\n", sizeof(struct file));
    for (int i = 0; i < 256; i++) {
        uint64_t addr = (uint64_t)&global_file_table[i];
        if (addr == 0x105 || (addr & 0xFFFF) == 0x105) {
            printf("Found 0x105 at index %d!\n", i);
        }
    }
    printf("Done.\n");
    return 0;
}
