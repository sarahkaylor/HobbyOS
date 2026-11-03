/* xf86drm.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * Three TUs include it: gallium renderonly, gallium u_screen (PRIME check)
 * and util/u_sync_provider (timeline syncobj).  All three only need
 * drmGetCap() plus the DRM_CAP_* ids; on softpipe there is no DRM fd at all,
 * so drmGetCap() returns -1 (gl_stubs.c) and every caller takes its
 * "unsupported" branch.  This is the same 2-symbol libdrm surface the host
 * build measured (MESA-NOTES §2: libdrm contributes exactly drmGetCap). */
#ifndef HOBBYOS_XF86DRM_H
#define HOBBYOS_XF86DRM_H 1

#include <stdint.h>
/* Mesa's own copy of the kernel uapi header (include/drm-uapi/drm.h); the
 * real libdrm's <xf86drm.h> includes <drm.h> the same way. */
#include <drm-uapi/drm.h>

#define DRM_CAP_DUMB_BUFFER      0x1
#define DRM_CAP_VBLANK_HIGH_CRTC 0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_SYNCOBJ              0x13
#define DRM_CAP_SYNCOBJ_TIMELINE     0x14

#define DRM_PRIME_CAP_IMPORT 0x1
#define DRM_PRIME_CAP_EXPORT 0x2

typedef struct _drmVersion {
  int version_major;
  int version_minor;
  int version_patchlevel;
  int name_len;
  char *name;
  int date_len;
  char *date;
  int desc_len;
  char *desc;
} drmVersion, *drmVersionPtr;

#ifdef __cplusplus
extern "C" {
#endif
int drmIoctl(int fd, unsigned long request, void *arg);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmClose(int fd);

/* Timeline syncobj surface (util/u_sync_provider.c).  All fail with -1/ENOSYS
 * in gl_stubs.c: there is no DRM device to create a syncobj on. */
typedef struct _drmSyncobjHandle *drmSyncobjHandle;
typedef struct _drmSyncobjTimeline {
  uint32_t handle;
  uint64_t value;
} drmSyncobjTimeline;

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                             const uint64_t *points, uint32_t handle_count);
int drmSyncobjQuery2(int fd, const uint32_t *handles, uint64_t *points,
                     uint32_t handle_count, uint32_t flags);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags);
int drmSyncobjQuery(int fd, const uint32_t *handles, uint64_t *points,
                    uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, const uint32_t *handles,
                           const uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags,
                           uint32_t *first_signaled);
int drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled);
#ifdef __cplusplus
}
#endif

#endif
