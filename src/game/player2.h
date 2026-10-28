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

// Game thread: player 2's actor record and her own hold / press words (as
// Marina's at 0x801370CC/CE, from her last update). False without player 2.
bool controls(uint8_t* rdram, uint32_t& record, uint16_t& hold, uint16_t& press);

// Keep player 2 within centre +- half_width of the camera now loaded (her
// x relative to it, Q16.16), as Marina's walls keep her (far_coop.cpp).
void clamp_x(uint8_t* rdram, int32_t centre, int32_t half_width);

// Actor loop (func_8001751C): before each actor's update, and once after the
// loop. What player 2 carries sees her position in Marina's globals.
void held_swap_begin(uint8_t* rdram, int actor);
void held_swap_end(uint8_t* rdram);

// Run fn with player 2 as Marina (slot 0, Marina's globals), keeping what it
// does to her: the movement cheats, written for Marina, act on her too.
void as_marina(uint8_t* rdram, void (*fn)(uint8_t*));

// Player 2 stands in for Marina (slot 0, Marina's globals) between these
// two calls; nothing she does meanwhile is kept (the second camera's pass).
void stand_in_begin(uint8_t* rdram);
void stand_in_end(uint8_t* rdram);

// Back next to player 1 if she is off the shared screen (far apart rules
// ended, far_coop.cpp).
void bring_back(uint8_t* rdram);

// The stage's item list, before the game handles it (func_800451E4): gems
// player 2 touches go to player 1, who collects them for both.
void collect_pickups(uint8_t* rdram, uint32_t list);

// Test: put player 2 at a world position (Q16.16).
void place_test(uint8_t* rdram, int32_t x, int32_t y);

// Slot of the live player 2, or -1.
int slot(uint8_t* rdram);

// Who plays which character. Slot 0 (the game's Marina) is the lead: what
// stage scripts, doors and teleporters answer to. Normally player 1; when
// player 2 does something only the lead can (walks into an exit, rides a
// star teleporter), the two swap characters (player2_lead.cpp): their
// controllers, tint, tags and cameras follow the people. Back to player 1
// with the next scene. Game memory, so replays and netplay carry it.
bool lead_swapped(uint8_t* rdram);

// After the actor loop: player 2's tint on slot 0 while she leads.
void after_actors(uint8_t* rdram);

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
