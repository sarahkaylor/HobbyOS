#include "fs.h"
#include "virtio_blk.h"
#include "lock.h"
#include "process.h"

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
static uint32_t bpb_total_sectors;

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
  uint32_t ts16 = (uint32_t)vbuf[19] | ((uint32_t)vbuf[20] << 8);
  uint32_t ts32 = (uint32_t)vbuf[32] | ((uint32_t)vbuf[33] << 8) |
                  ((uint32_t)vbuf[34] << 16) | ((uint32_t)vbuf[35] << 24);
  bpb_total_sectors = ts16 ? ts16 : ts32;

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
static void format_83(const char* query, char* formatted) {
  for (int i = 0; i < 11; i++) formatted[i] = ' ';
  int i = 0, j = 0;
  while (query[i] && query[i] != '.' && j < 8) {
    char c = query[i++];
    if (c >= 'a' && c <= 'z') c -= 32;
    formatted[j++] = c;
  }
  while (query[i] && query[i] != '.') i++;
  if (query[i] == '.') {
    i++; j = 8;
    while (query[i] && j < 11) {
      char c = query[i++];
      if (c >= 'a' && c <= 'z') c -= 32;
      formatted[j++] = c;
    }
  }
}

static void clean_path(const char *path, char *clean) {
  char *stack[16];
  int stack_top = 0;

  char temp[256];
  int len = 0;
  while (path[len] && len < 255) {
    temp[len] = path[len];
    len++;
  }
  temp[len] = '\0';

  char *p = temp;
  while (*p == '/') p++;

  while (*p) {
    char *comp = p;
    while (*p && *p != '/') p++;
    if (*p == '/') {
      *p = '\0';
      p++;
    }
    while (*p == '/') p++;

    if (comp[0] == '.' && comp[1] == '\0') {
      continue;
    }
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
      if (stack_top > 0) stack_top--;
    } else {
      if (stack_top < 16) {
        stack[stack_top++] = comp;
      }
    }
  }

  int pos = 0;
  clean[pos++] = '/';
  for (int i = 0; i < stack_top; i++) {
    if (i > 0) clean[pos++] = '/';
    int j = 0;
    while (stack[i][j] && pos < 127) {
      clean[pos++] = stack[i][j++];
    }
  }
  clean[pos] = '\0';
}

extern struct process *current_process(void);

static void fat16_absolute_path(const char *path, char *abs_path) {
  if (path[0] == '/') {
    clean_path(path, abs_path);
  } else {
    struct process *cur = current_process();
    char raw[256];
    int pos = 0;

    const char *cwd = "/";
    if (cur) cwd = cur->cwd;

    while (cwd[pos] && pos < 127) {
      raw[pos] = cwd[pos];
      pos++;
    }
    if (pos > 0 && raw[pos - 1] != '/') {
      raw[pos++] = '/';
    }
    int k = 0;
    while (path[k] && (pos + k) < 255) {
      raw[pos + k] = path[k];
      k++;
    }
    raw[pos + k] = '\0';
    clean_path(raw, abs_path);
  }
}

int fat16_chdir(const char *path, char *out_new_cwd) {
  char abs_path[256];
  fat16_absolute_path(path, abs_path);

  struct fat16_dir_entry entry;
  if (fat16_resolve_path(abs_path, &entry, 0, 0) != 0) {
    return -1;
  }
  if (!(entry.attr & 0x10)) {
    return -1;
  }

  int k = 0;
  while (abs_path[k] && k < 127) {
    out_new_cwd[k] = abs_path[k];
    k++;
  }
  out_new_cwd[k] = '\0';
  return 0;
}

static int find_entry_in_root(const char *name, struct fat16_dir_entry *out_entry, uint32_t *out_sector, uint32_t *out_offset) {
  uint8_t buf[SECTOR_SIZE];
  for (uint32_t i = 0; i < root_dir_sectors; i++) {
    if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) {
      return -1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
    for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
      if (entries[j].name[0] == 0x00) {
        spinlock_release_irqrestore(&fat_lock, flags);
        return -1;
      }
      if (entries[j].name[0] == (char)0xE5) continue;
      if (entries[j].attr == 0x0F) continue; // LFN

      if (match_name(entries[j].name, name)) {
        if (out_entry) *out_entry = entries[j];
        if (out_sector) *out_sector = root_dir_sector + i;
        if (out_offset) *out_offset = j;
        spinlock_release_irqrestore(&fat_lock, flags);
        return 0;
      }
    }
    spinlock_release_irqrestore(&fat_lock, flags);
  }
  return -1;
}

static int find_entry_in_subdir(uint16_t dir_cluster, const char *name, struct fat16_dir_entry *out_entry, uint32_t *out_sector, uint32_t *out_offset) {
  uint16_t cluster = dir_cluster;
  uint8_t buf[SECTOR_SIZE];

  while (cluster != 0 && cluster < 0xFFF0) {
    for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
      uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) {
        return -1;
      }
      uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
      struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
        if (entries[j].name[0] == 0x00) {
          spinlock_release_irqrestore(&fat_lock, flags);
          return -1;
        }
        if (entries[j].name[0] == (char)0xE5) continue;
        if (entries[j].attr == 0x0F) continue; // LFN

        if (match_name(entries[j].name, name)) {
          if (out_entry) *out_entry = entries[j];
          if (out_sector) *out_sector = sector_num;
          if (out_offset) *out_offset = j;
          spinlock_release_irqrestore(&fat_lock, flags);
          return 0;
        }
      }
      spinlock_release_irqrestore(&fat_lock, flags);
    }
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    cluster = read_fat(cluster);
    spinlock_release_irqrestore(&fat_lock, flags);
  }
  return -1;
}

static int alloc_entry_in_dir(uint16_t dir_cluster, const struct fat16_dir_entry *new_entry, uint32_t *out_sector, uint32_t *out_offset) {
  uint8_t buf[SECTOR_SIZE];

  if (dir_cluster == 0) {
    for (uint32_t i = 0; i < root_dir_sectors; i++) {
      if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) {
        return -1;
      }
      uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
      struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
        if (entries[j].name[0] == 0x00 || entries[j].name[0] == (char)0xE5) {
          entries[j] = *new_entry;
          spinlock_release_irqrestore(&fat_lock, flags);
          if (virtio_blk_write_sector(root_dir_sector + i, buf, 1) != 0) {
            return -1;
          }
          if (out_sector) *out_sector = root_dir_sector + i;
          if (out_offset) *out_offset = j;
          return 0;
        }
      }
      spinlock_release_irqrestore(&fat_lock, flags);
    }
    return -1;
  } else {
    uint16_t cluster = dir_cluster;
    uint16_t prev_cluster = 0;

    while (cluster != 0 && cluster < 0xFFF0) {
      for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
        uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
        if (virtio_blk_read_sector(sector_num, buf, 1) != 0) {
          return -1;
        }
        uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
        struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
          if (entries[j].name[0] == 0x00 || entries[j].name[0] == (char)0xE5) {
            entries[j] = *new_entry;
            spinlock_release_irqrestore(&fat_lock, flags);
            if (virtio_blk_write_sector(sector_num, buf, 1) != 0) {
              return -1;
            }
            if (out_sector) *out_sector = sector_num;
            if (out_offset) *out_offset = j;
            return 0;
          }
        }
        spinlock_release_irqrestore(&fat_lock, flags);
      }
      prev_cluster = cluster;
      uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
      cluster = read_fat(cluster);
      spinlock_release_irqrestore(&fat_lock, flags);
    }

    if (prev_cluster != 0) {
      uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
      uint16_t new_c = alloc_cluster();
      spinlock_release_irqrestore(&fat_lock, flags);
      if (new_c == 0) return -1;

      flags = spinlock_acquire_irqsave(&fat_lock);
      write_fat(prev_cluster, new_c);
      spinlock_release_irqrestore(&fat_lock, flags);

      uint32_t sector_num = data_sector + (new_c - 2) * bpb_sectors_per_cluster;
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) {
        return -1;
      }
      flags = spinlock_acquire_irqsave(&fat_lock);
      struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
      entries[0] = *new_entry;
      spinlock_release_irqrestore(&fat_lock, flags);
      if (virtio_blk_write_sector(sector_num, buf, 1) != 0) {
        return -1;
      }
      if (out_sector) *out_sector = sector_num;
      if (out_offset) *out_offset = 0;
      return 0;
    }
    return -1;
  }
}

int fat16_resolve_path(const char *path, struct fat16_dir_entry *out_entry, uint32_t *out_sector, uint32_t *out_offset) {
  if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    if (out_entry) {
      for (int k = 0; k < 11; k++) out_entry->name[k] = ' ';
      out_entry->attr = 0x10;
      out_entry->start_cluster = 0;
      out_entry->file_size = 0;
    }
    if (out_sector) *out_sector = 0;
    if (out_offset) *out_offset = 0;
    return 0;
  }

  uint16_t current_dir_cluster = 0;
  struct fat16_dir_entry current_entry;
  uint32_t current_sector = 0;
  uint32_t current_offset = 0;

  const char *p = path;
  if (*p == '/') p++;

  char component[64];
  while (*p) {
    int len = 0;
    while (*p && *p != '/' && len < 63) {
      component[len++] = *p++;
    }
    component[len] = '\0';
    while (*p == '/') p++;

    int res;
    if (current_dir_cluster == 0) {
      res = find_entry_in_root(component, &current_entry, &current_sector, &current_offset);
    } else {
      res = find_entry_in_subdir(current_dir_cluster, component, &current_entry, &current_sector, &current_offset);
    }

    if (res != 0) {
      return -1;
    }

    if (*p) {
      if (!(current_entry.attr & 0x10)) {
        return -1;
      }
      current_dir_cluster = current_entry.start_cluster;
    }
  }

  if (out_entry) *out_entry = current_entry;
  if (out_sector) *out_sector = current_sector;
  if (out_offset) *out_offset = current_offset;
  return 0;
}

int fat16_resolve_parent(const char *path, struct fat16_dir_entry *out_parent_entry, char *out_last_component) {
  const char *last_slash = 0;
  const char *p = path;
  while (*p) {
    if (*p == '/' && *(p+1) != '\0') {
      last_slash = p;
    }
    p++;
  }

  if (!last_slash) {
    if (out_parent_entry) {
      for (int k = 0; k < 11; k++) out_parent_entry->name[k] = ' ';
      out_parent_entry->attr = 0x10;
      out_parent_entry->start_cluster = 0;
      out_parent_entry->file_size = 0;
    }
    const char *comp = path;
    if (*comp == '/') comp++;
    int idx = 0;
    while (comp[idx] && idx < 63) {
      out_last_component[idx] = comp[idx];
      idx++;
    }
    out_last_component[idx] = '\0';
    return 0;
  }

  char parent_path[256];
  int parent_len = last_slash - path;
  if (parent_len == 0) {
    parent_path[0] = '/';
    parent_path[1] = '\0';
  } else {
    int idx = 0;
    while (idx < parent_len && idx < 255) {
      parent_path[idx] = path[idx];
      idx++;
    }
    parent_path[idx] = '\0';
  }

  const char *comp = last_slash + 1;
  int idx = 0;
  while (comp[idx] && idx < 63) {
    out_last_component[idx] = comp[idx];
    idx++;
  }
  out_last_component[idx] = '\0';

  return fat16_resolve_path(parent_path, out_parent_entry, 0, 0);
}

int fat16_open(const char* filename, struct file* f) {
  char abs_path[256];
  fat16_absolute_path(filename, abs_path);

  struct fat16_dir_entry entry;
  uint32_t sector = 0;
  uint32_t offset = 0;

  if (fat16_resolve_path(abs_path, &entry, &sector, &offset) == 0) {
    f->type = FILE_TYPE_FAT16;
    f->fat16.entry = entry;
    f->fat16.dir_sector = sector;
    f->fat16.dir_offset = offset;
    f->fat16.cursor = 0;
    f->fat16.dirty = 0;
    return 0;
  }

  struct fat16_dir_entry parent_entry;
  char last_comp[64];
  if (fat16_resolve_parent(abs_path, &parent_entry, last_comp) != 0) {
    return -1;
  }

  if (!(parent_entry.attr & 0x10)) {
    return -1;
  }

  struct fat16_dir_entry new_entry;
  char formatted_name[11];
  format_83(last_comp, formatted_name);
  for (int k = 0; k < 11; k++) {
    new_entry.name[k] = formatted_name[k];
  }
  new_entry.attr = 0;
  for (int k = 0; k < 10; k++) new_entry.reserved[k] = 0;
  new_entry.time = 0;
  new_entry.date = 0;
  new_entry.start_cluster = 0;
  new_entry.file_size = 0;

  if (alloc_entry_in_dir(parent_entry.start_cluster, &new_entry, &sector, &offset) != 0) {
    return -1;
  }

  f->type = FILE_TYPE_FAT16;
  f->fat16.entry = new_entry;
  f->fat16.dir_sector = sector;
  f->fat16.dir_offset = offset;
  f->fat16.cursor = 0;
  f->fat16.dirty = 0;
  return 0;
}

int fat16_read_dir(const char* path, int index, char* out_name, uint8_t* out_attr, uint32_t* out_size) {
  char abs_path[256];
  fat16_absolute_path(path, abs_path);

  struct fat16_dir_entry dir_entry;
  if (fat16_resolve_path(abs_path, &dir_entry, 0, 0) != 0) {
    uart_puts("[fat16_read_dir] resolve failed\n");
    return -1;
  }

  if (!(dir_entry.attr & 0x10)) {
    uart_puts("[fat16_read_dir] attr not directory\n");
    return -1;
  }

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint16_t cluster = dir_entry.start_cluster;
  uint8_t buf[SECTOR_SIZE];
  int current_idx = 0;

  if (cluster == 0) {
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
          return -1;
        }
        if (entries[j].name[0] == (char)0xE5) continue;
        if (entries[j].attr == 0x0F) continue; // LFN
        if (entries[j].attr & 0x08) continue; // Volume Label

        if (current_idx == index) {
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
          if (out_attr) *out_attr = entries[j].attr;
          if (out_size) *out_size = entries[j].file_size;

          spinlock_release_irqrestore(&fat_lock, flags);
          return 0;
        }
        current_idx++;
      }
    }
  } else {
    while (cluster != 0 && cluster < 0xFFF0) {
      for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
        uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
        spinlock_release_irqrestore(&fat_lock, flags);
        if (virtio_blk_read_sector(sector_num, buf, 1) != 0) {
          return -1;
        }
        flags = spinlock_acquire_irqsave(&fat_lock);

        struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
          if (entries[j].name[0] == 0x00) {
            spinlock_release_irqrestore(&fat_lock, flags);
            return -1;
          }
          if (entries[j].name[0] == (char)0xE5) continue;
          if (entries[j].attr == 0x0F) continue; // LFN

          if (current_idx == index) {
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
            if (out_attr) *out_attr = entries[j].attr;
            if (out_size) *out_size = entries[j].file_size;

            spinlock_release_irqrestore(&fat_lock, flags);
            return 0;
          }
          current_idx++;
        }
      }
      cluster = read_fat(cluster);
    }
  }

  spinlock_release_irqrestore(&fat_lock, flags);
  return -1;
}

int fat16_unlink(const char* filename) {
  char abs_path[256];
  fat16_absolute_path(filename, abs_path);

  struct fat16_dir_entry entry;
  uint32_t sector = 0;
  uint32_t offset = 0;

  if (fat16_resolve_path(abs_path, &entry, &sector, &offset) != 0) {
    return -1;
  }

  if (entry.attr & 0x10) {
    return -1;
  }

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint16_t cluster = entry.start_cluster;
  while (cluster != 0 && cluster < 0xFFF0) {
    uint16_t next = read_fat(cluster);
    write_fat(cluster, 0x0000);
    cluster = next;
  }

  uint8_t buf[SECTOR_SIZE];
  spinlock_release_irqrestore(&fat_lock, flags);
  if (virtio_blk_read_sector(sector, buf, 1) != 0) {
    return -1;
  }
  flags = spinlock_acquire_irqsave(&fat_lock);

  struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
  entries[offset].name[0] = (char)0xE5;

  spinlock_release_irqrestore(&fat_lock, flags);
  if (virtio_blk_write_sector(sector, buf, 1) != 0) {
    return -1;
  }
  return 0;
}

/* --- rename/move support --------------------------------------------- */

static int name11_equal(const char *a, const char *b) {
  for (int i = 0; i < 11; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* Does directory `cluster` (0 = root) already hold an entry named `comp`? */
static int dir_contains_name(uint16_t cluster, const char *comp,
                             struct fat16_dir_entry *out) {
  if (cluster == 0)
    return find_entry_in_root(comp, out, 0, 0) == 0;
  return find_entry_in_subdir(cluster, comp, out, 0, 0) == 0;
}

/* Is `ancestor_cluster` (the potential target parent) inside the directory
 * starting at `child_cluster` — i.e. would moving child into it create a
 * cycle?  Walks the ".." chain upward with a depth guard. */
static int dir_is_inside(uint16_t ancestor_cluster, uint16_t child_cluster) {
  if (child_cluster == 0 || ancestor_cluster == 0) return 0;
  uint16_t c = ancestor_cluster;
  for (int depth = 0; depth < 32; depth++) {
    if (c == child_cluster) return 1;
    if (c < 2) return 0;
    uint32_t sector = data_sector + (uint32_t)(c - 2) * bpb_sectors_per_cluster;
    uint8_t buf[SECTOR_SIZE];
    if (virtio_blk_read_sector(sector, buf, 1) != 0) return 0;
    struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
    uint16_t parent = 0;
    for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
      if (entries[j].name[0] == 0x00) return 0;
      if (entries[j].name[0] == (char)0xE5) continue;
      if (entries[j].name[0] == '.' && entries[j].name[1] == '.' &&
          entries[j].name[2] == ' ') {
        parent = entries[j].start_cluster;
        break;
      }
    }
    c = parent;
  }
  return 0;
}

/* After moving a directory, its ".." entry must point at the new parent
 * cluster (0 for the root). */
static int update_dotdot(uint16_t dir_cluster, uint16_t new_parent_cluster) {
  if (dir_cluster < 2) return -1;
  uint32_t sector = data_sector + (uint32_t)(dir_cluster - 2) * bpb_sectors_per_cluster;
  uint8_t buf[SECTOR_SIZE];
  if (virtio_blk_read_sector(sector, buf, 1) != 0) return -1;
  struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
  for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
    if (entries[j].name[0] == 0x00) return -1;
    if (entries[j].name[0] != '.') continue;
    if (!(entries[j].name[1] == '.' && entries[j].name[2] == ' ')) continue;
    entries[j].start_cluster = new_parent_cluster;
    return virtio_blk_write_sector(sector, buf, 1) == 0 ? 0 : -1;
  }
  return -1;
}

int fat16_rename(const char* oldname, const char* newname) {
  char abs_old[256];
  char abs_new[256];
  fat16_absolute_path(oldname, abs_old);
  fat16_absolute_path(newname, abs_new);

  struct fat16_dir_entry entry;
  uint32_t sector = 0;
  uint32_t offset = 0;

  if (fat16_resolve_path(abs_old, &entry, &sector, &offset) != 0) {
    return -1;
  }

  struct fat16_dir_entry new_parent;
  char last_comp[64];
  if (fat16_resolve_parent(abs_new, &new_parent, last_comp) != 0) {
    return -1;
  }

  struct fat16_dir_entry old_parent;
  if (fat16_resolve_parent(abs_old, &old_parent, 0) != 0) {
    return -1;
  }

  char formatted_name[11];
  format_83(last_comp, formatted_name);

  int is_dir = (entry.attr & 0x10) != 0;
  int same_dir = (old_parent.start_cluster == new_parent.start_cluster) &&
                 ((old_parent.attr & 0x10) == (new_parent.attr & 0x10));

  if (same_dir) {
    /* In-place rename.  POSIX rename(2) replaces an existing destination:
       when the new name belongs to a different, existing entry, unlink
       that entry first (freeing its clusters) so the name rewrite below
       performs the replacement.  sed -i's temp-file rename needs this. */
    if (!name11_equal(entry.name, formatted_name) &&
        dir_contains_name(new_parent.start_cluster, last_comp, 0)) {
      if (fat16_unlink(abs_new) != 0) {
        return -1;
      }
    }
    uint8_t buf[SECTOR_SIZE];
    if (virtio_blk_read_sector(sector, buf, 1) != 0) {
      return -1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
    for (int k = 0; k < 11; k++) {
      entries[offset].name[k] = formatted_name[k];
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    if (virtio_blk_write_sector(sector, buf, 1) != 0) {
      return -1;
    }
    return 0;
  }

  /* Cross-directory move.  Replace an existing destination (POSIX rename
   * semantics), refuse moving a directory into its own subtree, then place
   * a copy in the target directory and mark the old slot deleted. */
  if (dir_contains_name(new_parent.start_cluster, last_comp, 0)) {
    if (fat16_unlink(abs_new) != 0) {
      return -1;
    }
  }
  if (is_dir && dir_is_inside(new_parent.start_cluster, entry.start_cluster)) {
    return -1;
  }

  struct fat16_dir_entry moved = entry;
  for (int k = 0; k < 11; k++) moved.name[k] = formatted_name[k];

  uint32_t new_sector = 0;
  uint32_t new_offset = 0;
  if (alloc_entry_in_dir(new_parent.start_cluster, &moved,
                         &new_sector, &new_offset) != 0) {
    return -1;
  }

  uint8_t buf[SECTOR_SIZE];
  if (virtio_blk_read_sector(sector, buf, 1) != 0) {
    /* Roll the new entry back so no duplicate is left behind. */
    uint8_t nbuf[SECTOR_SIZE];
    if (virtio_blk_read_sector(new_sector, nbuf, 1) == 0) {
      struct fat16_dir_entry *es = (struct fat16_dir_entry *)nbuf;
      es[new_offset].name[0] = (char)0xE5;
      (void)virtio_blk_write_sector(new_sector, nbuf, 1);
    }
    return -1;
  }
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
  entries[offset].name[0] = (char)0xE5;
  spinlock_release_irqrestore(&fat_lock, flags);
  if (virtio_blk_write_sector(sector, buf, 1) != 0) {
    return -1;
  }

  if (is_dir) {
    (void)update_dotdot(entry.start_cluster, new_parent.start_cluster);
  }
  return 0;
}

int fat16_mkdir(const char *path) {
  char abs_path[256];
  fat16_absolute_path(path, abs_path);

  struct fat16_dir_entry temp;
  if (fat16_resolve_path(abs_path, &temp, 0, 0) == 0) {
    return -1; // already exists
  }

  struct fat16_dir_entry parent_entry;
  char last_comp[64];
  if (fat16_resolve_parent(abs_path, &parent_entry, last_comp) != 0) {
    return -1; // parent not found
  }

  if (!(parent_entry.attr & 0x10)) {
    return -1;
  }

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint16_t new_cluster = alloc_cluster();
  spinlock_release_irqrestore(&fat_lock, flags);
  if (new_cluster == 0) {
    return -1;
  }

  uint8_t sector_buf[SECTOR_SIZE];
  for (int i = 0; i < SECTOR_SIZE; i++) sector_buf[i] = 0;
  struct fat16_dir_entry *entries = (struct fat16_dir_entry *)sector_buf;

  entries[0].name[0] = '.';
  for (int k = 1; k < 11; k++) entries[0].name[k] = ' ';
  entries[0].attr = 0x10;
  entries[0].start_cluster = new_cluster;
  entries[0].file_size = 0;

  entries[1].name[0] = '.';
  entries[1].name[1] = '.';
  for (int k = 2; k < 11; k++) entries[1].name[k] = ' ';
  entries[1].attr = 0x10;
  entries[1].start_cluster = parent_entry.start_cluster;
  entries[1].file_size = 0;

  uint32_t new_dir_sector = data_sector + (new_cluster - 2) * bpb_sectors_per_cluster;
  if (virtio_blk_write_sector(new_dir_sector, sector_buf, 1) != 0) {
    flags = spinlock_acquire_irqsave(&fat_lock);
    write_fat(new_cluster, 0x0000);
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }

  struct fat16_dir_entry new_dir_entry;
  char formatted_name[11];
  format_83(last_comp, formatted_name);
  for (int k = 0; k < 11; k++) {
    new_dir_entry.name[k] = formatted_name[k];
  }
  new_dir_entry.attr = 0x10;
  for (int k = 0; k < 10; k++) new_dir_entry.reserved[k] = 0;
  new_dir_entry.time = 0;
  new_dir_entry.date = 0;
  new_dir_entry.start_cluster = new_cluster;
  new_dir_entry.file_size = 0;

  if (alloc_entry_in_dir(parent_entry.start_cluster, &new_dir_entry, 0, 0) != 0) {
    flags = spinlock_acquire_irqsave(&fat_lock);
    write_fat(new_cluster, 0x0000);
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }

  return 0;
}

/**
 * Closes a FAT16 file. Updates the directory entry on disk (e.g., file size).
 */
/* Persist the in-memory directory entry (size, start cluster, ...) back
 * to the on-disk directory — mirrors what close() must do, kept in one
 * place so write-time sync and close-time sync can share it.  Call with
 * the fat_lock RELEASED (does virtio sector I/O). */
/* Byte-wise equality for on-disk entries (no libc memcmp in the kernel). */
static int fat16_entry_eq(const struct fat16_dir_entry* a,
                          const struct fat16_dir_entry* b) {
  const uint8_t* pa = (const uint8_t*)a;
  const uint8_t* pb = (const uint8_t*)b;
  for (unsigned i = 0; i < sizeof(*a); i++) {
    if (pa[i] != pb[i]) {
      return 0;
    }
  }
  return 1;
}

/**
 * Writes one 32-byte directory entry with a read-modify-write that is safe
 * against concurrent writers: after the write, the sector is re-read and our
 * entry re-checked; a parallel writer that interleaved its own RMW (we cannot
 * hold a spinlock across the IRQ-driven virtio transfer) would have clobbered
 * it, so retry until our bytes stick.
 */
static void fat16_dir_entry_write(uint32_t dir_sector, uint32_t dir_offset,
                                  const struct fat16_dir_entry* entry) {
  uint8_t buf[SECTOR_SIZE];
  for (int attempt = 0; attempt < 16; attempt++) {
    if (virtio_blk_read_sector(dir_sector, buf, 1) != 0) {
      return;
    }
    ((struct fat16_dir_entry*)buf)[dir_offset] = *entry;
    if (virtio_blk_write_sector(dir_sector, buf, 1) != 0) {
      return;
    }
    if (virtio_blk_read_sector(dir_sector, buf, 1) != 0) {
      return;
    }
    if (fat16_entry_eq(&((struct fat16_dir_entry*)buf)[dir_offset], entry)) {
      return; /* our entry is on disk */
    }
  }
}

static void fat16_sync_entry(struct file* f) {
  uint8_t buf[SECTOR_SIZE];
  /* Only entries this handle actually changed may be written back: a
     long-lived read descriptor (e.g. sed's input) must not clobber a
     directory entry that was recreated/renamed under it by writing its
     stale cached copy at close. */
  if (!f->fat16.dirty) {
    return;
  }
  struct fat16_dir_entry entry = *(struct fat16_dir_entry*)&f->fat16.entry;
  fat16_dir_entry_write(f->fat16.dir_sector, f->fat16.dir_offset, &entry);
}

int fat16_close(struct file* f) {
  // Update directory entry dynamically on disk
  fat16_sync_entry(f);
  return 0;
}

/**
 * Empties an existing file (open with O_TRUNC): frees its whole cluster
 * chain and resets the directory entry to zero length, so a subsequent
 * write starts from an empty file.  POSIX fopen("w") semantics.
 */
int fat16_truncate(struct file* f) {
  if (!f) return -1;

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint16_t c = f->fat16.entry.start_cluster;
  while (c >= 2 && c < 0xFFF8) {
    uint16_t next = read_fat(c);
    write_fat(c, 0);
    c = next;
  }
  f->fat16.entry.start_cluster = 0;
  f->fat16.entry.file_size = 0;
  f->fat16.cursor = 0;
  spinlock_release_irqrestore(&fat_lock, flags);
  f->fat16.dirty = 1;

  fat16_sync_entry(f);
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

  if (f->fat16.cursor >= f->fat16.entry.file_size) {
    /* past EOF: return 0, not wrapped-around-garbage (the offset
       clamp below underflows in uint32 when cursor > size) */
    spinlock_release_irqrestore(&fat_lock, flags);
    return 0;
  }
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
  if (written_bytes > 0) {
    f->fat16.dirty = 1;
    fat16_sync_entry(f);   /* keep the on-disk dir entry (size/clusters)
                              current so stat-by-path sees it immediately */
  }
  return written_bytes;
}

/* Computes data-area statistics for the volume.
 * total_bytes = all data clusters * cluster size;
 * free_bytes  = clusters whose FAT entry is 0 * cluster size.
 * Returns 0 on success, -1 on error. */
int fat16_stats(uint64_t *out_total, uint64_t *out_free) {
  if (!out_total || !out_free || bpb_total_sectors == 0 || bpb_sectors_per_cluster == 0) {
    return -1;
  }

  /* Number of clusters in the data area (clusters are numbered from 2). */
  uint32_t data_sectors = bpb_total_sectors > data_sector ? bpb_total_sectors - data_sector : 0;
  uint32_t total_clusters = data_sectors / bpb_sectors_per_cluster;
  if (total_clusters > 0xFFF0u) total_clusters = 0xFFF0u; /* FAT16 limit */

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint64_t free_clusters = 0;
  uint8_t buf[SECTOR_SIZE];
  uint32_t cluster = 2;
  for (uint32_t s = 0; s < bpb_sectors_per_fat && cluster <= total_clusters + 1; s++) {
    if (virtio_blk_read_sector(fat_sector + s, buf, 1) != 0) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -1;
    }
    volatile uint8_t *p = (volatile uint8_t *)buf;
    for (uint32_t off = 0; off + 1 < SECTOR_SIZE && cluster <= total_clusters + 1; off += 2) {
      uint32_t idx = (s * SECTOR_SIZE + off) / 2;
      if (idx < 2) continue; /* entries 0/1 are reserved */
      uint16_t val = (uint16_t)(p[off] | (p[off + 1] << 8));
      if (val == 0x0000) free_clusters++;
      cluster = idx + 1;
    }
  }
  spinlock_release_irqrestore(&fat_lock, flags);

  *out_total = (uint64_t)total_clusters * cluster_size;
  *out_free = free_clusters * cluster_size;
  return 0;
}
