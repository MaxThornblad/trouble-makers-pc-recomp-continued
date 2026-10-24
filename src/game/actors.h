// Actor toolkit: create and remove game objects ("actors") from host code.
//
// Every object in Mischief Makers lives in one of 208 actor slots of 0x198
// bytes from 0x800EF510 (slot 0 is Marina). A slot's type (+0xD2) picks its
// behaviour: types below 0x100 are shared ones whose code and data stay
// loaded in every stage (table 0x800C7FE0, entries 0x01-0x7C), higher types
// belong to the current stage's code package. The game prepares a slot for
// its type with func_8001E2D0 (hitbox, health and flags from per-type
// tables); spawning here does what the game's own spawners do: clear a free
// slot, write the type, run that routine, then place the object.
//
// Requests may come from any thread (the debug menu) and are carried out on
// the game thread by tick(). Spawned objects are capped, tracked so they can
// be removed again, and forgotten when the stage changes or a save state is
// loaded (the game clears its slots on a stage change itself).
#pragma once

#include <cstdint>

namespace mm::actors {

constexpr int kMaxSpawned = 8;

// Any thread. Queue a spawn of `type` a short step in front of Marina.
void request_spawn(uint16_t type);
// Any thread. Remove every object spawned through this module.
void request_clear();

// Number of spawned objects still alive, and the last request's outcome
// ("Spawned type 0x18 in slot 121", "No free slot", ...), for the menu.
int spawned_count();
const char* last_result();

// Game thread, once per frame.
void tick(uint8_t* rdram);

// Game thread. Spawn now; returns the slot or -1. World offsets are pixels
// relative to Marina (+y up).
int spawn_now(uint8_t* rdram, uint16_t type, int dx, int dy);

} // namespace mm::actors
