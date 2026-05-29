#!/bin/bash
# ==============================================================================
# run_proxmox_gpu.sh
#
# Automates the build, deployment, and execution of HobbyOS on the Proxmox server
# (192.168.10.174) with NVIDIA RTX 4090 GPU remote sharing over UDP/IP RDMA.
# ==============================================================================

PROXMOX_IP="192.168.10.174"
SSH_KEY="~/.ssh/mac_to_r1"
SSH_CMD="ssh -i $SSH_KEY root@$PROXMOX_IP"
SCP_CMD="scp -i $SSH_KEY"

HOST_VMID=205
GUEST_VMID=206

NVIDIA_VENDOR_ID="0x10de"
NVIDIA_DEVICE_ID="0x2684" # RTX 4090 Device ID

echo "======================================================"
echo " 1. Building HobbyOS for Intel (x86_64) Unit Tests"
echo "======================================================"
make ARCH=intel MODE=unit_tests clean
make ARCH=intel MODE=unit_tests disk.img

if [ $? -ne 0 ]; then
    echo "HobbyOS compilation failed!"
    exit 1
fi

echo "======================================================"
echo " 2. Uploading disk images and kernels to Proxmox"
echo "======================================================"
$SSH_CMD "mkdir -p /root/hobbyos"
$SCP_CMD disk.img root@$PROXMOX_IP:/root/hobbyos/disk_host.raw
$SCP_CMD disk.img root@$PROXMOX_IP:/root/hobbyos/disk_guest.raw
$SCP_CMD hobbyos.elf root@$PROXMOX_IP:/root/hobbyos/hobbyos.elf

echo "======================================================"
echo "======================================================"
echo " 3. Setting up Host VM ($HOST_VMID) on Proxmox"
echo "======================================================"
# Stop and Purge existing VM if any
$SSH_CMD "qm stop $HOST_VMID 2>/dev/null || true"
$SSH_CMD "qm destroy $HOST_VMID --purge 2>/dev/null || true"
$SSH_CMD "rm -f /root/hobbyos/host.log /root/hobbyos/guest.log"

# Create new VM with 4 cores, 4GB RAM, virtio network, and q35 machine model
$SSH_CMD "qm create $HOST_VMID --name HobbyOSHostGPU --cores 4 --memory 4096 --machine q35 --net0 virtio=52:54:00:12:34:56,bridge=vmbr0,firewall=0"

# Pass-through the physical NVIDIA RTX 4090 GPU (PCIE address 0000:01:00)
# Enabling PCIe mode ensures correct BAR configurations and device capability matching
$SSH_CMD "qm set $HOST_VMID --hostpci0 0000:01:00,pcie=1,x-vga=0"

# Configure Host Firmware Configuration (fw_cfg) role parameter, direct kernel boot, serial redirection, and NVMe disk
$SSH_CMD "qm set $HOST_VMID --args \"-kernel /root/hobbyos/hobbyos.elf -fw_cfg name=opt/pcishare,string=host:$NVIDIA_VENDOR_ID:$NVIDIA_DEVICE_ID -serial file:/root/hobbyos/host.log -drive file=/root/hobbyos/disk_host.raw,format=raw,id=disk0,if=none -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 -device nvme,drive=disk0,serial=1234,bus=pcie.1\""

echo "======================================================"
echo " 4. Setting up Guest VM ($GUEST_VMID) on Proxmox"
echo "======================================================"
$SSH_CMD "qm stop $GUEST_VMID 2>/dev/null || true"
$SSH_CMD "qm destroy $GUEST_VMID --purge 2>/dev/null || true"
$SSH_CMD "qm create $GUEST_VMID --name HobbyOSGuestGPU --cores 4 --memory 4096 --machine q35 --net0 virtio=52:54:00:12:34:57,bridge=vmbr0,firewall=0"

# Configure Guest Firmware Configuration (fw_cfg) role parameter, direct kernel boot, serial redirection, and NVMe disk
$SSH_CMD "qm set $GUEST_VMID --args \"-kernel /root/hobbyos/hobbyos.elf -fw_cfg name=opt/pcishare,string=guest:$NVIDIA_VENDOR_ID:$NVIDIA_DEVICE_ID -serial file:/root/hobbyos/guest.log -drive file=/root/hobbyos/disk_guest.raw,format=raw,id=disk0,if=none -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 -device nvme,drive=disk0,serial=1234,bus=pcie.1\""

echo "======================================================"
echo " 5. Starting Host and Guest VMs on Proxmox"
echo "======================================================"
echo "Starting Host VM ($HOST_VMID)..."
$SSH_CMD "qm start $HOST_VMID"

# Wait for Host to initialize PCIe networking daemon
sleep 5

echo "Starting Guest VM ($GUEST_VMID)..."
$SSH_CMD "qm start $GUEST_VMID"

echo "======================================================"
echo " Deployment Complete!"
echo "======================================================"
echo "To monitor Host VM console:   ssh -t -i $SSH_KEY root@$PROXMOX_IP 'tail -f /root/hobbyos/host.log'"
echo "To monitor Guest VM console:  ssh -t -i $SSH_KEY root@$PROXMOX_IP 'tail -f /root/hobbyos/guest.log'"
echo "To stop VMs:                  ssh -i $SSH_KEY root@$PROXMOX_IP 'qm stop $HOST_VMID; qm stop $GUEST_VMID'"
