#!/bin/bash
# (c) 2026 Suprath PS. All rights reserved.
# AarchGate: guest initrd.img Builder Script

set -e

echo "========================================================"
echo "         AarchGate Guest Initrd Builder                 "
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

# 2. Extract rootfs
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

# 4. Write custom init script
echo "[Builder] Injecting custom sbin/init boot script..."
rm -f "$WORKDIR/sbin/init"
cat << 'EOF' > "$WORKDIR/sbin/init"
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t debugfs debugfs /sys/kernel/debug
mount -t tmpfs tmpfs /tmp

# Mount the macOS shared workspace folder using the tag from vm_controller.mm
mkdir -p /workspace
mount -t virtiofs aarchgate_share /workspace

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

# 5. Package as initrd
echo "[Builder] Packaging initrd.img to /Users/suprathps/initrd.img..."
cd "$WORKDIR"
find . | cpio -o -H newc | gzip -9 > /Users/suprathps/initrd.img

# 6. Cleanup
rm -rf "$WORKDIR"
echo "========================================================"
echo "✓ SUCCESS: initrd.img created at /Users/suprathps/initrd.img"
echo "========================================================"
