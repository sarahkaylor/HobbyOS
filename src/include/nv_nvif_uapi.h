#ifndef NV_NVIF_UAPI_H
#define NV_NVIF_UAPI_H

/* ============================================================================
 *  NVIF -- DRM_IOCTL_NOUVEAU_NVIF, the object model a stock nvk actually uses
 *  (PLAN.md §3 WP-F, ladder rung N2 -> N5; round 5, WP-NVIF).
 *
 *  WHY THIS HEADER EXISTS.  /dev/nvshim already answers 16 real
 *  DRM_IOCTL_NOUVEAU_* rows, but a stock nouveau winsys does not stop there:
 *  before it can create a channel object it creates a DEVICE object through
 *  NVIF and asks that object for the chip's identity, then asks the channel
 *  which engine classes it offers and instantiates one per engine.  Both lane
 *  reports (SWARM-REPORT-WP-F2 §5.7, SWARM-REPORT-WP-EL0 §9) name NVIF as the
 *  single row between this node and a winsys that can enumerate.  This header
 *  is the contract for that row.
 *
 *  SOURCES (contracts, not code):
 *    drivers/gpu/drm/nouveau/include/nvif/ioctl.h
 *        struct nvif_ioctl_v0, nvif_ioctl_sclass_v0, nvif_ioctl_new_v0,
 *        nvif_ioctl_del, nvif_ioctl_mthd_v0, nvif_ioctl_map_v0, the type
 *        numbers (SCLASS, NEW, DEL, MTHD, MAP, UNMAP) and the OWNER_ / ROUTE_
 *        constant families
 *    drivers/gpu/drm/nouveau/include/nvif/cl0080.h
 *        NV_DEVICE_V0_INFO/TIME/GCX_READY, struct nv_device_info_v0 and its
 *        platform/family enumerations, struct nv_device_time_v0
 *    drivers/gpu/drm/nouveau/include/nvif/class.h
 *        NV_DEVICE 0x00000080 and the AD102 engine classes named below
 *    drivers/gpu/drm/nouveau/nouveau_abi16.c  (the SERVER side of the ABI:
 *        argc rules, route rules, which request keys on which field)
 *    Mesa src/nouveau/winsys/nouveau_device.c, nouveau_context.c  (the CLIENT
 *        side: the exact call order and sizes a stock nvk sends)
 *
 *  WHAT THE SERVER SIDE ACTUALLY CONTRACTUALLY REQUIRES (measured against
 *  nouveau_abi16_ioctl, which is the only server this ABI has):
 *
 *   - argc is the WHOLE ioctl buffer (header + payload).  Unlike the other 16
 *     rows on this node, NVIF is not one fixed struct: the request's encoded
 *     _IOC size is the caller's buffer length and each `type` has its own
 *     shape rule.  So NVIF is validated per type, not by an equality check.
 *   - ioctl->version must be 0; ioctl->route must be 0x00 (NVIF) or 0xff
 *     (hidden, i.e. "under the object named by token").
 *   - NEW, route 0x00: ioctl->object must be 0 and oclass must be NV_DEVICE
 *     (0x0080).  The object's identity is the CLIENT'S cookie in
 *     new.object -- the kernel never hands one back, it echoes what it was
 *     given, which is why Mesa passes (uintptr_t)its own struct and later
 *     names it in ioctl->object.  A second NEW with the same cookie is
 *     -EEXIST.
 *   - NEW, route 0xff: ioctl->token is a channel handle; oclass is an engine
 *     class the channel offered via SCLASS.  This instantiates a real HW
 *     class on that channel's usermode object -- the class of operation that
 *     cannot exist without the RM (see "what this node cannot answer" below).
 *   - SCLASS: route must be non-zero, token is a channel handle, and
 *     argc - sizeof(sclass) must equal count * sizeof(oclass entry) exactly.
 *     The reply's count is the number of classes the channel HAS, which may
 *     exceed the caller's count (the caller's array is then truncated).
 *   - MTHD: route must be 0; ioctl->object names a DEVICE object (any other
 *     object is -EINVAL); only NV_DEVICE_V0_INFO is served, and its payload
 *     must be exactly sizeof(struct nv_device_info_v0) with version 0.
 *   - DEL: route must be 0 and argc must be exactly sizeof(the header).  An
 *     unknown cookie is not an error (nouveau's handler returns 0).
 *
 *  WHAT THIS NODE SERVES, AND WHAT IT REFUSES BY NAME
 *
 *    served:  NEW(oclass 0x0080) -> a DEVICE object, keyed by the client's
 *             cookie; MTHD NV_DEVICE_V0_INFO; DEL of a DEVICE object;
 *             SCLASS on a channel this client owns.
 *    refused: NEW of every engine class SCLASS reports (-ENOSYS, one named
 *             row carrying the class id and `need=gsp_rm`), MTHD
 *             NV_DEVICE_V0_TIME (-ENOSYS, `need=ptimer`), MTHD GCX_READY
 *             (-ENOSYS, `need=gsp_rm`), MAP/UNMAP (-ENOSYS, `need=gsp_rm`).
 *
 *  WHY SCLASS REPORTS CLASSES THIS NODE THEN REFUSES.  SCLASS is a question
 *  about the CHIP ("which classes does this channel's runlist carry"), not a
 *  promise by the node ("which can I instantiate").  On AD102 the answer is
 *  fixed by class.h and by nouveau's engine tables (see the ids below), and a
 *  winsys that asks for the real class id is far more useful than one that is
 *  handed an empty list and asserts on a zero class.  Every entry is then
 *  refused by name with the class id in the row, which is where the wall is
 *  and what this lane exists to make visible.  The alternative -- inventing a
 *  fake object for 0xc797 -- would let a client build and submit a command
 *  stream that no engine will ever read.
 *
 *  DATA LAYOUT RULES (hobbyos-kernel-constraints): naturally aligned, explicit
 *  padding, no bitfields, the same size in the freestanding kernel and in a
 *  hosted client (asserted on both sides).
 * ========================================================================== */

#include <stdint.h>

/* ---- header: struct nvif_ioctl_v0 (24 bytes, then its data[]) ---------- */

struct nvshim_nvif_hdr {
  uint8_t version;  /* 0: must be 0 */
  uint8_t type;     /* NVSHIM_NVIF_* */
  uint8_t pad02[4];
  uint8_t owner;    /* NVSHIM_NVIF_OWNER_*; nouveau ignores this field */
  uint8_t route;    /* 0x00 = NVIF (this client), 0xff = under `token` */
  uint64_t token;   /* route 0xff: the parent's handle (a channel) */
  uint64_t object;  /* the client's cookie for the object being named */
};                  /* 24 bytes */

#define NVSHIM_NVIF_SCLASS 0x01
#define NVSHIM_NVIF_NEW    0x02
#define NVSHIM_NVIF_DEL    0x03
#define NVSHIM_NVIF_MTHD   0x04
#define NVSHIM_NVIF_MAP    0x07
#define NVSHIM_NVIF_UNMAP  0x08

#define NVSHIM_NVIF_OWNER_NVIF 0x00
#define NVSHIM_NVIF_OWNER_ANY  0xff
#define NVSHIM_NVIF_ROUTE_NVIF   0x00
#define NVSHIM_NVIF_ROUTE_HIDDEN 0xff

/* ---- SCLASS: struct nvif_ioctl_sclass_v0 (8 bytes) + oclass[] ---------- */

struct nvshim_nvif_sclass {
  uint8_t version; /* 0 */
  uint8_t count;   /* in: how many entries the caller's array holds
                    * out: how many the channel HAS (may be larger) */
  uint8_t pad02[6];
};                 /* 8 bytes */

struct nvshim_nvif_sclass_oclass {
  int32_t oclass; /* the class id, e.g. 0x0000c797 */
  int16_t minver; /* -1 = any (the spelling nouveau's engine tables use) */
  int16_t maxver;
};                /* 8 bytes */

#define NVSHIM_NVIF_MAX_SCLASS 16 /* what a winsys asks for (Mesa asks 16) */

/* ---- NEW: struct nvif_ioctl_new_v0 (32 bytes) + class data ------------- */

struct nvshim_nvif_new {
  uint8_t version; /* 0 */
  uint8_t pad01[6];
  uint8_t route;   /* the new object's route; NVIF echoes what it was given */
  uint64_t token;  /* the new object's token */
  uint64_t object; /* in: the CLIENT's cookie for the new object */
  uint32_t handle; /* in: class-specific handle (0 for everything here) */
  int32_t oclass;  /* in: the class to instantiate */
};                 /* 32 bytes */

/* ---- MTHD: struct nvif_ioctl_mthd_v0 (8 bytes) + method data ----------- */

struct nvshim_nvif_mthd {
  uint8_t version; /* 0 */
  uint8_t method;  /* NVSHIM_DEVICE_V0_* */
  uint8_t pad02[6];
};                 /* 8 bytes */

/* ---- class NV_DEVICE (0x0080): cl0080.h -------------------------------- */

#define NVSHIM_DEVICE_V0_INFO       0x00
#define NVSHIM_DEVICE_V0_TIME       0x01
#define NVSHIM_DEVICE_V0_GCX_READY  0x02

/* struct nv_device_v0: the class data a DEVICE NEW may carry.  nouveau's
 * abi16 ignores it for the device object (it only validates oclass), so this
 * node accepts a NEW with or without it and never reads `device`. */
struct nvshim_device_v0 {
  uint8_t version;  /* 0 */
  uint8_t pad01[7];
  uint64_t device;  /* ~0 = this client's default device */
};                  /* 16 bytes */

#define NVSHIM_DEVICE_INFO_V0_IGP  0x00
#define NVSHIM_DEVICE_INFO_V0_PCI  0x01
#define NVSHIM_DEVICE_INFO_V0_AGP  0x02
#define NVSHIM_DEVICE_INFO_V0_PCIE 0x03
#define NVSHIM_DEVICE_INFO_V0_SOC  0x04

#define NVSHIM_DEVICE_INFO_V0_KEPLER   0x08
#define NVSHIM_DEVICE_INFO_V0_MAXWELL  0x09
#define NVSHIM_DEVICE_INFO_V0_PASCAL   0x0a
#define NVSHIM_DEVICE_INFO_V0_VOLTA    0x0b
#define NVSHIM_DEVICE_INFO_V0_TURING   0x0c
#define NVSHIM_DEVICE_INFO_V0_AMPERE   0x0d
#define NVSHIM_DEVICE_INFO_V0_ADA      0x0e /* AD102 answers this */

/* struct nv_device_info_v0 -- what NV_DEVICE_V0_INFO fills.  `ram_user` is
 * what a winsys sizes its heaps from (Mesa calls it vram_size_B). */
struct nvshim_device_info_v0 {
  uint8_t version;   /* 0 */
  uint8_t platform;  /* NVSHIM_DEVICE_INFO_V0_* */
  uint16_t chipset;  /* PMC_BOOT_0 chipset field: 0x192 = AD102 */
  uint8_t revision;  /* PMC_BOOT_0 revision byte */
  uint8_t family;    /* NVSHIM_DEVICE_INFO_V0_ADA for AD102 */
  uint8_t pad06[2];
  uint64_t ram_size;
  uint64_t ram_user;
  char chip[16];     /* "AD102" */
  char name[64];     /* the product name */
};                   /* 104 bytes */

/* struct nv_device_time_v0 -- MTHD NV_DEVICE_V0_TIME.  Needs the card's
 * PTIMER; refused here by name rather than answered from the CPU clock. */
struct nvshim_device_time_v0 {
  uint8_t version;
  uint8_t pad01[7];
  uint64_t time;
};                   /* 16 bytes */

/* ---- class ids (class.h).  The five engine classes a stock winsys looks
 * for on AD102 are matched by their LOW BYTE (Mesa's
 * nouveau_ws_context_find_class: 0x2d 2D, 0x39/0x40 M2MF, 0x97 3D, 0xb5 copy,
 * 0xc0 compute), so the ids below are the ones it will actually request.
 *
 * SOURCE for each id and for its membership in an AD102 channel's list:
 *   gr/ga102.c  .sclass = { FERMI_TWOD_A, KEPLER_INLINE_TO_MEMORY_B,
 *                           AMPERE_B, AMPERE_COMPUTE_B }
 *   ce/ga102.c  .sclass = { AMPERE_DMA_COPY_A, AMPERE_DMA_COPY_B }
 * (nv192_chipset dispatches AD102 to ga102_gr_new/ga102_ce_new.) */

#define NVSHIM_NVIF_CLASS_DEVICE 0x00000080 /* NV_DEVICE (cl0080.h) */

#define NVSHIM_NVIF_CLASS_FERMI_TWOD_A            0x0000902d /* 2D   */
#define NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B  0x0000a140 /* M2MF */
#define NVSHIM_NVIF_CLASS_AMPERE_B                0x0000c797 /* 3D   */
#define NVSHIM_NVIF_CLASS_AMPERE_COMPUTE_B        0x0000c7c0 /* comp */
#define NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_A       0x0000c6b5 /* copy */
#define NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B       0x0000c7b5 /* copy */

/* ---- request number and size rules ------------------------------------ *
 * NVIF is the one row on this node whose encoded _IOC size is a BUFFER
 * LENGTH, not a struct width, so the row's "expected size" in the dispatch
 * table is the MINIMUM (the bare header) and each type validates its own
 * shape.  The helpers below are what the client uses to build a request of a
 * given total length, exactly as libdrm's drmCommandWrite* does. */

#define NVSHIM_NVIF_MIN_SIZE 24u /* sizeof(struct nvshim_nvif_hdr) */

/* Headerless payload sizes, for the size rules (per type). */
#define NVSHIM_NVIF_NEW_HDR     32u
#define NVSHIM_NVIF_MTHD_HDR    8u
#define NVSHIM_NVIF_SCLASS_HDR  8u

#endif /* NV_NVIF_UAPI_H */
