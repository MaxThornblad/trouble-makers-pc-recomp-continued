// A health bar each (co-op): who goes down, and how they come back.
//
// Each character keeps its own health (+0xE0 of its record): slot 0's is the
// one the game's HUD and defeat answer to, player 2's slot's is hers. When a
// character's health reaches 0 while the other is still up, the game must not
// see it (Marina's defeat ends the stage for both), so:
//
//  - the downed character is always the one in player 2's slot: if it is slot
//    0's (player 1's, or player 2's person while she leads), the two swap
//    characters first, as the lead swap does (player2_lead.cpp);
//  - while down, that character is not updated, drawn or hit, and stays with
//    the other (the shared camera and the far-apart rules see one player);
//  - after kDownFrames it comes back next to the other with half health.
//
// Only when both are down does the game's own defeat run. Health a pickup
// gives goes to whoever touched it (player 2's gems are collected against
// player 1's hit box with player 2's health swapped in for the collection).
//
// All of it lives in game memory (player 2's block, see player2.cpp), so save
// states, replays, rollback and the second machine carry it.
#pragma once

#include <cstdint>

namespace mm::player2::health {

constexpr int kDownFrames = 180;

// The most health a player can have (the most either has had).
int16_t full(uint8_t* rdram);

// Health for player 2 when she joins (a new scene, or back from being down):
// half after being down, what she had when she left an earlier scene of this
// stage, else full. Ends any down time.
int16_t joining(uint8_t* rdram);

// GameState_Gameplay's entry, before Marina's update: slot 0 down while the
// other character is up: swap them (the downed one into player 2's slot).
void frame_begin(uint8_t* rdram);

// Player 2's update, first: true while her character is down (her update is
// skipped). Brings her back when her time is up.
bool down_update(uint8_t* rdram, int slot);
bool is_down(uint8_t* rdram);

// Player 2's pickups are collected against slot 0's hit box (the game's
// gem code knows only Marina). For that collection slot 0 carries player 2's
// health, so the game heals her (up to the cap, even with player 1's bar
// full), then player 1's own health goes back:
// - stage gems (item records), around the item pass (func_800451E4);
void pickup_begin(uint8_t* rdram);
void pickup_end(uint8_t* rdram);
// - gem actors (knocked out of enemies): marked when she touches one (in
//   game memory, until it is collected or gone), and swapped around that
//   actor's own update, where the game collects it (func_800303A8; the actor
//   loop calls actor_begin before each actor, tick ends any left open).
void gem_actor_touched(uint8_t* rdram, int actor);
void actor_begin(uint8_t* rdram, int actor);

// Once a frame, after the actors (mm_debug_menu_tick).
void tick(uint8_t* rdram);

} // namespace mm::player2::health
