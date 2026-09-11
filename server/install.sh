#!/bin/bash
# Install the RationAI server as a launchd agent on this Mac.
#
# Generates the plist from wherever the repo actually sits, so the same repo
# works on a second machine with a different username and path. Run it once
# per laptop:
#
#     bash server/install.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="$(command -v python3 || true)"
LABEL="com.rationai.server"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

if [ -z "$PY" ]; then
  echo "python3 not found on PATH" >&2
  exit 1
fi

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$PY</string>
    <string>$HERE/usage_server.py</string>
  </array>
  <key>WorkingDirectory</key><string>$HERE</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>30</integer>
  <key>StandardOutPath</key><string>/tmp/rationai-server.log</string>
  <key>StandardErrorPath</key><string>/tmp/rationai-server.log</string>
</dict>
</plist>
PLIST_EOF

# Retire the pre-rename agent if it is still loaded.
launchctl unload "$HOME/Library/LaunchAgents/com.ration.server.plist" 2>/dev/null || true
launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"

echo "installed  $PLIST"
echo "python     $PY"
echo "server     $HERE/usage_server.py"
echo
printf 'waiting for the server to answer'
for _ in $(seq 1 15); do
  if curl -sf --max-time 2 http://127.0.0.1:8787/usage >/dev/null 2>&1; then
    echo " — up"
    echo
    echo "It advertises itself on the network as _rationai._tcp, so the display"
    echo "finds it with nothing configured. Check with:"
    echo "    dns-sd -B _rationai._tcp"
    exit 0
  fi
  printf '.'
  sleep 1
done

echo " — no answer"
echo "Check the log: tail /tmp/rationai-server.log" >&2
exit 1
