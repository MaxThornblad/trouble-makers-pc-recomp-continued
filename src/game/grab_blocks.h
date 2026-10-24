// Grabbable blocks (a cheat on the Cheats page).
//
// Stage blocks are item records, not actors (docs/GAME_MAP.md, Stage items),
// so Marina cannot grab them. When she presses grab facing a plain block, the
// record is hidden with the game's own "gone" bit and a loose block actor
// (custom type 0x7F01, custom_actors.cpp) takes its place in the same frame;
// Marina's normal grab then picks the actor up a frame or two later, because
// it answers the grab handshake like any grabbable object. Thrown or dropped,
// it flies with the throw velocity under the game's own terrain collision.
// Once it has stopped, it snaps to the 32-pixel block grid and is written back
// into its original record there, solid again.
#pragma once

#include <cstdint>

namespace mm::grab_blocks {

// Item-list hook (func_800451E4 entry), every frame: turns a block into a
// loose block when Marina grabs at it, and remembers the list for landing.
void on_item_list(uint8_t* rdram, uint32_t list);

// Called by the loose block when it has come to rest: writes it back as a
// block record. With `home` (lost off screen, or never picked up) it goes back
// to where it came from. Returns true when it did (the actor should then
// disappear); false means wait (Marina stands in the way).
bool settle(uint8_t* rdram, int slot, bool home);

} // namespace mm::grab_blocks
