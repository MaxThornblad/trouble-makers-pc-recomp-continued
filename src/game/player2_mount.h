// Player 2's mount (co-op, docs/NETPLAY.md).
//
// Some stages put Marina on something that carries her: 3-12 (CERBERUS) has
// the cat she rides through the loop. Such a mount is an actor of its own
// that follows the one Marina the game knows (slot 0), and its code answers
// to one fixed slot (the stage spawns the cat into slot 96; a cat anywhere
// else is an idle one). So player 2 rode nothing: her character sat in the
// riding pose with no cat under it.
//
// She gets a mount of her own: a copy of the stage's mount, controller and
// part sprites, in free slots, as custom actor type 0x7F03. Each frame its
// update runs the game's own mount code on it: her copy takes the stage's
// slots for the moment and she stands in for Marina (player2.cpp
// as_marina), so the code finds "the cat" in its slot and "Marina" in
// hers; then the copies go back and player 1's mount and record are
// restored. All of it is in game memory (the actor records and player 2's
// block), so save states, rollbacks, replays and resyncs carry it.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace mm::player2_mount {

constexpr uint8_t kCustomIndex = 0x03;   // custom actor type 0x7F03

// Custom actor update (registered in custom_actors.cpp): the controller
// slot of player 2's mount.
void update(uint8_t* rdram, recomp_context* ctx, int slot);

// The stage puts Marina on a mount (its code places her; the screen-edge
// clamp must leave the riders alone, or it pulls them off their mounts).
bool riding(uint8_t* rdram);

// Around each actor's update (player2.cpp held_swap_begin / _end): a
// missile of 3-12 talks to player 2's cat (see kMissileMessage in the .cpp).
void missile_begin(uint8_t* rdram, int actor);
void missile_end(uint8_t* rdram);

// Once a frame from player2::tick: gives player 2 a mount when the stage
// has one for Marina and she has none; takes hers away when she is gone.
void tick(uint8_t* rdram);

} // namespace mm::player2_mount
