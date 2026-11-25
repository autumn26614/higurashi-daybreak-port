#!/bin/sh
# Prints a RECOMP_KEYS script that goes from the title (with RECOMP_SKIP_MOVIES) through story
# mode's team select into the first battle, for the base game and Kai alike:
#   tools/run_shots.sh 250 600 3000 "$(tools/battle_keys.sh)"
# Z (2c) confirms; team select takes Z and Down (d0) in turn; then Z keeps the fight going.
# Kai reads a press only if it is held for more than a few frames, hence 12.
hold=${HOLD:-12}
keys="700:2c:down,$((700 + hold)):2c:up"
for f in $(seq 1000 100 2000); do keys="$keys,$f:2c:down,$((f + hold)):2c:up"; done
for f in $(seq 2100 100 4200); do
    if [ $(((f / 100) % 2)) = 1 ]; then k=d0; else k=2c; fi
    keys="$keys,$f:$k:down,$((f + hold)):$k:up"
done
for f in $(seq 4300 40 9000); do keys="$keys,$f:2c:down,$((f + hold)):2c:up"; done
echo "$keys"
