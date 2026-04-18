#include "fat16.h"
#include "virtio_blk.h"

#define MAX_OPEN_FILES 4
#define SECTOR_SIZE 512

extern void uart_puts(const char* s);

static uint32_t bpb_bytes_per_sector;
static uint32_t bpb_sectors_per_cluster;
static uint32_t bpb_reserved_sectors;
static uint32_t bpb_fat_count;
static uint32_t bpb_root_dir_entries;
static uint32_t bpb_sectors_per_fat;

struct fat16_dir_entry {
    char name[11];
    uint8_t attr;
    uint8_t reserved[10];
    uint16_t time;
    uint16_t date;
    uint16_t start_cluster;
    uint32_t file_size;
} __attribute__((packed));

static uint32_t fat_sector;
static uint32_t root_dir_sector;
static uint32_t root_dir_sectors;
static uint32_t data_sector;
static uint32_t cluster_size;

typedef struct {
    int active;
    struct fat16_dir_entry entry;
    uint32_t dir_sector;
    uint32_t dir_offset;
    uint32_t cursor;
} file_descriptor;

static file_descriptor open_files[MAX_OPEN_FILES];

// Helper: match 8.3 filename
static int match_name(const char* fat_name, const char* query) {
    char formatted[11];
    for (int i = 0; i < 11; i++) formatted[i] = ' ';
    int i = 0, j = 0;
    while (query[i] && query[i] != '.' && j < 8) formatted[j++] = query[i++];
    if (query[i] == '.') {
        i++; j = 8;
        while (query[i] && j < 11) formatted[j++] = query[i++];
    }
    for (int k = 0; k < 11; k++) {
        char a = fat_name[k]; char b = formatted[k];
        if (a >= 'a' && a <= 'z') a -= 32; // Uppercase
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

int fat16_init(void) {
    uint8_t buf[SECTOR_SIZE];
    if (virtio_blk_read_sector(0, buf, 1) != 0) {
        return -1;
    }
    
    // [CRITICAL DESIGN DECISION - UNALIGNED ACCESS TRAPS]
    // Because the OS operates natively without enabling the Memory Management Unit (MMU),
    // AArch64 defaults the environment to strictly `Device-nGnRnE` memory constraints.
    // Standard LLVM Clang optimizations will detect structures like `((uint8_t*)&bpb)[...` 
    // or packed struct variable copies, reducing them down to grouped 16/32-bit loads (`ldrh`).
    // Attempting to `ldrh` from an odd byte boundary (e.g. offset 11) within strict Device Memory
    // throws an immediate execution abort via Synchronous Exception.
    // By casting the fetch target via `volatile uint8_t*` and bit-shifting piecemeal,
    // we explicitly restrict Clang to use individual byte loads (`ldrb`), remaining safe across boundaries.
    volatile uint8_t* vbuf = (volatile uint8_t*)buf;
    bpb_bytes_per_sector = vbuf[11] | (vbuf[12] << 8);
    bpb_sectors_per_cluster = vbuf[13];
    bpb_reserved_sectors = vbuf[14] | (vbuf[15] << 8);
    bpb_fat_count = vbuf[16];
    bpb_root_dir_entries = vbuf[17] | (vbuf[18] << 8);
    bpb_sectors_per_fat = vbuf[22] | (vbuf[23] << 8);
    
    if (bpb_bytes_per_sector != SECTOR_SIZE) {
        return -1;
    }
    
    fat_sector = bpb_reserved_sectors;
    root_dir_sector = fat_sector + (bpb_fat_count * bpb_sectors_per_fat);
    root_dir_sectors = (bpb_root_dir_entries * 32 + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
    data_sector = root_dir_sector + root_dir_sectors;
    cluster_size = bpb_sectors_per_cluster * SECTOR_SIZE;
    
    for (int i = 0; i < MAX_OPEN_FILES; i++) open_files[i].active = 0;
    return 0;
}

static uint16_t read_fat(uint16_t cluster) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t offset = cluster * 2;
    uint32_t sector = fat_sector + (offset / SECTOR_SIZE);
    virtio_blk_read_sector(sector, buf, 1);
    // Align manually to map FAT byte exactly 
    uint8_t* p = buf + (offset % SECTOR_SIZE);
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void write_fat(uint16_t cluster, uint16_t val) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t offset = cluster * 2;
    uint32_t sector = fat_sector + (offset / SECTOR_SIZE);
    virtio_blk_read_sector(sector, buf, 1);
    
    uint8_t* p = buf + (offset % SECTOR_SIZE);
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
    virtio_blk_write_sector(sector, buf, 1);
    
    if (bpb_fat_count > 1) {
        virtio_blk_write_sector(sector + bpb_sectors_per_fat, buf, 1);
    }
}

static uint16_t alloc_cluster(void) {
    for (uint16_t c = 2; c < 0xFFF0; c++) {
        if (read_fat(c) == 0x0000) {
            write_fat(c, 0xFFFF);
            // Zero memory in the new cluster 
            uint8_t zero[SECTOR_SIZE];
            for (int i = 0; i < SECTOR_SIZE; i++) zero[i] = 0;
            uint32_t s = data_sector + (c - 2) * bpb_sectors_per_cluster;
            for (uint32_t i = 0; i < bpb_sectors_per_cluster; i++) {
                virtio_blk_write_sector(s + i, zero, 1);
            }
            return c;
        }
    }
    return 0; // Disk full
}

int file_open(const char* filename) {
    uint8_t buf[SECTOR_SIZE];
    int empty_found = 0;
    uint32_t empty_sec = 0;
    uint32_t empty_off = 0;

    for (uint32_t i = 0; i < root_dir_sectors; i++) {
        virtio_blk_read_sector(root_dir_sector + i, buf, 1);
        struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
            if (entries[j].name[0] == 0x00) { // End of dir
                if (!empty_found) { empty_found = 1; empty_sec = root_dir_sector + i; empty_off = j; }
                break; 
            }
            if (entries[j].name[0] == (char)0xE5) { // Deleted
                if (!empty_found) { empty_found = 1; empty_sec = root_dir_sector + i; empty_off = j; }
                continue;
            }
            if (match_name(entries[j].name, filename)) {
                for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
                    if (!open_files[fd].active) {
                        open_files[fd].active = 1;
                        open_files[fd].entry = entries[j];
                        open_files[fd].dir_sector = root_dir_sector + i;
                        open_files[fd].dir_offset = j;
                        open_files[fd].cursor = 0;
                        return fd;
                    }
                }
                return -1; // Max open files
            }
        }
    }
    
    // File not found. Create it if space found
    if (!empty_found) return -1;
    
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!open_files[fd].active) {
            virtio_blk_read_sector(empty_sec, buf, 1);
            struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
            
            // Format name explicitly
            char formatted[11];
            for (int k = 0; k < 11; k++) formatted[k] = ' ';
            int p = 0, q = 0;
            while (filename[p] && filename[p] != '.' && q < 8) formatted[q++] = filename[p++];
            if (filename[p] == '.') {
                p++; q = 8;
                while (filename[p] && q < 11) formatted[q++] = filename[p++];
            }
            for (int k = 0; k < 11; k++) entries[empty_off].name[k] = formatted[k] >= 'a' && formatted[k] <= 'z' ? formatted[k] - 32 : formatted[k];
            
            entries[empty_off].attr = 0;
            entries[empty_off].start_cluster = 0;
            entries[empty_off].file_size = 0;
            
            virtio_blk_write_sector(empty_sec, buf, 1);
            
            open_files[fd].active = 1;
            open_files[fd].entry = entries[empty_off];
            open_files[fd].dir_sector = empty_sec;
            open_files[fd].dir_offset = empty_off;
            open_files[fd].cursor = 0;
            return fd;
        }
    }
    return -1;
}

int file_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].active) return -1;
    
    // Update directory entry dynamically on disk
    uint8_t buf[SECTOR_SIZE];
    virtio_blk_read_sector(open_files[fd].dir_sector, buf, 1);
    struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
    entries[open_files[fd].dir_offset] = open_files[fd].entry;
    virtio_blk_write_sector(open_files[fd].dir_sector, buf, 1);
    
    open_files[fd].active = 0;
    return 0;
}

static uint16_t get_cluster_for_offset(uint16_t start, uint32_t offset) {
    uint32_t jumps = offset / cluster_size;
    uint16_t current = start;
    for (uint32_t i = 0; i < jumps; i++) {
        if (current >= 0xFFF8 || current == 0) return 0;
        current = read_fat(current);
    }
    return current;
}

int file_seek(int fd, int offset) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].active) return -1;
    if (offset < 0 || (uint32_t)offset > open_files[fd].entry.file_size) return -1;
    open_files[fd].cursor = offset;
    return 0;
}

int file_read(int fd, void* buf, int size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].active) return -1;
    
    uint32_t remaining = open_files[fd].entry.file_size - open_files[fd].cursor;
    if ((uint32_t)size > remaining) size = remaining;
    if (size == 0) return 0;
    
    uint8_t* out = (uint8_t*)buf;
    int read_bytes = 0;
    
    while (size > 0) {
        uint16_t c = get_cluster_for_offset(open_files[fd].entry.start_cluster, open_files[fd].cursor);
        if (c == 0 || c >= 0xFFF8) break; // Reached EOF cluster early, corruption avoiding
        
        uint32_t offset_in_cluster = open_files[fd].cursor % cluster_size;
        uint32_t bytes_to_read = cluster_size - offset_in_cluster;
        if (bytes_to_read > (uint32_t)size) bytes_to_read = size;
        
        uint32_t sector_num = data_sector + (c - 2) * bpb_sectors_per_cluster + (offset_in_cluster / SECTOR_SIZE);
        uint32_t offset_in_sector = offset_in_cluster % SECTOR_SIZE;
        
        uint8_t sec_buf[SECTOR_SIZE];
        virtio_blk_read_sector(sector_num, sec_buf, 1);
        
        uint32_t chunk = SECTOR_SIZE - offset_in_sector;
        if (chunk > bytes_to_read) chunk = bytes_to_read;
        
        for (uint32_t i = 0; i < chunk; i++) out[read_bytes++] = sec_buf[offset_in_sector + i];
        
        open_files[fd].cursor += chunk;
        size -= chunk;
    }
    return read_bytes;
}

int file_write(int fd, const void* buf, int size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].active) return -1;
    if (size == 0) return 0;
    
    const uint8_t* in = (const uint8_t*)buf;
    int written_bytes = 0;
    
    if (open_files[fd].entry.start_cluster == 0) {
        open_files[fd].entry.start_cluster = alloc_cluster();
        if (open_files[fd].entry.start_cluster == 0) return 0;
    }
    
    while (size > 0) {
        uint16_t c = open_files[fd].entry.start_cluster;
        uint32_t target_cluster_idx = open_files[fd].cursor / cluster_size;
        
        uint16_t prev = 0;
        for (uint32_t i = 0; i < target_cluster_idx; i++) {
            prev = c;
            c = read_fat(c);
            if (c >= 0xFFF8) { // Grow cluster chain
                c = alloc_cluster();
                if (c == 0) break; // Disk full stop chunking
                write_fat(prev, c);
            }
        }
        if (c == 0) break;
        
        uint32_t offset_in_cluster = open_files[fd].cursor % cluster_size;
        uint32_t bytes_to_write = cluster_size - offset_in_cluster;
        if (bytes_to_write > (uint32_t)size) bytes_to_write = size;
        
        uint32_t sector_num = data_sector + (c - 2) * bpb_sectors_per_cluster + (offset_in_cluster / SECTOR_SIZE);
        uint32_t offset_in_sector = offset_in_cluster % SECTOR_SIZE;
        
        uint8_t sec_buf[SECTOR_SIZE];
        uint32_t chunk = SECTOR_SIZE - offset_in_sector;
        if (chunk > bytes_to_write) chunk = bytes_to_write;

        if (chunk < SECTOR_SIZE) virtio_blk_read_sector(sector_num, sec_buf, 1); // RMW Cycle
        
        for (uint32_t i = 0; i < chunk; i++) sec_buf[offset_in_sector + i] = in[written_bytes++];
        
        virtio_blk_write_sector(sector_num, sec_buf, 1);
        
        open_files[fd].cursor += chunk;
        if (open_files[fd].cursor > open_files[fd].entry.file_size) {
            open_files[fd].entry.file_size = open_files[fd].cursor;
        }
        size -= chunk;
    }
    return written_bytes;
}
