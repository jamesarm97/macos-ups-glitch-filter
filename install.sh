#!/bin/zsh
# Build and install upsfilter as a LaunchDaemon. Run with: sudo ./install.sh
set -e
cd "${0:A:h}"
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

clang -O2 -Wall -o upsfilter upsfilter.c -framework IOKit -framework CoreFoundation
mkdir -p /usr/local/bin
install -m 755 -o root -g wheel upsfilter /usr/local/bin/upsfilter
install -m 644 -o root -g wheel local.upsfilter.plist /Library/LaunchDaemons/local.upsfilter.plist

launchctl bootout system/local.upsfilter 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/local.upsfilter.plist
sleep 2
echo "--- /var/log/upsfilter.log ---"
tail -n 20 /var/log/upsfilter.log
