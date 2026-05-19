#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

int main() {
    FILE *f = fopen("disk.img", "rb");
    if (!f) return 1;
    fseek(f, 0x100000, SEEK_SET); // Assume root dir is somewhere here? Wait, we can parse the BPB.
    
    uint8_t bpb[512];
    fseek(f, 0, SEEK_SET);
    fread(bpb, 1, 512, f);
    
    uint16_t reserved = *(uint16_t*)&bpb[14];
    uint8_t fats = bpb[16];
    uint16_t root_entries = *(uint16_t*)&bpb[17];
    uint16_t fat_size = *(uint16_t*)&bpb[22];
    
    uint32_t root_dir_offset = (reserved + fats * fat_size) * 512;
    printf("Root dir at offset %u\n", root_dir_offset);
    
    fseek(f, root_dir_offset, SEEK_SET);
    for (int i = 0; i < 32; i++) {
        uint8_t entry[32];
        fread(entry, 1, 32, f);
        if (entry[0] == 0) {
            printf("Entry %d: EMPTY (0x00)\n", i);
            break;
        }
        if (entry[0] == 0xE5) {
            printf("Entry %d: DELETED\n", i);
            continue;
        }
        if (entry[11] == 0x0F) {
            printf("Entry %d: LFN\n", i);
            continue;
        }
        printf("Entry %d: %11.11s\n", i, entry);
    }
    fclose(f);
    return 0;
}
