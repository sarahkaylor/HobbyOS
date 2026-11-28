/* nv_libos_log -- see src/include/nv_libos_log.h for the two vendor objects
 * and every citation.  This TU is pure memory work: no MMIO, no allocation,
 * no architecture conditional, so the same bytes are produced by the x86-64
 * boot rung, the kernel unit tier and (compiled but unused) the ARM tier. */

#include "nv_libos_log.h"

#include "errno.h"

/* The vendor's release-build log set, in ITS order (kernel_gsp.c:3152-3164;
 * the ordering note "LOGINIT must be first for early init logging to work" is
 * at :5191), with the memory ids and prefixes from
 * nv-firmware-chip-family-select.h:107-113.
 * Sizes are logInitValues' release values; LOGDBG is the one task the vendor
 * builds a region for but does not decode (bEnableNvlog = NV_FALSE). */
const struct nv_libos_log_task nv_libos_log_tasks[] = {
    {"LOGINIT", "INIT", 0x10000u, 1u}, /* kernel_gsp.c:3154, 64KB stack traces */
    {"LOGINTR", "INTR", 0x10000u, 1u}, /* :3159, 64KB ISR log (release)      */
    {"LOGRM",   "RM",   0x10000u, 1u}, /* :3160, 64KB RM log (release)       */
    {"LOGMNOC", "MNOC", 0x10000u, 1u}, /* :3162                               */
    {"LOGDBG",  "DBG",  0x10000u, 0u}, /* :3163, NV_FALSE: no nvlog decoding  */
    {"LOGROOT", "ROOT", 0x1000u,  1u}, /* :3164                               */
};

const unsigned nv_libos_log_task_count =
    (unsigned)(sizeof(nv_libos_log_tasks) / sizeof(nv_libos_log_tasks[0]));

/* Header and put pointer must agree with the vendor's compiled struct.  The
 * struct is field-for-field LIBOS_LOG_NVLOG_BUFFER_V2 (liblogdecode.h:123). */
_Static_assert(sizeof(struct nv_libos_nvlog_header) == NV_LIBOS_NVLOG_HEADER_BYTES,
               "LIBOS_LOG_NVLOG_BUFFER_V2 header is 72 bytes at this ABI");
_Static_assert(NV_LIBOS_NVLOG_PUT_OFF == NV_LIBOS_NVLOG_HEADER_BYTES,
               "the put pointer follows the header");

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static void zero(uint8_t *p, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

uint32_t nv_libos_log_total_bytes(void) {
  uint32_t off = 0;
  for (unsigned i = 0; i < nv_libos_log_task_count; i++) {
    /* every region starts on a page, as the vendor's memdescCreate does */
    off = (off + 0xFFFu) & ~0xFFFu;
    off += nv_libos_log_tasks[i].bytes;
  }
  return (off + 0xFFFu) & ~0xFFFu;
}

int nv_libos_log_region(unsigned idx, uint32_t *off, uint32_t *bytes) {
  if (!off || !bytes || idx >= nv_libos_log_task_count) return -EINVAL;
  uint32_t o = 0;
  for (unsigned i = 0; i < idx; i++) {
    o = (o + 0xFFFu) & ~0xFFFu;
    o += nv_libos_log_tasks[i].bytes;
  }
  o = (o + 0xFFFu) & ~0xFFFu;
  *off = o;
  *bytes = nv_libos_log_tasks[idx].bytes;
  return 0;
}

int nv_libos_nvlog_encode(void *container, uint64_t container_len,
                          uint32_t gpu_arch, uint32_t gpu_impl,
                          const char *task_prefix, uint64_t timer_delta,
                          const uint8_t *build_id, uint32_t build_id_len,
                          uint32_t flags) {
  if (!container || !task_prefix) return -EINVAL;
  if (container_len < (uint64_t)NV_LIBOS_NVLOG_HEADER_BYTES + 8u) return -E2BIG;
  if (build_id_len > NV_LIBOS_NVLOG_BUILD_ID_MAX) return -EINVAL;
  if (build_id_len && !build_id) return -EINVAL;

  uint32_t plen = 0;
  while (plen < NV_LIBOS_NVLOG_TASK_PREFIX && task_prefix[plen]) plen++;
  if (plen == 0 || plen > NV_LIBOS_NVLOG_TASK_PREFIX) return -EINVAL;

  uint8_t *p = (uint8_t *)container;
  zero(p, (uint64_t)NV_LIBOS_NVLOG_HEADER_BYTES + 8u);
  put32(p + 0x00, gpu_arch);
  put32(p + 0x04, gpu_impl);
  put32(p + 0x08, NV_LIBOS_NVLOG_VERSION);
  put32(p + 0x0C, build_id_len);
  for (uint32_t i = 0; i < NV_LIBOS_NVLOG_TASK_PREFIX; i++)
    p[0x10 + i] = (i < plen) ? (uint8_t)task_prefix[i] : 0;
  put64(p + 0x18, timer_delta);
  for (uint32_t i = 0; i < build_id_len; i++) p[0x20 + i] = build_id[i];
  put32(p + 0x40, flags);
  put32(p + 0x44, 0); /* reserved: libosLogInitHeader never sets it */
  put64(p + NV_LIBOS_NVLOG_PUT_OFF, 0); /* an empty record area */
  return 0;
}

int nv_libos_nvlog_decode(const void *container, uint64_t container_len,
                          struct nv_libos_nvlog_header *out) {
  if (!container || container_len < (uint64_t)NV_LIBOS_NVLOG_RECORDS_OFF)
    return -EINVAL;
  const uint8_t *p = (const uint8_t *)container;

  uint32_t version = get32(p + 0x08);
  if (version != NV_LIBOS_NVLOG_VERSION) return -EINVAL;
  uint32_t build_id_len = get32(p + 0x0C);
  if (build_id_len > NV_LIBOS_NVLOG_BUILD_ID_MAX) return -EINVAL;
  if (get32(p + 0x44) != 0) return -EINVAL; /* reserved is not this layout */

  /* taskPrefix: the decoder prints it, so it must be a terminated, non-empty
   * string (the vendor's field is char[8] and libosLogInitHeader copies into it
   * with portStringCopy, liblogdecode.c:1994-1997). */
  uint32_t plen = 0;
  while (plen < NV_LIBOS_NVLOG_TASK_PREFIX && p[0x10 + plen]) plen++;
  if (plen == 0 || plen >= NV_LIBOS_NVLOG_TASK_PREFIX) return -EINVAL;

  uint64_t put = get64(p + NV_LIBOS_NVLOG_PUT_OFF);
  uint64_t entries = (container_len - NV_LIBOS_NVLOG_RECORDS_OFF) / 8u;
  if (put > entries) return -E2BIG;

  if (out) {
    out->gpu_arch = get32(p + 0x00);
    out->gpu_impl = get32(p + 0x04);
    out->version = version;
    out->build_id_length = build_id_len;
    for (uint32_t i = 0; i < NV_LIBOS_NVLOG_TASK_PREFIX; i++)
      out->task_prefix[i] = (char)p[0x10 + i];
    out->timer_delta = get64(p + 0x18);
    for (uint32_t i = 0; i < NV_LIBOS_NVLOG_BUILD_ID_MAX; i++)
      out->build_id[i] = p[0x20 + i];
    out->flags = get32(p + 0x40);
    out->reserved = get32(p + 0x44);
  }
  return 0;
}

uint64_t nv_libos_log_region_put(const void *region, uint64_t region_len) {
  if (!region || region_len < 8u) return 0;
  return get64((const uint8_t *)region);
}

uint64_t nv_libos_log_region_records(const void *region, uint64_t region_len) {
  uint64_t put = nv_libos_log_region_put(region, region_len);
  uint64_t fit = (region_len >= 8u) ? (region_len - 8u) / 8u : 0;
  return put > fit ? fit : put;
}

int nv_libos_nvlog_snapshot(const void *region, uint64_t region_len,
                            void *container, uint64_t container_len,
                            uint32_t gpu_arch, uint32_t gpu_impl,
                            const char *task_prefix, uint32_t flags) {
  if (!region) return -EINVAL;
  if (container_len < region_len ||
      container_len - region_len < (uint64_t)NV_LIBOS_NVLOG_HEADER_BYTES)
    return -E2BIG;
  int rc = nv_libos_nvlog_encode(container, container_len, gpu_arch, gpu_impl,
                                 task_prefix, 0, 0, 0, flags);
  if (rc != 0) return rc;
  /* The region's own put pointer and records become the container's data,
   * exactly as libosCopyLogToNvlog_nowrap() copies physicLogBuffer into
   * pNoWrapBuf->data (liblogdecode.c:1514-1523). */
  const uint8_t *src = (const uint8_t *)region;
  uint8_t *dst = (uint8_t *)container;
  for (uint64_t i = 0; i < region_len; i++)
    dst[NV_LIBOS_NVLOG_PUT_OFF + i] = src[i];
  return 0;
}

uint32_t nv_libos_log_region_nonzero(const void *region, uint64_t region_len) {
  if (!region) return 0;
  const uint32_t *w = (const uint32_t *)region;
  uint32_t nz = 0;
  for (uint64_t i = 0; i + 4u <= region_len; i += 4u)
    if (w[i / 4u] != 0) nz++;
  return nz;
}
