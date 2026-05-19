#!/bin/bash
# Install the CodexMeter host daemon as a macOS LaunchAgent.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LABEL="com.codexmeter.sync"
PLIST_DIR="$HOME/Library/LaunchAgents"
PLIST_PATH="$PLIST_DIR/$LABEL.plist"
PYTHON_BIN="${PYTHON_BIN:-$(command -v python3)}"
LOG_DIR="$HOME/Library/Logs/CodexMeter"

if [ -z "$PYTHON_BIN" ]; then
    echo "Error: python3 is required but was not found"
    exit 1
fi

echo "=== CodexMeter - Install macOS LaunchAgent ==="
echo ""
echo "Project: $SCRIPT_DIR"
echo "Python:  $PYTHON_BIN"
echo "Label:   $LABEL"
echo ""

mkdir -p "$PLIST_DIR" "$LOG_DIR"

cat > "$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$LABEL</string>

  <key>ProgramArguments</key>
  <array>
    <string>$PYTHON_BIN</string>
    <string>$SCRIPT_DIR/daemon/codexmeter_daemon.py</string>
    <string>--watch</string>
    <string>--ble</string>
    <string>--sync-pet-sprite</string>
    <string>--cwd</string>
    <string>$SCRIPT_DIR</string>
    <string>--auto-serial-port</string>
    <string>/dev/cu.usbmodem*</string>
    <string>--control-port</string>
    <string>3490</string>
  </array>

  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>

  <key>StandardOutPath</key>
  <string>$LOG_DIR/sync.out.log</string>
  <key>StandardErrorPath</key>
  <string>$LOG_DIR/sync.err.log</string>
</dict>
</plist>
EOF

launchctl bootout "gui/$UID" "$PLIST_PATH" >/dev/null 2>&1 || true
launchctl bootstrap "gui/$UID" "$PLIST_PATH"
launchctl enable "gui/$UID/$LABEL"
launchctl kickstart -k "gui/$UID/$LABEL"

echo ""
echo "=== Done ==="
echo "Status:"
echo "  launchctl print gui/$UID/$LABEL"
echo ""
echo "Logs:"
echo "  tail -f $LOG_DIR/sync.out.log"
echo "  tail -f $LOG_DIR/sync.err.log"
echo ""
echo "Control endpoint:"
echo "  http://127.0.0.1:3490/api/status"
