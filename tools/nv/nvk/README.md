# tools/nv/nvk — Mesa NVK as a HobbyOS Vulkan ICD

`libvulkan_nouveau.so` (Mesa's NVK) built for the x86_64 HobbyOS **Linux-ABI compat
class**, plus the smallest probe that drives it, and the guest harness that runs it
against `/dev/nvshim`.

```sh
bash tools/nv/nvk/build.sh all                 # sysroot → zlib → libelf → libdrm → mesa → probe → audit
NVK_SKIP_KERNEL_BUILD=1 bash tools/nv/nvk/run_guest.sh
```

State (2026-11-24): the ICD loads in the guest, creates a Vulkan instance, identifies
**AD102** through `NVIF NV_DEVICE_V0_INFO`, allocates a channel and its SCLASS list, and
stops at the first engine class object — `NVIF NEW` class `0xC7B5`
(`AMPERE_DMA_COPY_B`) refused with `need=gsp_rm missing=rm_chan_object`. Enumeration is
0 devices; nothing is faked.

Read `SWARM-REPORT-WP-NVK.md` (repo root) before changing anything here: it lists every
ABI gap found, why the build pins Mesa 25.0.7 instead of the tree's 26.2.6, and what the
next steps are. `patches/0001-…patch` is the only Mesa change (node discovery + named
refusals); `build.sh audit` re-checks every claim the report makes about the DSO.
