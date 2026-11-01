#!/bin/bash
# Build the test kit tools/tm_tests.py runs on, in a fresh container.
#
#   tools/make_test_kit.sh KIT BIN ROM [OWNER_STATES_DIR]
#
# KIT              folder to fill (outside the repo, e.g. /tmp/claude-0/kit)
# BIN              a headless build (build_headless/src/game/troublemakers);
#                  copy it first if you will rebuild while this runs
# ROM              the US 1.1 ROM (input/troublemakers.us1.z64)
# OWNER_STATES_DIR optional: the owner's save states, already renamed to the
#                  names the tests use (split-mountains.tmstate, ...; the list
#                  is in docs/HANDOVER-netcode-v2.md, "Test kit"). Copied in.
#
# Makes KIT/home (the test settings: co-op, split screen, the owner's cheats,
# a display.cfg a headless run starts with) and KIT/states/stage-<label>.tmstate,
# each stage at its start from the debug warp (the split-screen sweep, tag
# sweep). Existing stage states are kept, so a rerun only makes what is
# missing. STAGES="1-1 5-3" limits the stages made (a few seconds each).
#
# Nothing here goes into the repo: no ROM, no save states (CLAUDE.md).
set -u
[ $# -ge 3 ] || { sed -n '2,20p' "$0"; exit 2; }
KIT=$1; BIN=$(readlink -f "$2"); ROM=$(readlink -f "$3"); OWNER=${4:-}
CFG=$KIT/home/.config/troublemakers-recomp
mkdir -p "$CFG" "$KIT/states" "$KIT/work"

cat > "$CFG/cheats.cfg" <<'EOF'
endless_jet=0
moon_jump=1
no_clip=0
gem_magnet=1
grab_blocks=1
mortal_npcs=1
charged_throw=1
cat_ride=1
fast_forward_speed=3
rank_timer=0
gem_hints=1
gem_compass=0
rumble=2
enemy_bars=1
damage_numbers=1
hold_dash=1
coop=1
split_screen=1
throw_objects=100
throw_players=100
EOF
# (fps=0 / vsync=1 from a real install kept a headless run from starting.)
cat > "$CFG/display.cfg" <<EOF
window_w=1280
window_h=720
fullscreen=0
widescreen=1
ssaa=1
msaa=0
fps=0
vsync=0
debug_menu=1
rom=$ROM
EOF
cp "$ROM" "$CFG/troublemakers.n64.us.1.z64"

if [ -n "$OWNER" ]; then
    for f in "$OWNER"/*.tmstate; do
        [ -e "$f" ] || continue
        [ -e "$KIT/states/$(basename "$f")" ] && continue
        cp "$f" "$KIT/states/" && echo "owner state $(basename "$f")"
    done
fi

ALL="1-1 1-2 1-3 1-4 1-5 1-6 1-7 1-8 1-9 1-10 2-1 2-2 2-3 2-4 2-5 2-6 2-7 2-8 2-9 2-10 2-11
     3-1 3-2 3-3 3-4 3-5 3-6 3-7 3-8 3-9 3-10 3-11 3-12 4-1 4-2 4-3 4-4 4-5 4-6 4-7 4-8 4-9
     4-10 4-11 5-1 5-2 5-3 5-4 5-5 5-6 5-7 5-8"
for L in ${STAGES:-$ALL}; do
    OUT=$KIT/states/stage-$L.tmstate
    [ -f "$OUT" ] && { echo "stage $L kept"; continue; }
    # A home of its own per stage (slot 1), from the kit's settings.
    H=$KIT/work/home-$L
    rm -rf "$H"; cp -r "$KIT/home" "$H"
    env MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME="$H" \
        MM_TEST_WARP="$L" MM_TEST_AUTO_ADVANCE=1 MM_TEST_SAVESTATE_SLOT=1 MM_TEST_SAVESTATE=save@ready \
        MM_REPLAY_SPEED=4 MM_TEST_EXIT_AFTER_SAVE=1 \
        timeout -k 5 150 "$BIN" "$ROM" > "$KIT/work/make-$L.log" 2>&1
    S=$H/.config/troublemakers-recomp/savestates/slot1.tmstate
    if [ -f "$S" ]; then cp "$S" "$OUT"; echo "stage $L saved"; else echo "stage $L FAILED (log: $KIT/work/make-$L.log)"; fi
    rm -rf "$H"
done
