// Custom actor table: new actor types whose behaviour is written in C++.
//
// The game runs every active actor once a frame from func_8001751C: types
// below 0x100 call their entry in the shared table at 0x800C7FE0, higher
// types go through func_80016E70, which picks a stage package's table by the
// type's high byte (banks 0x01-0x2C) and does nothing for any other bank.
// A generated hook at func_80016E70's entry hands bank 0x7F to this module,
// so types 0x7F00-0x7FFF form a table of up to 256 new actor types, available
// in every stage. func_8001E2D0 gives such a slot generic defaults (a 32x32
// hitbox, 10 health, active + drawn); each type's setup sets the rest.
//
// An actor draws whatever its fields describe: with image 0x46 at +0x84 and
// +0x94 = 0x208 the game's actor renderer draws the sprite whose pointer is
// at +0x18C (the same path a breaking block's flying piece uses), which is
// how custom types reuse the game's shared graphics. Collision comes from
// +0x80 flag bits 0x00040000 (solid) and 0x2000 (stand-on platform), which
// the game honours for slots below 0x90.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace mm::custom_actors {

constexpr uint16_t kBank = 0x7F;

constexpr uint16_t type_id(uint8_t index) {
    return static_cast<uint16_t>((kBank << 8) | index);
}

// Custom types defined so far.
enum : uint8_t {
    kTestBlock = 0x00,   // a block that bobs in place: proves the table works
    kBlock = 0x01,       // a solid block object Marina can carry and throw (grabbable blocks)
    kPlayer2 = 0x02,     // a second Marina (player2.cpp)
    kPlayer2Mount = 0x03,   // what player 2 rides where Marina rides something (player2_mount.cpp)
    kPlayer1Mount = 0x04,   // player 1's cat from the "Ride the cat" cheat (player2_mount.cpp)
};

// Per-frame update, called with the actor's slot (and the game's context, so
// it may call recompiled game functions).
using UpdateFn = void (*)(uint8_t* rdram, recomp_context* ctx, int slot);

// Setup right after func_8001E2D0 when spawned through mm::actors.
using SetupFn = void (*)(uint8_t* rdram, int slot);

struct TypeInfo {
    const char* name;
    UpdateFn update;
    SetupFn setup;
};

// Turn a kBlock into a slope block: at rest it writes these two terrain
// cell codes (left, right) instead of being a solid box.
void make_slope(uint8_t* rdram, int slot, uint8_t left_code, uint8_t right_code);

// nullptr for an index with no type.
const TypeInfo* info(uint16_t type);

} // namespace mm::custom_actors
