#include "fs.h"
#include "virtio_blk.h"
#include "lock.h"

#define SECTOR_SIZE 512

static uint32_t bpb_bytes_per_sector;
static uint32_t bpb_sectors_per_cluster;
static uint32_t bpb_reserved_sectors;
static uint32_t bpb_fat_count;
static uint32_t bpb_root_dir_entries;
static uint32_t bpb_sectors_per_fat;

static uint32_t fat_sector;
static uint32_t root_dir_sector;
static uint32_t root_dir_sectors;
static uint32_t data_sector;
static uint32_t cluster_size;

static spinlock_t fat_lock;

// Helper: match 8.3 filename
static int match_name(const char* fat_name, const char* query) {
    char formatted[11];
    for (int i = 0; i < 11; i++) formatted[i] = ' ';
    int i = 0, j = 0;
    while (query[i] && query[i] != '.' && j < 8) formatted[j++] = query[i++];
    // Skip any characters in the query name that exceed the 8-character limit
    while (query[i] && query[i] != '.') i++;
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

/**
 * Initializes the FAT16 filesystem.
 * Reads the BIOS Parameter Block (BPB) from sector 0 to calculate filesystem layout.
 * 
 * Returns:
 *   0 on success, -1 on failure.
 */
int fat16_init(void) {
    spinlock_init(&fat_lock);
    uint8_t buf[SECTOR_SIZE];
    if (virtio_blk_read_sector(0, buf, 1) != 0) {
        return -1;
    }
    
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
    
    return 0;
}

static uint16_t read_fat(uint16_t cluster) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t offset = cluster * 2;
    uint32_t sector = fat_sector + (offset / SECTOR_SIZE);
    virtio_blk_read_sector(sector, buf, 1);
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

extern void uart_puts(const char* s);

/**
 * Opens a file on the FAT16 filesystem by searching the root directory.
 * 
 * Parameters:
 *   filename - The name of the file to open.
 *   f        - Pointer to the file structure to populate.
 * 
 * Returns:
 *   0 on success, -1 if the file is not found or an error occurs.
 */
int fat16_open(const char* filename, struct file* f) {
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    uint8_t buf[SECTOR_SIZE];

    for (uint32_t i = 0; i < root_dir_sectors; i++) {
        spinlock_release_irqrestore(&fat_lock, flags);
        if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) {
            return -1;
        }
        flags = spinlock_acquire_irqsave(&fat_lock);

        struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
            if (entries[j].name[0] == 0x00) break;
            if (entries[j].name[0] == (char)0xE5) continue;
            
            if (match_name(entries[j].name, filename)) {
                f->type = FILE_TYPE_FAT16;
                f->fat16.entry = *(struct fat16_dir_entry*)&entries[j];
                f->fat16.dir_sector = root_dir_sector + i;
                f->fat16.dir_offset = j;
                f->fat16.cursor = 0;
                
                spinlock_release_irqrestore(&fat_lock, flags);
                return 0;
            }
        }
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
}

/**
 * Reads the filename of the N-th valid entry in the root directory.
 */
int fat16_read_dir(int index, char *out_name) {
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    uint8_t buf[SECTOR_SIZE];
    int current_idx = 0;

    for (uint32_t i = 0; i < root_dir_sectors; i++) {
        spinlock_release_irqrestore(&fat_lock, flags);
        if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) {
            return -1;
        }
        flags = spinlock_acquire_irqsave(&fat_lock);

        struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
            if (entries[j].name[0] == 0x00) {
                spinlock_release_irqrestore(&fat_lock, flags);
                return -1; // End of directory
            }
            if (entries[j].name[0] == (char)0xE5) continue; // Deleted
            
            // It's a valid entry
            if (current_idx == index) {
                // Format name "XXXXXXXX.YYY"
                int out_pos = 0;
                for (int k = 0; k < 8; k++) {
                    if (entries[j].name[k] != ' ') {
                        out_name[out_pos++] = entries[j].name[k];
                    }
                }
                if (entries[j].name[8] != ' ') {
                    out_name[out_pos++] = '.';
                    for (int k = 8; k < 11; k++) {
                        if (entries[j].name[k] != ' ') {
                            out_name[out_pos++] = entries[j].name[k];
                        }
                    }
                }
                out_name[out_pos] = '\0';
                
                spinlock_release_irqrestore(&fat_lock, flags);
                return 0;
            }
            current_idx++;
        }
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
}

/**
 * Closes a FAT16 file. Updates the directory entry on disk (e.g., file size).
 */
int fat16_close(struct file* f) {
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    
    // Update directory entry dynamically on disk
    uint8_t buf[SECTOR_SIZE];
    uint32_t dir_sector = f->fat16.dir_sector;
    uint32_t dir_offset = f->fat16.dir_offset;
    struct fat16_dir_entry entry = *(struct fat16_dir_entry*)&f->fat16.entry;
    
    spinlock_release_irqrestore(&fat_lock, flags);
    virtio_blk_read_sector(dir_sector, buf, 1);
    struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
    entries[dir_offset] = entry;
    virtio_blk_write_sector(dir_sector, buf, 1);
    
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

/**
 * Updates the file cursor for seeking within a FAT16 file.
 */
int fat16_seek(struct file* f, int offset) {
    if (offset < 0 || (uint32_t)offset > f->fat16.entry.file_size) return -1;
    f->fat16.cursor = offset;
    return 0;
}

/**
 * Reads data from a FAT16 file at the current cursor position.
 * Traverses the FAT cluster chain to locate the data on disk.
 * 
 * Returns:
 *   Number of bytes read.
 */
int fat16_read(struct file* f, void* buf, int size) {
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    
    uint32_t remaining = f->fat16.entry.file_size - f->fat16.cursor;
    if ((uint32_t)size > remaining) size = remaining;
    if (size == 0) {
        spinlock_release_irqrestore(&fat_lock, flags);
        return 0;
    }
    
    uint8_t* out = (uint8_t*)buf;
    int read_bytes = 0;
    
    while (size > 0) {
        uint16_t c = get_cluster_for_offset(f->fat16.entry.start_cluster, f->fat16.cursor);
        if (c == 0 || c >= 0xFFF8) break; 
        
        uint32_t offset_in_cluster = f->fat16.cursor % cluster_size;
        uint32_t bytes_to_read = cluster_size - offset_in_cluster;
        if (bytes_to_read > (uint32_t)size) bytes_to_read = size;
        
        uint32_t sector_num = data_sector + (c - 2) * bpb_sectors_per_cluster + (offset_in_cluster / SECTOR_SIZE);
        uint32_t offset_in_sector = offset_in_cluster % SECTOR_SIZE;
        
        uint8_t sec_buf[SECTOR_SIZE];
        spinlock_release_irqrestore(&fat_lock, flags);
        virtio_blk_read_sector(sector_num, sec_buf, 1);
        flags = spinlock_acquire_irqsave(&fat_lock);
        
        uint32_t chunk = SECTOR_SIZE - offset_in_sector;
        if (chunk > bytes_to_read) chunk = bytes_to_read;
        
        for (uint32_t i = 0; i < chunk; i++) out[read_bytes++] = sec_buf[offset_in_sector + i];
        
        f->fat16.cursor += chunk;
        size -= chunk;
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    return read_bytes;
}

/**
 * Writes data to a FAT16 file at the current cursor position.
 * Allocates new clusters if the file grows beyond its current capacity.
 * 
 * Returns:
 *   Number of bytes written.
 */
int fat16_write(struct file* f, const void* buf, int size) {
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    if (size == 0) {
        spinlock_release_irqrestore(&fat_lock, flags);
        return 0;
    }
    
    const uint8_t* in = (const uint8_t*)buf;
    int written_bytes = 0;
    
    if (f->fat16.entry.start_cluster == 0) {
        f->fat16.entry.start_cluster = alloc_cluster();
        if (f->fat16.entry.start_cluster == 0) {
            spinlock_release_irqrestore(&fat_lock, flags);
            return 0;
        }
    }
    
    while (size > 0) {
        uint16_t c = f->fat16.entry.start_cluster;
        uint32_t target_cluster_idx = f->fat16.cursor / cluster_size;
        
        uint16_t prev = 0;
        for (uint32_t i = 0; i < target_cluster_idx; i++) {
            prev = c;
            c = read_fat(c);
            if (c >= 0xFFF8) { 
                c = alloc_cluster();
                if (c == 0) break;
                write_fat(prev, c);
            }
        }
        if (c == 0) break;
        
        uint32_t offset_in_cluster = f->fat16.cursor % cluster_size;
        uint32_t bytes_to_write = cluster_size - offset_in_cluster;
        if (bytes_to_write > (uint32_t)size) bytes_to_write = size;
        
        uint32_t sector_num = data_sector + (c - 2) * bpb_sectors_per_cluster + (offset_in_cluster / SECTOR_SIZE);
        uint32_t offset_in_sector = offset_in_cluster % SECTOR_SIZE;
        
        uint8_t sec_buf[SECTOR_SIZE];
        uint32_t chunk = SECTOR_SIZE - offset_in_sector;
        if (chunk > bytes_to_write) chunk = bytes_to_write;

        spinlock_release_irqrestore(&fat_lock, flags);
        if (chunk < SECTOR_SIZE) virtio_blk_read_sector(sector_num, sec_buf, 1); 
        for (uint32_t i = 0; i < chunk; i++) sec_buf[offset_in_sector + i] = in[written_bytes++];
        virtio_blk_write_sector(sector_num, sec_buf, 1);
        flags = spinlock_acquire_irqsave(&fat_lock);
        
        f->fat16.cursor += chunk;
        if (f->fat16.cursor > f->fat16.entry.file_size) {
            f->fat16.entry.file_size = f->fat16.cursor;
        }
        size -= chunk;
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    return written_bytes;
}
