#!/bin/bash
# (c) 2026 Suprath PS. All rights reserved.
# AarchGate: Clean stale Iceoryx IPC state before starting fresh

echo "[AarchGate] Stopping all running AarchGate processes..."
pkill -f aarchgate_monitor 2>/dev/null
pkill -f aarchgate_daemon 2>/dev/null
pkill -f iox-roudi 2>/dev/null
sleep 1

echo "[AarchGate] Removing stale Iceoryx UNIX sockets and lock files..."
rm -f /tmp/iox1_0_i_roudi
rm -f /tmp/iox1_0_i_roudi.lock
rm -f /tmp/iox1_0_i_unique_roudi.lock
rm -f /tmp/iox1_0_u_AARCHGATE_MONITOR
rm -f /tmp/iox1_0_u_AARCHGATE_MONITOR.lock
rm -f /tmp/iox1_0_u_AARCHGATE_DAEMON
rm -f /tmp/iox1_0_u_AARCHGATE_DAEMON.lock

echo "[AarchGate] ✓ Clean. Ready to start fresh."
echo ""
echo "Now run in order:"
echo "  Terminal 1: /Users/suprathps/code/ag-npm/build/iox-roudi"
echo "  Terminal 2: /Users/suprathps/code/ag-npm/build/aarchgate_monitor"
echo "  Terminal 3: /Users/suprathps/code/ag-npm/build/aarchgate_daemon --kernel ~/vmlinuz.img --initrd ~/initrd.img --share ~/code/my-sandbox-test"
