#!/usr/bin/env bash
# Rebuild xemu on the Pi without taking the board down with it.
#
# The 2 GB board has been wedged twice by memory pressure: once by two
# concurrent -j4 builds, and once by the LTO link alone. GCC runs LTO in
# parallel by default - two lto1 processes were observed, each close to a
# gigabyte - which on this board means swap thrash the kernel does not
# recover from. The machine stopped answering ping and needed a power cycle.
#
# Guards, in the order they matter:
#   * -flto=1 so the link stage is single-threaded. Slower, same output.
#   * a free-memory check before starting, so a loaded board is not pushed
#     over by a build.
#   * flock, so a scheduled loop cannot start a second build.
#   * any stale remote build is killed first.
#   * -j2, because QEMU sources peak near 1 GB per cc1plus.
set -uo pipefail
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR -o ConnectTimeout=20 retropie"
JOBS=2
MIN_FREE_MB=900
BUILD_CFLAGS="${XEMU_BUILD_CFLAGS:--O3 -flto=1 -mcpu=cortex-a76}"
BUILD_LDFLAGS="${XEMU_BUILD_LDFLAGS:--flto=1}"

exec 9>/tmp/xemu-rebuild.lock
flock -n 9 || { echo "[$(date +%T)] another rebuild holds the lock - skipping"; exit 0; }

$SSH 'echo alive' >/dev/null 2>&1 || {
    echo "[$(date +%T)] pi unreachable - not starting a build"; exit 1; }

FREE=$($SSH "free -m | awk '/^Mem:/{print \$7}'" 2>/dev/null || echo 0)
if [ "${FREE:-0}" -lt "$MIN_FREE_MB" ]; then
    echo "[$(date +%T)] only ${FREE}MB available, need ${MIN_FREE_MB}MB - refusing"
    echo "  stop whatever is running on the board first"
    exit 1
fi
echo "[$(date +%T)] ${FREE}MB available"

echo "[$(date +%T)] killing any stale remote build"
$SSH 'pkill -f "[b]uild.sh" 2>/dev/null; pkill -x ninja 2>/dev/null; pkill -x cc1plus 2>/dev/null; pkill -x lto1 2>/dev/null; sleep 2; true'

echo "[$(date +%T)] pulling branch"
BRANCH="${XEMU_BRANCH:-vulkan-ui-without-gl4}"
# The Pi is a single-branch clone, so name the refspec explicitly or a new
# branch never gets a remote-tracking ref.
$SSH "cd ~/xemu-build && git fetch -q origin +refs/heads/$BRANCH:refs/remotes/origin/$BRANCH && git reset -q --hard origin/$BRANCH && git log --oneline -1" || exit 1

echo "[$(date +%T)] building -j${JOBS} with CFLAGS=${BUILD_CFLAGS}"
$SSH "cd ~/xemu-build && CFLAGS='$BUILD_CFLAGS' LDFLAGS='$BUILD_LDFLAGS' ./build.sh -j${JOBS} --enable-lto > /tmp/xbuild.log 2>&1; echo rc=\$?; tail -6 /tmp/xbuild.log"
$SSH 'free -m | sed -n 2,3p'
# Only six lines of the build log come back above, so compile errors would go
# unseen. Surface them explicitly, and confirm the binary really was relinked.
$SSH 'grep -E "error:|FAILED" /tmp/xbuild.log | head -20'
$SSH 'cd ~/xemu-build && find dist/xemu -newermt "-30 minutes" >/dev/null 2>&1 && echo BUILD_FRESH || echo BUILD_STALE'
