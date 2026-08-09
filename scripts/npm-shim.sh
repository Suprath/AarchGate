#!/bin/bash
# /usr/local/bin/npm  (or ~/.local/bin/npm — put before real npm in PATH)
# AarchGate transparent npm shim
# Intercepts `npm install` and routes through the sandbox automatically.
# All other npm commands (run, test, publish, etc.) pass through unchanged.

REAL_NPM=$(which -a npm | grep -v "$0" | head -1)
DAEMON=/usr/local/bin/aarchgate_daemon
KERNEL=~/.aarchgate/vmlinuz.img
INITRD=~/.aarchgate/initrd.img

# Only sandbox install/ci commands — pass everything else straight through
case "$1" in
  install|i|ci|add)
    # Check daemon background services are healthy
    if ! pgrep -q iox-roudi; then
      echo "[AarchGate] Starting sandbox services..." >&2
      /usr/local/bin/iox-roudi > /dev/null 2>&1 &
      sleep 1
    fi

    echo "[AarchGate] 🛡️  Running sandboxed: npm $*" >&2
    echo "[AarchGate]    Monitor: aarchgate_monitor" >&2

    # Run the actual install inside the AarchGate VM sandbox
    "$DAEMON" \
      --kernel "$KERNEL" \
      --initrd "$INITRD" \
      --share  "$(pwd)"

    EXIT=$?
    if [ $EXIT -eq 0 ]; then
      echo "[AarchGate] ✅ Install complete. No policy violations detected." >&2
    else
      echo "[AarchGate] 🚨 BLOCKED: Policy violation detected. node_modules NOT installed." >&2
      exit 1
    fi
    ;;
  *)
    # Passthrough: npm run, npm test, npm publish, etc.
    exec "$REAL_NPM" "$@"
    ;;
esac
