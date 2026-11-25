#!/bin/sh
# Headless test run: captures frames as PNG and copies them where Windows tools can see them.
#   tools/run_shots.sh <seconds> <every> <first> [keys]     e.g. 120 60 240 '300:1c:down,306:1c:up'
# Uses the game copy in $GAME (default /tmp/game) and the build in $BUILD (default /tmp/rbuild).
GAME=${GAME:-/tmp/game}
BUILD=${BUILD:-/tmp/rbuild}
OUT=${OUT:-/tmp/shots}
WINOUT=${WINOUT:-/mnt/c/Users/Autumn/AppData/Local/Temp/shots}
rm -rf "$OUT" && mkdir -p "$OUT"
cd /tmp && RECOMP_SKIP_MOVIES=${SKIP:-} RECOMP_KEYS="$4" RECOMP_SHOTS="$OUT:$2:$3" timeout "$1" "$BUILD/daybreak-recomp" "$GAME" > /tmp/shots.log 2>&1
grep -a 'wrote\|fatal\|movie\|trap' /tmp/shots.log | tail -${TAIL:-12}
rm -rf "$WINOUT" && mkdir -p "$WINOUT" && cp "$OUT"/*.png "$WINOUT"/ 2>/dev/null
ls "$OUT" | wc -l
