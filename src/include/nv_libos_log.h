#ifndef NV_LIBOS_LOG_H
#define NV_LIBOS_LOG_H

#include <stdint.h>

/* ============================================================================
 *  nv_libos_log -- the LIBOS task log channel, built to the vendor's own bytes.
 *
 *  WHY THIS EXISTS.  Every GB202 boot since the ACR wall ends at m0=0x0B or
 *  0x824 with statq_written=0 linked=0 initdone=0, and the FSP's own DMEM log
 *  is not a channel on this part (docs/gb202-dmem-log.md section 0: FSP v4.76
 *  ignores the download-log command, kern_fsp_gb100.c:403-407).  The remaining
 *  channel is the one the vendor opens before it starts GSP-RM: one sysmem
 *  region per LIBOS task, named in the libos init-args table, into which the
 *  task writes a put pointer and its log records.  This file owns the layout
 *  of that channel and nothing else, so the boot path, the reader and the unit
 *  tier all use one definition of it.
 *
 *  THE VENDOR'S TWO OBJECTS -- they are NOT the same buffer, and this is the
 *  fact the mode-4 probe got wrong:
 *
 *   1. the REGION handed to firmware (kernel_gsp.c:_setupLogBufferBaremetal,
 *      :3052-3134).  Layout: one NvU64 put pointer at +0, log records from +8
 *      (liblogdecode.c:1282: "logBufferSize / sizeof(NvU64) - 1", the -1 being
 *      the PUT pointer; record reads start at physicLogBuffer[1], :1308/:1365).
 *      The host zeroes it and the task owns it from then on.  The args-table
 *      entry's pa is the region's own physical address: `pTaskLogBuffer[1]`
 *      (kernel_gsp.c:3104, :5199) is the PTE[0] value memdescGetPhysAddrs just
 *      filled in, not an 8-byte offset.
 *
 *   2. the NVLOG CONTAINER the RM decodes (liblogdecode.c:1985-2013
 *      libosLogInitHeader + :2086 pos = offsetof(data) + sizeof(NvU64)).  This
 *      one has a 72-byte header (LIBOS_LOG_NVLOG_BUFFER_V2,
 *      liblogdecode.h:123-135) followed by the same put-pointer + records.  It
 *      is the object a dump file carries and the object a reader validates.
 *
 *  So a reader of a boot has to do what libosLogAddLogEx does: wrap the region
 *  (1) in a container (2).  nv_libos_nvlog_snapshot() below is that wrap, and
 *  nv_libos_nvlog_decode() is the check a dump reader does before it trusts a
 *  container.  A zeroed container is NOT a valid nvlog header -- version 0 is
 *  not LIBOS_LOG_NVLOG_BUFFER_VERSION_2 (liblogdecode.h:137-141) -- which is
 *  exactly why the mode-4 probe could never return a record: it handed the RM
 *  buffers it never described.
 *
 *  FIELD PROVENANCE (open-gpu-kernel-modules 580.178.04, MIT):
 *    liblogdecode.h:90          TASK_NAME_MAX_LENGTH 8
 *    liblogdecode.h:95          BUILD_ID_MAX_LENGTH 32
 *    liblogdecode.h:123-135     LIBOS_LOG_NVLOG_BUFFER_V2 (field order below)
 *    liblogdecode.h:137-141     VERSION 0/1/2; current = 2
 *    liblogdecode.h:120-121     FLAG_PACKED_METADATA / FLAG_MERGED
 *    liblogdecode.c:1985-2013   libosLogInitHeader: gpuArch, gpuImpl, version,
 *                               flags, taskPrefix, buildIdLength, buildId
 *    liblogdecode.c:2086        put pointer at offsetof(data) + 8
 *    kernel_gsp.c:3111-3113     libos3 sets FLAG_PACKED_METADATA
 *    kernel_gsp.c:3152-3164     logInitValues: the tasks and their sizes
 *    nv-firmware-chip-family-select.h:107-113  memory ids and prefixes
 *    kernel_gsp.c:5194-5206     the LOG* entries in the libos args table
 *
 *  The header is 72 bytes on both x86-64 and AArch64 (no packing pragma; a
 *  _Static_assert below holds it), and the put pointer starts at 0x48.
 * ========================================================================== */

#define NV_LIBOS_NVLOG_VERSION        2u
#define NV_LIBOS_NVLOG_TASK_PREFIX    8u   /* liblogdecode.h:90  */
#define NV_LIBOS_NVLOG_BUILD_ID_MAX   32u  /* liblogdecode.h:95  */
#define NV_LIBOS_NVLOG_HEADER_BYTES   0x48u
#define NV_LIBOS_NVLOG_PUT_OFF        0x48u
#define NV_LIBOS_NVLOG_RECORDS_OFF    0x50u

#define NV_LIBOS_NVLOG_FLAG_PACKED_METADATA 0x00000001u
#define NV_LIBOS_NVLOG_FLAG_MERGED_NVLOG    0x00000002u

/* GB202 on this part: gpuGetChipArch >> GPU_ARCH_SHIFT and gpuGetChipImpl.
 * nv_arch.h:53 GPU_ARCHITECTURE_BLACKWELL_GB2XX = 0x01B0, :117
 * GPU_IMPLEMENTATION_GB202 = 0x02; GPU_ARCH_SHIFT = 4 (g_gpu_nvoc.h:554). */
#define NV_LIBOS_GPU_ARCH_GB2XX 0x1Bu
#define NV_LIBOS_GPU_IMPL_GB202 0x02u

/* LIBOS_LOG_NVLOG_BUFFER_V2, field for field (liblogdecode.h:123-135). */
struct nv_libos_nvlog_header {
  uint32_t gpu_arch;       /* +0x00 */
  uint32_t gpu_impl;       /* +0x04 */
  uint32_t version;        /* +0x08 */
  uint32_t build_id_length;/* +0x0C */
  char task_prefix[NV_LIBOS_NVLOG_TASK_PREFIX]; /* +0x10 */
  uint64_t timer_delta;    /* +0x18 localToGlobalTimerDelta */
  uint8_t build_id[NV_LIBOS_NVLOG_BUILD_ID_MAX]; /* +0x20 */
  uint32_t flags;          /* +0x40 */
  uint32_t reserved;       /* +0x44 */
};

/* One task's log region, as the vendor's own table names it.  `bytes` is the
 * size the host hands the task; `nvlog` is logInitValues' bEnableNvlog (0
 * only for LOGDBG, kernel_gsp.c:3163). */
struct nv_libos_log_task {
  const char *id;      /* libos args-table id, nv-firmware-chip-family-select.h */
  const char *prefix;  /* taskPrefix the decoder prints */
  uint32_t bytes;
  uint32_t nvlog;
};

extern const struct nv_libos_log_task nv_libos_log_tasks[];
extern const unsigned nv_libos_log_task_count;

/* Bytes the whole set needs, each region page-aligned in one contiguous run. */
uint32_t nv_libos_log_total_bytes(void);

/* Where task `idx` starts inside that run, and how many bytes it owns.
 *   -EINVAL  idx out of range, or a NULL out pointer. */
int nv_libos_log_region(unsigned idx, uint32_t *off, uint32_t *bytes);

/* Build a valid, empty NVLOG container at `container`:
 * V2 header (gpuArch/gpuImpl/version/buildIdLength/taskPrefix/timerDelta/
 * buildId/flags/reserved), then put = 0 (an empty record area), then zeros.
 *   -EINVAL  NULL, a region smaller than HEADER_BYTES, an over-long/empty
 *            prefix, build_id_len > 32, or a build id without its length.
 *   -E2BIG   region cannot hold header + put pointer.
 * Returns 0 and leaves every header field in the vendor's order. */
int nv_libos_nvlog_encode(void *container, uint64_t container_len,
                          uint32_t gpu_arch, uint32_t gpu_impl,
                          const char *task_prefix, uint64_t timer_delta,
                          const uint8_t *build_id, uint32_t build_id_len,
                          uint32_t flags);

/* Read a container back and validate it the way a dump reader must:
 *   version == 2, reserved == 0, buildIdLength <= 32, a NUL-terminated
 *   non-empty taskPrefix, and a put pointer that fits the record area.
 *   -EINVAL  NULL/short region, or any field that is not this layout (a
 *            ZEROED region fails here: version 0 is not 2).
 *   -E2BIG   put pointer names records past the end of the container.
 * `out` may be NULL (validity only). */
int nv_libos_nvlog_decode(const void *container, uint64_t container_len,
                          struct nv_libos_nvlog_header *out);

/* The put pointer of a raw FIRMWARE region (object 1 above): count in NvU64
 * entries, records from +8.  Returns 0 when the region is too short. */
uint64_t nv_libos_log_region_put(const void *region, uint64_t region_len);
uint64_t nv_libos_log_region_records(const void *region, uint64_t region_len);

/* Wrap a raw firmware region in a V2 container, as libosLogAddLogEx does: the
 * header, then the region's put pointer and record area verbatim.
 *   -EINVAL/-E2BIG  as nv_libos_nvlog_encode() (the container must hold the
 *                   whole region); region_len must be <= container_len-0x48.
 * Returns 0, or the encode error. */
int nv_libos_nvlog_snapshot(const void *region, uint64_t region_len,
                            void *container, uint64_t container_len,
                            uint32_t gpu_arch, uint32_t gpu_impl,
                            const char *task_prefix, uint32_t flags);

/* How many NvU64 words of a region are not zero, the whole region included.
 * This is the "did the task write anything at all" row; it is not a decode. */
uint32_t nv_libos_log_region_nonzero(const void *region, uint64_t region_len);

#endif /* NV_LIBOS_LOG_H */
