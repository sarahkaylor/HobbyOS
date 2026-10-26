#include "fs.h"
#include "virtio_blk.h"
#include "lock.h"
#include "process.h"
#include "errno.h"

/* Wall-clock for dirent timestamps (selfhost w10-fstime): the kernel's
 * RTC-anchored REALTIME clock (kernel/time.c — the same clock sysinfo 6
 * and time() report) read once per stamp.  FAT timestamps are UTC
 * everywhere: the RTC is UTC on both arches (QEMU's default) and there is
 * no timezone database in the OS, so local == UTC for all tools. */
extern uint64_t hb_clock_realtime_ms(void);
extern void rtc_epoch_to_time(uint64_t epoch, int *year, int *month, int *day,
                              int *hour, int *minute, int *second,
                              int *weekday);
extern uint64_t rtc_make_epoch(int year, int month, int day,
                               int hour, int minute, int second);

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

/* One-sector cache for FAT table reads.  The read path walks cluster
   chains sequentially, so 256 consecutive clusters share one FAT sector;
   without this cache every chain step cost a full device round trip
   (O(n^2) block operations per file read - the dominant cost of the
   test wave's program loads).  Guarded by its own small lock so it is
   safe on every call path (including pre-scheduler boot). */
static spinlock_t fat_cache_lock;
static uint8_t fat_cache[SECTOR_SIZE];
static uint8_t fat_cache_fill[SECTOR_SIZE];
static uint32_t fat_cache_sector;
static int fat_cache_valid;

/* Helper: match an 8.3 filename.  A query that cannot be represented in
   8.3 (a base longer than 8 chars or an extension longer than 3) never
   matches a short entry: truncating it onto one would let a name like
   'UNEXPAND_T.BIN' silently open 'UNEXPAND.BIN' — executing the wrong
   file.  Non-8.3 names are resolved through their VFAT long-name records
   (the lookup loops below) instead. */
static int match_name(const char* fat_name, const char* query) {
  char formatted[11];
  int i = 0, j = 0;
  for (int k = 0; k < 11; k++) formatted[k] = ' ';
  while (query[i] && query[i] != '.' && j < 8) formatted[j++] = query[i++];
  if (query[i] && query[i] != '.') return 0; /* base longer than 8 chars */
  if (query[i] == '.') {
    i++;
    j = 8;
    while (query[i] && j < 11) formatted[j++] = query[i++];
    if (query[i]) return 0; /* extension longer than 3 chars */
  }
  for (int k = 0; k < 11; k++) {
    char a = fat_name[k];
    char b = formatted[k];
    if (a >= 'a' && a <= 'z') a -= 32; // Uppercase
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return 0;
  }
  return 1;
}

/* --- VFAT long-file-name support ----------------------------------- */
/* A name that does not fit 8.3 is stored as a run of attribute-0x0F
   records written in reverse order (the final 13-character chunk first),
   followed by the 8.3 short entry.  Every record carries a checksum of
   that short entry.  mtools, Windows and GNU tools all produce these
   records. */

struct lfn_state {
  char name[256];    /* reconstructed long name */
  int len;           /* bytes used in name[] */
  int have;          /* at least one record of a sequence seen */
  int next_ord;      /* ordinal expected from the next record */
  unsigned char sum; /* checksum carried by the records */
};

static unsigned char lfn_checksum(const char *name) {
  unsigned char sum = 0;
  for (int i = 0; i < 11; i++) {
    sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (unsigned char)name[i];
  }
  return sum;
}

static void lfn_reset(struct lfn_state *st) {
  st->len = 0;
  st->have = 0;
  st->next_ord = 0;
}

static void lfn_add_chunk(struct lfn_state *st, const unsigned char *ent) {
  int ord = ent[0] & 0x1F;
  int n = 0;
  char chunk[13];
  if (ord == 0) return;
  if ((ent[0] & 0x40) || !st->have || ord != st->next_ord) {
    /* Start (or restart) a sequence: this record is the name's tail. */
    st->len = 0;
    st->have = 1;
  }
  st->sum = ent[13];
  /* The 13 UTF-16LE characters sit at these byte offsets in the record. */
  static const int offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
  for (int i = 0; i < 13; i++) {
    unsigned int c = ent[offs[i]] | ((unsigned int)ent[offs[i] + 1] << 8);
    if (c == 0 || c == 0xFFFF) break; /* padding / terminator */
    if (c > 0x7F) { /* non-ASCII names are not matchable: drop the chain */
      st->len = 0;
      st->have = 0;
      return;
    }
    chunk[n++] = (char)c;
  }
  /* Records are stored tail-first, each chunk in forward order, so the
     whole chunk is prepended (not char-by-char, which would reverse it). */
  if (st->len + n > (int)sizeof(st->name) - 1) n = (int)sizeof(st->name) - 1 - st->len;
  if (n <= 0) return;
  for (int k = st->len - 1; k >= 0; k--) st->name[k + n] = st->name[k];
  for (int i = 0; i < n; i++) st->name[i] = chunk[i];
  st->len += n;
  st->next_ord = ord - 1;
}

static int lfn_matches(const struct lfn_state *st, const char *query) {
  int i = 0;
  if (!st->have) return 0;
  while (i < st->len && query[i]) {
    char a = st->name[i];
    char b = query[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return 0;
    i++;
  }
  return i == st->len && query[i] == '\0';
}

/* True if `name` is representable as an exact 8.3 name (the same test
   match_name() applies), so a long name is never truncated into one. */
static int name_is_83(const char *name) {
  int base = 0;
  while (name[base] && name[base] != '.' && base < 8) base++;
  if (name[base] && name[base] != '.') return 0;
  if (name[base] == '.') {
    base++;
    int ext = 0;
    while (name[base + ext] && ext < 3) ext++;
    if (name[base + ext]) return 0;
  }
  return 1;
}

/* Display name for a directory entry: the VFAT long name when a valid
   0x0F run (checksum over the short entry matches) immediately precedes
   it, otherwise the 8.3 reconstruction.  `cap` is the total bytes of
   `out`; the name is truncated to fit (the read_dir syscall caps at 31).
   This is what makes a listing of the build payload show the real names
   instead of 8.3 mangles. */
static void lfn_entry_display(const struct lfn_state *st,
                              const struct fat16_dir_entry *entry,
                              char *out, int cap) {
  if (cap <= 0) return;
  if (st->have && st->len > 0 &&
      lfn_checksum(entry->name) == st->sum) {
    int n = st->len;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = st->name[i];
    out[n] = '\0';
    return;
  }
  int out_pos = 0;
  for (int k = 0; k < 8; k++) {
    if (entry->name[k] != ' ' && out_pos < cap - 1)
      out[out_pos++] = entry->name[k];
  }
  if (entry->name[8] != ' ' && out_pos < cap - 1) {
    out[out_pos++] = '.';
    for (int k = 8; k < 11; k++) {
      if (entry->name[k] != ' ' && out_pos < cap - 1)
        out[out_pos++] = entry->name[k];
    }
  }
  out[out_pos] = '\0';
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
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_register(&fat_lock, "fat_lock", LOCK_RANK_FAT); /* SEC-OBS-B */
#endif
  spinlock_init(&fat_cache_lock);
  fat_cache_valid = 0;
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
  uint32_t offset = cluster * 2;
  uint32_t sector = fat_sector + (offset / SECTOR_SIZE);
  spinlock_acquire(&fat_cache_lock);
  if (!fat_cache_valid || fat_cache_sector != sector) {
    spinlock_release(&fat_cache_lock);
    virtio_blk_read_sector(sector, fat_cache_fill, 1);
    spinlock_acquire(&fat_cache_lock);
    for (int i = 0; i < SECTOR_SIZE; i++) fat_cache[i] = fat_cache_fill[i];
    fat_cache_sector = sector;
    fat_cache_valid = 1;
  }
  uint8_t* p = fat_cache + (offset % SECTOR_SIZE);
  uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
  spinlock_release(&fat_cache_lock);
  return v;
}

static void write_fat(uint16_t cluster, uint16_t val) {
  uint8_t buf[SECTOR_SIZE];
  uint32_t offset = cluster * 2;
  uint32_t sector = fat_sector + (offset / SECTOR_SIZE);

  /* Reuse the cache when it holds this sector; otherwise fetch it. */
  spinlock_acquire(&fat_cache_lock);
  int cached = (fat_cache_valid && fat_cache_sector == sector);
  if (cached) {
    for (int i = 0; i < SECTOR_SIZE; i++) buf[i] = fat_cache[i];
  }
  spinlock_release(&fat_cache_lock);
  if (!cached) {
    virtio_blk_read_sector(sector, buf, 1);
  }

  uint8_t* p = buf + (offset % SECTOR_SIZE);
  p[0] = val & 0xFF;
  p[1] = (val >> 8) & 0xFF;
  virtio_blk_write_sector(sector, buf, 1);

  if (bpb_fat_count > 1) {
    virtio_blk_write_sector(sector + bpb_sectors_per_fat, buf, 1);
  }

  /* Refresh/install the cache with the modified sector. */
  spinlock_acquire(&fat_cache_lock);
  for (int i = 0; i < SECTOR_SIZE; i++) fat_cache[i] = buf[i];
  fat_cache_sector = sector;
  fat_cache_valid = 1;
  spinlock_release(&fat_cache_lock);
}

/* W4-kernel (selfhost): free-cluster hint.  alloc_cluster() used to scan
 * the FAT from cluster 2 on EVERY allocation — O(n^2) total across a big
 * zero-fill extend (128 MiB @ spc=4 = 65536 clusters = ~2e9 scan steps; even
 * on the 1 GiB boot volume's 4096-cluster OUT.IMG it re-scanned ~8e6
 * entries).  The hint remembers the next cluster to try, turning allocation
 * into amortized O(n) while keeping the "lowest-free-on-empty-disk"
 * behavior identical (sequential fills allocate contiguously from 2).  Only
 * real callers (all call sites hold fat_lock) touch the statics, so no lock
 * is added.  A freed cluster below the hint is simply not reused until the
 * scan wraps — valid FAT behavior, standard in production FAT drivers. */
static uint16_t alloc_hint = 2;

static uint16_t alloc_cluster(void) {
  uint16_t start = alloc_hint;
  uint16_t c = start;
  for (;;) {
    if (read_fat(c) == 0x0000) {
      write_fat(c, 0xFFFF);
      uint8_t zero[SECTOR_SIZE];
      for (int i = 0; i < SECTOR_SIZE; i++) zero[i] = 0;
      uint32_t s = data_sector + (c - 2) * bpb_sectors_per_cluster;
      for (uint32_t i = 0; i < bpb_sectors_per_cluster; i++) {
        virtio_blk_write_sector(s + i, zero, 1);
      }
      /* Advance the hint past the cluster we just took (wrap to 2). */
      alloc_hint = (c + 1 >= 0xFFF0) ? 2 : (uint16_t)(c + 1);
      return c;
    }
    c++;
    if (c >= 0xFFF0) c = 2;      /* wrap to the bottom of the FAT */
    if (c == start) break;        /* full circle: disk full */
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
    struct process *cur = process_group(current_process()); /* P1 (D7) */
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
  struct lfn_state st;
  lfn_reset(&st);

  /* W9 (selfhost S2): like find_entry_in_subdir, a genuine 8.3 entry with
     the EXACT name must win over a colliding LFN alias (an 8.3-representable
     query such as "net_rdma.o" may also be the 8.3 truncation of a LONG
     name's alias).  Remembered and preferred over the first OR-match. */
  int rep83 = name_is_83(name);
  int have83 = 0;
  uint32_t a83_sector = 0, a83_offset = 0;
  struct fat16_dir_entry a83_entry;
  int or_seen = 0;
  uint32_t or_sector = 0, or_offset = 0;
  struct fat16_dir_entry or_entry;

  for (uint32_t i = 0; i < root_dir_sectors; i++) {
    if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) {
      return -1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
    for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
      if (entries[j].name[0] == 0x00) {
        spinlock_release_irqrestore(&fat_lock, flags);
        goto done_scan;
      }
      if (entries[j].name[0] == (char)0xE5) {
        lfn_reset(&st);
        continue;
      }
      if (entries[j].attr == 0x0F) {
        lfn_add_chunk(&st, (const unsigned char *)&entries[j]);
        continue;
      }
      if (rep83 && !st.have && match_name(entries[j].name, name) && !have83) {
        have83 = 1;
        a83_entry = entries[j];
        a83_sector = root_dir_sector + i;
        a83_offset = j;
      }
      if (!or_seen &&
          (match_name(entries[j].name, name) ||
           (lfn_matches(&st, name) && lfn_checksum(entries[j].name) == st.sum))) {
        or_seen = 1;
        or_entry = entries[j];
        or_sector = root_dir_sector + i;
        or_offset = j;
      }
      lfn_reset(&st);
    }
    spinlock_release_irqrestore(&fat_lock, flags);
  }
done_scan:
  {
    uint32_t sec; uint32_t off; struct fat16_dir_entry *ent;
    if (have83) { sec = a83_sector; off = a83_offset; ent = &a83_entry; }
    else if (or_seen) { sec = or_sector; off = or_offset; ent = &or_entry; }
    else { return -1; }
    if (out_entry) *out_entry = *ent;
    if (out_sector) *out_sector = sec;
    if (out_offset) *out_offset = off;
    return 0;
  }
}

static int find_entry_in_subdir(uint16_t dir_cluster, const char *name, struct fat16_dir_entry *out_entry, uint32_t *out_sector, uint32_t *out_offset) {
  uint16_t cluster = dir_cluster;
  uint8_t buf[SECTOR_SIZE];
  struct lfn_state st;
  lfn_reset(&st);

  /* W9 (selfhost S2): an 8.3-representable name can COLLIDE with the 8.3
     alias of a LONG name stored as a VFAT 0x0F run (net_rdma.o and
     net_rdma_test.o both alias to "NET_RDMAO"): the first entry in
     directory order with a matching 8.3 name used to win, so opening
     "/SRC/obj/arm/net_rdma.o" could return net_rdma_test.o's alias and
     lld then failed with "undefined symbol: net_rdma_fast_write".  A
     genuine 8.3 entry (no attached 0x0F chain) with the EXACT 8.3 name
     takes priority over any LFN alias that merely truncates to it. */
  int rep83 = name_is_83(name);
  int have83 = 0;
  uint32_t a83_sector = 0, a83_offset = 0;
  struct fat16_dir_entry a83_entry;
  int or_seen = 0;
  uint32_t or_sector = 0, or_offset = 0;
  struct fat16_dir_entry or_entry;

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
          goto done_scan;
        }
        if (entries[j].name[0] == (char)0xE5) {
          lfn_reset(&st);
          continue;
        }
        if (entries[j].attr == 0x0F) {
          lfn_add_chunk(&st, (const unsigned char *)&entries[j]);
          continue;
        }
        if (rep83 && !st.have && match_name(entries[j].name, name) && !have83) {
          /* exact 8.3 entry with no attached LFN chain -- the genuine short
             file; remembered even if an LFN alias was seen earlier. */
          have83 = 1;
          a83_entry = entries[j];
          a83_sector = sector_num;
          a83_offset = j;
        }
        if (!or_seen &&
            (match_name(entries[j].name, name) ||
             (lfn_matches(&st, name) && lfn_checksum(entries[j].name) == st.sum))) {
          or_seen = 1;
          or_entry = entries[j];
          or_sector = sector_num;
          or_offset = j;
        }
        lfn_reset(&st);
      }
      spinlock_release_irqrestore(&fat_lock, flags);
    }
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    cluster = read_fat(cluster);
    spinlock_release_irqrestore(&fat_lock, flags);
  }
done_scan:
  {
    uint32_t sec; uint32_t off; struct fat16_dir_entry *ent;
    if (have83) { sec = a83_sector; off = a83_offset; ent = &a83_entry; }
    else if (or_seen) { sec = or_sector; off = or_offset; ent = &or_entry; }
    else { return -1; }
    if (out_entry) *out_entry = *ent;
    if (out_sector) *out_sector = sec;
    if (out_offset) *out_offset = off;
    return 0;
  }
}

static int name11_same(const char *a, const char *b) {
  for (int k = 0; k < 11; k++)
    if (a[k] != b[k]) return 0;
  return 1;
}

/* SEC-1: does one sector buffer already hold a live entry whose 11-byte
   short name equals `name11`?  Mirrors every other lookup by stopping at the
   first never-used (0x00) slot; deleted slots and LFN records (whose name[]
   holds UTF-16 fragments, not a name) are skipped.  Sets *ended when the
   scan reached the directory terminator. */
static int name_match_in_sector(const uint8_t *buf, const char *name11,
                                int *ended) {
  const struct fat16_dir_entry *entries = (const struct fat16_dir_entry *)buf;
  for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
    if (entries[j].name[0] == 0x00) {
      *ended = 1;
      return 0;
    }
    if (entries[j].name[0] == (char)0xE5) continue;
    if ((unsigned char)entries[j].attr == 0x0F) continue;
    if (name11_same(entries[j].name, name11)) return 1;
  }
  return 0;
}

/* SEC-1: scan the WHOLE directory (root when dir_cluster == 0, else the
   cluster chain) for a live entry named `name11`.  Caller holds fat_lock —
   like every other FAT16 site this runs virtio_blk_read_sector and the raw
   read_fat primitive under that single lock.  Returns 1 found / 0 absent /
   -1 on I/O error. */
static int dir_name_taken_locked(uint16_t dir_cluster, const char *name11) {
  uint8_t buf[SECTOR_SIZE];

  if (dir_cluster == 0) {
    for (uint32_t i = 0; i < root_dir_sectors; i++) {
      if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) return -1;
      int ended = 0;
      int hit = name_match_in_sector(buf, name11, &ended);
      if (hit) return 1;
      if (ended) return 0;
    }
    return 0;
  }

  uint16_t cluster = dir_cluster;
  while (cluster != 0 && cluster < 0xFFF0) {
    for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
      uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) return -1;
      int ended = 0;
      int hit = name_match_in_sector(buf, name11, &ended);
      if (hit) return 1;
      if (ended) return 0;
    }
    cluster = read_fat(cluster);
  }
  return 0;
}

/* SEC-1: LFN create probe.  Caller holds fat_lock.  Walks the directory with
   the same lfn_state accumulation as find_entry_in_root and reports:
     1  an entry with THIS long name already exists (true EEXIST),
     2  only the truncated 8.3 short form is taken by a DIFFERENT long name
        (VFAT mints a ~N alias for this case — never an error),
     0  nothing related present, -1 I/O error. */
static int lfn_create_state_locked(uint16_t dir_cluster, const char *longname,
                                   const char *short83) {
  uint8_t buf[SECTOR_SIZE];
  struct lfn_state st;
  int short_taken = 0;

#define LFN_PROBE_SECTOR(BUF)                                                  \
  do {                                                                         \
    struct fat16_dir_entry *entries = (struct fat16_dir_entry *)(BUF);         \
    for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {                      \
      if (entries[j].name[0] == 0x00) return short_taken ? 2 : 0;              \
      if (entries[j].name[0] == (char)0xE5) { lfn_reset(&st); continue; }      \
      if ((unsigned char)entries[j].attr == 0x0F) {                            \
        lfn_add_chunk(&st, (const unsigned char *)&entries[j]);                \
        continue;                                                              \
      }                                                                        \
      if (st.have && lfn_matches(&st, longname) &&                             \
          lfn_checksum(entries[j].name) == st.sum)                             \
        return 1; /* same long name */                                         \
      if (name11_same(entries[j].name, short83)) short_taken = 2;              \
      lfn_reset(&st);                                                          \
    }                                                                          \
  } while (0)

  lfn_reset(&st);
  if (dir_cluster == 0) {
    for (uint32_t i = 0; i < root_dir_sectors; i++) {
      if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) return -1;
      LFN_PROBE_SECTOR(buf);
    }
    return short_taken;
  }
  uint16_t cluster = dir_cluster;
  while (cluster != 0 && cluster < 0xFFF0) {
    for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
      uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) return -1;
      LFN_PROBE_SECTOR(buf);
    }
    cluster = read_fat(cluster);
  }
#undef LFN_PROBE_SECTOR
  return short_taken;
}

/* SEC-1: VFAT volume-label style alias: first 6 chars of the base + "~k" +
   the original extension.  Returns 0 on success (out11 filled). */
static void lfn_make_alias(const char *short83, int k, char *out11) {
  for (int i = 0; i < 11; i++) out11[i] = short83[i];
  out11[6] = '~';
  out11[7] = (char)('0' + (k % 10));
}

/* Claim a free slot.  Caller holds fat_lock: the whole scan-and-claim is ONE
   critical section, so two concurrent creates can never pick the same slot
   nor both pass a duplicate-name check before either writes (SEC-1). */
static int alloc_entry_in_dir_locked(uint16_t dir_cluster, const struct fat16_dir_entry *new_entry, uint32_t *out_sector, uint32_t *out_offset) {
  uint8_t buf[SECTOR_SIZE];

  if (dir_cluster == 0) {
    for (uint32_t i = 0; i < root_dir_sectors; i++) {
      if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) return -1;
      struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
        if (entries[j].name[0] == 0x00 || entries[j].name[0] == (char)0xE5) {
          entries[j] = *new_entry;
          if (virtio_blk_write_sector(root_dir_sector + i, buf, 1) != 0) return -1;
          if (out_sector) *out_sector = root_dir_sector + i;
          if (out_offset) *out_offset = j;
          return 0;
        }
      }
    }
    return -1;
  } else {
    uint16_t cluster = dir_cluster;
    uint16_t prev_cluster = 0;

    while (cluster != 0 && cluster < 0xFFF0) {
      for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
        uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
        if (virtio_blk_read_sector(sector_num, buf, 1) != 0) return -1;
        struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
          if (entries[j].name[0] == 0x00 || entries[j].name[0] == (char)0xE5) {
            entries[j] = *new_entry;
            if (virtio_blk_write_sector(sector_num, buf, 1) != 0) return -1;
            if (out_sector) *out_sector = sector_num;
            if (out_offset) *out_offset = j;
            return 0;
          }
        }
      }
      prev_cluster = cluster;
      cluster = read_fat(cluster);
    }

    if (prev_cluster != 0) {
      uint16_t new_c = alloc_cluster();
      if (new_c == 0) return -1;
      write_fat(prev_cluster, new_c);

      uint32_t sector_num = data_sector + (new_c - 2) * bpb_sectors_per_cluster;
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) return -1;
      struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
      entries[0] = *new_entry;
      if (virtio_blk_write_sector(sector_num, buf, 1) != 0) return -1;
      if (out_sector) *out_sector = sector_num;
      if (out_offset) *out_offset = 0;
      return 0;
    }
    return -1;
  }
}

/* Public-ish wrapper.  With check_dup set, existence is verified over the
   ENTIRE directory inside the same fat_lock critical section as the claim:
   O_CREAT|O_EXCL (and mkdir) get create-if-absent atomically; a collision
   returns -2 so callers can map it to EEXIST. */
static int alloc_entry_in_dir(uint16_t dir_cluster, const struct fat16_dir_entry *new_entry, uint32_t *out_sector, uint32_t *out_offset, int check_dup) {
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  int rc;
  if (check_dup) {
    int taken = dir_name_taken_locked(dir_cluster, new_entry->name);
    if (taken < 0) rc = -1;
    else if (taken > 0) rc = -2; /* name already exists */
    else rc = alloc_entry_in_dir_locked(dir_cluster, new_entry, out_sector, out_offset);
  } else {
    rc = alloc_entry_in_dir_locked(dir_cluster, new_entry, out_sector, out_offset);
  }
  spinlock_release_irqrestore(&fat_lock, flags);
  return rc;
}

/* Fill `slots` with nchunks VFAT 0x0F records followed by the 8.3 entry.
   The chain is stored tail-first: slot 0 holds the final 13-character
   chunk and carries the 0x40 last-flag. */
static void lfn_fill_run(struct fat16_dir_entry *slots, const char *longname,
                         int nchunks, const char *short83) {
  static const int offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
  int ln = 0;
  unsigned char sum = lfn_checksum(short83);
  while (longname[ln]) ln++;
  for (int s = 0; s < nchunks; s++) {
    int part = nchunks - 1 - s; /* 0 = head of the name */
    unsigned char *e = (unsigned char *)&slots[s];
    for (int k = 0; k < 32; k++) e[k] = 0;
    e[0] = (unsigned char)((s == 0 ? 0x40 : 0) | (nchunks - s));
    e[11] = 0x0F;
    e[13] = sum;
    int nchar = 0;
    for (int i = 0; i < 13; i++) {
      int idx = part * 13 + i;
      unsigned int v = 0xFFFF;
      if (idx < ln) {
        v = (unsigned char)longname[idx];
        nchar = i + 1;
      } else if (i == nchar) {
        v = 0x0000; /* terminator at the end of a partial chunk */
      }
      e[offs[i]] = (unsigned char)v;
      e[offs[i] + 1] = (unsigned char)(v >> 8);
    }
  }
}

/* Store a run of `need` entries that begins at slot `term` of `e1` and,
   when it does not all fit, continues at the start of `e2` (the following
   sector).  `first` = min(16 - term, need) entries go to `e1`. */
static void lfn_place_run(struct fat16_dir_entry *e1, struct fat16_dir_entry *e2,
                          int term, int first, int need, const char *longname,
                          int nchunks, const char *short83,
                          const struct fat16_dir_entry *short_entry) {
  struct fat16_dir_entry run[SECTOR_SIZE / 32];
  lfn_fill_run(run, longname, nchunks, short83);
  run[nchunks] = *short_entry;
  for (int k = 0; k < first; k++) e1[term + k] = run[k];
  for (int k = first; k < need; k++) e2[k - first] = run[k];
}

/* Allocate a run of (nchunks + 1) entries: the long name's 0x0F records
   immediately followed by `short_entry`.  A run starts at or before the
   directory's first 0x00 slot, because every lookup stops at that slot --
   entries placed past it would be invisible.  When the room left at the
   first 0x00 slot is too small for the whole run, the run continues in the
   next sector instead of skipping ahead.  Returns the location of the
   short entry, or -1. */
static int alloc_lfn_run_locked(uint16_t dir_cluster, const char *longname,
                                const char *short83, int nchunks,
                                const struct fat16_dir_entry *short_entry,
                                uint32_t *out_sector, uint32_t *out_offset) {
  uint8_t buf[SECTOR_SIZE];
  int need = nchunks + 1;

  if (dir_cluster == 0) {
    for (uint32_t i = 0; i < root_dir_sectors; i++) {
      if (virtio_blk_read_sector(root_dir_sector + i, buf, 1) != 0) return -1;
      struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
      int term = -1;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
        if (entries[j].name[0] == 0x00) { term = (int)j; break; }
      }
      unsigned int jmax = (term >= 0) ? (unsigned int)term
                                      : (SECTOR_SIZE / 32 - need);
      for (unsigned int j = 0; j + need <= SECTOR_SIZE / 32 && j <= jmax; j++) {
        int run = 0;
        for (int k = 0; k < need; k++) {
          if (entries[j + k].name[0] == 0x00 ||
              entries[j + k].name[0] == (char)0xE5)
            run++;
        }
        if (run == need) {
          lfn_fill_run(entries + j, longname, nchunks, short83);
          entries[j + nchunks] = *short_entry;
          if (virtio_blk_write_sector(root_dir_sector + i, buf, 1) != 0) return -1;
          if (out_sector) *out_sector = root_dir_sector + i;
          if (out_offset) *out_offset = j + nchunks;
          return 0;
        }
      }
      if (term >= 0 && (SECTOR_SIZE / 32 - (unsigned int)term) < (unsigned int)need &&
          i + 1 < root_dir_sectors) {
        uint8_t buf2[SECTOR_SIZE];
        if (virtio_blk_read_sector(root_dir_sector + i + 1, buf2, 1) != 0) return -1;
        int first = SECTOR_SIZE / 32 - term;
        struct fat16_dir_entry *e2 = (struct fat16_dir_entry *)buf2;
        int fits = 1;
        for (int k = 0; k < need - first; k++) {
          if (e2[k].name[0] != 0x00 && e2[k].name[0] != (char)0xE5) fits = 0;
        }
        if (fits) {
          lfn_place_run(entries, e2, term, first, need, longname, nchunks,
                        short83, short_entry);
          if (virtio_blk_write_sector(root_dir_sector + i, buf, 1) != 0 ||
              virtio_blk_write_sector(root_dir_sector + i + 1, buf2, 1) != 0) return -1;
          if (out_sector) *out_sector = (nchunks < first)
              ? (root_dir_sector + i) : (root_dir_sector + i + 1);
          if (out_offset) *out_offset = (nchunks < first)
              ? (uint32_t)(term + nchunks) : (uint32_t)(nchunks - first);
          return 0;
        }
      }
    }
    return -1;
  }

  uint16_t cluster = dir_cluster;
  uint16_t prev_cluster = 0;
  while (cluster != 0 && cluster < 0xFFF0) {
    for (uint32_t s = 0; s < bpb_sectors_per_cluster; s++) {
      uint32_t sector_num = data_sector + (cluster - 2) * bpb_sectors_per_cluster + s;
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) return -1;
      struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
      int term = -1;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
        if (entries[j].name[0] == 0x00) { term = (int)j; break; }
      }
      unsigned int jmax = (term >= 0) ? (unsigned int)term
                                      : (SECTOR_SIZE / 32 - need);
      for (unsigned int j = 0; j + need <= SECTOR_SIZE / 32 && j <= jmax; j++) {
        int run = 0;
        for (int k = 0; k < need; k++) {
          if (entries[j + k].name[0] == 0x00 ||
              entries[j + k].name[0] == (char)0xE5)
            run++;
        }
        if (run == need) {
          lfn_fill_run(entries + j, longname, nchunks, short83);
          entries[j + nchunks] = *short_entry;
          if (virtio_blk_write_sector(sector_num, buf, 1) != 0) return -1;
          if (out_sector) *out_sector = sector_num;
          if (out_offset) *out_offset = j + nchunks;
          return 0;
        }
      }
      if (term >= 0 && (SECTOR_SIZE / 32 - (unsigned int)term) < (unsigned int)need &&
          s + 1 < bpb_sectors_per_cluster) {
        uint32_t next_sector = sector_num + 1;
        uint8_t buf2[SECTOR_SIZE];
        if (virtio_blk_read_sector(next_sector, buf2, 1) != 0) return -1;
        int first = SECTOR_SIZE / 32 - term;
        struct fat16_dir_entry *e2 = (struct fat16_dir_entry *)buf2;
        int fits = 1;
        for (int k = 0; k < need - first; k++) {
          if (e2[k].name[0] != 0x00 && e2[k].name[0] != (char)0xE5) fits = 0;
        }
        if (fits) {
          lfn_place_run(entries, e2, term, first, need, longname, nchunks,
                        short83, short_entry);
          if (virtio_blk_write_sector(sector_num, buf, 1) != 0 ||
              virtio_blk_write_sector(next_sector, buf2, 1) != 0) return -1;
          if (out_sector) *out_sector = (nchunks < first) ? sector_num : next_sector;
          if (out_offset) *out_offset = (nchunks < first)
              ? (uint32_t)(term + nchunks) : (uint32_t)(nchunks - first);
          return 0;
        }
      }
    }
    prev_cluster = cluster;
    cluster = read_fat(cluster);
  }

  /* No run of free slots: extend the directory with a fresh cluster and
     place the run at its start, mirroring alloc_entry_in_dir(). */
  if (prev_cluster != 0) {
    uint16_t new_c = alloc_cluster();
    if (new_c == 0) return -1;
    write_fat(prev_cluster, new_c);

    uint32_t sector_num = data_sector + (new_c - 2) * bpb_sectors_per_cluster;
    if (virtio_blk_read_sector(sector_num, buf, 1) != 0) return -1;
    struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
    lfn_fill_run(entries, longname, nchunks, short83);
    entries[nchunks] = *short_entry;
    if (virtio_blk_write_sector(sector_num, buf, 1) != 0) return -1;
    if (out_sector) *out_sector = sector_num;
    if (out_offset) *out_offset = nchunks;
    return 0;
  }
  return -1;
}

/* SEC-1 wrapper: with check_dup, the whole-directory name scan and the run
   claim share ONE fat_lock critical section (collision -> -2).  A truncated
   short form taken by a DIFFERENT long name is not an error: VFAT mints a
   ~N alias so both files coexist, exactly as mcopy/FAT volumes do. */
static int alloc_lfn_run(uint16_t dir_cluster, const char *longname,
                         const char *short83, int nchunks,
                         const struct fat16_dir_entry *short_entry,
                         uint32_t *out_sector, uint32_t *out_offset,
                         int check_dup, char *out_short83) {
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  int rc = -1;
  if (check_dup) {
    int st = lfn_create_state_locked(dir_cluster, longname, short83);
    if (st < 0) {
      rc = -1;
    } else if (st == 1) {
      rc = -2; /* this exact long name already exists */
    } else {
      char alias[11];
      if (st == 2) {
        /* Short form taken by a different long name: mint a free ~N alias. */
        int k, free_alias = 0;
        for (k = 1; k <= 9; k++) {
          lfn_make_alias(short83, k, alias);
          int taken = dir_name_taken_locked(dir_cluster, alias);
          if (taken < 0) break; /* I/O error: rc stays -1 */
          if (!taken) { free_alias = 1; break; }
        }
        if (free_alias) {
          /* The short entry MUST carry the alias too: its bytes feed the
             LFN checksum the resolver verifies, and fat16_sync_entry()
             would later write the caller's un-aliased copy back over it. */
          struct fat16_dir_entry aliased = *short_entry;
          for (int i = 0; i < 11; i++) aliased.name[i] = alias[i];
          rc = alloc_lfn_run_locked(dir_cluster, longname, alias, nchunks,
                                    &aliased, out_sector, out_offset);
          if (rc == 0 && out_short83)
            for (int i = 0; i < 11; i++) out_short83[i] = alias[i];
        } else if (k <= 9)
          rc = -1; /* I/O error during alias probe */
        else
          rc = -2; /* alias space exhausted: report taken */
      } else {
        rc = alloc_lfn_run_locked(dir_cluster, longname, short83, nchunks,
                                  short_entry, out_sector, out_offset);
        if (rc == 0 && out_short83)
          for (int i = 0; i < 11; i++) out_short83[i] = short83[i];
      }
    }
  } else {
    rc = alloc_lfn_run_locked(dir_cluster, longname, short83, nchunks,
                              short_entry, out_sector, out_offset);
    if (rc == 0 && out_short83)
      for (int i = 0; i < 11; i++) out_short83[i] = short83[i];
  }
  spinlock_release_irqrestore(&fat_lock, flags);
  return rc;
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

/* ------------------------------------------------------------------
 * RTC-backed FAT dirent timestamps (selfhost w10-fstime)
 * -----------------------------------------------------------------
 * The FAT16 spec encodes a timestamp in two 16-bit fields:
 *   date = ((year-1980) << 9) | (month << 5) | day
 *   time = (hour << 11) | (minute << 5) | (second / 2)   (2 s resolution)
 * Until now the writer left both zeroed (1980-01-01 00:00), so every
 * in-OS file had the same mtime as every other: in-OS GNU make could
 * never see a target NEWER than its prerequisites and rebuilt all 59
 * kernel TUs on every run (~90 min).  All entry writes below stamp the
 * REAL wall clock (UTC, RTC-read at boot, kernel/time.c).  The encode
 * clamps pathological years (no-RTC fallback collapses to epoch 0) to
 * a valid pre-1980 sentinel so the on-disk fields stay well-formed. */
static uint64_t fat16_now_epoch(void) {
  return hb_clock_realtime_ms() / 1000ULL;
}

static void fat16_stamp_entry(struct fat16_dir_entry *e) {
  int year, month, day, hour, minute, second, weekday;
  uint64_t now = fat16_now_epoch();
  rtc_epoch_to_time(now, &year, &month, &day, &hour, &minute, &second, &weekday);
  if (year < 1980 || year > 2107) {
    year = 1980; month = 1; day = 1;
    hour = 0; minute = 0; second = 0;
  } else {
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (day < 1) day = 1;
    if (day > 31) day = 31;
  }
  e->date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
  e->time = (uint16_t)((hour << 11) | (minute << 5) | (second / 2));
}

/* Decode a dirent's date/time back to Unix epoch seconds (UTC).  The
 * stat path and the unit tests use this; it mirrors fat16_stamp_entry. */
uint64_t fat16_entry_epoch(const struct fat16_dir_entry *e) {
  int year = ((e->date >> 9) & 0x7F) + 1980;
  int month = (e->date >> 5) & 0x0F;
  int day = e->date & 0x1F;
  int hour = (e->time >> 11) & 0x1F;
  int minute = (e->time >> 5) & 0x3F;
  int second = (e->time & 0x1F) * 2;
  return rtc_make_epoch(year, month, day, hour, minute, second);
}

/* SEC-1: excl != 0 gives O_CREAT|O_EXCL semantics atomically.  Return codes:
   0 = opened/created, -2 = name exists and excl was set (EEXIST), -1 = other
   failure.  With excl == 0 the create path still collides-checked inside one
   fat_lock critical section: if a peer creator wins the race we simply
   re-resolve and open the winner's entry instead of appending a duplicate. */
int fat16_open_ex(const char* filename, struct file* f, int excl) {
  char abs_path[256];
  fat16_absolute_path(filename, abs_path);

  uart_puts(abs_path);
  uart_puts("\n");

  for (int attempt = 0;; attempt++) {
    struct fat16_dir_entry entry;
    uint32_t sector = 0;
    uint32_t offset = 0;

    if (fat16_resolve_path(abs_path, &entry, &sector, &offset) == 0) {
      /* Never hand back a file handle for a DIRECTORY.  The synthetic root
         entry ("/" or "") reports dir_sector == 0 and attr 0x10; with a
         handle on it, file_open()'s O_TRUNC path calls fat16_truncate() ->
         fat16_sync_entry(), which read-modify-writes LBA 0 and splices the
         synthetic entry over the BOOT SECTOR (observed post-wave: boot
         sector replaced by an 11-space name + attr 0x10 entry).  A real
         subdirectory has the same shape at its own dir_sector and would get
         its entry's cluster/size fields clobbered the same way.  Directories
         are reached via chdir/read_dir, never as files. */
      if (sector == 0 || (entry.attr & 0x10)) {
        return -1;
      }
      if (excl) {
        return -2; /* O_EXCL: the name is taken */
      }
      f->type = FILE_TYPE_FAT16;
      f->fat16.entry = entry;
      f->fat16.disk_seen = entry; /* SEC-5 snapshot: exactly what the slot holds */
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
    /* GB-067: a freshly created regular file MUST carry the archive attribute
       (0x20), not attr=0.  attr=0 is not a valid 8.3 entry for a file: it names
       nothing the FAT spec defines, and a later re-scan that sees only attr == 0
       (probe58: "attr=0, cluster=0, size=0" survived) reads the entry as empty/absent.
       mcopy / host-staged files all carry 0x20, so we match the ecosystem. */
    new_entry.attr = 0x20; /* ATTR_ARCHIVE, same as host-written regular files */
    for (int k = 0; k < 10; k++) new_entry.reserved[k] = 0;
    fat16_stamp_entry(&new_entry);   /* real RTC date/time, not 1980-01-01 */
    new_entry.start_cluster = 0;
    new_entry.file_size = 0;

    int rc;
    char used_short[11];
    if (name_is_83(last_comp)) {
      rc = alloc_entry_in_dir(parent_entry.start_cluster, &new_entry, &sector, &offset, 1);
      for (int k = 0; k < 11; k++) used_short[k] = formatted_name[k];
    } else {
      /* Long name: write the 0x0F records plus the 8.3 entry as one run so
         later opens match by the long name (and by the short name). */
      int ln = 0;
      while (last_comp[ln]) ln++;
      int nchunks = (ln + 12) / 13;
      if (nchunks < 1) nchunks = 1;
      rc = alloc_lfn_run(parent_entry.start_cluster, last_comp, formatted_name,
                         nchunks, &new_entry, &sector, &offset, 1, used_short);
    }

    if (rc == -2) {
      /* Lost the create race.  O_EXCL reports EEXIST; a plain open re-resolves
         and opens whoever won — never appends a second entry for the name. */
      if (excl || attempt > 0) return excl ? -2 : -1;
      continue;
    }
    if (rc != 0) {
      return -1;
    }

    /* SEC-1: on an alias collision the run was stored under a minted ~N
       short name; keep our in-memory copy identical to the on-disk entry so
       fat16_sync_entry() never writes the un-aliased name back. */
    for (int k = 0; k < 11; k++) new_entry.name[k] = used_short[k];

    f->type = FILE_TYPE_FAT16;
    f->fat16.entry = new_entry;
    f->fat16.disk_seen = new_entry; /* SEC-5: alloc just wrote these bytes */
    f->fat16.dir_sector = sector;
    f->fat16.dir_offset = offset;
    f->fat16.cursor = 0;
    f->fat16.dirty = 0;
    return 0;
  }
}

int fat16_open(const char* filename, struct file* f) {
  return fat16_open_ex(filename, f, 0);
}

/* GB-020: open a DIRECTORY for getdents64.  Resolves `path`, requires it to be
 * an actual directory (attr 0x10) and not the synthetic root (which has no
 * on-disk entry - dir_sector 0), and fills the file with the directory's own
 * entry so fat16_getdents64 can read its children.  The resulting fd is typed
 * FILE_TYPE_FAT16DIR by the caller, which keeps O_TRUNC / data-read paths off
 * it (a directory has no data path, and truncating one must never reach
 * fat16_truncate).  Returns 0 on success, -1 on error (ENOTDIR/ENOENT). */
int fat16_open_dir(const char* path, struct file* f) {
  if (!path || !f) return -1;
  char abs_path[256];
  fat16_absolute_path(path, abs_path);

  struct fat16_dir_entry entry;
  uint32_t sector = 0, offset = 0;
  if (fat16_resolve_path(abs_path, &entry, &sector, &offset) != 0) return -1;
  if (!(entry.attr & 0x10)) return -1;      /* not a directory */
  if (sector == 0) return -1;               /* synthetic root: no children fd */

  f->type = FILE_TYPE_FAT16DIR;
  f->fat16.entry = entry;                   /* the directory's own entry */
  f->fat16.dir_sector = sector;
  f->fat16.dir_offset = offset;
  f->fat16.cursor = 0;                      /* getdents resume index */
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
    /* LFN state must carry ACROSS sector boundaries: a run that spans
       sectors (alloc_lfn_run's cross-sector placement) has its 0x0F
       records at the end of one sector and the short entry at the start
       of the next, so the accumulator belongs OUTSIDE the sector loop
       (mirrors find_entry_in_root). */
    struct lfn_state st;
    lfn_reset(&st);
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
        if (entries[j].name[0] == (char)0xE5) {
          lfn_reset(&st);
          continue;
        }
        if (entries[j].attr == 0x0F) { // LFN
          lfn_add_chunk(&st, (const unsigned char *)&entries[j]);
          continue;
        }
        if (entries[j].attr & 0x08) { // Volume Label
          lfn_reset(&st);
          continue;
        }

        if (current_idx == index) {
          lfn_entry_display(&st, &entries[j], out_name, 32);
          if (out_attr) *out_attr = entries[j].attr;
          if (out_size) *out_size = entries[j].file_size;

          spinlock_release_irqrestore(&fat_lock, flags);
          return 0;
        }
        lfn_reset(&st);
        current_idx++;
      }
    }
  } else {
    /* LFN state carries across sectors AND cluster boundaries, mirroring
       find_entry_in_subdir (a cross-sector run wraps at the end of one
       sector into the start of the next). */
    struct lfn_state st;
    lfn_reset(&st);
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
          if (entries[j].name[0] == (char)0xE5) {
            lfn_reset(&st);
            continue;
          }
          if (entries[j].attr == 0x0F) { // LFN
            lfn_add_chunk(&st, (const unsigned char *)&entries[j]);
            continue;
          }

          if (current_idx == index) {
            lfn_entry_display(&st, &entries[j], out_name, 32);
            if (out_attr) *out_attr = entries[j].attr;
            if (out_size) *out_size = entries[j].file_size;

            spinlock_release_irqrestore(&fat_lock, flags);
            return 0;
          }
          lfn_reset(&st);
          current_idx++;
        }
      }
      cluster = read_fat(cluster);
    }
  }

  spinlock_release_irqrestore(&fat_lock, flags);
  return -1;
}

/* Mark deleted (0xE5) the short entry at (sector, offset) along with the
   VFAT 0x0F long-name records that immediately precede it: in the same
   sector and — when the entry is the FIRST slot of a sector — the tail of
   the previous sector (a run placed across the sector boundary by
   alloc_lfn_run).  Returning the slots keeps unlink/rename/rollback from
   leaking 0x0F records. */
static int lfn_del_entry(uint32_t sector, uint32_t offset) {
  uint8_t buf[SECTOR_SIZE];
  if (offset == 0 && sector > 0) {
    if (virtio_blk_read_sector(sector - 1, buf, 1) != 0) return -1;
    struct fat16_dir_entry *prev = (struct fat16_dir_entry *)buf;
    for (int k = SECTOR_SIZE / 32 - 1; k >= 0; k--) {
      if (prev[k].attr != 0x0F) break;
      prev[k].name[0] = (char)0xE5;
    }
    if (virtio_blk_write_sector(sector - 1, buf, 1) != 0) return -1;
  }
  if (virtio_blk_read_sector(sector, buf, 1) != 0) return -1;
  struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
  for (int k = (int)offset - 1; k >= 0; k--) {
    if (entries[k].attr != 0x0F) break;
    entries[k].name[0] = (char)0xE5;
  }
  entries[offset].name[0] = (char)0xE5;
  if (virtio_blk_write_sector(sector, buf, 1) != 0) return -1;
  return 0;
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

  spinlock_release_irqrestore(&fat_lock, flags);
  flags = spinlock_acquire_irqsave(&fat_lock);
  /* Free the short entry AND its 0x0F long-name records (same sector plus
     any that spill into the previous sector) — the records belong to it
     and would otherwise leak slots. */
  if (lfn_del_entry(sector, offset) != 0) {
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }
  spinlock_release_irqrestore(&fat_lock, flags);
  return 0;
}

/* ------------------------------------------------------------------
 * W2-14 (selfhost): rmdir -- remove an EMPTY directory.
 * ----------------------------------------------------------------- */

/* Is directory `cluster` (0 = volume root) EMPTY -- contains only "."
 * and ".." plus deleted (0xE5)/end (0x00) markers?  Any live entry --
 * INCLUDING an orphan 0x0F long-name record or a volume label -- answers
 * "not empty" so rmdir stays conservative and never destroys data.
 * Caller holds fat_lock (mirrors alloc_entry_in_dir's sector I/O under
 * the lock).  Returns 1 empty, 0 not, -1 on I/O error. */
static int fat16_dir_empty(uint16_t cluster) {
  uint8_t buf[SECTOR_SIZE];
  uint32_t total = (cluster == 0) ? root_dir_sectors
                                  : bpb_sectors_per_cluster;
  if (total == 0)
    return 1; /* nothing to scan: empty */
  uint16_t c = cluster;
  for (;;) {
    uint32_t base = (c == 0) ? root_dir_sector
                    : data_sector + (uint32_t)(c - 2) * bpb_sectors_per_cluster;
    for (uint32_t s = 0; s < total; s++) {
      if (virtio_blk_read_sector(base + s, buf, 1) != 0)
        return -1;
      struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
        uint8_t b0 = (uint8_t)entries[j].name[0];
        if (b0 == 0x00)
          return 1; /* end marker: rest of the dir is empty */
        if (b0 == 0xE5)
          continue; /* deleted slot */
        struct fat16_dir_entry *e = &entries[j];
        if (e->attr == 0x0F)
          return 0; /* orphan LFN record (conservative) */
        if (e->attr & 0x08)
          return 0; /* volume label */
        if (e->name[0] == '.' &&
            (e->name[1] == ' ' ||
             (e->name[1] == '.' && e->name[2] == ' ')))
          continue; /* "." or ".." */
        return 0;   /* a real file/dir entry: NOT empty */
      }
    }
    if (c == 0)
      return 1; /* whole root scanned, no live entry */
    c = read_fat(c);
    if (c == 0 || c >= 0xFFF0)
      return 1; /* cluster chain ended */
  }
}

/* Remove an EMPTY directory, POSIX-errno-faithful per what fat16
 * supports:
 *   -ENOENT    path does not resolve,
 *   -ENOTDIR   path names a regular file (rmdir is dir-only),
 *   -ENOTEMPTY the directory still holds any entry (incl. orphan LFN /
 *              volume-label records -- conservative),
 *   -EBUSY     the volume root, or "." / ".." (POSIX leaves these to
 *              EBUSY/EINVAL; EBUSY chosen and documented),
 *   -EIO       sector I/O failure mid-operation.
 * unlink (fat16_unlink) deliberately STAYS dir-refusing (-1 / folded to
 * -ENOENT by the trap layer): POSIX unlink(2) must not remove
 * directories -- rmdir is the directory-removal primitive.
 * NOTE (documented limitation): the kernel does not track which
 * directory is a process's cwd, so "directory in use as cwd" is NOT
 * reported as EBUSY (POSIX allows it; LLVM never does this). */
int fat16_rmdir(const char *path) {
  char abs_path[256];
  fat16_absolute_path(path, abs_path);

  if (!abs_path[0] || !abs_path[1])
    return -EBUSY; /* the volume root itself */

  struct fat16_dir_entry entry;
  uint32_t sector = 0;
  uint32_t offset = 0;
  if (fat16_resolve_path(abs_path, &entry, &sector, &offset) != 0)
    return -ENOENT;

  if (!(entry.attr & 0x10))
    return -ENOTDIR;

  /* On-disk "." (name ".          ") or ".." (name "..         "). */
  if (entry.name[0] == '.' &&
      (entry.name[1] == ' ' ||
       (entry.name[1] == '.' && entry.name[2] == ' ')))
    return -EBUSY;

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  int empty = fat16_dir_empty(entry.start_cluster);
  if (empty < 0) {
    spinlock_release_irqrestore(&fat_lock, flags);
    return -EIO;
  }
  if (!empty) {
    spinlock_release_irqrestore(&fat_lock, flags);
    return -ENOTEMPTY;
  }

  /* Free the directory's cluster chain (mirrors fat16_unlink). */
  uint16_t cluster = entry.start_cluster;
  while (cluster != 0 && cluster < 0xFFF0) {
    uint16_t next = read_fat(cluster);
    write_fat(cluster, 0x0000);
    cluster = next;
  }

  /* Free the short entry AND its 0x0F long-name records (same sector
     plus any that spill into the previous sector). */
  int r = lfn_del_entry(sector, offset);
  spinlock_release_irqrestore(&fat_lock, flags);
  return r == 0 ? 0 : -EIO;
}

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
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  if (virtio_blk_read_sector(sector, buf, 1) != 0) {
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }
  struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
  for (unsigned int j = 0; j < SECTOR_SIZE / 32; j++) {
    if (entries[j].name[0] == 0x00) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -1;
    }
    if (entries[j].name[0] != '.') continue;
    if (!(entries[j].name[1] == '.' && entries[j].name[2] == ' ')) continue;
    entries[j].start_cluster = new_parent_cluster;
    if (virtio_blk_write_sector(sector, buf, 1) != 0) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -1;
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    return 0;
  }
  spinlock_release_irqrestore(&fat_lock, flags);
  return -1;
}

int fat16_rename(const char* oldname, const char* newname) {
  char abs_old[256];
  char abs_new[256];
  fat16_absolute_path(oldname, abs_old);
  fat16_absolute_path(newname, abs_new);

  uart_puts(abs_old);
  uart_puts(" new=");
  uart_puts(abs_new);
  uart_puts("\n");

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

    /* Pure 8.3 -> 8.3 rename rewrites the short name in place.  When the
       old or the new name is long, the whole VFAT run must be rewritten:
       a fresh slot (with a new 0x0F run when the new name is long) is
       allocated carrying the entry's data, then the old slot and its
       records are freed — otherwise the long name is silently lost. */
    uint8_t buf[SECTOR_SIZE];
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    int old_has_lfn = 0;
    if (virtio_blk_read_sector(sector, buf, 1) != 0) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -1;
    }
    struct fat16_dir_entry* entries = (struct fat16_dir_entry*)buf;
    old_has_lfn = (offset > 0 && entries[offset - 1].attr == 0x0F);
    if (!old_has_lfn && name_is_83(last_comp)) {
      for (int k = 0; k < 11; k++) {
        entries[offset].name[k] = formatted_name[k];
      }
      if (virtio_blk_write_sector(sector, buf, 1) != 0) {
        spinlock_release_irqrestore(&fat_lock, flags);
        return -1;
      }
      spinlock_release_irqrestore(&fat_lock, flags);
      return 0;
    }
    spinlock_release_irqrestore(&fat_lock, flags);

    struct fat16_dir_entry moved = entry;
    for (int k = 0; k < 11; k++) moved.name[k] = formatted_name[k];

    uint32_t new_sector = 0;
    uint32_t new_offset = 0;
    if (name_is_83(last_comp)) {
      if (alloc_entry_in_dir(new_parent.start_cluster, &moved,
                             &new_sector, &new_offset, 0) != 0) {
        return -1;
      }
    } else {
      int ln = 0;
      while (last_comp[ln]) ln++;
      int nchunks = (ln + 12) / 13;
      if (nchunks < 1) nchunks = 1;
      if (alloc_lfn_run(new_parent.start_cluster, last_comp, formatted_name,
                        nchunks, &moved, &new_sector, &new_offset, 0, 0) != 0) {
        return -1;
      }
    }
    flags = spinlock_acquire_irqsave(&fat_lock);
    int del = lfn_del_entry(sector, offset);
    spinlock_release_irqrestore(&fat_lock, flags);
    if (del != 0) {
      /* Roll the new entry back so no duplicate is left behind. */
      uint8_t nbuf[SECTOR_SIZE];
      flags = spinlock_acquire_irqsave(&fat_lock);
      if (virtio_blk_read_sector(new_sector, nbuf, 1) == 0) {
        struct fat16_dir_entry *es = (struct fat16_dir_entry *)nbuf;
        es[new_offset].name[0] = (char)0xE5;
        (void)virtio_blk_write_sector(new_sector, nbuf, 1);
      }
      spinlock_release_irqrestore(&fat_lock, flags);
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
  if (name_is_83(last_comp)) {
    if (alloc_entry_in_dir(new_parent.start_cluster, &moved,
                           &new_sector, &new_offset, 0) != 0) {
      return -1;
    }
  } else {
    /* A long destination name needs its own 0x0F run in the target
       directory, exactly like fat16_open's create path. */
    int ln = 0;
    while (last_comp[ln]) ln++;
    int nchunks = (ln + 12) / 13;
    if (nchunks < 1) nchunks = 1;
    if (alloc_lfn_run(new_parent.start_cluster, last_comp, formatted_name,
                      nchunks, &moved, &new_sector, &new_offset, 0, 0) != 0) {
      return -1;
    }
  }

  uint8_t buf[SECTOR_SIZE];
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  if (virtio_blk_read_sector(sector, buf, 1) != 0) {
    /* Roll the new entry back so no duplicate is left behind. */
    uint8_t nbuf[SECTOR_SIZE];
    if (virtio_blk_read_sector(new_sector, nbuf, 1) == 0) {
      struct fat16_dir_entry *es = (struct fat16_dir_entry *)nbuf;
      es[new_offset].name[0] = (char)0xE5;
      (void)virtio_blk_write_sector(new_sector, nbuf, 1);
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }
  /* Free the old slot AND its 0x0F long-name records (they belong to it). */
  if (lfn_del_entry(sector, offset) != 0) {
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }
  spinlock_release_irqrestore(&fat_lock, flags);

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
  fat16_stamp_entry(&new_dir_entry);   /* real RTC date/time (mkdir is a write) */
  new_dir_entry.start_cluster = new_cluster;
  new_dir_entry.file_size = 0;

  if (name_is_83(last_comp)) {
    if (alloc_entry_in_dir(parent_entry.start_cluster, &new_dir_entry, 0, 0, 1) != 0) {
      flags = spinlock_acquire_irqsave(&fat_lock);
      write_fat(new_cluster, 0x0000);
      spinlock_release_irqrestore(&fat_lock, flags);
      return -1;
    }
  } else {
    /* Long directory name: write the 0x0F run + 8.3 entry like the file
       create path, so the directory stays reachable by its real name. */
    int ln = 0;
    while (last_comp[ln]) ln++;
    int nchunks = (ln + 12) / 13;
    if (nchunks < 1) nchunks = 1;
    if (alloc_lfn_run(parent_entry.start_cluster, last_comp, formatted_name,
                      nchunks, &new_dir_entry, 0, 0, 1, 0) != 0) {
      flags = spinlock_acquire_irqsave(&fat_lock);
      write_fat(new_cluster, 0x0000);
      spinlock_release_irqrestore(&fat_lock, flags);
      return -1;
    }
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
 * Writes one 32-byte directory entry as a single read-modify-write that is
 * atomic against concurrent writers: fat_lock is held across the sector
 * read, the entry splice, and the write-back.  (The lock is safe to hold
 * across virtio transfers; the truncate path already relies on that.)  A
 * bounded verify pass follows as a belt-and-braces check; with the lock in
 * place it always succeeds on the first try.
 */
/* SEC-5: guarded dirent write-back.  The slot may only be rewritten while it
   still holds EXACTLY the bytes this handle last saw (snapshot taken at open
   and refreshed after each of our own write-backs).  That blocks the W9
   rename/reuse case AND the stronger one the old name-only guard missed: a
   same-name unlink+recreate that swapped cluster/size under an open handle —
   writing our stale cached copy there would silently point the new file at
   the wrong clusters.  Check and write happen under ONE fat_lock critical
   section, so no peer can slip between them (the old guard read the sector
   unlocked).  Returns 0 written, -EIO skipped or failed. */
static int fat16_sync_entry(struct file* f) {
  /* Only entries this handle actually changed may be written back: a
     long-lived read descriptor (e.g. sed's input) must not touch the slot. */
  if (!f->fat16.dirty) {
    return 0;
  }
  struct fat16_dir_entry entry = *(struct fat16_dir_entry*)&f->fat16.entry;
  if (f->fat16.dir_sector == 0) {
    return 0; /* no real on-disk slot (synthetic root handle) */
  }

  uint8_t buf[SECTOR_SIZE];
  for (int attempt = 0; attempt < 4; attempt++) {
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    if (virtio_blk_read_sector(f->fat16.dir_sector, buf, 1) != 0) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -EIO;
    }
    struct fat16_dir_entry *on =
        &((struct fat16_dir_entry *)buf)[f->fat16.dir_offset];
    unsigned char b0 = (unsigned char)on->name[0];
    if (b0 == 0x00 || b0 == (char)0xE5) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -EIO; /* the entry was deleted under this open handle */
    }
    if (!fat16_entry_eq(on, &f->fat16.disk_seen)) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -EIO; /* someone else renamed/recreated the slot: skip */
    }
    *on = entry;
    if (virtio_blk_write_sector(f->fat16.dir_sector, buf, 1) != 0) {
      spinlock_release_irqrestore(&fat_lock, flags);
      return -EIO;
    }
    spinlock_release_irqrestore(&fat_lock, flags);
    /* Belt-and-braces verify (mirrors the old dir_entry_write): with the
       lock held across the RMW this passes first try. */
    if (virtio_blk_read_sector(f->fat16.dir_sector, buf, 1) != 0) return -EIO;
    if (fat16_entry_eq(&((struct fat16_dir_entry *)buf)[f->fat16.dir_offset],
                       &entry)) {
      f->fat16.disk_seen = entry; /* our write is the new baseline */
      return 0;
    }
  }
  return -EIO;
}

int fat16_close(struct file* f) {
  // Update directory entry dynamically on disk (SEC-5: guarded).
  return fat16_sync_entry(f);
}

/**
 * Empties an existing file (open with O_TRUNC): frees its whole cluster
 * chain and resets the directory entry to zero length, so a subsequent
 * write starts from an empty file.  POSIX fopen("w") semantics.
 */
int fat16_truncate(struct file* f) {
  if (!f) return -1;
  /* A handle whose dir_sector is 0 addresses no real directory entry (the
     synthetic root of fat16_resolve_path("/")); truncating it would sync
     the entry over LBA 0.  Fat16_open() refuses such handles now -- this
     is the belt for any already-crafted one. */
  if (f->fat16.dir_sector == 0) return -1;

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
  fat16_stamp_entry(&f->fat16.entry);  /* O_TRUNC is a modification */
  spinlock_release_irqrestore(&fat_lock, flags);
  f->fat16.dirty = 1;

  fat16_sync_entry(f);
  return 0;
}

/**
 * P6.1: resize an existing file to `new_size` bytes -- byte-granular like
 * fat16_write.  Extending appends zero fill (allocating clusters as
 * needed); shrinking frees every cluster past the one holding byte
 * new_size-1 and re-terminates that cluster's FAT entry.  Callers hold the
 * file's lock (fs.c file_ftruncate); this never holds fat_lock across a
 * fat16_write call (that takes it itself).
 */
int fat16_truncate_to(struct file* f, uint32_t new_size) {
  if (!f) return -1;
  if (new_size == 0) return fat16_truncate(f);

  uint32_t old_size = f->fat16.entry.file_size;
  if (new_size == old_size) return 0;

  if (new_size > old_size) {
    /* W4-kernel (selfhost, Mission B): bulk extension.  The old zero-fill
       loop pushed 4 KiB chunks through fat16_write, and every fat16_write
       call re-walked the cluster chain from start_cluster to the cursor —
       O(chunks^2) total.  Measured in-OS under TCG (MKFSFAT OUT.IMG at
       acceptance size): 16 MiB ftruncate = 78.7 s, 64 MiB = 347 s —
       quadratic — and 128 MiB exceeded even the 720 s unit-tier budget.
       New path: extend the chain to cover new_size-1 in ONE forward walk
       (O(n); alloc_cluster's free-cluster hint makes each allocation
       amortized O(1)), then zero-fill only the partial tail cluster via
       fat16_write.  Every fully-fresh cluster is already zeroed by
       alloc_cluster, so the bulk needs no content write and reads back as
       all zeros (POSIX ftruncate extension semantics). */
    uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
    if (f->fat16.entry.start_cluster == 0) {
      uint16_t first = alloc_cluster();
      if (first == 0) {
        spinlock_release_irqrestore(&fat_lock, flags);
        return -1;
      }
      f->fat16.entry.start_cluster = first;
    }
    uint32_t target_idx = (new_size - 1) / cluster_size;
    uint16_t c = f->fat16.entry.start_cluster;
    for (uint32_t i = 0; i < target_idx; i++) {
      uint16_t nxt = read_fat(c);
      if (nxt == 0) { /* broken chain (a shrink would have rejected it) */
        spinlock_release_irqrestore(&fat_lock, flags);
        return -1;
      }
      if (nxt >= 0xFFF8) {
        nxt = alloc_cluster();
        if (nxt == 0) {
          spinlock_release_irqrestore(&fat_lock, flags);
          return -1; /* disk full */
        }
        write_fat(c, nxt);
      }
      c = nxt;
    }
    spinlock_release_irqrestore(&fat_lock, flags);

    /* Zero the partial tail: the bytes from old_size to the end of its
       cluster may hold stale pre-shrink data; full clusters beyond are
       already zeroed by alloc_cluster.  At most one cluster's worth, so
       the fat16_write re-walk cost is bounded. */
    uint32_t cluster_end = ((old_size / cluster_size) + 1) * cluster_size;
    uint32_t tail_end = (new_size < cluster_end) ? new_size : cluster_end;
    if (tail_end > old_size) {
      if (fat16_seek(f, (int)old_size) != 0) return -1;
      uint8_t zeros[4096];
      for (int i = 0; i < 4096; i++) zeros[i] = 0;
      uint32_t left = tail_end - old_size;
      while (left > 0) {
        uint32_t chunk = left > sizeof zeros ? (uint32_t)sizeof zeros : left;
        if (fat16_write(f, zeros, (int)chunk) != (int)chunk) return -1;
        left -= chunk;
      }
    } else {
      /* No partial tail (boundary-aligned extend): keep the cursor valid. */
      if ((int)old_size > f->fat16.cursor) f->fat16.cursor = (int)old_size;
    }
    f->fat16.entry.file_size = new_size;
    fat16_stamp_entry(&f->fat16.entry);  /* ftruncate extend is a modification */
    f->fat16.dirty = 1;
    fat16_sync_entry(f);
    return 0;
  }

  /* Shrink: walk to the last cluster to keep, then free the rest. */
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint32_t keep = (new_size + cluster_size - 1) / cluster_size;
  uint16_t c = f->fat16.entry.start_cluster;
  for (uint32_t i = 1; i < keep && c >= 2 && c < 0xFFF8; i++)
    c = read_fat(c);
  if (c < 2 || c >= 0xFFF8) {
    /* No cluster chain covers the kept range: collapsed chain. */
    spinlock_release_irqrestore(&fat_lock, flags);
    return -1;
  }
  uint16_t rest = read_fat(c);
  write_fat(c, 0xFFFF); /* re-terminate the last kept cluster */
  while (rest >= 2 && rest < 0xFFF8) {
    uint16_t next = read_fat(rest);
    write_fat(rest, 0);
    rest = next;
  }
  spinlock_release_irqrestore(&fat_lock, flags);

  f->fat16.entry.file_size = new_size;
  if (f->fat16.cursor > new_size) f->fat16.cursor = new_size;
  fat16_stamp_entry(&f->fat16.entry);  /* ftruncate shrink is a modification */
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

  /* Walk the cluster chain forward instead of re-walking it from the
     start for every single sector: the old per-iteration
     get_cluster_for_offset() made reads O(n^2) in chain steps. */
  uint16_t c = get_cluster_for_offset(f->fat16.entry.start_cluster, f->fat16.cursor);
  while (size > 0) {
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
    if ((f->fat16.cursor % cluster_size) == 0) {
      c = read_fat(c); /* crossing into the next cluster of the chain */
    }
  }
  spinlock_release_irqrestore(&fat_lock, flags);
  return read_bytes;
}

/**
 * The loop behind fat16_read_direct / fat16_read_direct_pos.  Caller holds
 * fat_lock and passes the saved IRQ state (`flags`) so per-request
 * releases/restores match; f->fat16.cursor / entry.start_cluster /
 * entry.file_size define the position and bounds.  Returns the number of
 * bytes read (0 at EOF).
 */
static int fat16_read_direct_locked(struct file *f, uint64_t dest, int size,
                                    uint64_t flags) {
  if (f->fat16.cursor >= f->fat16.entry.file_size)
    return 0;
  uint32_t remaining = f->fat16.entry.file_size - f->fat16.cursor;
  if ((uint32_t)size > remaining) size = remaining;
  if (size == 0)
    return 0;

  uint64_t out = dest;
  int read_bytes = 0;

  while (size > 0) {
    uint16_t c = get_cluster_for_offset(f->fat16.entry.start_cluster, f->fat16.cursor);
    if (c == 0 || c >= 0xFFF8) break;

    uint32_t offset_in_cluster = f->fat16.cursor % cluster_size;
    uint32_t offset_in_sector = offset_in_cluster % SECTOR_SIZE;
    uint32_t max_sectors = (uint32_t)size / SECTOR_SIZE;

    if (offset_in_sector != 0 || (out % SECTOR_SIZE) != 0 || max_sectors == 0) {
      /* Unaligned edge or sub-sector tail: one sector through a bounce. */
      uint32_t sector_num = data_sector + (c - 2) * bpb_sectors_per_cluster
      + (offset_in_cluster / SECTOR_SIZE);
      uint8_t sec_buf[SECTOR_SIZE];
      spinlock_release_irqrestore(&fat_lock, flags);
      int res = virtio_blk_read_sector(sector_num, sec_buf, 1);
      flags = spinlock_acquire_irqsave(&fat_lock);
      if (res != 0) break;

      uint32_t chunk = SECTOR_SIZE - offset_in_sector;
      if (chunk > (uint32_t)size) chunk = size;
      uint8_t* o = (uint8_t*)out;
      for (uint32_t i = 0; i < chunk; i++) o[i] = sec_buf[offset_in_sector + i];
      out += chunk;
      read_bytes += chunk;
      f->fat16.cursor += chunk;
      size -= chunk;
      continue;
    }

    /* Whole sectors available in the current cluster, extended across
       chain-contiguous successors ((c, c+1, c+2, ...) = adjacent LBAs). */
    uint32_t sectors = (cluster_size - offset_in_cluster) / SECTOR_SIZE;
    uint16_t last = c;
    while (sectors < max_sectors) {
      uint16_t nxt = read_fat(last);
      if (nxt != (uint16_t)(last + 1)) break;
      last = nxt;
      sectors += bpb_sectors_per_cluster;
    }
    if (sectors > max_sectors) sectors = max_sectors;
    if (sectors > 1024) sectors = 1024; /* device request cap */

    uint32_t lba = data_sector + (c - 2) * bpb_sectors_per_cluster
    + (offset_in_cluster / SECTOR_SIZE);
    spinlock_release_irqrestore(&fat_lock, flags);
    int res = virtio_blk_read_sector(lba, (void*)out, sectors);
    flags = spinlock_acquire_irqsave(&fat_lock);
    if (res != 0) break;

    uint32_t bytes = sectors * SECTOR_SIZE;
    out += bytes;
    read_bytes += bytes;
    f->fat16.cursor += bytes;
    size -= bytes;
  }
  return read_bytes;
}

/**
 * Reads `size` bytes at the file cursor straight into an identity-mapped
 * physical destination, coalescing contiguous cluster runs into single
 * multi-sector device requests.  Used by the program loader (destination
 * = the child's physical block): a ~150 KiB program that used to cost
 * hundreds of single-sector round trips now needs a handful of requests.
 * A sub-sector tail (or an unaligned edge) is completed with a one-sector
 * bounce read.
 *
 * Returns the number of bytes read.
 */
int fat16_read_direct(struct file* f, uint64_t dest, int size) {
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  int read_bytes = fat16_read_direct_locked(f, dest, size, flags);
  spinlock_release_irqrestore(&fat_lock, flags);
  return read_bytes;
}

/**
 * L9 (pagein lane): positional coalesced read for demand-paged v2 images.
 * Reads `size` bytes at file offset `pos` into an identity-mapped physical
 * destination, using the same coalesced loop as fat16_read_direct but
 * resolving the position from the entry identity (start cluster + size)
 * instead of a live cursor — the exact use the EL0 page-fault fill needs
 * (any number of CPUs may fault different pages of the same lazy image).
 * Serialized under fat_lock like every other FAT16 op.
 *
 * Returns the number of bytes read (0 when pos is at/after EOF).
 */
int fat16_read_direct_pos(uint16_t start_cluster, uint32_t file_size,
                          uint64_t dest, int size, uint32_t pos) {
  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  struct file tmp;
  __builtin_memset(&tmp, 0, sizeof(tmp));
  tmp.fat16.entry.start_cluster = start_cluster;
  tmp.fat16.entry.file_size = file_size;
  tmp.fat16.cursor = pos;
  int read_bytes = fat16_read_direct_locked(&tmp, dest, size, flags);
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

  /* Walk the chain ONCE to the cluster holding the cursor, extending it as
     needed.  The cursor advances monotonically for the rest of this call,
     so the walk state carries forward instead of restarting from the start
     per chunk — the old restart made a >1 MiB create/write O(n^2) in chain
     length (a 2 MiB write walked ~2e6 FAT entries and blew the unit-tier
     budget; a 30 MiB blob was effectively unwritable under TCG). */
  uint16_t c = f->fat16.entry.start_cluster;
  uint32_t target_cluster_idx = f->fat16.cursor / cluster_size;
  for (uint32_t i = 0; i < target_cluster_idx; i++) {
    uint16_t nxt = read_fat(c);
    if (nxt >= 0xFFF8) {
      nxt = alloc_cluster();
      if (nxt == 0) break;
      write_fat(c, nxt);
    }
    c = nxt;
  }

  while (size > 0) {
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

    /* Advance to the next cluster only at a real cluster boundary: a chunk
       is one sector (512 B) while a cluster spans bpb_sectors_per_cluster
       sectors, so crossing early would skip the rest of the current cluster
       (the tail-checksum test in fat16_test.c catches exactly this). */
    if (size > 0 && (f->fat16.cursor % cluster_size) == 0) {
      uint16_t nxt = read_fat(c);
      if (nxt >= 0xFFF8) {
        nxt = alloc_cluster();
        if (nxt == 0) break;
        write_fat(c, nxt);
      }
      c = nxt;
    }
  }
  spinlock_release_irqrestore(&fat_lock, flags);
  if (written_bytes > 0) {
    fat16_stamp_entry(&f->fat16.entry);  /* last-write time (FAT mtime) */
    f->fat16.dirty = 1;
    fat16_sync_entry(f);   /* keep the on-disk dir entry (size/clusters/time)
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

/* GB-020: getdents64 backend.  For an open directory fd, enumerate the
 * directory's children by cluster into linux_dirent64 records (x86_64 layout:
 * d_ino@0 u64, d_off@8 s64, d_reclen@16 u16, d_type@18 u8, d_name@19).  The
 * two literal "." and ".." entries are synthesized first (Linux opendir
 * present them; musl/getdents callers skip them by name).  dir_cluster==0
 * means the root directory (fixed region).
 */
int fat16_getdents64(struct file *f, int start_index, void *ubuf, int bufsize) {
  if (!f || !ubuf || bufsize <= 0) return -1;

  uint64_t flags = spinlock_acquire_irqsave(&fat_lock);
  uint16_t cluster = f->fat16.entry.start_cluster;
  uint8_t buf[SECTOR_SIZE];
  struct lfn_state st;
  lfn_reset(&st);

  int used = 0;
  /* Real children occupy logical entry indices 2,3,... ("."=0, ".."=1). */
  int current_idx = (start_index < 2) ? 0 : 2;

  /* "." and ".." are implicit in every FAT directory.  They occupy logical
   * entry indices 0 and 1.  Provide them when the resume index allows. */
  if (start_index < 2) {
    static const char *const selves[2] = {".", ".."};
    for (int s = 0; s < 2 && used < bufsize; s++) {
      if (current_idx < start_index) { current_idx++; continue; }
      int nlen = (int)(selves[s] ? 2 : 1) - 1;
      /* nlen is the string length: "."=>1 ".."=>2 */
      nlen = (s == 0) ? 1 : 2;
      int reclen = (19 + nlen + 1 + 7) & ~7;
      if (used + reclen > bufsize) break;
      uint8_t *p = (uint8_t *)ubuf + used;
      uint64_t ino = 4096u + (uint64_t)(current_idx + 1);
      uint64_t off = (uint64_t)(current_idx + 1);
      for (int k = 0; k < 8; k++) p[k] = (uint8_t)(ino >> (k * 8));
      for (int k = 0; k < 8; k++) p[8 + k] = (uint8_t)(off >> (k * 8));
      p[16] = (uint8_t)(reclen & 0xFF); p[17] = (uint8_t)((reclen >> 8) & 0xFF);
      p[18] = 4; /* DT_DIR */
      for (int n = 0; n < nlen; n++) p[19 + n] = (uint8_t)selves[s][n];
      p[19 + nlen] = 0;
      used += reclen;
      current_idx++;
    }
  }

  /* Walk the real children. */
  int stopped = 0;
  if (cluster == 0) {
    /* root directory: fixed region of root_dir_sectors sectors */
    for (uint32_t i = 0; i < root_dir_sectors && !stopped; i++) {
      uint32_t sector_num = root_dir_sector + i;
      spinlock_release_irqrestore(&fat_lock, flags);
      if (virtio_blk_read_sector(sector_num, buf, 1) != 0) {
        spinlock_acquire_irqsave(&fat_lock);
        return used > 0 ? used : -1;
      }
      flags = spinlock_acquire_irqsave(&fat_lock);
      struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
      for (unsigned int j = 0; j < SECTOR_SIZE / 32 && !stopped; j++) {
        if (entries[j].name[0] == 0x00) { stopped = 1; break; }
        if (entries[j].name[0] == (char)0xE5) { lfn_reset(&st); continue; }
        if (entries[j].attr == 0x0F) { lfn_add_chunk(&st, (const unsigned char *)&entries[j]); continue; }
        if (entries[j].attr & 0x08) { lfn_reset(&st); continue; }
        if (current_idx < start_index) { current_idx++; lfn_reset(&st); continue; }
        /* pack */
        char name[32];
        lfn_entry_display(&st, &entries[j], name, sizeof name);
        int nlen = 0; while (name[nlen] && nlen < 255) nlen++;
        int reclen = (19 + nlen + 1 + 7) & ~7;
        if (used + reclen > bufsize) { stopped = 1; break; }
        uint8_t *p = (uint8_t *)ubuf + used;
        uint64_t ino = 4096u + (uint64_t)(current_idx + 1);
        uint64_t off = (uint64_t)(current_idx + 1);
        for (int k = 0; k < 8; k++) p[k] = (uint8_t)(ino >> (k * 8));
        for (int k = 0; k < 8; k++) p[8 + k] = (uint8_t)(off >> (k * 8));
        p[16] = (uint8_t)(reclen & 0xFF); p[17] = (uint8_t)((reclen >> 8) & 0xFF);
        p[18] = (entries[j].attr & 0x10) ? 4 : 8;
        for (int n = 0; n < nlen; n++) p[19 + n] = (uint8_t)name[n];
        p[19 + nlen] = 0;
        used += reclen;
        current_idx++;
        lfn_reset(&st);
      }
    }
  } else {
    uint16_t c = cluster;
    while (c != 0 && c < 0xFFF0 && !stopped) {
      for (uint32_t s = 0; s < bpb_sectors_per_cluster && !stopped; s++) {
        uint32_t sector_num = data_sector + (c - 2) * bpb_sectors_per_cluster + s;
        spinlock_release_irqrestore(&fat_lock, flags);
        if (virtio_blk_read_sector(sector_num, buf, 1) != 0) {
          spinlock_acquire_irqsave(&fat_lock);
          return used > 0 ? used : -1;
        }
        flags = spinlock_acquire_irqsave(&fat_lock);
        struct fat16_dir_entry *entries = (struct fat16_dir_entry *)buf;
        for (unsigned int j = 0; j < SECTOR_SIZE / 32 && !stopped; j++) {
          if (entries[j].name[0] == 0x00) { stopped = 1; break; }
          if (entries[j].name[0] == (char)0xE5) { lfn_reset(&st); continue; }
          if (entries[j].attr == 0x0F) { lfn_add_chunk(&st, (const unsigned char *)&entries[j]); continue; }
          if (entries[j].attr & 0x08) { lfn_reset(&st); continue; }
          if (current_idx < start_index) { current_idx++; lfn_reset(&st); continue; }
          char name[32];
          lfn_entry_display(&st, &entries[j], name, sizeof name);
          int nlen = 0; while (name[nlen] && nlen < 255) nlen++;
          int reclen = (19 + nlen + 1 + 7) & ~7;
          if (used + reclen > bufsize) { stopped = 1; break; }
          uint8_t *p = (uint8_t *)ubuf + used;
          uint64_t ino = 4096u + (uint64_t)(current_idx + 1);
          uint64_t off = (uint64_t)(current_idx + 1);
          for (int k = 0; k < 8; k++) p[k] = (uint8_t)(ino >> (k * 8));
          for (int k = 0; k < 8; k++) p[8 + k] = (uint8_t)(off >> (k * 8));
          p[16] = (uint8_t)(reclen & 0xFF); p[17] = (uint8_t)((reclen >> 8) & 0xFF);
          p[18] = (entries[j].attr & 0x10) ? 4 : 8;
          for (int n = 0; n < nlen; n++) p[19 + n] = (uint8_t)name[n];
          p[19 + nlen] = 0;
          used += reclen;
          current_idx++;
          lfn_reset(&st);
        }
      }
      c = read_fat(c);
    }
  }

  spinlock_release_irqrestore(&fat_lock, flags);
  f->fat16.cursor = current_idx; /* resume position for a subsequent call */
  return used;
}
