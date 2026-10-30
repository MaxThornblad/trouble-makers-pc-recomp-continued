// Stage objects outside their stage (cheats: the 3-12 cat to ride anywhere,
// Lunar's missiles, 3-11's Cat Clancer).
//
// Types 0x100 and up belong to a stage's code package: the bank (the type's
// high byte) picks a dispatch table that func_80016E70 calls through, and the
// table and the code are DMA'd from ROM into a fixed place when a stage that
// uses them loads (3-12's bank 0x0F at 0x80192000, 3-11's bank 0x28 at
// 0x801B0800). Elsewhere that place holds another stage's package, so a
// 3-12 type in 1-1 would jump into 1-1's code.
//
// A foreign package is kept whole (table, code, data and bss) in expansion
// memory, made from the ROM the first time it is needed in a stage. For one
// of its objects' updates, it is swapped into its place: the stage's bytes
// there are put aside, the package's live copy goes in, the runtime maps its
// functions, the game's own update runs, then the package's state goes back
// to its copy and the stage's bytes and functions come back. Everything the
// package keeps lives in game memory, so save states, replays, rollback and
// the second machine carry it.
#pragma once

#include <cstdint>
#include <functional>

#include "recomp.h"

namespace mm::foreign_actors {

// A type whose package this module can bring in (whatever stage this is).
bool known(uint16_t type);

// The type's package is the one loaded now (its own stage): no swap needed.
bool native(uint8_t* rdram, uint16_t type);

// Runs `body` with the type's package in place (at once when it is native).
// False when the type is not known.
bool with_package(uint8_t* rdram, uint16_t type, const std::function<void(uint8_t*)>& body);

// Generated hook at func_80016E70's entry (via custom_actors.cpp): a foreign
// object's update, with its package swapped in. True when handled.
bool dispatch(uint8_t* rdram, recomp_context* ctx, int slot);

// Inside a swapped-in update (so other host code can tell).
bool swapped_in();

// Slots an object of the type takes from its own on: its first update
// builds its parts in the slots after it, whatever is in them (Cat Clancer
// writes 13, so spawned in a busy part of the table it overwrote player 2).
// 1 for most.
int span(uint16_t type);

// Right after spawning an object of another stage (actors.cpp): what its
// stage's spawner fills in that its own setup does not (Lunar's missile's
// picture).
void setup(uint8_t* rdram, int slot);

// Where a word of the type's package lives right now: at its own address
// while the package is in place (its stage, or swapped in), else in its
// kept copy (made now if need be). For code that talks to a package's
// globals from outside its objects' updates.
uint32_t live(uint8_t* rdram, uint16_t type, uint32_t vaddr);

} // namespace mm::foreign_actors
