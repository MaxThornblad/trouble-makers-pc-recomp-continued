// Grabbable blocks (a cheat on the Cheats page).
//
// Stage blocks are item records drawn by the item list, made solid by their
// cells in the terrain collision map (docs/GAME_MAP.md), so Marina cannot
// grab them. When she presses grab facing a plain block, the block is taken
// out of the stage for good (its record gets the game's own "gone" bit and
// its terrain cells are emptied) and a block object (custom actor type
// 0x7F01, custom_actors.cpp) appears in its place in the same frame; her
// normal grab picks that up, because it answers the grab handshake like any
// grabbable object. From then on it is an object like the pot: carried,
// thrown, set down anywhere (no grid), solid to stand on and bump into.
#pragma once

#include <cstdint>

namespace mm::grab_blocks {

// Item-list hook (func_800451E4 entry), every frame: turns a block into a
// block object when Marina grabs at it.
void on_item_list(uint8_t* rdram, uint32_t list);

} // namespace mm::grab_blocks
