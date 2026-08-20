#!/usr/bin/env bash
# Stop EmulationStation so a benchmark can own the display.
#
# Two traps: getty respawns the frontend, so the unit has to be stopped rather
# than the process killed; and "pkill -f emulationstation" matches any shell
# whose arguments mention it, including the ssh command asking for the kill.
sudo systemctl stop getty@tty1 2>/dev/null
pkill -9 -x emulationstation 2>/dev/null
pkill -9 -x emulationstatio 2>/dev/null
sleep 3
if pgrep -x emulationstatio >/dev/null 2>&1; then
    echo "ES STILL UP"
else
    echo "ES stopped"
fi
