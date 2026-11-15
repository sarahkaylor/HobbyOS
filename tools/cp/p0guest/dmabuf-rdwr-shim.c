/*
 * cp/p0guest: LD_PRELOAD shim for QEMU + virgl_render_server (host side).
 *
 * Venus maps host-visible VkDeviceMemory into the guest by exporting it from
 * the render server as a dma-buf and having QEMU mmap() that fd
 * PROT_READ|PROT_WRITE, MAP_SHARED (virgl_renderer_resource_map). On strix
 * (Ubuntu 26.04: Mesa 26.0.8 RADV, libdrm 2.4.131, virglrenderer 1.2.0,
 * QEMU 10.2.1) the exported dma-buf is read-only, the mmap fails with EACCES,
 * QEMU logs "virtio_gpu_virgl_map_resource_blob: failed to map virgl
 * resource: Invalid argument", and every guest vkMapMemory fails
 * (vkcube asserts in demo_prepare_texture_image).
 *
 * The shim ORs DRM_RDWR into every drmPrimeHandleToFD() call and, with
 * CP_SHIM_LOG=1, logs the flags it saw (the evidence for the diagnosis).
 * Build: cc -O2 -shared -fPIC -o dmabuf-rdwr-shim.so dmabuf-rdwr-shim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef DRM_RDWR
#define DRM_RDWR 02 /* O_RDWR, as xf86drm.h defines it */
#endif

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	static int (*real)(int, uint32_t, uint32_t, int *);
	static int log = -1;
	if (!real) {
		/* RADV is dlopen()ed RTLD_LOCAL by the Vulkan loader, so its libdrm
		 * is not in the global scope and RTLD_NEXT finds nothing there
		 * (a NULL call: the render-server worker segfaults). Ask for the
		 * already-loaded libdrm by name instead. */
		void *h = dlopen("libdrm.so.2", RTLD_NOW | RTLD_NOLOAD);
		if (!h)
			h = dlopen("libdrm.so.2", RTLD_NOW);
		real = (int (*)(int, uint32_t, uint32_t, int *))dlsym(h, "drmPrimeHandleToFD");
		if (!real) {
			fprintf(stderr, "cp-shim: libdrm drmPrimeHandleToFD not found\n");
			errno = ENOSYS;
			return -ENOSYS;
		}
	}
	if (log < 0) {
		const char *e = getenv("CP_SHIM_LOG");
		log = e && *e == '1';
	}
	int ret = real(fd, handle, flags | DRM_RDWR, prime_fd);
	if (log)
		fprintf(stderr, "cp-shim: pid %d (%s) drmPrimeHandleToFD(handle=%u, flags=0x%x)%s = %d\n",
			(int)getpid(), program_invocation_short_name, handle, flags,
			(flags & DRM_RDWR) ? "" : " -> +DRM_RDWR", ret);
	return ret;
}
