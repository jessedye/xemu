#!/usr/bin/env bash
sudo systemctl start getty@tty1
sleep 6
if pgrep -x emulationstatio >/dev/null 2>&1; then
    echo "EmulationStation running"
else
    echo "EmulationStation did not come up"
fi
