#!/bin/zsh
# Remove upsfilter and hand the UPS back to macOS. Run with: sudo ./uninstall.sh
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
launchctl bootout system/local.upsfilter 2>/dev/null
rm -f /Library/LaunchDaemons/local.upsfilter.plist /usr/local/bin/upsfilter
echo "removed — unplug/replug the UPS USB cable so macOS re-reads it"
