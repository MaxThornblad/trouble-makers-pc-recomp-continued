#include "player2_health.h"

#include <algorithm>
#include <cstdio>

#include "cheats.h"
#include "custom_actors.h"
#include "far_coop.h"
#include "frame_input.h"
#include "player2.h"
#include "player2_internal.h"

#include "librecomp/game.hpp"

namespace mm::player2::health {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint32_t kGameState = 0x800BE4F0u;       // 6 and 10 = stage gameplay
constexpr uint32_t kStage = 0x800D28E4u;           // current stage id
constexpr uint32_t kHoldingFlag = 0x02000000u;     // +0x80: carrying (slot at +0xD6)
constexpr uint32_t kFullHealth = 0x807F040Cu;      // the most health either player has had (player2.cpp)

// Player 2's block (layout in player2.cpp's header).
constexpr uint32_t kDown = 0x807F0638u;            // s32: frames until the downed character comes back, 0 = up
constexpr uint32_t kSavedHealth = 0x807F0640u;     // s16: player 2's health, kept for her next scene
constexpr uint32_t kSavedStage = 0x807F0642u;      // u16: ... of this stage
constexpr uint32_t kComeBack = 0x807F0644u;        // s32: the next join is a come-back (half health)
constexpr uint32_t kGemMarks = 0x807F0650u;        // 0xD0 bits: gem actors she touched, not yet collected

// +0x80 bits a downed character loses: drawn, attack box, hit box.
constexpr uint32_t kHiddenBits = 0x00000001u | 0x00000080u | 0x00000400u;
constexpr uint16_t kFall = 0x0216;                 // Marina's fall state
constexpr uint16_t kCarried = 0x2F;                // (low byte) held by the other player
constexpr uint16_t kGemActor = 8;

// A pickup collected for player 2 (pickup_begin / pickup_end): within one
// collection, never across frames.
struct Pickup {
    bool on = false;
    int16_t p1 = 0;    // player 1's own health, put back after
    int p2 = -1;
};
Pickup g_pickup;
constexpr int kSlots = 0xD0;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}
gpr record(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kStride);
}
bool gameplay(uint8_t* rdram) {
    const uint16_t s = MEM_HU(0, addr(kGameState));
    return s == 6 || s == 10;
}
// A gem actor's mark: set when player 2 touches it, kept until it is
// collected (it may not be catchable yet: one knocked out of an enemy
// waits a few frames) or gone.
bool marked(uint8_t* rdram, int actor) {
    return (MEM_BU(actor >> 3, addr(kGemMarks)) >> (actor & 7)) & 1;
}
void set_mark(uint8_t* rdram, int actor, bool on) {
    const gpr b = addr(kGemMarks);
    const uint8_t bit = static_cast<uint8_t>(1u << (actor & 7));
    const uint8_t v = MEM_BU(actor >> 3, b);
    MEM_B(actor >> 3, b) = static_cast<int8_t>(on ? (v | bit) : (v & ~bit));
}
bool gem_actor(uint8_t* rdram, int actor) {
    const gpr r = record(actor);
    return MEM_W(0x80, r) != 0 && MEM_HU(0xD2, r) == kGemActor;
}

void log(const char* what) {
    if (!mm::faraway::in_shadow()) {
        std::fprintf(stderr, "[player2] %s (frame %ld)\n", what, mm::frame_input::number());
    }
}

// Neither carries the other any more (one of them is going down).
void let_go(uint8_t* rdram, int p2) {
    for (int s : {0, p2}) {
        const gpr r = record(s);
        const int other = s == 0 ? p2 : 0;
        if ((static_cast<uint32_t>(MEM_W(0x80, r)) & kHoldingFlag) == 0 || MEM_HU(0xD6, r) != other) continue;
        MEM_W(0x80, r) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, r)) & ~kHoldingFlag);
        MEM_H(0xD6, r) = 0;
        const gpr held = record(other);
        if ((MEM_HU(0xD0, held) & 0xFF) == kCarried) MEM_H(0xD0, held) = static_cast<int16_t>(kFall);
    }
}

// While down: out of the picture and out of reach, with the other character.
void stay_down(uint8_t* rdram, int slot) {
    const gpr me = record(slot);
    MEM_W(0x80, me) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, me)) & ~kHiddenBits);
    MEM_H(0xE0, me) = 0;
    follow_lead(rdram, slot);
}

} // namespace

int16_t full(uint8_t* rdram) {
    return std::max<int16_t>(1, MEM_H(0, addr(kFullHealth)));
}

int16_t joining(uint8_t* rdram) {
    MEM_W(0, addr(kDown)) = 0;
    if (MEM_W(0, addr(kComeBack)) != 0) {
        MEM_W(0, addr(kComeBack)) = 0;
        return std::max<int16_t>(1, full(rdram) / 2);
    }
    if (MEM_HU(0, addr(kSavedStage)) == MEM_HU(0, addr(kStage)) && MEM_H(0, addr(kSavedHealth)) > 0) {
        return MEM_H(0, addr(kSavedHealth));
    }
    return std::max(full(rdram), MEM_H(0xE0, record(0)));
}

bool is_down(uint8_t* rdram) {
    return MEM_W(0, addr(kDown)) != 0;
}

void frame_begin(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (p2 <= 0 || is_down(rdram) || !gameplay(rdram)) return;
    if (MEM_H(0xE0, record(0)) > 0 || MEM_H(0xE0, record(p2)) <= 0) return;
    // Slot 0's character is down, the other is up: the game must not see a
    // defeated Marina, so the two swap and the downed one goes down in
    // player 2's slot.
    let_go(rdram, p2);
    swap_lead_for_down(rdram);
}

bool down_update(uint8_t* rdram, int slot) {
    int32_t left = MEM_W(0, addr(kDown));
    if (left == 0) {
        if (MEM_H(0xE0, record(slot)) > 0) return false;
        let_go(rdram, slot);
        left = kDownFrames;
        log("down");
    }
    // Both down: the game's own defeat runs (for slot 0); she waits.
    if (MEM_H(0xE0, record(0)) > 0 && --left == 0) {
        MEM_W(0, addr(kDown)) = 0;
        MEM_W(0, addr(kComeBack)) = 1;
        log("back next to the other player");
        come_back(rdram, slot);
        return false;
    }
    MEM_W(0, addr(kDown)) = left;
    stay_down(rdram, slot);
    return true;
}

void pickup_begin(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (g_pickup.on || p2 <= 0 || is_down(rdram)) return;
    const gpr m = record(0);
    g_pickup = {true, MEM_H(0xE0, m), p2};
    MEM_H(0xE0, m) = MEM_H(0xE0, record(p2));
}

void pickup_end(uint8_t* rdram) {
    if (!g_pickup.on) return;
    g_pickup.on = false;
    const gpr m = record(0);
    const int16_t healed = MEM_H(0xE0, m);
    MEM_H(0xE0, m) = g_pickup.p1;
    if (slot(rdram) == g_pickup.p2 && healed > MEM_H(0xE0, record(g_pickup.p2))) {
        MEM_H(0xE0, record(g_pickup.p2)) = healed;
    }
}

void gem_actor_touched(uint8_t* rdram, int actor) {
    if (actor > 0 && actor < kSlots) set_mark(rdram, actor, true);
}

void actor_begin(uint8_t* rdram, int actor) {
    pickup_end(rdram);
    if (actor <= 0 || actor >= kSlots || !marked(rdram, actor)) return;
    if (!gem_actor(rdram, actor)) {
        set_mark(rdram, actor, false);   // collected (or gone) since
        return;
    }
    pickup_begin(rdram);
}

void tick(uint8_t* rdram) {
    pickup_end(rdram);   // (none is left open past the actor loop)
    for (int a = 1; a < kSlots; ++a) {
        if (marked(rdram, a) && !gem_actor(rdram, a)) set_mark(rdram, a, false);
    }
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    // Kept for her next scene of this stage (she joins each scene afresh).
    if (!is_down(rdram) && MEM_H(0xE0, record(p2)) > 0 && gameplay(rdram)) {
        MEM_H(0, addr(kSavedHealth)) = MEM_H(0xE0, record(p2));
        MEM_H(0, addr(kSavedStage)) = static_cast<int16_t>(MEM_HU(0, addr(kStage)));
    }
}

} // namespace mm::player2::health
