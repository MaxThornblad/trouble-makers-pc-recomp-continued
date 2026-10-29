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
constexpr uint32_t kCreditFrom = 0x807F063Cu;      // s16: player 1's health when a pickup went to her for player 2
constexpr uint32_t kCreditFrames = 0x807F063Eu;    // s16: frames that credit stays open
constexpr uint32_t kSavedHealth = 0x807F0640u;     // s16: player 2's health, kept for her next scene
constexpr uint32_t kSavedStage = 0x807F0642u;      // u16: ... of this stage
constexpr uint32_t kComeBack = 0x807F0644u;        // s32: the next join is a come-back (half health)

// +0x80 bits a downed character loses: drawn, attack box, hit box.
constexpr uint32_t kHiddenBits = 0x00000001u | 0x00000080u | 0x00000400u;
constexpr uint16_t kFall = 0x0216;                 // Marina's fall state
constexpr uint16_t kCarried = 0x2F;                // (low byte) held by the other player

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

void credit_to_p2(uint8_t* rdram) {
    if (MEM_H(0, addr(kCreditFrames)) > 0) return;   // already open: the first snapshot stands
    MEM_H(0, addr(kCreditFrom)) = MEM_H(0xE0, record(0));
    MEM_H(0, addr(kCreditFrames)) = 3;               // item records go this frame, gem actors next
}

void tick(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (p2 <= 0) {
        MEM_H(0, addr(kCreditFrames)) = 0;
        return;
    }
    // Health player 1 gained from player 2's pickups is hers.
    if (int16_t frames = MEM_H(0, addr(kCreditFrames)); frames > 0) {
        const gpr m = record(0), b = record(p2);
        const int16_t now = MEM_H(0xE0, m);
        const int gain = now - MEM_H(0, addr(kCreditFrom));
        if (gain > 0 && now > 0 && !is_down(rdram)) {
            MEM_H(0xE0, m) = static_cast<int16_t>(now - gain);
            MEM_H(0xE0, b) = static_cast<int16_t>(std::min<int>(full(rdram), MEM_H(0xE0, b) + gain));
        }
        MEM_H(0, addr(kCreditFrom)) = MEM_H(0xE0, m);
        MEM_H(0, addr(kCreditFrames)) = static_cast<int16_t>(frames - 1);
    }
    // Kept for her next scene of this stage (she joins each scene afresh).
    if (!is_down(rdram) && MEM_H(0xE0, record(p2)) > 0 && gameplay(rdram)) {
        MEM_H(0, addr(kSavedHealth)) = MEM_H(0xE0, record(p2));
        MEM_H(0, addr(kSavedStage)) = static_cast<int16_t>(MEM_HU(0, addr(kStage)));
    }
}

} // namespace mm::player2::health
