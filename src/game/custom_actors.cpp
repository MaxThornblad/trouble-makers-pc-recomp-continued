#include "custom_actors.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "grab_blocks.h"

namespace mm::custom_actors {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorStride = 0x198u;
constexpr uint32_t kBlockSprites = 0x800D1968u;  // block sprite per colour (item handler type 1)
constexpr uint16_t kDrawSprite = 0x208;          // +0x94: draw the sprite at +0x18C
// +0x80 flags for a grabbable object, as the game's own objects use them:
constexpr uint32_t kFreeFlags = 0x00011403u;     // lying free, can be grabbed
constexpr uint32_t kHeldFlags = 0x00020123u;     // a held enemy's flags
constexpr uint32_t kFlyingFlags = 0x00021483u;   // a thrown enemy's flags

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
    int32_t last_x = 0, last_y = 0;              // loose block: last position
    int still = 0;                               // loose block: frames at rest
};
std::array<Scratch, 208> g_scratch;

// --- Test block: a block sprite that bobs where it was spawned -------------

void test_block_setup(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
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

// --- Loose block: a block Marina carries and throws -------------------------
// Held protocol seen with a held enemy: Marina keeps the slot at her +0xD6 and
// flag 0x02000000 while holding; the held actor sits in state 0x?61 and
// Marina's code moves it; on release her code sets the held actor to state
// 0x0170 with a velocity, and the actor flies on its own (0x0171).

void loose_block_setup(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    MEM_H(0x94, a) = kDrawSprite;
    MEM_W(0x18C, a) = MEM_W(4 * 4, addr(kBlockSprites));
    MEM_W(0x80, a) = static_cast<int32_t>(kFreeFlags);
    MEM_W(0x110, a) = 0;                         // no item record (grab_blocks sets one)
    g_scratch[slot] = Scratch{};
}

// Frees the slot (the actor disappears next frame).
void remove(uint8_t* rdram, int slot) {
    MEM_W(0x80, slot_addr(slot)) = 0;
    g_scratch[slot] = Scratch{};
}

void start_falling(uint8_t* rdram, gpr a, int32_t vx, int32_t vy) {
    MEM_W(0xEC, a) = vx;
    MEM_W(0xF0, a) = vy;
    MEM_H(0xD0, a) = 0x0170;
    MEM_W(0x80, a) = static_cast<int32_t>(kFlyingFlags);
}

void loose_block_update(uint8_t* rdram, recomp_context* /*ctx*/, int slot) {
    const gpr a = slot_addr(slot);
    const uint16_t state = MEM_HU(0xD0, a);
    Scratch& sc = g_scratch[slot];
    ++sc.frames;
    const bool was_held = (state & 0xFF) == 0x61;
    // Grab handshake: Marina's grab (func_800528F4) sets bit 0x200 at +0x98
    // and the hold point at +0x104/+0x108 every frame she holds us, and treats
    // a bit still standing on her next check as "let go", so the held actor
    // clears it each frame to stay held. Marina (slot 0) updates before us.
    const bool holder_set = (MEM_W(0x98, a) & 0x200) != 0;
    if (holder_set) {
        MEM_W(0x98, a) = MEM_W(0x98, a) & ~0x200;
        if (!was_held) {
            MEM_H(0xD0, a) = 0x0161;
            MEM_W(0x80, a) = static_cast<int32_t>(kHeldFlags);
        }
    }
    // Thrown: the holder sets 0x400 at +0x98 and leaves the throw velocity at
    // +0xF8/+0xFC.
    if ((MEM_W(0x98, a) & 0x400) != 0) {
        MEM_W(0x98, a) = MEM_W(0x98, a) & ~0x400;
        start_falling(rdram, a, MEM_W(0xF8, a), MEM_W(0xFC, a));
    } else if (was_held && !holder_set) {
        start_falling(rdram, a, 0, 0);           // set down / let go without a throw
    }
    const uint16_t now = MEM_HU(0xD0, a);
    if ((now & 0xFF) == 0x61) {                  // carried: follow the hold point
        MEM_W(0x88, a) = MEM_W(0x104, a);
        MEM_W(0x8C, a) = MEM_W(0x108, a);
        sc.still = 0;
        return;
    }
    const bool from_record = MEM_W(0x110, a) != 0;
    if ((now & 0xFF) == 0x70 || (now & 0xFF) == 0x71) {
        MEM_H(0xD0, a) = 0x0171;
        // Terrain collision (func_80016CB4, flag bit 0x2) runs after the
        // update and pushes the block out of floors and walls, so a block at
        // rest shows the same position frame after frame.
        const int32_t x = MEM_W(0x88, a), y = MEM_W(0x8C, a);
        if (std::abs(x - sc.last_x) < 0x8000 && std::abs(y - sc.last_y) < 0x8000) {
            ++sc.still;
        } else {
            sc.still = 0;
        }
        sc.last_x = x;
        sc.last_y = y;
        MEM_W(0x88, a) = x + MEM_W(0xEC, a);
        MEM_W(0x8C, a) = y + MEM_W(0xF0, a);
        MEM_W(0xF0, a) = std::max<int32_t>(MEM_W(0xF0, a) - 0x6000, -0x60000);
        const bool lost = sc.frames > 60 * 20 ||
                          std::abs(MEM_H(0x88, a)) > 600 || MEM_H(0x8C, a) < -400;
        if (from_record && (sc.still >= 6 || lost)) {
            if (mm::grab_blocks::settle(rdram, slot, lost)) remove(rdram, slot);
        }
        return;
    }
    // Free and never picked up: a block that came from the stage goes back
    // if Marina's grab missed it.
    if (from_record && sc.frames > 20) {
        if (mm::grab_blocks::settle(rdram, slot, true)) remove(rdram, slot);
    }
}

std::array<TypeInfo, 256> make_table() {
    std::array<TypeInfo, 256> t{};
    t[kTestBlock] = {"Test block (custom type)", test_block_update, test_block_setup};
    t[kLooseBlock] = {"Loose block", loose_block_update, loose_block_setup};
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
