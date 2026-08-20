#!/usr/bin/env bash
# Switch a pack between tiers. Verify the target exists BEFORE unlinking the
# current one - the reverse order cost a 9.5 GB pack earlier today.
#   tier-switch.sh "<PACK NAME>" 4k|hd
set -uo pipefail
C=/home/pi/RetroPie/BIOS/Mupen64plus/cache
NAME="$1"; TIER="$2"
HTS="$C/${NAME}_HIRESTEXTURES.hts"
SRC="$C/${NAME}_HIRESTEXTURES.hts.${TIER}"

[ -e "$SRC" ] || { echo "  no .${TIER} tier for $NAME - refusing"; exit 1; }
SZ=$(stat -c%s "$SRC")
[ "$SZ" -gt 1000000 ] || { echo "  .${TIER} tier is only $SZ bytes - refusing"; exit 1; }

# Keep the current data reachable under some other name before unlinking.
if [ -e "$HTS" ]; then
    CUR_LINKS=$(stat -c%h "$HTS")
    if [ "$CUR_LINKS" -lt 2 ]; then
        echo "  current .hts has no other link; preserving it as .hts.prev"
        sudo chmod u+w "$C"
        ln "$HTS" "$C/${NAME}_HIRESTEXTURES.hts.prev" 2>/dev/null || true
    fi
fi

sudo chmod u+w "$C"
rm -f "$HTS"
ln "$SRC" "$HTS"
sudo chmod 555 "$C"
printf "  %s -> %s tier, %s bytes\n" "$NAME" "$TIER" "$(stat -c%s "$HTS")"
