#include "custom_actors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

extern "C" void func_80012AB4(uint8_t* rdram, recomp_context* ctx);

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
constexpr uint32_t kPlatform = 0x00002000u;     // stand-on-top only, 8 px thick (the pot)
// Grabbable object states, as the game's own objects use them:
constexpr uint32_t kRestFlags = 0x00011403u | kSolid;  // lying still, can be grabbed and stood on
constexpr uint32_t kHeldFlags = 0x00020503u;           // a held brown carry block's flags
constexpr uint32_t kFlyingFlags = 0x00021483u;         // a thrown enemy's flags

// Scratch stack for calling the game's leaf collision tests from the host
// (host-only expansion memory, as mm::actors uses for func_8001E2D0).
constexpr uint32_t kScratchStack = 0x8045FF00u;

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

// Is there terrain or a solid actor just under the block's bottom edge?
// Terrain: the game's func_80012AB4(x, y) (camera-relative point, returns the
// cell byte: 0xC0 solid, 0x40 platform). Actors: the solid / platform boxes
// of slots 0x00-0x8F, read from their current positions. (The game's own list
// test, func_8001FA78, uses boxes rebuilt earlier in the frame, before actors
// are shifted for camera movement, so while the camera moved - when Marina
// jumped - stacked blocks lost their support and poured down.)
bool supported(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    const int x = MEM_H(0x88, a), y = MEM_H(0x8C, a);
    const int left = x + MEM_H(0xAA, a) + 2, right = x + MEM_H(0xAC, a) - 2;
    const int bottom = y + MEM_H(0xB0, a);
    for (int px : {left, x, right}) {
        recomp_context ctx{};
        ctx.r29 = addr(kScratchStack);
        ctx.r4 = static_cast<gpr>(static_cast<int16_t>(px));
        ctx.r5 = static_cast<gpr>(static_cast<int16_t>(bottom - 1));
        func_80012AB4(rdram, &ctx);
        if ((ctx.r2 & 0xC0) != 0) return true;
    }
    for (int s = 0; s < 0x90; ++s) {
        if (s == slot) continue;
        const gpr o = slot_addr(s);
        const uint32_t flags = static_cast<uint32_t>(MEM_W(0x80, o));
        if ((flags & (kSolid | kPlatform)) == 0) continue;
        const int ox = MEM_H(0x88, o), oy = MEM_H(0x8C, o);
        const int top = oy + MEM_H(0xAE, o);
        if (right < ox + MEM_H(0xAA, o) || left > ox + MEM_H(0xAC, o)) continue;
        if (bottom - top <= 2 && top - bottom <= 3) return true;  // resting on its top
    }
    return false;
}

// Falling from old_bottom to new_bottom (camera-relative px, +y up) with
// the block's columns at x: the highest floor met on the way (terrain cell
// top or a solid actor's top), or kNoFloor. Landing is done here rather than
// left to the terrain collision, which pushes a block that sinks into the
// edge of another sideways, so blocks slid off instead of stacking.
constexpr int kNoFloor = -100000;

bool terrain_solid(uint8_t* rdram, int x, int y) {
    recomp_context ctx{};
    ctx.r29 = addr(kScratchStack);
    ctx.r4 = static_cast<gpr>(static_cast<int16_t>(x));
    ctx.r5 = static_cast<gpr>(static_cast<int16_t>(y));
    func_80012AB4(rdram, &ctx);
    return (ctx.r2 & 0xC0) != 0;
}

int floor_between(uint8_t* rdram, int slot, int x, int old_bottom, int new_bottom) {
    const gpr a = slot_addr(slot);
    const int left = x + MEM_H(0xAA, a) + 2, right = x + MEM_H(0xAC, a) - 2;
    int best = kNoFloor;
    for (int px : {left, x, right}) {
        for (int py = old_bottom - 1; py >= new_bottom - 1; --py) {
            if (terrain_solid(rdram, px, py)) {
                if (!terrain_solid(rdram, px, py + 1)) best = std::max(best, py + 1);
                break;
            }
        }
    }
    for (int s = 0; s < 0x90; ++s) {
        if (s == slot) continue;
        const gpr o = slot_addr(s);
        if ((static_cast<uint32_t>(MEM_W(0x80, o)) & (kSolid | kPlatform)) == 0) continue;
        const int ox = MEM_H(0x88, o);
        if (right < ox + MEM_H(0xAA, o) || left > ox + MEM_H(0xAC, o)) continue;
        const int top = MEM_H(0x8C, o) + MEM_H(0xAE, o);
        if (top <= old_bottom + 2 && top >= new_bottom) best = std::max(best, top);
    }
    return best;
}

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

    // At rest the block does not move at all while something holds it up:
    // nudging it down by gravity every frame would sink it into what it
    // stands on, and the terrain collision then pushes it out sideways too,
    // which made stacks and rows of blocks slide apart.
    Scratch& sc = g_scratch[slot];
    if ((MEM_HU(0xD0, a) & 0xFF) == 0x01) {
        MEM_W(0xEC, a) = 0;
        MEM_W(0xF0, a) = 0;
        if (supported(rdram, slot)) return;
        MEM_H(0xD0, a) = 0x0171;                 // support gone: fall (not solid meanwhile)
        MEM_W(0x80, a) = static_cast<int32_t>(kFlyingFlags);
        sc.frames = 0;
    }

    // Flying or falling: move by the velocity under gravity. The terrain
    // collision that runs after this update pushes the block back out of
    // anything solid, so landing shows up as no vertical movement next frame.
    const int32_t x = MEM_W(0x88, a), y = MEM_W(0x8C, a);
    const bool blocked_y = sc.frames > 0 && std::abs(y - sc.base_y) < 0x4000 && MEM_W(0xF0, a) < 0;
    ++sc.frames;
    sc.base_y = y;
    int32_t vx = MEM_W(0xEC, a), vy = MEM_W(0xF0, a);
    if (blocked_y && supported(rdram, slot)) {
        MEM_W(0xEC, a) = 0;                      // landed: rest, solid again
        MEM_W(0xF0, a) = 0;
        MEM_H(0xD0, a) = 0x0001;
        MEM_W(0x80, a) = static_cast<int32_t>(kRestFlags);
        return;
    }
    if (blocked_y) vx /= 2;                      // scraping along: slow down
    vy = std::max(vy - kGravity, kMaxFall);
    if (vy < 0) {
        const int nx = (x + vx) >> 16;
        const int bottom = (y >> 16) + MEM_H(0xB0, a);
        const int floor = floor_between(rdram, slot, nx, bottom, bottom + (vy >> 16) - 1);
        if (floor != kNoFloor) {
            MEM_W(0x88, a) = x + vx;             // land exactly on top of it
            MEM_W(0x8C, a) = (floor - MEM_H(0xB0, a)) << 16;
            MEM_W(0xEC, a) = 0;
            MEM_W(0xF0, a) = 0;
            MEM_H(0xD0, a) = 0x0001;
            MEM_W(0x80, a) = static_cast<int32_t>(kRestFlags);
            return;
        }
    }
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
