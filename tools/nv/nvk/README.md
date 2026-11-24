# tools/nv/nvk — Mesa NVK as a HobbyOS Vulkan ICD (harness only)

`libvulkan_nouveau.so` (Mesa's NVK) is built by the tree's Mesa configuration,
**`tools/mesa/build-nvk.sh`**, for the x86_64 HobbyOS **Linux-ABI compat class**.
This directory keeps what is specific to the NVIDIA lane: the probe that drives the
ICD against `/dev/nvshim` and the guest harness that runs it.

```sh
bash tools/mesa/build-nvk.sh all               # sysroot → zlib → libelf → libcxx → libdrm → mesa → probe → audit
bash tools/nv/nvk/run_guest.sh                 # stage glibc 2.31 + the ICD, boot MODE=test, grade the log
```

`build.sh` here is a three-line driver of `tools/mesa/build-nvk.sh` (same step names, same
output); it exists for the lane's documented command. The one Mesa change lives with the
other Mesa patches: `tools/mesa/patches/0014-nvk-hobbyos-node-and-named-refusals.patch`.

State (2026-11-24): the ICD loads in the guest, creates a Vulkan instance, identifies
**AD102** through `NVIF NV_DEVICE_V0_INFO`, allocates a channel and its SCLASS list, and
stops at the first engine class object — `NVIF NEW` class `0xC7B5`
(`AMPERE_DMA_COPY_B`) refused with `need=gsp_rm missing=rm_chan_object`. Enumeration is
0 devices; nothing is faked.

Read `SWARM-REPORT-WP-NVK.md` (repo root) before changing anything here: it lists every
ABI gap found, why the build pins Mesa 25.0.7 instead of the tree's 26.2.6, and what the
next steps are. `tools/mesa/build-nvk.sh audit` re-checks every claim the report makes
about the DSO (SONAME, `DT_NEEDED`, entry points, undefined-symbol provenance).
