// Player 2: a second Marina (co-op and netplay groundwork, docs/NETPLAY.md).
//
// The game only knows one Marina: actor slot 0, updated once a frame by
// func_8004ED10(slot) from func_80020024, with a handful of globals of her
// own (input words, a player-state block at 0x801373D8, her world position
// at 0x800BE5D8 / 0x800BE5E8 that enemies aim at, ...). Player 2 lives in
// another slot as custom actor type 0x7F02. Its update swaps its record and
// its own copy of those globals into slot 0, puts controller 2's buttons
// where Marina reads hers, runs Marina's update, and swaps everything back,
// so the game believes it updated Marina twice while enemies, the camera
// and the HUD keep seeing player 1. The list of globals came from a probe
// that diffed game memory around Marina's update over thousands of frames
// of varied play (MM_TEST_P2PROBE).
#pragma once

#include <cstdint>

#include "recomp.h"

namespace mm::player2 {

// Custom actor index (type 0x7F02).
constexpr uint8_t kCustomIndex = 0x02;

// Custom actor callbacks (registered in custom_actors.cpp).
void setup(uint8_t* rdram, int slot);
void update(uint8_t* rdram, recomp_context* ctx, int slot);

// The picture's offset from the game camera (px, +y up) while netplay shows
// each player their own view; overlays drawn at camera positions subtract it.
// Any thread.
void view_offset(float& x, float& y);

// Game thread: player 2's actor record and her own hold / press words (as
// Marina's at 0x801370CC/CE, from her last update). False without player 2.
bool controls(uint8_t* rdram, uint32_t& record, uint16_t& hold, uint16_t& press);

// Keep player 2 within centre +- half_width of the camera now loaded (her
// x relative to it, Q16.16), as Marina's walls keep her (far_coop.cpp).
void clamp_x(uint8_t* rdram, int32_t centre, int32_t half_width);

// Slot of the live player 2, or -1.
int slot(uint8_t* rdram);

// Game thread, once a frame (debug menu tick): with co-op on, player 2 joins
// a moment into every scene, is pulled back next to player 1 after wandering
// too far off for a while, and leaves when co-op is turned off. Everything it
// decides on is in game memory, so replays and netplay stay exact.
void tick(uint8_t* rdram);

#ifdef MM_HAS_GRAPHICS
// Render thread: the "2P" tag and health bar over her head.
void draw_overlay();
#endif

} // namespace mm::player2
