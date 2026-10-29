// Far-apart co-op: the two players can be anywhere in the stage (netplay).
//
// The game keeps one camera, and the world is only alive and drawn near it:
// map actors are spawned and removed by windows around the camera, enemies
// go idle or vanish by their own distance checks, and only what is near it
// is drawn. Player 2 used to be held on player 1's screen for that reason.
//
// Here the game gets a second camera that follows player 2, with the whole
// set of camera globals (0x800BE544..0x800BE578 and 0x800BE61C..0x800BE624)
// kept for it in game memory (0x807F0500), so replays, save states and
// netplay carry it:
//
//  - each frame the game's own camera code runs a second time for it,
//    following player 2 (func_80012830 with her position put in Marina's);
//  - the map actor prefetch (func_8001DE30) runs a second time around it;
//  - the onscreen-only despawner (func_80016D94) spares actors in its view;
//  - in the actor loop (func_8001751C) each actor runs with the camera it is
//    nearest to: all camera-relative positions (actor +0x88 / +0x8C, which
//    the game scrolls with the camera) are rebased, the camera globals
//    swapped, so every distance and view check in its code (69 actor
//    families have their own) sees "its" camera;
//  - player 2 has Marina's walls (camera +-0x90) around her own camera.
//
// Both machines run all of that, so the game stays identical on both.
// What each machine shows is its own player's camera: player 1's is the
// game's; player 2's machine builds a second picture each frame (the
// "shadow" frame): the game's whole frame runs once more from a copy of
// memory with the two cameras' roles swapped, its display list (with every
// piece of memory it reads that changed) is copied above 8 MB where the
// renderer still reads it, memory is put back exactly, and the real frame's
// list jumps to the copy.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace mm::faraway {

// The far-apart rules are on for this frame (netplay session, or MM_FAR=1 to
// test locally, with player 2 in the stage). Game thread.
bool active(uint8_t* rdram);

// Inside a frame that is thrown away: the shadow frame, or a lead probe
// (player2_lead.cpp). Host hooks with state of their own sit it out.
bool in_shadow();

// Local split screen is on (cheats.h, not online): both cameras, the second
// view drawn into its own framebuffer for the window's other half.
bool split_local();

// The stage's item list, before the game handles it (real frames only): a
// golden gem the save already has is marked gone by its handler only when
// the game's own camera reaches it (func_800446F0: YellowGem_GetFlag, then
// +0x4000), after it was drawn that frame. Player 2's picture, which runs
// a frame from a copy, met it there every frame: drawn, never gone, and
// worth nothing when she took it. Such gems leave the list at once.
void hide_collected_golden_gems(uint8_t* rdram, uint32_t list);
void set_probing(bool on);

// The roles are swapped: the game's camera follows player 2 (shadow frame).
bool swapped();

// The camera globals are being run for the second camera (func_80012830
// must follow the given position as is, not the co-op midpoint).
bool secondary_pass();

// Player 2's camera centre, relative to the camera now loaded (Q16.16 px).
// False when the far rules are off.
bool p2_camera_offset(uint8_t* rdram, int32_t& dx, int32_t& dy);

// Forget the second camera (player 2 (re)joins next to player 1): it starts
// again from the game's camera.
void reset(uint8_t* rdram);

// This machine shows the second camera, the one following player 2's slot:
// its person plays that slot (the netplay guest, or the host while player 2
// leads, player2.h), or MM_FAR_SHADOW=1.
bool shows_second_view(uint8_t* rdram);

// Swap the game's camera with the second one, keeping world positions (the
// two players swapped characters, player2_lead.cpp). Between frames.
void exchange_cameras(uint8_t* rdram);

} // namespace mm::faraway
