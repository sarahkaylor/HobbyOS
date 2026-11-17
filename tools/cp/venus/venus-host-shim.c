/*
 * tools/cp/venus/venus-host-shim.c -- LD_PRELOAD for QEMU and its
 * virgl_render_server children (lane C6-venus; tools/cp/host-venus.sh).
 *
 * 1. dma-buf exports get DRM_RDWR (P0's fix, tools/cp/p0guest/dmabuf-rdwr-shim.c,
 *    docs/cp-p0-guest.md §2.1 item 3: without it QEMU cannot mmap a host-visible
 *    blob and every guest vkMapMemory fails).
 * 2. Render-node fencing: every open of a path listed in CP_SHIM_DENY
 *    (colon-separated, e.g. "/dev/dri/renderD128:/dev/dri/card1") fails with
 *    ENOENT.  strix's desktop runs on the 8060S; RADV enumerates every DRM
 *    device and opens each render node, and the device-select layer can only
 *    hide a device after RADV has opened it (and its `!` form breaks Venus'
 *    enumeration).  Refusing the open makes RADV skip the device, so the
 *    render server never touches the desktop GPU and the guest sees one GPU.
 *
 * CP_SHIM_LOG=1 logs every override to stderr (QEMU's stderr file).
 * Build: cc -O2 -Wall -shared -fPIC -o venus-host-shim.so venus-host-shim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef DRM_RDWR
#define DRM_RDWR 02
#endif

static int shim_log(void) {
	static int log = -1;
	if (log < 0) {
		const char *e = getenv("CP_SHIM_LOG");
		log = e && *e == '1';
	}
	return log;
}

static int denied(const char *path) {
	const char *list = getenv("CP_SHIM_DENY");
	if (!path || !list || !*list)
		return 0;
	size_t n = strlen(path);
	for (const char *p = list; *p;) {
		const char *q = strchr(p, ':');
		size_t len = q ? (size_t)(q - p) : strlen(p);
		if (len == n && strncmp(p, path, n) == 0) {
			if (shim_log())
				fprintf(stderr, "cp-shim: pid %d (%s) open(%s) denied\n", (int)getpid(),
					program_invocation_short_name, path);
			return 1;
		}
		if (!q)
			break;
		p = q + 1;
	}
	return 0;
}

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef int (*open2_fn)(const char *, int);

#define REAL(name, type)                                                  \
	static type real_##name;                                          \
	if (!real_##name)                                                 \
		real_##name = (type)dlsym(RTLD_NEXT, #name)

static mode_t mode_arg(int flags, va_list ap) {
	return (flags & (O_CREAT | O_TMPFILE)) ? (mode_t)va_arg(ap, int) : 0;
}

int open(const char *path, int flags, ...) {
	REAL(open, open_fn);
	va_list ap;
	va_start(ap, flags);
	mode_t m = mode_arg(flags, ap);
	va_end(ap);
	if (denied(path)) {
		errno = ENOENT;
		return -1;
	}
	return real_open(path, flags, m);
}

int open64(const char *path, int flags, ...) {
	REAL(open64, open_fn);
	va_list ap;
	va_start(ap, flags);
	mode_t m = mode_arg(flags, ap);
	va_end(ap);
	if (denied(path)) {
		errno = ENOENT;
		return -1;
	}
	return real_open64(path, flags, m);
}

int openat(int dirfd, const char *path, int flags, ...) {
	REAL(openat, openat_fn);
	va_list ap;
	va_start(ap, flags);
	mode_t m = mode_arg(flags, ap);
	va_end(ap);
	if (denied(path)) {
		errno = ENOENT;
		return -1;
	}
	return real_openat(dirfd, path, flags, m);
}

int openat64(int dirfd, const char *path, int flags, ...) {
	REAL(openat64, openat_fn);
	va_list ap;
	va_start(ap, flags);
	mode_t m = mode_arg(flags, ap);
	va_end(ap);
	if (denied(path)) {
		errno = ENOENT;
		return -1;
	}
	return real_openat64(dirfd, path, flags, m);
}

int __open_2(const char *path, int flags) {
	REAL(__open_2, open2_fn);
	if (denied(path)) {
		errno = ENOENT;
		return -1;
	}
	return real___open_2(path, flags);
}

int __open64_2(const char *path, int flags) {
	REAL(__open64_2, open2_fn);
	if (denied(path)) {
		errno = ENOENT;
		return -1;
	}
	return real___open64_2(path, flags);
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
	static int (*real)(int, uint32_t, uint32_t, int *);
	if (!real) {
		/* RADV is dlopen()ed RTLD_LOCAL by the Vulkan loader, so RTLD_NEXT
		 * finds nothing; ask for the loaded libdrm by name (P0). */
		void *h = dlopen("libdrm.so.2", RTLD_NOW | RTLD_NOLOAD);
		if (!h)
			h = dlopen("libdrm.so.2", RTLD_NOW);
		real = h ? (int (*)(int, uint32_t, uint32_t, int *))dlsym(h, "drmPrimeHandleToFD") : 0;
		if (!real) {
			fprintf(stderr, "cp-shim: libdrm drmPrimeHandleToFD not found\n");
			errno = ENOSYS;
			return -ENOSYS;
		}
	}
	int ret = real(fd, handle, flags | DRM_RDWR, prime_fd);
	if (shim_log())
		fprintf(stderr, "cp-shim: pid %d (%s) drmPrimeHandleToFD(handle=%u, flags=0x%x)%s = %d\n",
			(int)getpid(), program_invocation_short_name, handle, flags,
			(flags & DRM_RDWR) ? "" : " -> +DRM_RDWR", ret);
	return ret;
}
