#!/usr/bin/env bash
# Install the NEXUS portal as a macOS launchd service: starts at login,
# restarts if it crashes, and pulls from GitHub every 2 minutes.
# Usage:  bash deploy/install-macos.sh
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PY="$(command -v python3)"
UID_N="$(id -u)"
AGENTS="$HOME/Library/LaunchAgents"
mkdir -p "$AGENTS"

case "$REPO" in
    "$HOME/Documents"*|"$HOME/Desktop"*|"$HOME/Downloads"*)
        echo "ERROR: macOS privacy protection blocks background services from $REPO."
        echo "Clone the repo somewhere neutral instead, e.g.:"
        echo "  git clone https://github.com/RonakToprani/nexus ~/nexus && bash ~/nexus/deploy/install-macos.sh"
        exit 1;;
esac

if [ ! -w "$REPO/portal" ]; then
    echo "ERROR: $REPO is not writable by $(whoami) (cloned with sudo?)."
    echo "Fix with:  sudo chown -R $(whoami) $REPO   then re-run this script."
    exit 1
fi

cat > "$AGENTS/com.nexus.portal.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.nexus.portal</string>
  <key>ProgramArguments</key><array>
    <string>$PY</string>
    <string>$REPO/portal/server.py</string>
    <string>--port</string><string>8484</string>
  </array>
  <key>WorkingDirectory</key><string>$REPO/portal</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$REPO/portal/server.log</string>
  <key>StandardErrorPath</key><string>$REPO/portal/server.log</string>
</dict></plist>
EOF

cat > "$AGENTS/com.nexus.update.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.nexus.update</string>
  <key>ProgramArguments</key><array>
    <string>/bin/bash</string>
    <string>$REPO/deploy/update.sh</string>
  </array>
  <key>StartInterval</key><integer>120</integer>
</dict></plist>
EOF

launchctl bootout "gui/$UID_N/com.nexus.portal" 2>/dev/null || true
launchctl bootout "gui/$UID_N/com.nexus.update" 2>/dev/null || true
launchctl bootstrap "gui/$UID_N" "$AGENTS/com.nexus.portal.plist"
launchctl bootstrap "gui/$UID_N" "$AGENTS/com.nexus.update.plist"

IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '<this-mac-ip>')"
echo "Installed. Portal: http://$IP:8484  (auto-starts at login, auto-syncs from GitHub)"
echo "Keep this Mac awake:  sudo pmset -a sleep 0"
