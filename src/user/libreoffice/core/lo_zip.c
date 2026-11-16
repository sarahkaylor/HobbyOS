/*
 * src/user/libreoffice/core/lo_zip.c
 *
 * Full genuine OpenDocument Format (ODF) ZIP Container Engine.
 * Fully compliant with OASIS ODF 1.2 Part 3, ISO/IEC 26300, and PKWARE APPNOTE.
 * Enforces unaligned memory safety for bare-metal AArch64.
 */

#include "lo_zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#define ZIP_LOCAL_MAGIC   0x04034b50
#define ZIP_CENTRAL_MAGIC 0x02014b50
#define ZIP_EOCD_MAGIC    0x06054b50

#define METHOD_STORED     0
#define METHOD_DEFLATED   8

/*
 * Memory safety on AArch64 / Device Memory:
 * Explicit little-endian byte readers and writers avoid hardware unaligned
 * memory access exceptions.
 */
static inline uint16_t get_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

typedef struct lo_zip_entry {
    char name[128];
    uint16_t method;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_header_offset;
    uint8_t *data; /* Cached payload for in-memory / write mode */
} lo_zip_entry_t;

struct lo_zip_archive {
    char path[256];
    int mode;
    FILE *fp;
    lo_zip_entry_t *entries;
    size_t entry_count;
    size_t entry_cap;
    uint8_t *file_data; /* Memory buffer for read mode */
    size_t file_size;
};

/* Raw DEFLATE compression using zlib without zlib/gzip headers */
static int raw_deflate(const void *src, size_t src_len, void **out_buf, size_t *out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* Negative windowBits (-MAX_WBITS) produces raw DEFLATE stream */
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return LO_ERR_CORRUPT;
    }

    size_t cap = deflateBound(&strm, (uLong)src_len) + 64;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        deflateEnd(&strm);
        return LO_ERR_GENERIC;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = buf;
    strm.avail_out = (uInt)cap;

    int ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END && ret != Z_OK) {
        free(buf);
        return LO_ERR_IO;
    }

    *out_len = strm.total_out;
    *out_buf = buf;
    return LO_OK;
}

/* Raw INFLATE decompression using zlib without zlib/gzip headers */
static int raw_inflate(const void *src, size_t src_len, size_t uncomp_len, void **out_buf) {
    if (uncomp_len > 512 * 1024 * 1024 || (size_t)uncomp_len + 1 < uncomp_len) {
        return LO_ERR_CORRUPT;
    }

    if (uncomp_len == 0) {
        uint8_t *empty = (uint8_t *)malloc(1);
        if (!empty) return LO_ERR_GENERIC;
        empty[0] = '\0';
        *out_buf = empty;
        return LO_OK;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* Negative windowBits (-MAX_WBITS) tells zlib to parse raw DEFLATE */
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return LO_ERR_CORRUPT;
    }

    uint8_t *buf = (uint8_t *)malloc(uncomp_len + 1);
    if (!buf) {
        inflateEnd(&strm);
        return LO_ERR_GENERIC;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = buf;
    strm.avail_out = (uInt)uncomp_len;

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if ((ret != Z_STREAM_END && ret != Z_OK) || strm.total_out != (uLong)uncomp_len) {
        free(buf);
        return LO_ERR_CORRUPT;
    }

    buf[uncomp_len] = '\0';
    *out_buf = buf;
    return LO_OK;
}

lo_zip_archive_t *lo_zip_open(const char *path, int mode) {
    if (!path) return NULL;
    lo_zip_archive_t *zip = (lo_zip_archive_t *)calloc(1, sizeof(lo_zip_archive_t));
    if (!zip) return NULL;

    strncpy(zip->path, path, sizeof(zip->path) - 1);
    zip->path[sizeof(zip->path) - 1] = '\0';
    zip->mode = mode;

    if (mode == LO_ZIP_MODE_READ) {
        zip->fp = fopen(path, "rb");
        if (!zip->fp) {
            free(zip);
            return NULL;
        }

        if (fseek(zip->fp, 0, SEEK_END) != 0) {
            fclose(zip->fp);
            free(zip);
            return NULL;
        }
        long fsize = ftell(zip->fp);
        if (fsize < 0) {
            fclose(zip->fp);
            free(zip);
            return NULL;
        }
        zip->file_size = (size_t)fsize;
        fseek(zip->fp, 0, SEEK_SET);

        if (zip->file_size < 22) {
            /* Minimum valid ZIP file size is 22 bytes (empty EOCD) */
            fclose(zip->fp);
            free(zip);
            return NULL;
        }

        zip->file_data = (uint8_t *)malloc(zip->file_size);
        if (!zip->file_data || fread(zip->file_data, 1, zip->file_size, zip->fp) != zip->file_size) {
            if (zip->file_data) free(zip->file_data);
            fclose(zip->fp);
            free(zip);
            return NULL;
        }

        /* Locate End of Central Directory (EOCD) signature */
        size_t scan_limit = (zip->file_size > 65557) ? (zip->file_size - 65557) : 0;
        bool found_eocd = false;

        for (size_t i = zip->file_size - 22; i >= scan_limit; i--) {
            if (get_u32_le(zip->file_data + i) == ZIP_EOCD_MAGIC) {
                uint16_t num_entries = get_u16_le(zip->file_data + i + 10);
                uint32_t cd_size = get_u32_le(zip->file_data + i + 12);
                uint32_t cd_offset = get_u32_le(zip->file_data + i + 16);

                (void)cd_size;
                zip->entry_count = num_entries;
                zip->entry_cap = num_entries;
                if (num_entries > 0) {
                    zip->entries = (lo_zip_entry_t *)calloc(num_entries, sizeof(lo_zip_entry_t));
                    if (!zip->entries) {
                        free(zip->file_data);
                        fclose(zip->fp);
                        free(zip);
                        return NULL;
                    }
                }

                size_t curr = cd_offset;
                for (uint16_t e = 0; e < num_entries && (uint64_t)curr + 46 <= zip->file_size; e++) {
                    if (get_u32_le(zip->file_data + curr) != ZIP_CENTRAL_MAGIC) break;

                    zip->entries[e].method = get_u16_le(zip->file_data + curr + 10);
                    zip->entries[e].crc32 = get_u32_le(zip->file_data + curr + 16);
                    zip->entries[e].comp_size = get_u32_le(zip->file_data + curr + 20);
                    zip->entries[e].uncomp_size = get_u32_le(zip->file_data + curr + 24);
                    uint16_t n_len = get_u16_le(zip->file_data + curr + 28);
                    uint16_t x_len = get_u16_le(zip->file_data + curr + 30);
                    uint16_t c_len = get_u16_le(zip->file_data + curr + 32);
                    zip->entries[e].local_header_offset = get_u32_le(zip->file_data + curr + 42);

                    size_t copy_len = n_len < sizeof(zip->entries[e].name) - 1 ? n_len : sizeof(zip->entries[e].name) - 1;
                    if ((uint64_t)curr + 46 + n_len <= zip->file_size) {
                        memcpy(zip->entries[e].name, zip->file_data + curr + 46, copy_len);
                        zip->entries[e].name[copy_len] = '\0';
                    }

                    uint64_t next_curr = (uint64_t)curr + 46 + n_len + x_len + c_len;
                    if (next_curr > zip->file_size) break;
                    curr = (size_t)next_curr;
                }
                found_eocd = true;
                break;
            }
            if (i == 0) break;
        }

        if (!found_eocd) {
            free(zip->file_data);
            fclose(zip->fp);
            free(zip);
            return NULL;
        }
    }

    return zip;
}

int lo_zip_read_file(lo_zip_archive_t *zip, const char *entry_name, void **buf, size_t *size) {
    if (!zip || !entry_name || !buf || !size) return LO_ERR_NULL_ARG;

    for (size_t i = 0; i < zip->entry_count; i++) {
        if (strcmp(zip->entries[i].name, entry_name) == 0) {
            lo_zip_entry_t *e = &zip->entries[i];

            /* Defect 3: Sanity check uncompressed size against integer wrap and excessive allocation */
            if (e->uncomp_size > 512 * 1024 * 1024 || (size_t)e->uncomp_size + 1 < e->uncomp_size) {
                return LO_ERR_CORRUPT;
            }

            /* Case 1: Entry has pre-buffered data in memory */
            if (e->data != NULL) {
                if (e->method == METHOD_STORED) {
                    /* Defect 1: For STORED entries, comp_size must equal uncomp_size */
                    if (e->comp_size != e->uncomp_size) return LO_ERR_CORRUPT;
                    uint8_t *out = (uint8_t *)malloc(e->uncomp_size + 1);
                    if (!out) return LO_ERR_GENERIC;
                    if (e->uncomp_size > 0) memcpy(out, e->data, e->uncomp_size);
                    out[e->uncomp_size] = '\0';
                    *buf = out;
                    *size = e->uncomp_size;
                    return LO_OK;
                } else if (e->method == METHOD_DEFLATED) {
                    int res = raw_inflate(e->data, e->comp_size, e->uncomp_size, buf);
                    if (res == LO_OK) *size = e->uncomp_size;
                    return res;
                }
                return LO_ERR_CORRUPT;
            }

            /* Defect 5: Handle 0-byte entries in write mode (or when no file_data is present) */
            if (e->uncomp_size == 0 && (zip->mode == LO_ZIP_MODE_WRITE || !zip->file_data)) {
                char *out = (char *)malloc(1);
                if (!out) return LO_ERR_GENERIC;
                out[0] = '\0';
                *buf = out;
                *size = 0;
                return LO_OK;
            }

            /* Case 2: Read from file buffer */
            if (!zip->file_data) return LO_ERR_IO;

            /* Defect 2: Use 64-bit unsigned arithmetic to prevent integer wrap */
            uint32_t l_off = e->local_header_offset;
            if ((uint64_t)l_off + 30 > zip->file_size) return LO_ERR_CORRUPT;
            if (get_u32_le(zip->file_data + l_off) != ZIP_LOCAL_MAGIC) return LO_ERR_CORRUPT;

            uint16_t n_len = get_u16_le(zip->file_data + l_off + 26);
            uint16_t x_len = get_u16_le(zip->file_data + l_off + 28);
            uint64_t payload_offset = (uint64_t)l_off + 30 + n_len + x_len;
            if (payload_offset > zip->file_size) return LO_ERR_CORRUPT;
            if (payload_offset + e->comp_size > zip->file_size) return LO_ERR_CORRUPT;

            uint8_t *payload = zip->file_data + (size_t)payload_offset;

            if (e->method == METHOD_STORED) {
                /* Defect 1: For STORED entries, comp_size must equal uncomp_size and payload must not exceed file_size */
                if (e->comp_size != e->uncomp_size) return LO_ERR_CORRUPT;
                if (payload_offset + e->uncomp_size > zip->file_size) return LO_ERR_CORRUPT;

                uint8_t *out = (uint8_t *)malloc(e->uncomp_size + 1);
                if (!out) return LO_ERR_GENERIC;
                if (e->uncomp_size > 0) memcpy(out, payload, e->uncomp_size);
                out[e->uncomp_size] = '\0';
                *buf = out;
                *size = e->uncomp_size;
                return LO_OK;
            } else if (e->method == METHOD_DEFLATED) {
                int res = raw_inflate(payload, e->comp_size, e->uncomp_size, buf);
                if (res == LO_OK) *size = e->uncomp_size;
                return res;
            }
            return LO_ERR_CORRUPT;
        }
    }
    return LO_ERR_NOT_FOUND;
}

int lo_zip_write_file(lo_zip_archive_t *zip, const char *entry_name, const void *buf, size_t size) {
    if (!zip || !entry_name) return LO_ERR_NULL_ARG;

    /* If opened in read mode, convert to write/update mode by buffering existing entries */
    if (zip->mode == LO_ZIP_MODE_READ) {
        for (size_t i = 0; i < zip->entry_count; i++) {
            lo_zip_entry_t *e = &zip->entries[i];
            if (!e->data && zip->file_data) {
                uint32_t l_off = e->local_header_offset;
                if ((uint64_t)l_off + 30 <= zip->file_size && get_u32_le(zip->file_data + l_off) == ZIP_LOCAL_MAGIC) {
                    uint16_t n_len = get_u16_le(zip->file_data + l_off + 26);
                    uint16_t x_len = get_u16_le(zip->file_data + l_off + 28);
                    uint64_t p_off = (uint64_t)l_off + 30 + n_len + x_len;
                    if (p_off <= zip->file_size && p_off + e->comp_size <= zip->file_size && e->comp_size > 0) {
                        e->data = (uint8_t *)malloc(e->comp_size);
                        if (e->data) memcpy(e->data, zip->file_data + (size_t)p_off, e->comp_size);
                    }
                }
            }
        }
        zip->mode = LO_ZIP_MODE_WRITE;
    }

    /* Check if entry with this name already exists (Update operation) */
    lo_zip_entry_t *e = NULL;
    for (size_t i = 0; i < zip->entry_count; i++) {
        if (strcmp(zip->entries[i].name, entry_name) == 0) {
            e = &zip->entries[i];
            if (e->data) {
                free(e->data);
                e->data = NULL;
            }
            break;
        }
    }

    /* New entry */
    if (!e) {
        if (zip->entry_count >= zip->entry_cap) {
            size_t new_cap = zip->entry_cap ? zip->entry_cap * 2 : 16;
            lo_zip_entry_t *new_entries = (lo_zip_entry_t *)realloc(zip->entries, new_cap * sizeof(lo_zip_entry_t));
            if (!new_entries) return LO_ERR_GENERIC;
            zip->entries = new_entries;
            zip->entry_cap = new_cap;
        }
        e = &zip->entries[zip->entry_count++];
    }

    memset(e, 0, sizeof(*e));
    strncpy(e->name, entry_name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->uncomp_size = (uint32_t)size;
    e->crc32 = (uint32_t)crc32(0L, (const Bytef *)(buf ? buf : ""), (uInt)size);

    /*
     * OASIS ODF 1.2 Part 3 §3.3 rule:
     * 'mimetype' MUST be uncompressed (method 0) with zero extra field bytes.
     */
    if (strcmp(entry_name, "mimetype") == 0 || size == 0) {
        e->method = METHOD_STORED;
        e->comp_size = e->uncomp_size;
        if (size > 0 && buf) {
            e->data = (uint8_t *)malloc(size);
            if (!e->data) return LO_ERR_GENERIC;
            memcpy(e->data, buf, size);
        } else {
            e->data = NULL;
        }
    } else {
        void *comp_buf = NULL;
        size_t comp_len = 0;
        int ret = raw_deflate(buf, size, &comp_buf, &comp_len);
        if (ret == LO_OK && comp_len < size) {
            e->method = METHOD_DEFLATED;
            e->comp_size = (uint32_t)comp_len;
            e->data = (uint8_t *)comp_buf;
        } else {
            if (comp_buf) free(comp_buf);
            /* If deflation did not reduce size, store uncompressed */
            e->method = METHOD_STORED;
            e->comp_size = e->uncomp_size;
            e->data = (uint8_t *)malloc(size);
            if (!e->data) return LO_ERR_GENERIC;
            memcpy(e->data, buf, size);
        }
    }

    return LO_OK;
}

bool lo_zip_has_entry(lo_zip_archive_t *zip, const char *entry_name) {
    if (!zip || !entry_name) return false;
    for (size_t i = 0; i < zip->entry_count; i++) {
        if (strcmp(zip->entries[i].name, entry_name) == 0) return true;
    }
    return false;
}

size_t lo_zip_get_entry_count(const lo_zip_archive_t *zip) {
    return zip ? zip->entry_count : 0;
}

const char *lo_zip_get_entry_name(const lo_zip_archive_t *zip, size_t index) {
    if (!zip || index >= zip->entry_count) return NULL;
    return zip->entries[index].name;
}

int lo_zip_close(lo_zip_archive_t *zip) {
    if (!zip) return LO_ERR_NULL_ARG;

    if (zip->mode == LO_ZIP_MODE_WRITE) {
        /*
         * Enforce OASIS ODF 1.2 Part 3 §3.3:
         * If 'mimetype' entry is present, it MUST be the very first entry.
         */
        size_t mime_idx = (size_t)-1;
        for (size_t i = 0; i < zip->entry_count; i++) {
            if (strcmp(zip->entries[i].name, "mimetype") == 0) {
                mime_idx = i;
                break;
            }
        }
        if (mime_idx != (size_t)-1 && mime_idx != 0) {
            lo_zip_entry_t tmp = zip->entries[0];
            zip->entries[0] = zip->entries[mime_idx];
            zip->entries[mime_idx] = tmp;
        }

        FILE *fp = fopen(zip->path, "wb");
        if (!fp) {
            if (zip->entries) {
                for (size_t i = 0; i < zip->entry_count; i++) {
                    if (zip->entries[i].data) free(zip->entries[i].data);
                }
                free(zip->entries);
            }
            if (zip->file_data) free(zip->file_data);
            if (zip->fp) fclose(zip->fp);
            free(zip);
            return LO_ERR_IO;
        }

        uint32_t current_pos = 0;

        /* 1. Write Local File Headers and payloads */
        for (size_t i = 0; i < zip->entry_count; i++) {
            lo_zip_entry_t *e = &zip->entries[i];
            e->local_header_offset = current_pos;

            uint8_t hdr[30];
            put_u32_le(hdr, ZIP_LOCAL_MAGIC);
            put_u16_le(hdr + 4, 20);            /* Version needed: 2.0 */
            put_u16_le(hdr + 6, 0);             /* General purpose bit flag */
            put_u16_le(hdr + 8, e->method);     /* Compression method */
            put_u16_le(hdr + 10, 0);            /* Last mod file time */
            put_u16_le(hdr + 12, 0);            /* Last mod file date */
            put_u32_le(hdr + 14, e->crc32);     /* CRC-32 */
            put_u32_le(hdr + 18, e->comp_size); /* Compressed size */
            put_u32_le(hdr + 22, e->uncomp_size);/* Uncompressed size */
            uint16_t n_len = (uint16_t)strlen(e->name);
            put_u16_le(hdr + 26, n_len);        /* File name length */
            put_u16_le(hdr + 28, 0);            /* Extra field length: strictly 0 */

            fwrite(hdr, 1, 30, fp);
            current_pos += 30;

            fwrite(e->name, 1, n_len, fp);
            current_pos += n_len;

            if (e->comp_size > 0 && e->data) {
                fwrite(e->data, 1, e->comp_size, fp);
                current_pos += e->comp_size;
            }
        }

        /* 2. Write Central Directory */
        uint32_t cd_offset = current_pos;
        for (size_t i = 0; i < zip->entry_count; i++) {
            lo_zip_entry_t *e = &zip->entries[i];
            uint8_t cd[46];
            put_u32_le(cd, ZIP_CENTRAL_MAGIC);
            put_u16_le(cd + 4, 0x0314);         /* Version made by: Unix 2.0 */
            put_u16_le(cd + 6, 20);             /* Version needed: 2.0 */
            put_u16_le(cd + 8, 0);              /* Flags */
            put_u16_le(cd + 10, e->method);     /* Method */
            put_u16_le(cd + 12, 0);             /* Time */
            put_u16_le(cd + 14, 0);             /* Date */
            put_u32_le(cd + 16, e->crc32);      /* CRC-32 */
            put_u32_le(cd + 20, e->comp_size);  /* Compressed size */
            put_u32_le(cd + 24, e->uncomp_size);/* Uncompressed size */
            uint16_t n_len = (uint16_t)strlen(e->name);
            put_u16_le(cd + 28, n_len);         /* File name length */
            put_u16_le(cd + 30, 0);             /* Extra field length */
            put_u16_le(cd + 32, 0);             /* File comment length */
            put_u16_le(cd + 34, 0);             /* Disk number start */
            put_u16_le(cd + 36, 0);             /* Internal file attributes */
            put_u32_le(cd + 38, 0100644U << 16); /* External file attributes: regular file */
            put_u32_le(cd + 42, e->local_header_offset);

            fwrite(cd, 1, 46, fp);
            current_pos += 46;

            fwrite(e->name, 1, n_len, fp);
            current_pos += n_len;
        }
        uint32_t cd_size = current_pos - cd_offset;

        /* 3. Write End of Central Directory Record (EOCD) */
        uint8_t eocd[22];
        put_u32_le(eocd, ZIP_EOCD_MAGIC);
        put_u16_le(eocd + 4, 0);            /* Disk number */
        put_u16_le(eocd + 6, 0);            /* Disk where CD starts */
        put_u16_le(eocd + 8, (uint16_t)zip->entry_count);  /* Number of records on disk */
        put_u16_le(eocd + 10, (uint16_t)zip->entry_count); /* Total number of records */
        put_u32_le(eocd + 12, cd_size);     /* Size of central directory */
        put_u32_le(eocd + 16, cd_offset);   /* Offset of central directory */
        put_u16_le(eocd + 20, 0);           /* ZIP comment length */

        fwrite(eocd, 1, 22, fp);
        fclose(fp);
    }

    if (zip->entries) {
        for (size_t i = 0; i < zip->entry_count; i++) {
            if (zip->entries[i].data) free(zip->entries[i].data);
        }
        free(zip->entries);
    }
    if (zip->file_data) free(zip->file_data);
    if (zip->fp) fclose(zip->fp);
    free(zip);
    return LO_OK;
}
