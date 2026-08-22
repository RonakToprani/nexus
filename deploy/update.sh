#!/usr/bin/env bash
# Pull the latest nexus code; restart the portal only if something changed.
# Run it from cron for automatic sync with GitHub pushes:
#   */2 * * * * /opt/nexus/deploy/update.sh >/dev/null 2>&1
set -euo pipefail
cd "$(dirname "$0")/.."

OLD=$(git rev-parse HEAD)
git pull --ff-only --quiet
NEW=$(git rev-parse HEAD)

if [ "$OLD" != "$NEW" ]; then
    echo "updated $OLD -> $NEW, restarting portal"
    if command -v systemctl >/dev/null 2>&1; then
        systemctl restart nexus-portal 2>/dev/null \
            || sudo systemctl restart nexus-portal
    else  # macOS
        launchctl kickstart -k "gui/$(id -u)/com.nexus.portal"
    fi
fi
