# cp/p0guest: source inside the guest. Pins every Vulkan app to the RX 9070 XT
# through Venus. The host side (host-venus.sh, VKFILTER=order) already drops the
# host's lavapipe and orders the 9070 XT first; this hides the guest's own
# lavapipe and the 8060S (Venus GPU1). The guest-side "vid:did!" form works;
# the same form on the host breaks vkr (see host-venus.sh).
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json
export VK_ICD_FILENAMES=$VK_DRIVER_FILES
export MESA_VK_DEVICE_SELECT=1002:7550!
export DISPLAY=${DISPLAY:-:0}
