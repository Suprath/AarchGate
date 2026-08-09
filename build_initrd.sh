#!/bin/bash
# (c) 2026 Suprath PS. All rights reserved.
# AarchGate: guest initrd.img Builder Script with Ubuntu modules

set -e

echo "========================================================"
echo "    AarchGate Guest Initrd & Kernel Setup Script        "
echo "========================================================"

WORKDIR="/tmp/aarchgate_initrd_root"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

# 1. Download Alpine minirootfs
MINIROOTFS_URL="https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz"
MINIROOTFS_PATH="/tmp/alpine-minirootfs.tar.gz"

if [ ! -f "$MINIROOTFS_PATH" ]; then
    echo "[Builder] Downloading Alpine minirootfs (3.3 MB)..."
    curl -L -o "$MINIROOTFS_PATH" "$MINIROOTFS_URL"
else
    echo "[Builder] Found existing Alpine minirootfs archive at $MINIROOTFS_PATH"
fi

# 2. Extract minirootfs
echo "[Builder] Extracting rootfs..."
tar -C "$WORKDIR" -xf "$MINIROOTFS_PATH"

# 3. Copy guest agent binary
AGENT_SRC="/Users/suprathps/code/ag-npm/src/guest/aarchgate_agent"
if [ ! -f "$AGENT_SRC" ]; then
    echo "ERROR: aarchgate_agent not found at $AGENT_SRC. Did compile succeed?"
    exit 1
fi
echo "[Builder] Copying guest agent to ramdisk..."
cp "$AGENT_SRC" "$WORKDIR/usr/bin/"

# 4. Download and extract flat Ubuntu kernel Image (PE32 format is incompatible)
DEB_IMAGE_URL="https://kernel.ubuntu.com/~kernel-ppa/mainline/v6.8.9/arm64/linux-image-unsigned-6.8.9-060809-generic_6.8.9-060809.202501292017_arm64.deb"
DEB_IMAGE_PATH="/tmp/linux-image.deb"

if [ ! -f "$DEB_IMAGE_PATH" ]; then
    echo "[Builder] Downloading Ubuntu kernel Image package (17 MB)..."
    curl -L -o "$DEB_IMAGE_PATH" "$DEB_IMAGE_URL"
fi

echo "[Builder] Extracting and decompressing Ubuntu flat kernel Image..."
rm -rf /tmp/extracted_image
mkdir -p /tmp/extracted_image
cd /tmp/extracted_image
ar x "$DEB_IMAGE_PATH"
tar -xf data.tar
mv boot/vmlinuz-6.8.9-060809-generic /Users/suprathps/vmlinuz.img.gz
# Force overwrite if vmlinuz.img exists
rm -f /Users/suprathps/vmlinuz.img
gzip -d /Users/suprathps/vmlinuz.img.gz
rm -rf /tmp/extracted_image

# 5. Download and extract separate Ubuntu modules package
DEB_MODULES_URL="https://kernel.ubuntu.com/~kernel-ppa/mainline/v6.8.9/arm64/linux-modules-6.8.9-060809-generic_6.8.9-060809.202501292017_arm64.deb"
DEB_MODULES_PATH="/tmp/linux-modules.deb"
MODULES_DIR="/tmp/extracted_modules"

if [ ! -f "$DEB_MODULES_PATH" ]; then
    echo "[Builder] Downloading Ubuntu modules package (30 MB)..."
    curl -L -o "$DEB_MODULES_PATH" "$DEB_MODULES_URL"
fi

echo "[Builder] Extracting FUSE and Virtio-fs modules..."
rm -rf "$MODULES_DIR"
mkdir -p "$MODULES_DIR"
cd "$MODULES_DIR"
ar x "$DEB_MODULES_PATH"
tar -xf data.tar

# Copy and decompress the required module: virtiofs.ko.zst
mkdir -p "$WORKDIR/lib/modules"
zstd -d lib/modules/6.8.9-060809-generic/kernel/fs/fuse/virtiofs.ko.zst -o "$WORKDIR/lib/modules/virtiofs.ko"
 
# Copy and decompress VSOCK modules
zstd -d lib/modules/6.8.9-060809-generic/kernel/net/vmw_vsock/vsock.ko.zst -o "$WORKDIR/lib/modules/vsock.ko"
zstd -d lib/modules/6.8.9-060809-generic/kernel/net/vmw_vsock/vmw_vsock_virtio_transport_common.ko.zst -o "$WORKDIR/lib/modules/vmw_vsock_virtio_transport_common.ko"
zstd -d lib/modules/6.8.9-060809-generic/kernel/net/vmw_vsock/vmw_vsock_virtio_transport.ko.zst -o "$WORKDIR/lib/modules/vmw_vsock_virtio_transport.ko"
rm -rf "$MODULES_DIR"
 
# Write current macOS host time to sync the guest VM clock
echo "[Builder] Writing build time to sync system clock..."
date -u +"%Y-%m-%d %H:%M:%S" > "$WORKDIR/etc/build_time"

# 6. Inject custom sbin/init boot script (with module loading!)
echo "[Builder] Injecting custom sbin/init boot script..."
rm -f "$WORKDIR/sbin/init"
cat << 'EOF' > "$WORKDIR/sbin/init"
#!/bin/sh
# Mount essential virtual filesystems
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t debugfs debugfs /sys/kernel/debug
mount -t tmpfs tmpfs /tmp
 
# Synchronize system time to prevent SSL/TLS certificate errors
if [ -f /etc/build_time ]; then
    date -s "$(cat /etc/build_time)"
fi
 
# Load Virtio-fs kernel module (FUSE is built-in)
insmod /lib/modules/virtiofs.ko
 
# Load VSOCK protocol and transport modules
insmod /lib/modules/vsock.ko
insmod /lib/modules/vmw_vsock_virtio_transport_common.ko
insmod /lib/modules/vmw_vsock_virtio_transport.ko
 
# Setup network interface dynamically to allow Node/npm package downloads
for dev in /sys/class/net/*; do
    if [ "$dev" != "/sys/class/net/lo" ] && [ -d "$dev" ]; then
        iface=$(basename "$dev")
        ip link set "$iface" up
        udhcpc -i "$iface"
    fi
done
mkdir -p /etc
echo "nameserver 8.8.8.8" > /etc/resolv.conf
 
# Mount the macOS shared workspace folder using the tag from vm_controller.mm
mkdir -p /workspace
mount -t virtiofs aarchgate_share /workspace
 
# Install Node and npm inside the VM environment
echo "[Guest Init] Installing Node.js and npm..."
echo "http://dl-cdn.alpinelinux.org/alpine/v3.20/main" > /etc/apk/repositories
echo "http://dl-cdn.alpinelinux.org/alpine/v3.20/community" >> /etc/apk/repositories
apk update
apk add nodejs npm libstdc++ libbpf elfutils-libelf
 
# Start the AarchGate guest telemetry agent in the background
/usr/bin/aarchgate_agent &

# Execute automated test script if present in workspace
if [ -f /workspace/run.sh ]; then
    echo "[Guest Init] Executing sandboxed script /workspace/run.sh..."
    chmod +x /workspace/run.sh
    /workspace/run.sh
fi

# Keep the virtual machine open and running
exec /bin/sh
EOF

chmod +x "$WORKDIR/sbin/init"

# 7. Package as initrd
echo "[Builder] Packaging initrd.img to /Users/suprathps/initrd.img..."
cd "$WORKDIR"
find . | cpio -o -H newc | gzip -9 > /Users/suprathps/initrd.img

# 8. Cleanup
rm -rf "$WORKDIR"
echo "========================================================"
echo "✓ SUCCESS: Kernel decompressed and initrd.img packaged!"
echo "========================================================"
