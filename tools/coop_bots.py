#!/usr/bin/env python3
"""Co-op bot tests: two scripted players in the real game, checked every frame.

Runs the headless build with scripted input for both players (replay
recording, so every run is deterministic) from save states, reads the
per-frame player trace (MM_P2_TRACE, src/game/player2.cpp) and checks:

  jumps      neither player's world position moves more than 24 px in a frame
             (except right after player 2 (re)joins)
  screen     both stay within the shared screen (|x| <= 181 px from the camera)
  holding    a player flagged as carrying always carries a live object
  rejoins    player 2 is not pulled back more often than the scenario allows
  present    player 2 is in the game for most of the run

Netplay scenarios run a host and a guest process over localhost and also
require identical memory fingerprints on both sides and no resync.

    tools/coop_bots.py --bin build_headless/src/game/troublemakers \
        --rom input/troublemakers.us1.z64 --home <HOME with savestates> [--only name]

Prints one line per scenario and ALL OK / FAILED at the end.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TRACE = re.compile(
    r"\[p2trace\] (\d+) cam (-?\d+),(-?\d+) p1 (-?\d+),(-?\d+) st (\w+) fl (\w+) hold (\w+) \| "
    r"p2\((-?\d+)\) (-?\d+),(-?\d+) st (\w+) fl (\w+) hold (\w+)")
HELD = re.compile(r"\[p2trace\] held_ok (\d) (\d) hp (-?\d+) (-?\d+)")
FRAME_OF = re.compile(r"\(frame (\d+)\)\s*$")


def fuzz(seed, steps=300, grab_bias=False):
    rnd = random.Random(seed)
    keys = ["", "l", "r", "A", "B", "lA", "rA", "lB", "rB", "u", "d", "uB", "dB", "ll", "rr"]
    if grab_bias:
        keys += ["B", "B", "lB", "rB"]
    return ",".join("%d:%s" % (rnd.randint(3, 25), rnd.choice(keys)) for _ in range(steps))


# name, save slot, frames, player 1 script, player 2 script, allowed leash rejoins
SCENARIOS = [
    ("apart-1", 1, 700, "60:,600:r", "60:,600:l", 0),
    ("apart-4", 4, 700, "60:,600:l", "60:,600:r", 0),
    ("apart-6", 6, 700, "60:,600:r", "60:,600:l", 0),
    ("apart-9", 9, 700, "60:,600:r", "60:,600:l", 0),
    ("p1-carries-p2", 1, 500, "80:,6:l,10:B,20:,60:r,30:,60:l,20:,10:B,200:", "500:", 0),
    ("p2-carries-p1", 1, 500, "500:", "80:,6:r,10:B,20:,60:l,30:,60:r,20:,10:B,200:", 0),
    # Player 2 carries player 1 to the right across the stage: at Marina's
    # carry speed (2 px a frame), also once the camera moves (it follows the
    # player being carried; she walked at half speed).
    ("p2-carries-p1-walk", 1, 520, "520:", "80:,6:r,10:B,30:,400:r", 0,
     dict(final=lambda s0, s2: s2[0] > 650)),
    ("both-grab", 1, 400, "80:,6:l,10:B,100:,10:B,200:", "80:,6:r,10:B,100:,10:B,200:", 0),
    ("carry-jump-throw", 4, 500, "80:,6:r,10:B,20:,10:A,40:,30:r,10:B,300:", "500:", 0),
    ("p2-carry-jump", 4, 500, "500:", "80:,6:l,10:B,20:,10:A,40:,30:l,10:B,300:", 0),
    # Player 2 throws player 1 with the charged throw (on in the test home's
    # cheats.cfg): her aim used to be wiped (it shared a word with what she
    # shook) and she put player 1 down instead.
    ("p2-throws-p1", 1, 600, "600:", "80:,6:r,10:B,20:,60:l,30:,60:r,20:,10:lB,390:", 0,
     dict(expect=["(a player), velocity"])),
    # A health bar each (player2_health.cpp): player 2 at 0 health goes down
    # and comes back next to player 1 three seconds later, with no defeat.
    ("p2-down", 1, 600, "60:,300:r,240:", "60:,300:l,240:", 1,
     dict(env={"MM_TEST_SETHP": "p2:0@150"}, expect=["[player2] down", "[player2] back next to the other player"])),
    # Player 1 at 0 health while player 2 is up: the two swap (player 2 leads)
    # and player 1's person comes back; the stage goes on.
    ("p1-down", 1, 600, "60:,300:r,240:", "60:,300:l,240:", 1,
     dict(env={"MM_TEST_SETHP": "0:0@150"}, expect=["slot 0's character is down", "[player2] back next to the other player"])),
]
# "Ride the cat" (player2_mount.cpp, foreign_actors.cpp) in stages without
# a cat: both players get one and ride it around (random play), in 1-1's
# desert (1), the other test stages and 3-12 itself (9, the stage's own).
CAT = {"MM_TEST_CHEATS": "cat_ride=1"}
for slot in (1, 2, 3, 4, 5, 6, 7, 8, 9):
    SCENARIOS.append(("cat-%d" % slot, slot, 1800, fuzz(1100 + slot), fuzz(1150 + slot), 3,
                      dict(env=CAT, expect=["[mount] player 1's own cat"] if slot != 9 else [])))
SCENARIOS += [
    # Turned off mid-ride: both riders drop off (no rider left in the ride
    # state, no loop scroll carrying them) and walk on.
    ("cat-off", 1, 900, "200:r,200:,300:l,200:", "900:", 0,
     dict(env=dict(CAT, MM_TEST_TOGGLE="7@400"), expect=["[cheats] Ride the cat off"])),
    # Turned on mid-stage: both get a cat at their feet and sit on it where
    # they stand (player 2 was put next to player 1 for a few frames online).
    ("cat-on", 1, 700, "60:,100:r,200:,200:l,140:", "60:,100:l,200:,200:r,140:", 0,
     dict(env={"MM_TEST_TOGGLE": "7@100"}, expect=["[cheats] Ride the cat on", "[mount] player 2's own cat"])),
    # The same in random play (net-cat's scripts: on while player 2 lands
    # from player 1's throw).
    ("cat-on-fuzz", 1, 1500, fuzz(905), fuzz(906), 3,
     dict(env={"MM_TEST_TOGGLE": "7@100"}, expect=["[mount] player 2's own cat"])),
    # Lunar's missile spawned ahead (as the Spawn page puts it) flies back
    # past player 1; her cat's jump lands on it and she steers it.
    ("cat-missile", 1, 700, "128:,25:A,60:,80:r,80:l,350:", "700:", 0,
     dict(env=dict(CAT, MM_TEST_SPAWN="F09@80@200@8"), expect=["[mount] player 1's cat lands on missile"])),
    # The owner's preview-16 states (his slots 0-3, copied to the test
    # home's 50-53; skipped where the home lacks them). 52: player 1 rides
    # her cheat cat left into 1-1's block house (an 18 px floor step, a
    # 50 px door); it stopped dead at the step. 53: a jump left onto the
    # house's roof; it stopped dead at the face and fell back.
    ("cat-house-door", 52, 400, "400:l", "400:", 0,
     dict(env=CAT, final=lambda s0, s2: s0[0] < -200)),
    ("cat-house-roof", 53, 300, "20:,12:lA,268:l", "300:", 0,
     dict(env=CAT, final=lambda s0, s2: s0[0] < -150)),
    # The mount button (Z, player2_mount.cpp): a missile spawned ahead flies
    # back past the riders; Z with it in reach puts the cat onto it (its own
    # landing then), the rider steers right, Z again gets off.
    ("cat-missile-board", 1, 420, "142:,4:Z,60:r,54:,4:Z,156:", "420:", 0,
     dict(env=dict(CAT, MM_TEST_SPAWN="F09@80@200@8"),
          expect=["player 1's cat boards missile", "player 1's cat lands on missile", "player 1's cat gets off missile"])),
    ("p2-cat-missile-board", 1, 420, "420:", "142:,4:Z,60:r,54:,4:Z,156:", 0,
     dict(env=dict(CAT, MM_TEST_SPAWN="F09@80@200@8"),
          expect=["player 2's cat boards missile", "her cat lands on missile", "player 2's cat gets off missile"])),
    # 3-11's Cat Clancer spawned anywhere is a pet (pet_clancer.cpp): he
    # falls to the ground, follows the player and stops near her, can be
    # picked up and thrown, and comes back; a spawned block in his way he
    # jumps onto.
    # (Player 2 is in this state too, and he follows whoever is nearest:
    # she walks off to the left edge first.)
    ("pet-clancer", 1, 700, "60:,90:r,80:,12:l,100:,6:B,60:,6:rB,286:", "20:,120:l,560:", 1,
     dict(env={"MM_TEST_SPAWN": "2801@20@80@0"},
          expect=["[pet] Cat Clancer lands", "[pet] Cat Clancer caught up", "[pet] Cat Clancer is picked up",
                  "[pet] Cat Clancer is thrown", "[pet] Cat Clancer lands after a throw"])),
    ("pet-clancer-wall", 1, 400, "400:", "400:", 0,
     dict(env={"MM_TEST_SPAWN": "7F01@20@40@0;2801@22@150@0"},
          expect=["[pet] Cat Clancer jumps at a wall", "[pet] Cat Clancer caught up"])),
    # The owner's states (test-home slots 62 and 64, see the handover): the
    # pet at the foot of 1-1's step pyramid with the player on top (he
    # climbs it in sized jumps), and Marina holding a block next to 1-1's
    # campfire (stepping in, she drops it; the fire's hit knocks it out,
    # and it can be picked up again).
    ("pet-pyramid", 62, 400, "400:", "400:", 0,
     dict(expect=["[pet] Cat Clancer jumps at a wall", "[pet] Cat Clancer caught up"])),
    ("block-fire", 64, 200, "10:,20:r,60:,6:B,104:", "200:", 0,
     dict(expect=["[blocks] block 121 knocked out of a hit"])),
    # Z in the owner's states (test-home slots 60: inside 1-1's block house,
    # 61: next to the pet): the missile is called and boarded, rides into
    # the roof and the floor without passing through or blowing up.
    ("cat-missile-house-up", 60, 450, "30:,4:Z,20:,396:u", "450:", 0,
     dict(env=CAT, expect=["player 1's cat lands on missile"], absent=["ridden missile"])),
    ("cat-missile-house-down", 60, 450, "30:,4:Z,20:,396:d", "450:", 0,
     dict(env=CAT, expect=["player 1's cat lands on missile"], absent=["ridden missile"])),
    ("cat-missile-61", 61, 300, "40:,4:Z,256:", "300:", 0,
     dict(env=CAT, expect=["player 1's cat lands on missile"], absent=["ridden missile"])),
    # On the cat next to 1-1's star teleporter (owner's state, slot 65): B
    # gets her off the cat, she grabs the star and a shake flies her.
    ("cat-star", 65, 400, "30:,6:B,10:," + ",".join(["5:u", "5:d"] * 20) + ",154:", "400:", 0,
     dict(env=CAT, expect=["player 1 gets off the cat for a star"])),
    # Z on the cat with no missile about: it calls one of its own.
    ("cat-missile-call", 1, 420, "60:,4:Z,80:,60:r,40:,4:Z,172:", "420:", 0,
     dict(env=CAT, expect=["player 1's cat calls missile", "player 1's cat boards missile", "player 1's cat lands on missile", "player 1's cat gets off missile"])),
    # 3-11's Cat Clancer spawned anywhere builds his parts in a free run
    # (he wrote over player 2's slot).
    ("clancer", 1, 900, fuzz(1201), fuzz(1202), 0,
     dict(env={"MM_TEST_SPAWN": "2801@120@80@0"}, expect=["spawned 2801"], max_joins=1)),
]

# Slot 9: stage 3-12 (CERBERUS), where the owner saw trouble: the looping,
# auto-scrolling arena (check_trace loop=True).
LOOP_SLOT = 9
for slot in (1, 2, 3, 4, 5, 6, 7, 8, 9):
    for seed in (1, 2):
        SCENARIOS.append(("fuzz-%d-%d" % (slot, seed), slot, 2400,
                          fuzz(seed * 100 + slot, grab_bias=True), fuzz(seed * 100 + slot + 50, grab_bias=True), 3))

# Boss and special stages (save slots 10-26, made from warps with
# --make-boss-states): random play for both players, on the shared screen
# and (FAR, below) far apart, where player 2's picture must not change the
# game. Bosses bring their own actors, cameras and mounts (3-11's cat).
BOSSES = [(10, "1-6"), (11, "1-8"), (12, "2-6"), (13, "2-11"), (14, "3-8"), (15, "3-11"), (16, "4-2"),
          (17, "4-6"), (18, "4-9"), (19, "4-11"), (20, "5-3"), (21, "5-4"), (22, "5-6"), (23, "5-7"),
          (24, "5-8"), (25, "3-3")]
# Leash rejoins are how player 2 is brought back when random play leaves
# her behind: 5-6 is a free fall where player 1 holds on to the boss and a
# player 2 who does not falls away (every few seconds); 5-3's arena drops
# her out of view now and then.
BOSS_REJOINS = {"5-3": 5, "5-6": 12}
for slot, label in BOSSES:
    SCENARIOS.append(("boss-%s" % label, slot, 1800, fuzz(700 + slot, grab_bias=True), fuzz(800 + slot, grab_bias=True),
                      BOSS_REJOINS.get(label, 3)))

# Stages that play differently (rides, rafts, races, free falls; states made
# like the bosses' with --make-boss-states, slots 32 on): random play, and a
# run to the right by both players.
STAGES = [(32, "2-7"), (33, "2-8"), (34, "2-9"), (35, "3-5"), (36, "3-6"), (37, "4-1"), (38, "4-8"),
          (39, "1-9"), (41, "2-4"), (42, "4-7"), (43, "1-3"), (44, "3-1")]
for slot, label in STAGES:
    SCENARIOS.append(("stage-%s" % label, slot, 1800, fuzz(1300 + slot, grab_bias=True), fuzz(1400 + slot), 3))
    SCENARIOS.append(("stage-%s-run" % label, slot, 1500, "1500:r", "60:,1440:r", 3))

# "Ride the cat" anywhere: random play on cats in every boss and special
# stage (rides, rafts, free falls, arenas), and Cat Clancer spawned in a few
# of them. 3-11 has its own cat machine and 5-3 its bee (each player
# rides the stage's mount there, no cheat cat), and 5-6 is a free fall (a
# cat is made only over ground): no expectation.
OWN_MOUNTS = ("3-11", "5-3", "5-6")
for slot, label in BOSSES + STAGES:
    SCENARIOS.append(("catx-%s" % label, slot, 1500, fuzz(1500 + slot), fuzz(1550 + slot), BOSS_REJOINS.get(label, 3) + 2,
                      dict(env=CAT, expect=["[mount] player 1's own cat"] if label not in OWN_MOUNTS else [])))
for slot, label in ((4, "t4"), (32, "2-7"), (14, "3-8"), (42, "4-7")):
    SCENARIOS.append(("clancer-%s" % label, slot, 900, fuzz(1601 + slot), fuzz(1651 + slot), 3,
                      dict(env={"MM_TEST_SPAWN": "2801@120@80@0"}, expect=["spawned 2801"])))

# Far apart (far_coop.cpp), run locally with MM_FAR=1: once as player 1's
# machine sees it and once building player 2's picture each frame
# (MM_FAR_SHADOW=1), which must not change the game at all (same memory
# fingerprints every frame). Name, slot, frames, scripts, px apart wanted,
# and optionally: extra environment, lines the run must print, and a check
# of where the two characters end (slot 0's and player 2's slot's world x,
# from where they started).
STAR_SHAKE = "60:,8:r,10:uB," + ",".join(["6:Bl,6:Br"] * 25) + ",600:"
FAR = [
    ("far-apart-1", 1, 900, "60:,840:r", "60:,840:l", 600),
    # Player 2 carries player 1 across the stage far-apart style (split
    # screen, online): at Marina's carry speed too (the owner saw her slow).
    ("far-p2-carries-p1-walk", 1, 520, "520:", "80:,6:r,10:B,30:,400:r", 0,
     dict(final=lambda s0, s2: s2[0] > 650)),
    ("far-apart-4", 4, 900, "60:,840:l", "60:,840:r", 0),
    ("far-apart-6", 6, 900, "60:,840:r", "60:,840:l", 0),
    ("far-apart-9", 9, 900, "60:,840:r", "60:,840:l", 0),
    ("far-there-and-back", 1, 1200, "60:,500:r,600:", "60:,300:l,300:r,500:", 400),
    ("far-fuzz-4", 4, 1800, fuzz(404, grab_bias=True), "60:," + fuzz(405), 0),
    ("far-fuzz-2", 2, 1800, fuzz(202), "60:,300:l," + fuzz(203), 0),
    # Both on cats (the cheat) far apart: player 2's cat in her own view.
    ("far-cat", 1, 1200, "60:,500:r,600:", "60:,500:l,600:", 300, dict(env={"MM_TEST_CHEATS": "cat_ride=1"})),
    ("far-cat-fuzz", 1, 1500, fuzz(1501), fuzz(1502), 0, dict(env={"MM_TEST_CHEATS": "cat_ride=1"})),
    ("far-cat-apart-4", 4, 1200, "60:,500:l,600:", "60:,500:r,600:", 0, dict(env={"MM_TEST_CHEATS": "cat_ride=1"})),
    ("far-cat-9", 9, 1200, "60:,500:r,600:", "60:,500:l,600:", 0, dict(env={"MM_TEST_CHEATS": "cat_ride=1"})),
] + [
    ("far-catx-%s" % label, slot, 1200, fuzz(1700 + slot), "60:,240:l," + fuzz(1750 + slot), 0,
     dict(env={"MM_TEST_CHEATS": "cat_ride=1"}))
    for slot, label in ((32, "2-7"), (42, "4-7"), (20, "5-3"), (25, "3-3"))
] + [
    ("far-boss-%s" % label, slot, 1500, fuzz(900 + slot), "60:,240:l," + fuzz(950 + slot), 0)
    for slot, label in BOSSES
] + [
    # The lead swap (player2_lead.cpp). Slot 0 holds 1-1 next to the star
    # teleporter inside the red block pyramid: player 2 shakes it and flies
    # (as the lead), and player 1 joins her where she lands.
    ("lead-star", 0, 1000, "1000:", "200:," + STAR_SHAKE, 0,
     dict(env={"MM_TEST_PLACE": "120:1800,318,2"}, expect=["star teleporter: player 2 leads now", "the other player comes along"],
          final=lambda s0, s2: s0[0] < 1720 and s0[1] > 560 and abs(s2[0] - s0[0]) < 64 and abs(s2[1] - s0[1]) < 32)),
    # Player 2 picks up a stage block (grabbable blocks) and walks with it:
    # no faster than Marina carrying (2 px a frame; she ran up to 10, her
    # carry code counted the camera's step on top of her own scroll).
    ("p2-carry-block", 0, 700, "700:", "60:,140:r,4:,10:B,30:,200:l,10:A,60:,100:l,80:", 0,
     dict(expect=["is now an object"], max_step2=4)),
    # Swapped by hand (MM_TEST_LEAD_SWAP) early on: each person keeps their
    # controller, so slot 0 (player 2 now) walks left and player 1 right.
    ("lead-controls", 1, 900, "60:,840:r", "60:,840:l", 0,
     dict(env={"MM_TEST_LEAD_SWAP": "100"}, expect=["test: player 2 leads now"],
          final=lambda s0, s2: s0[0] < -60 and s2[0] > 200)),
]

NETPLAY = [
    ("net-apart", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r"),
    # Player 2 leads from the middle on: the host's machine builds the second
    # picture from then on, and both stay identical.
    ("net-lead", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r", dict(env={"MM_TEST_LEAD_SWAP": "500"})),
    # The host turns moon jump on and off mid-session; the guest's own toggle
    # is refused. Both machines switch on the same frame and stay identical.
    ("net-cheats", 1, 1200, "60:,300:r,20:A,100:,20:A,700:l", "60:,300:l,20:A,100:,20:A,700:r",
     dict(env={"MM_NET_TOGGLE": "1@400,1@800"},
          same=("settings changed at", 2),
          guest=["is set by the host during online play"])),
    # A slow, jittery connection (each side holds its sends back 60 +-15 ms)
    # in lockstep: the delay grows to cover it and both games stay identical.
    ("net-lag", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r",
     dict(env={"MM_NET_LAG": "60,15,0", "MM_NET_LOCKSTEP": "1"}, host_re=[r"delay now ([5-9]|10) frames"])),
    # Lost packets (resent, holding up the ones behind them): no resyncs.
    ("net-loss", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r", dict(env={"MM_NET_LAG": "20,5,1"})),
    # Rollback: on the slow link the game must not wait and must roll back
    # (frames run again), still with identical fingerprints and no resync.
    ("net-rollback", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r",
     dict(env={"MM_NET_LAG": "60,15,0"}, host_re=[r"rollbacks: [1-9]\d* \(", r"waited over a frame for the partner: [0-3],"])),
    # Every frame runs twice (rollback of one frame each frame): the second
    # run must give the same game.
    ("net-synctest", 1, 900, "60:,400:r,440:l", "60:,400:l,440:r",
     dict(env={"MM_NET_SYNC_TEST": "1"}, host_re=[r"rollbacks: [5-9]\d\d "])),
    # Mortal NPCs on (as the test home's cheats.cfg has it), then off at
    # frame 500: the characters it made mortal are immortal again.
    ("net-mortal-off", 6, 700, "60:,300:r,300:l", "700:",
     dict(env={"MM_NET_TOGGLE": "5@500"}, host_re=[r"mortal NPCs off: \d+ immortal again"])),
    # Charged throw (cheat 6) on from frame 30: player 1 picks up a block,
    # charges for 150 frames and throws; both machines see the same strong
    # throw (fingerprints), the host logs its strength.
    ("net-throw", 4, 700, "80:,6:r,10:B,20:,10:A,40:,30:r,150:rB,300:", "700:",
     dict(env={"MM_NET_TOGGLE": "6@30", "MM_TEST_CHEATS": "charged_throw=0"},   # (off first, whatever the test home has)
          host_re=[r"throw at (?!100%)\d+%: slot \d+ \(an object\)"])),
    # The guest's connection drops at frame 400: it connects again, the host
    # restarts both from its game, and the session goes on (no resync counted).
    ("net-reconnect", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r",
     dict(env={"MM_NET_TEST_DROP": "400"}, host_re=[r"reconnected from the host's game"], guest=["] reconnected (epoch 2"])),
    # Lockstep stays as a fallback (without lag; net-lag is lockstep with lag).
    ("net-lockstep", 1, 900, "60:,400:r,440:l", "60:,400:l,440:r", dict(env={"MM_NET_LOCKSTEP": "1"})),
    ("net-far", 1, 1500, "60:,1440:r", "60:,1440:l"),
    ("net-steam", 1, 1200, "60:,600:r,540:l", "60:,600:l,540:r"),   # over the Steam stand-in (--steam-mock)
    ("net-carry", 1, 1200, "80:,6:l,10:B,20:,60:r,30:,60:l,20:,10:B,900:", "600:,6:r,10:B,20:,60:l,30:,60:r,20:,10:B,400:"),
    ("net-fuzz", 4, 1800, fuzz(901, grab_bias=True), fuzz(902, grab_bias=True)),
    ("net-fuzz-9", 9, 1800, fuzz(903, grab_bias=True), fuzz(904, grab_bias=True)),
    # "Ride the cat" (cheat 7) turned on by the host: both machines build the
    # same cats from 3-12's package and stay identical.
    ("net-cat", 1, 1500, fuzz(905), fuzz(906), dict(env={"MM_NET_TOGGLE": "7@100"})),
    # The mount button online: the host spawns a missile (a command, as the
    # Spawn page sends it), the guest's cat boards it and gets off it; both
    # machines stay identical. (Here she touches the missile landing on it:
    # boarding must not let that hurt her or throw her off.)
    ("net-cat-missile", 1, 600, "600:", "188:,4:Z,60:r,54:,4:Z,290:",
     dict(env=dict(CAT, MM_NET_SPAWN="F09@120"),
          host_re=[r"player 2's cat boards missile", r"player 2's cat gets off missile"],
          guest=["player 2's cat boards missile", "player 2's cat gets off missile"])),
    # 5-3: player 2's own bee online.
    ("net-bee", 20, 1500, fuzz(907, grab_bias=True), fuzz(908, grab_bias=True)),
]

BASE_ENV = {"MM_HEADLESS_GFX": "1", "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}

# On Windows the game keeps its config and saves in %LOCALAPPDATA% whatever
# HOME says, so there each run goes in portable mode: started in the test
# home's config folder, with a portable.txt there. The player's own saves are
# never touched.
# (MM_BOTS_PORTABLE=1 forces it elsewhere: the Windows build under Wine.)
PORTABLE = os.name == "nt" or os.environ.get("MM_BOTS_PORTABLE") == "1"


def merged(*dicts, extra=None, **kw):
    """The environment: dicts, then kw, then extra (a scenario's own env wins)."""
    out = {}
    for d in dicts:
        out.update(d)
    out.update(kw)
    out.update(extra or {})
    return out


def game_cwd(env):
    if not PORTABLE:
        return None
    d = Path(env["HOME"]) / ".config" / "troublemakers-recomp"
    d.mkdir(parents=True, exist_ok=True)
    (d / "portable.txt").touch()
    return str(d)


def run_game(args, env, timeout):
    """The game once with env, to the end or timeout: its output."""
    try:
        return subprocess.run([args.bin, args.rom], env=env, cwd=game_cwd(env), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout).stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as e:
        return (e.stdout or b"").decode(errors="replace")


# States in which a player's position is the carry's to set: being carried
# (0x16 grabbed, 0x2F; she snaps to the carrier's hands, and to the other side when the
# carrier turns), reaching (0x11F), carrying / throwing (0x122, 0x126, and
# the throw's end 0xA14 / 0xB14: on a cat she is put back on her seat then).
CARRY_STATES = {0x16, 0x2F, 0x11F, 0x122, 0x126, 0xA14, 0xB14}
# Scripted moves: a star teleporter's flight (0x301).
SCRIPTED_STATES = {0x301, 0x231,   # 0x231: back onto her cat (3-12) after a fall
                   0x401,          # hurt: a hazard (2-6's lava) puts her back on safe ground (the same spot each time)
                   0x001}          # moved by the stage (4-2's ride; 3-3: a missile flying off with her)
# Riding a mount (3-12's cats: 0x003, getting on / off and bucked 0x230,
# 0x231, 0x233, 0x531): the stage keeps riders in view itself, out to
# about 240 px from the camera, so the shared-screen check allows that
# for the rest of a trace once player 1 rode.
RIDING_STATES = {0x003, 0x230, 0x231, 0x233, 0x531}


def timeline(text):
    """The log's lines in the order the game finally ran them.

    Each traced frame carries its frame number; lines printed while a frame
    ran come before that frame's trace line. A rollback runs frames again,
    so a frame number that goes back supersedes everything logged from that
    frame on (the predicted run of it)."""
    out = []           # (frame, line)
    current = 0        # the frame the next lines belong to
    for line in text.splitlines():
        m = TRACE.match(line)
        if m:
            n = int(m.group(1))
            if n < current:
                while out and out[-1][0] >= n:
                    out.pop()
            out.append((n, line))
            current = n + 1
        else:
            m = FRAME_OF.search(line)   # a line that names its frame
            out.append((int(m.group(1)) if m else current, line))
    return [line for _, line in out]


def check_trace(text, allowed_rejoins, far=False, loop=False):
    """loop: an auto-scrolling, looping stage (3-12, save slot 9): steps are
    also judged against the camera's own step, and riders are allowed out to
    where its cats keep them."""
    problems = []
    frames = present = apart = 0
    prev = None
    prev_cam = (0, 0)
    joined_recently = 0
    held_bad = 0
    lines = timeline(text)
    rejoins = sum("rejoining" in l for l in lines)
    defeats = sum("defeated, rejoining" in l for l in lines)
    first = last = None
    max_step = [0, 0]
    riding = False   # a mount stage (once seen, for the rest of the trace)
    for line in lines:
        if "leads now" in line or "leads again" in line:
            # The two swapped characters (player2_lead.cpp): the trace's
            # columns are slots, so each person's previous place moves over.
            if prev:
                prev = (prev[1], prev[0], (prev[2][1], prev[2][0]))
            if first:
                first = (first[1], first[0])
            # (At an exit the other character is brought to the new lead.)
            joined_recently = 3
            continue
        # (Going down: the hidden character moves to the other at once,
        # player2_health.cpp; not a jump.)
        # (Boarding a missile with the mount button puts the cat onto it.)
        if ("joined in slot" in line or "rejoining" in line or "[test] player" in line or "[player2] down" in line
                or "boards missile" in line):
            joined_recently = 3
            continue
        m = HELD.match(line)
        if m:
            if m.group(1) == "0" or m.group(2) == "0":
                held_bad += 1
                if held_bad == 3:
                    problems.append("carrying flag without a live object (%s)" % line.strip())
            else:
                held_bad = 0
            continue
        m = TRACE.match(line)
        if not m:
            continue
        n = int(m.group(1))
        cx, cy = int(m.group(2)), int(m.group(3))
        states = (int(m.group(6), 16) & 0xFFF, int(m.group(12), 16) & 0xFFF)
        p2slot = int(m.group(9))
        riding = riding or states[0] in RIDING_STATES
        p1 = (cx + int(m.group(4)), cy + int(m.group(5)))
        p2 = (cx + int(m.group(10)), cy + int(m.group(11)))
        if p2slot < 0:
            prev = None
            continue
        frames += 1
        present += 1
        if first is None:
            first = (p1, p2)
        last = (p1, p2)
        apart = max(apart, abs(p1[0] - p2[0]))
        if not far:
            for k, who, rel in ((0, "p1", int(m.group(4))), (1, "p2", int(m.group(10)))):
                edge = 275 if riding and loop else 181   # (3-12: a rider at ~240 holds the other ahead of her)
                if abs(rel) > edge and joined_recently == 0:
                    problems.append("%s off the shared screen at frame %d (x %d)" % (who, n, rel))
        if prev and joined_recently == 0:
            for k, who, a, b in ((0, "p1", prev[0], p1), (1, "p2", prev[1], p2)):
                # A looping arena (3-12, CERBERUS: about 785 px round) wraps a
                # player's x back by a whole lap; not a jump.
                if abs(a[0] - b[0]) > 600 and abs(a[1] - b[1]) <= 8:
                    continue
                # A looping vertical arena (5-6, PHOENIX: a free fall about
                # 1010 px round) wraps the camera and the players back up.
                if abs(a[1] - b[1]) > 600 and abs(a[0] - b[0]) <= 8:
                    continue
                # Getting (back) on a mount: it seats its rider (a new cat
                # from the cheat, back on her bee after a grab).
                if states[k] in RIDING_STATES and prev[2][k] not in RIDING_STATES:
                    joined_recently = max(joined_recently, 2)   # (and the frame after)
                    continue
                if states[k] in CARRY_STATES or prev[2][k] in CARRY_STATES:
                    continue
                if states[k] in SCRIPTED_STATES or prev[2][k] in SCRIPTED_STATES:
                    continue
                # Landing (a new state) may snap her up onto what she lands on.
                max_step[k] = max(max_step[k], abs(a[0] - b[0]))
                landing = states[k] != prev[2][k] and abs(a[0] - b[0]) <= 8 and 0 < b[1] - a[1] <= 32
                big = abs(a[0] - b[0]) > 24 or abs(a[1] - b[1]) > 24
                if loop and big:
                    # An auto-scrolling stage (3-12) carries the players with
                    # the screen: judge the step against the camera's as well.
                    # (Only there: elsewhere the camera follows a player, so a
                    # teleport it snapped along with would pass unseen.)
                    dxc = (b[0] - a[0]) - (cx - prev_cam[0])
                    dyc = (b[1] - a[1]) - (cy - prev_cam[1])
                    big = abs(dxc) > 24 or abs(dyc) > 24
                if not landing and big:
                    problems.append("%s jumped at frame %d: %s -> %s" % (who, n, a, b))
        prev = (p1, p2, states)
        prev_cam = (cx, cy)
        joined_recently = max(0, joined_recently - 1)
    if rejoins - defeats > allowed_rejoins:
        problems.append("%d leash rejoins (allowed %d)" % (rejoins - defeats, allowed_rejoins))
    if defeats > 4:
        problems.append("%d defeat rejoins" % defeats)
    if frames < 100:
        problems.append("player 2 present only %d traced frames" % frames)
    # de-duplicate noisy repeats
    seen, out = set(), []
    for p in problems:
        key = p.split(" at frame")[0]
        if key not in seen:
            seen.add(key)
            out.append(p)
    moved = None
    if first and last:
        moved = ((last[0][0] - first[0][0], last[0][1] - first[0][1], last[0][0], last[0][1]),
                 (last[1][0] - first[1][0], last[1][1] - first[1][1], last[1][0], last[1][1]))
    check_trace.moved = moved
    check_trace.max_step = max_step
    return out, frames, apart


def check_extra(extra, out, moved):
    """The scenario's own checks: lines it must print, where the characters end."""
    problems = []
    for want in extra.get("expect", []):
        if want not in out:
            problems.append("no '%s'" % want)
    if "max_joins" in extra and out.count("[player2] joined in slot") > extra["max_joins"]:
        problems.append("player 2 joined %d times (allowed %d)" % (out.count("[player2] joined in slot"), extra["max_joins"]))
    for unwanted in extra.get("absent", []):
        if unwanted in out:
            problems.append("'%s' appeared" % unwanted)
    limit = extra.get("max_step2")
    if limit is not None and check_trace.max_step[1] > limit:
        problems.append("player 2 moved %d px in one frame (at most %d)" % (check_trace.max_step[1], limit))
    final = extra.get("final")
    if final and moved:
        # (world x, world y) at the end for "star" checks; moves for the others.
        s0 = (moved[0][2], moved[0][3]) if "MM_TEST_PLACE" in extra.get("env", {}) else moved[0][:2]
        s2 = (moved[1][2], moved[1][3]) if "MM_TEST_PLACE" in extra.get("env", {}) else moved[1][:2]
        if not final(s0, s2):
            problems.append("characters ended at %s / %s" % (s0, s2))
    return problems


def play_with_rollback(args, rec_dir, frames, extra_env=None):
    """Plays `rec_dir` back with a rollback every 30 frames (8 frames back): the
    frames run again must match the recording, which is what netcode-v2
    needs of every piece of state (docs/NETCODE-V2.md)."""
    env = merged(os.environ, BASE_ENV, HOME=args.home, MM_REPLAY_PLAY="%s@200" % rec_dir,
               MM_REPLAY_SPEED="1000", MM_TEST_ROLLBACK=args.rollback, extra=extra_env)
    out = run_game(args, env, 60 + frames // 10)
    problems = []
    if "memory matched every frame" not in out:
        m = re.search(r"memory (?:first )?differ(?:s|ed) (?:from the recording )?at frame (\d+)", out)
        problems.append("rollback: memory differed at frame %s" % (m.group(1) if m else "?"))
    m = re.search(r"\[rollback-test\] (\d+) rollbacks, (\d+) mismatched", out)
    if not m:
        problems.append("rollback: no rollbacks ran")
    elif int(m.group(2)):
        problems.append("rollback: %s of %s rollbacks mismatched" % (m.group(2), m.group(1)))
    for line in out.splitlines():
        if "restore failed" in line or "snapshot failed" in line:
            problems.append(line.strip())
            break
    return problems


def run_local(args, name, slot, frames, pad1, pad2, allowed, extra_env=None, far=False, want_apart=0, extra=None):
    with tempfile.TemporaryDirectory() as tmp:
        env = merged(os.environ, BASE_ENV, HOME=args.home, MM_P2_TRACE="1",
                   MM_REPLAY_RECORD="%s/rec@200" % tmp, MM_REPLAY_LENGTH=str(frames),
                   MM_REPLAY_SPEED="4", MM_REPLAY_PAD=pad1, MM_REPLAY_PAD2=pad2,
                   MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150", extra=extra_env)
        out = run_game(args, env, 30 + frames // 40)
        rollback_problems = play_with_rollback(args, "%s/rec" % tmp, frames, extra_env) if args.rollback else []
    if args.keep:   # the log, for a look after a failure
        keep = Path(args.keep) / name
        keep.mkdir(parents=True, exist_ok=True)
        (keep / "local.log").write_text(out)
    # Only the recorded part counts (from the recorder's load on).
    start = out.find("[replay] recording into")
    end = out.find("[replay] recording stopped")
    section = out[start:end if end > 0 else None] if start >= 0 else ""
    if start < 0:
        return ["did not start recording"], 0
    problems, traced, apart = check_trace(section, allowed, far, loop=slot == LOOP_SLOT)
    problems += rollback_problems
    if extra:
        problems += check_extra(extra, out, check_trace.moved)
    if want_apart and apart < want_apart:
        problems.append("only %d px apart at most (want %d)" % (apart, want_apart))
    return problems, traced


def run_far(args, name, slot, frames, pad1, pad2, want_apart, extra=None):
    extra = extra or {}
    hashes = []
    problems = []
    traced = 0
    for shadow in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            env = merged(os.environ, BASE_ENV, HOME=args.home, MM_P2_TRACE="1", MM_FAR="1",
                       MM_REPLAY_RECORD="%s/rec@200" % tmp, MM_REPLAY_LENGTH=str(frames),
                       MM_REPLAY_SPEED="4", MM_REPLAY_PAD=pad1, MM_REPLAY_PAD2=pad2,
                       MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150", extra=extra.get("env"))
            if shadow:
                env["MM_FAR_SHADOW"] = "1"
            out = run_game(args, env, 40 + frames // 20)
            if args.keep:   # the log, for a look after a failure
                keep = Path(args.keep) / name
                keep.mkdir(parents=True, exist_ok=True)
                (keep / ("shadow.log" if shadow else "plain.log")).write_text(out)
            h = Path(tmp) / "rec" / "hashes.bin"
            hashes.append(h.read_bytes() if h.exists() else b"")
            rollback_problems = []
            if shadow and args.rollback:
                rollback_problems = play_with_rollback(args, str(Path(tmp) / "rec"), frames,
                                                       dict(extra.get("env", {}), MM_FAR="1", MM_FAR_SHADOW="1"))
        start = out.find("[replay] recording into")
        end = out.find("[replay] recording stopped")
        if start < 0:
            return ["did not start recording"], 0
        if shadow:
            problems, traced, apart = check_trace(out[start:end if end > 0 else None], 3, far=True, loop=slot == LOOP_SLOT)
            if want_apart and apart < want_apart:
                problems.append("only %d px apart at most (want %d)" % (apart, want_apart))
            if "shadow frame too big" in out:
                problems.append("shadow frame overflowed its mirror")
            problems += check_extra(extra, out, check_trace.moved)
            problems += rollback_problems
    n = min(len(hashes[0]), len(hashes[1]))
    if n == 0 or abs(len(hashes[0]) - len(hashes[1])) > 0:
        problems.append("recordings differ in length (%d / %d bytes)" % (len(hashes[0]), len(hashes[1])))
    elif hashes[0][:n] != hashes[1][:n]:
        first = next(i for i in range(n) if hashes[0][i] != hashes[1][i])
        problems.append("player 2's picture changed the game (fingerprints differ from byte %d)" % first)
    return problems, traced


def run_netplay(args, name, slot, frames, pad1, pad2, port, extra=None):
    extra = extra or {}
    steam = name.startswith("net-steam")
    with tempfile.TemporaryDirectory() as tmp:
        guest_home = Path(tmp) / "guest"
        (guest_home / ".config" / "troublemakers-recomp").mkdir(parents=True)
        cfg = Path(args.home) / ".config" / "troublemakers-recomp"
        for f in ("troublemakers.n64.us.1.z64", "controls.json"):
            if (cfg / f).exists():
                (guest_home / ".config" / "troublemakers-recomp" / f).write_bytes((cfg / f).read_bytes())
        hlog, glog = Path(tmp) / "h.txt", Path(tmp) / "g.txt"
        common = merged(os.environ, BASE_ENV, MM_NET_LENGTH=str(frames), MM_P2_TRACE="1", extra=extra.get("env"))
        henv = dict(common, HOME=args.home, MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="load@150",
                    MM_NET_HOST="%d@200" % port, MM_NET_PAD=pad1, MM_NET_LOG=str(hlog))
        genv = dict(common, HOME=str(guest_home), MM_NET_JOIN="127.0.0.1:%d@300" % port, MM_NET_PAD=pad2,
                    MM_NET_LOG=str(glog))
        if steam:
            # Two Steam users who are friends; the guest finds the host's lobby
            # through the friends list (tools/steam_mock/mock_steam.c).
            host_id, guest_id = "76561190000000%03d" % (port % 1000), "76561190000001%03d" % (port % 1000)
            mock = dict(MM_STEAM_LIB=args.steam_mock, MOCK_STEAM_DIR=str(Path(tmp) / "steam"))
            henv.update(mock, MM_NET_HOST="steam@200", MOCK_STEAM_ID=host_id, MOCK_STEAM_FRIENDS=guest_id)
            genv.update(mock, MM_NET_JOIN="steam@300", MOCK_STEAM_ID=guest_id, MOCK_STEAM_FRIENDS=host_id)
        timeout = 60 + frames // 30
        hfile = open(Path(tmp) / "host.log", "wb")   # a file, not a pipe: nobody reads it while the game runs
        host = subprocess.Popen([args.bin, args.rom], env=henv, cwd=game_cwd(henv), stdout=hfile,
                                stderr=subprocess.STDOUT)
        time.sleep(1)
        gout = run_game(args, genv, timeout)
        try:
            host.wait(timeout=20)
        except subprocess.TimeoutExpired:
            host.kill()
            host.wait()
        hfile.close()
        hout = (Path(tmp) / "host.log").read_bytes().decode(errors="replace")
        if args.keep:   # the logs, for a look after a failure
            keep = Path(args.keep) / name
            keep.mkdir(parents=True, exist_ok=True)
            (keep / "host.log").write_text(hout)
            (keep / "guest.log").write_text(gout)
        problems = []
        if "session started" not in hout or "joined the host's game" not in gout:
            return ["session did not start"], 0
        resyncs = hout.count("[netplay] resync:")
        if resyncs:
            problems.append("%d resyncs" % resyncs)
        # Fingerprints by (epoch, frame): with rollback a side skips a frame it
        # ran on a prediction, so only the frames both took are compared.
        def hashes(path):
            out = {}
            for l in (path.read_text().splitlines() if path.exists() else []):
                parts = l.split()
                if len(parts) == 3:
                    out[(parts[0], int(parts[1]))] = parts[2]
            return out
        h, g = hashes(hlog), hashes(glog)
        common = sorted(set(h) & set(g))
        # One fingerprint every 60 frames on each side; nearly all should be
        # in common (a frame's hash waits until its inputs are confirmed, so
        # lag delays it, it does not lose it).
        if len(common) < max(2, frames // 60 * 2 // 3):
            problems.append("only %d fingerprint checks in common (of about %d)" % (len(common), frames // 60))
        bad = [k for k in common if h[k] != g[k]]
        if bad:
            problems.append("fingerprints differ (first at epoch %s frame %d)" % bad[0])
        start = hout.find("session started")
        end = hout.find("session ended", start)   # after it the host plays on alone
        if "same" in extra:   # these lines, as many, identical on both machines
            text, count = extra["same"]
            hl = [l for l in hout.splitlines() if text in l]
            gl = [l for l in gout.splitlines() if text in l]
            if len(hl) != count or hl != gl:
                problems.append("%r lines: host %s, guest %s" % (text, hl, gl))
        for pattern in extra.get("host_re", []):
            if not re.search(pattern, hout):
                problems.append("host: no %r" % pattern)
        for text in extra.get("guest", []):
            if text not in gout:
                problems.append("guest: missing %r" % text)
        more, traced, _ = check_trace(hout[start:end if end > 0 else None], 3, far=True, loop=slot == LOOP_SLOT)
        return problems + more, traced


def make_boss_states(args):
    """Each boss stage's save state: warp there (the debug warp), press A and
    B every two seconds through the introduction, save once the stage has
    been in plain play for four seconds (MM_TEST_SAVESTATE=save@ready)."""
    failed = 0
    for slot, label in BOSSES + STAGES:
        env = merged(os.environ, BASE_ENV, HOME=args.home, MM_TEST_WARP=label, MM_TEST_AUTO_ADVANCE="1",
                   MM_TEST_SAVESTATE_SLOT=str(slot), MM_TEST_SAVESTATE="save@ready", MM_REPLAY_SPEED="4",
                   MM_TEST_EXIT_AFTER_SAVE="1")
        # Made at the tests' own speed from boot (MM_REPLAY_SPEED=4): a boss
        # state made in a normal-speed session crashed in the game's sound
        # DMA (Sound_DmaCallback) when a 4x-from-boot test loaded it.
        state = Path(args.home) / ".config" / "troublemakers-recomp" / "savestates" / ("slot%d.tmstate" % slot)
        # Kept unless asked for by name (--only 5-3,2-7): the tests' states
        # must stay the same from run to run.
        if state.exists() and not (args.only and label in args.only.split(',')):
            print("%-6s slot %d: kept" % (label, slot), flush=True)
            continue
        before = state.stat().st_mtime if state.exists() else 0
        run_game(args, env, 150)
        # (The game exits at once after the save, before its log catches up:
        # the file says whether it was written.)
        ok = state.exists() and state.stat().st_mtime > before
        print("%-6s slot %d: %s" % (label, slot, "saved" if ok else "FAILED"), flush=True)
        failed += not ok
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--home", required=True)
    ap.add_argument("--only", default="", help="run only scenarios whose name contains this (comma-separated: any of these)")
    ap.add_argument("--no-net", action="store_true")
    ap.add_argument("--rollback", default="", metavar="EVERY:DEPTH",
                    help="also play each local and far recording back with rollbacks (e.g. 30:8): netcode-v2")
    ap.add_argument("--steam-mock", default="", help="the Steam stand-in library (cc -shared -fPIC "
                    "-o libsteam_api.so tools/steam_mock/mock_steam.c); without it net-steam is skipped")
    ap.add_argument("--port", type=int, default=7900, help="first port for the netplay scenarios")
    ap.add_argument("--keep", default="", metavar="DIR", help="keep each scenario's logs under DIR/<name>/")
    ap.add_argument("--make-boss-states", action="store_true",
                    help="make the boss stages' save states (slots 10-26) in --home from warps, then exit")
    args = ap.parse_args()
    # Absolute: on Windows each run starts in the test home's config folder.
    args.bin, args.rom, args.home = (str(Path(p).resolve()) for p in (args.bin, args.rom, args.home))
    if args.make_boss_states:
        return make_boss_states(args)
    failed = 0
    port = args.port
    states = Path(args.home) / ".config" / "troublemakers-recomp" / "savestates"
    for name, slot, frames, p1, p2, allowed, *extra in SCENARIOS:
        if args.only and not any(o in name for o in args.only.split(',')):
            continue
        if not (states / ("slot%d.tmstate" % slot)).exists():
            print("%-18s skip (no save state %d in the test home: --make-boss-states)" % (name, slot), flush=True)
            continue
        problems, traced = run_local(args, name, slot, frames, p1, p2, allowed,
                                     extra_env=extra[0].get("env") if extra else None, extra=extra[0] if extra else None)
        print("%-18s %s (%d frames)%s" % (name, "ok  " if not problems else "FAIL", traced,
                                          "" if not problems else ": " + "; ".join(problems[:4])), flush=True)
        failed += bool(problems)
    for name, slot, frames, p1, p2, want, *extra in FAR:
        if args.only and not any(o in name for o in args.only.split(',')):
            continue
        problems, traced = run_far(args, name, slot, frames, p1, p2, want, extra[0] if extra else None)
        print("%-18s %s (%d frames)%s" % (name, "ok  " if not problems else "FAIL", traced,
                                          "" if not problems else ": " + "; ".join(problems[:4])), flush=True)
        failed += bool(problems)
    if not args.no_net:
        for name, slot, frames, p1, p2, *extra in NETPLAY:
            if args.only and not any(o in name for o in args.only.split(',')):
                continue
            if name.startswith("net-steam") and not args.steam_mock:
                print("%-18s skip (no --steam-mock)" % name, flush=True)
                continue
            port += 1
            problems, traced = run_netplay(args, name, slot, frames, p1, p2, port, extra[0] if extra else None)
            print("%-18s %s (%d frames)%s" % (name, "ok  " if not problems else "FAIL", traced,
                                              "" if not problems else ": " + "; ".join(problems[:4])), flush=True)
            failed += bool(problems)
    print("ALL OK" if failed == 0 else "FAILED: %d scenario(s)" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
