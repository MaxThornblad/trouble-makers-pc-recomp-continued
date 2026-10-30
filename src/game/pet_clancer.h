// Cat Clancer (3-11's dodgeball cat, type 0x2801) away from 3-11: a pet.
//
// His own code is the dodgeball match: it stands him on the arena's floor
// (no gravity in his standing and running states), runs him to arena marks
// (x > 80: turn), and looks for dodgeballs in actor slots 0x40..0x43, which
// in any other stage hold whatever that stage has. Spawned elsewhere he ran
// to the stage's end in the air. Here his states are chosen for him each
// frame, out of the ones he already has (idle, run, jump, fall, land, held,
// thrown): he falls to the ground, follows the nearest player and stops
// near her, jumps at a wall in his way or up to a player above him, and can
// be picked up and thrown. All of it from game memory (his record), the
// same on both machines online.
#pragma once

#include <cstdint>

namespace mm::pet_clancer {

// Around his update, outside 3-11, once he is built (state >= 0x20).
// before() picks his state; after() keeps what his match code undid.
void before(uint8_t* rdram, int slot);
void after(uint8_t* rdram, int slot);

} // namespace mm::pet_clancer
