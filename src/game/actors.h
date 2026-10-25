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

// Not a game type: request_spawn() of this builds the desert stages' clanball
// machine (a face clanball and its controller, see spawn_clanball_machine).
constexpr uint16_t kClanballMachine = 0xFFF0;

// Any thread. Queue a spawn of `type` a short step in front of Marina.
// `param` is the stage item parameter (colour, variant) the type's setup
// reads: stored as a float at +0x110 before the setup runs, as the game's
// spawners do.
void request_spawn(uint16_t type, uint32_t param = 0);
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
int spawn_now(uint8_t* rdram, uint16_t type, int dx, int dy, uint32_t param = 0);
// Game thread. The same into a given (free) slot, for objects that work in
// fixed slot patterns (see the clanball machine in spawn_kits.cpp).
void spawn_in(uint8_t* rdram, int slot, uint16_t type, int dx, int dy, uint32_t param = 0);
// Game thread. First slot of `count` consecutive free slots from 0x30 up to
// `last` (the highest such run), or -1.
int free_run(uint8_t* rdram, int count, int last);

// Game thread. The rideable clanball machine of the desert stages: a face
// clanball (0x3F, item parameter 0x403) with its controller (0x181D) in the
// next slot; grabbing the clanball builds the machine in the 12 slots after
// the controller. Its code is in the stage package the desert stages load
// (bank 0x18, overlay at 0x801A6900), so elsewhere this returns nullptr
// without spawning; on success the clanball's slot is returned in *slot.
// Returns an error text, or nullptr.
const char* spawn_clanball_machine(uint8_t* rdram, int dx, int dy, int* slot);
// Game thread. Whether the current stage has the machine's code loaded.
bool clanball_machine_loaded(uint8_t* rdram);

} // namespace mm::actors
