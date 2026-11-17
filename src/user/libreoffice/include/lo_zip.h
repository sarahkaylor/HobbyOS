#ifndef LO_ZIP_H
#define LO_ZIP_H

#include <stddef.h>
#include <stdbool.h>
#include "lo_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LO_ZIP_MODE_READ   1
#define LO_ZIP_MODE_WRITE  2

typedef struct lo_zip_archive lo_zip_archive_t;

lo_zip_archive_t *lo_zip_open(const char *path, int mode);
int lo_zip_read_file(lo_zip_archive_t *zip, const char *entry_name, void **buf, size_t *size);
int lo_zip_write_file(lo_zip_archive_t *zip, const char *entry_name, const void *buf, size_t size);
bool lo_zip_has_entry(lo_zip_archive_t *zip, const char *entry_name);
size_t lo_zip_get_entry_count(const lo_zip_archive_t *zip);
const char *lo_zip_get_entry_name(const lo_zip_archive_t *zip, size_t index);
int lo_zip_close(lo_zip_archive_t *zip);

#ifdef __cplusplus
}
#endif

#endif /* LO_ZIP_H */
