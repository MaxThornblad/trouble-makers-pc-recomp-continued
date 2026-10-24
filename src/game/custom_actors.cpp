#include "custom_actors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace mm::custom_actors {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorStride = 0x198u;
constexpr uint32_t kBlockSprites = 0x800D1968u;  // block sprite per colour (item handler type 1)
// Drawing: the game's flying block piece (func_800441F4, spawned when a red
// block breaks) is an actor with image 0x46 at +0x84, draw mode 0x208 at
// +0x94 and the block's sprite pointer at +0x18C; the same three fields draw
// any block sprite on a custom actor.
constexpr uint16_t kBlockImage = 0x46;
constexpr uint16_t kDrawSprite = 0x208;

// +0x80 flag bits that make an actor collide (func_8001F88C rebuilds two
// lists every frame from slots 0x00-0x8F; func_8001FA78 tests a point
// against them for every actor's terrain collision, Marina's included):
constexpr uint32_t kSolid = 0x00040000u;         // solid box on all sides (hit box AA..AC, B0..AE)
[[maybe_unused]] constexpr uint32_t kPlatform = 0x00002000u;     // stand-on-top only, 8 px thick (the pot)
// Grabbable object states, as the game's own objects use them:
constexpr uint32_t kRestFlags = 0x00011403u | kSolid;  // lying still, can be grabbed and stood on
constexpr uint32_t kHeldFlags = 0x00020503u;           // a held brown carry block's flags
constexpr uint32_t kFlyingFlags = 0x00021483u;         // a thrown enemy's flags

constexpr int32_t kGravity = 0x6000;             // Q16.16 per frame
constexpr int32_t kMaxFall = -0x60000;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

// Host-side per-slot scratch for custom types (frame counters, anchors).
struct Scratch {
    int frames = 0;
    int32_t base_y = 0;
};
std::array<Scratch, 208> g_scratch;

// --- Test block: a block sprite that bobs where it was spawned -------------

void test_block_setup(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    MEM_H(0x84, a) = kBlockImage;
    MEM_H(0x94, a) = kDrawSprite;
    MEM_W(0x18C, a) = MEM_W(4 * 4, addr(kBlockSprites));
    g_scratch[slot] = Scratch{0, MEM_W(0x8C, a)};
}

void test_block_update(uint8_t* rdram, recomp_context* /*ctx*/, int slot) {
    const gpr a = slot_addr(slot);
    Scratch& s = g_scratch[slot];
    if (s.frames == 0 && s.base_y == 0) s.base_y = MEM_W(0x8C, a);
    ++s.frames;
    const double phase = s.frames * (2.0 * 3.14159265358979 / 90.0);
    MEM_W(0x8C, a) = s.base_y + static_cast<int32_t>(std::sin(phase) * 4.0 * 65536.0);
}

// --- Block: a solid block object Marina can carry and throw ----------------
//
// States (+0xD0 low byte): 0x01 resting / falling, 0x61 held, 0x71 flying.
// Resting it is solid on all sides (kSolid), so Marina and other actors
// stand on it and bump into it, and blocks stack. Held or flying it is not
// solid, so it never shoves Marina. Its own terrain collision (flag bit 0x2,
// func_80016CB4) stops it at floors, walls and other solid actors.

void block_setup(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    MEM_H(0x84, a) = kBlockImage;
    MEM_H(0x94, a) = kDrawSprite;
    MEM_W(0x18C, a) = MEM_W(4 * 4, addr(kBlockSprites));
    MEM_W(0x80, a) = static_cast<int32_t>(kRestFlags);
    MEM_H(0xD0, a) = 0x0001;
    g_scratch[slot] = Scratch{};
}

void block_update(uint8_t* rdram, recomp_context* /*ctx*/, int slot) {
    const gpr a = slot_addr(slot);
    const uint16_t state = MEM_HU(0xD0, a) & 0xFF;
    // Unbreakable: hits (a projectile, or Marina being hurt while holding it)
    // take health, and Marina will not hold or grab an actor at 0 health.
    MEM_H(0xE0, a) = 10;
    // Grab handshake: Marina's grab (func_800528F4) sets bit 0x200 at +0x98
    // and the hold point at +0x104/+0x108 every frame she holds us, and treats
    // a bit still standing on her next check as "let go", so the held actor
    // clears it each frame to stay held. Marina (slot 0) updates before us.
    const bool holder_set = (MEM_W(0x98, a) & 0x200) != 0;
    if (holder_set) {
        MEM_W(0x98, a) = MEM_W(0x98, a) & ~0x200;
        if (state != 0x61) {
            MEM_H(0xD0, a) = 0x0161;
            MEM_W(0x80, a) = static_cast<int32_t>(kHeldFlags);
        }
    }
    if ((MEM_W(0x98, a) & 0x400) != 0) {
        // Thrown: the holder sets 0x400 and leaves the velocity at +0xF8/+0xFC.
        MEM_W(0x98, a) = MEM_W(0x98, a) & ~0x400;
        MEM_W(0xEC, a) = MEM_W(0xF8, a);
        MEM_W(0xF0, a) = MEM_W(0xFC, a);
        MEM_H(0xD0, a) = 0x0171;
        MEM_W(0x80, a) = static_cast<int32_t>(kFlyingFlags);
    } else if (state == 0x61 && !holder_set) {
        // Set down (or let go without a throw): drop where it is.
        MEM_W(0xEC, a) = 0;
        MEM_W(0xF0, a) = 0;
        MEM_H(0xD0, a) = 0x0171;
        MEM_W(0x80, a) = static_cast<int32_t>(kFlyingFlags);
    }

    if ((MEM_HU(0xD0, a) & 0xFF) == 0x61) {      // carried: follow the hold point
        MEM_W(0x88, a) = MEM_W(0x104, a);
        MEM_W(0x8C, a) = MEM_W(0x108, a);
        MEM_W(0xEC, a) = 0;
        MEM_W(0xF0, a) = 0;
        return;
    }

    // Resting or flying: move by the velocity under gravity. The terrain
    // collision that runs after this update pushes the block back out of
    // anything solid, so a blocked axis shows up as no movement next frame.
    Scratch& sc = g_scratch[slot];
    const int32_t x = MEM_W(0x88, a), y = MEM_W(0x8C, a);
    const bool blocked_y = sc.frames > 0 && std::abs(y - sc.base_y) < 0x4000 && MEM_W(0xF0, a) < 0;
    ++sc.frames;
    sc.base_y = y;
    int32_t vx = MEM_W(0xEC, a), vy = MEM_W(0xF0, a);
    if (blocked_y) {
        vy = 0;                                  // on the ground: stop falling, slide to a halt
        vx = vx / 2;
        if (std::abs(vx) < 0x4000) vx = 0;
        if ((MEM_HU(0xD0, a) & 0xFF) != 0x01 && vx == 0) {
            MEM_H(0xD0, a) = 0x0001;             // landed: solid again
            MEM_W(0x80, a) = static_cast<int32_t>(kRestFlags);
        }
    }
    vy = std::max(vy - kGravity, kMaxFall);
    MEM_W(0x88, a) = x + vx;
    MEM_W(0x8C, a) = y + vy;
    MEM_W(0xEC, a) = vx;
    MEM_W(0xF0, a) = vy;
}

std::array<TypeInfo, 256> make_table() {
    std::array<TypeInfo, 256> t{};
    t[kTestBlock] = {"Bobbing block", test_block_update, test_block_setup};
    t[kBlock] = {"Block", block_update, block_setup};
    return t;
}
const std::array<TypeInfo, 256> kTable = make_table();

} // namespace

const TypeInfo* info(uint16_t type) {
    if ((type >> 8) != kBank) return nullptr;
    const TypeInfo& t = kTable[type & 0xFF];
    return t.update != nullptr ? &t : nullptr;
}

} // namespace mm::custom_actors

// Generated hook at func_80016E70's entry ($a0 = slot): run custom types and
// skip the game's bank switch for them.
extern "C" int mm_custom_actor_dispatch(uint8_t* rdram, recomp_context* ctx) {
    const int slot = static_cast<int>(ctx->r4 & 0xFFFF);
    if (slot <= 0 || slot >= 208) return 0;
    const gpr a = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + slot * 0x198u));
    const auto* t = mm::custom_actors::info(MEM_HU(0xD2, a));
    if (t == nullptr) return 0;
    t->update(rdram, ctx, slot);
    return 1;
}
