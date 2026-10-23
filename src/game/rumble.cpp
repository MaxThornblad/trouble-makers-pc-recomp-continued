#include "rumble.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "cheats.h"
#include "mm_audio_input.hpp"
#include "recomp.h"
#include "savestate.h"

namespace mm::rumble {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 and 10 = stage gameplay
constexpr uint32_t kGamePaused = 0x800BE4E8u;
constexpr uint32_t kCurrentStage = 0x80178162u;  // progression row
constexpr uint32_t kGoldGemsHi = 0x80171B10u;    // rows 32..63
constexpr uint32_t kGoldGemsLo = 0x80171B14u;    // rows 0..31
constexpr uint32_t kMarina = 0x800EF510u;

constexpr uint16_t kGroundJetState = 0x020Bu;
constexpr uint16_t kAirJetState = 0x0110u;
constexpr int32_t kHardLanding = 0x40000;        // 4 px/frame downward (16.16)

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

// Airborne: the jump/fall family (low byte 0x16) and the air jet.
bool airborne(uint16_t state) {
    return (state & 0xFFu) == 0x16u || state == kAirJetState;
}

bool gold_gem(uint8_t* rdram, int row) {
    if (row < 0 || row >= 64) return false;
    const uint32_t word = static_cast<uint32_t>(MEM_W(0, addr(row < 32 ? kGoldGemsLo : kGoldGemsHi)));
    return ((word >> (row & 31)) & 1u) != 0;
}

// strength 0..1 for each motor, scaled by the user's setting.
void pulse(float low, float high, uint32_t ms) {
    static const float kScale[] = {0.0f, 0.35f, 0.65f, 1.0f};
    const int level = std::clamp(mm::cheats::rumble_level(), 0, 3);
    const float scale = kScale[level];
    if (scale <= 0.0f) return;
    const auto motor = [scale](float v) {
        return static_cast<uint16_t>(std::clamp(v * scale, 0.0f, 1.0f) * 65535.0f);
    };
    mm_audio_input::request_rumble(motor(low), motor(high), ms);
}

struct Watch {
    bool valid = false;
    int hp = 0;
    uint16_t state = 0;
    int row = -1;
    bool gem = false;
    int32_t fall_speed = 0;      // fastest downward speed this time in the air
    uint32_t load_generation = 0;
    int since_hit = 1000;        // frames since the last damage pulse
};
Watch g_watch;

} // namespace

void tick(uint8_t* rdram) {
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    const bool gameplay = (game_state == 6 || game_state == 10) &&
                          MEM_HU(0, addr(kGamePaused)) == 0;
    const uint32_t generation = mm::savestate::load_generation();
    if (!gameplay || mm::cheats::rumble_level() == 0 ||
        generation != g_watch.load_generation) {
        g_watch.valid = false;
        g_watch.load_generation = generation;
        return;
    }

    const gpr marina = addr(kMarina);
    const int hp = MEM_H(0xE0, marina);
    const uint16_t state = MEM_HU(0xD0, marina);
    const int row = MEM_HU(0, addr(kCurrentStage));
    const bool gem = gold_gem(rdram, row);
    const int32_t vy = MEM_W(0xF0, marina);

    if (g_watch.valid && row == g_watch.row) {
        // Damage, scaled by the hit (a lava touch takes 100 of 1000).
        if (hp < g_watch.hp) {
            g_watch.since_hit = 0;
            if (hp <= 0) {
                pulse(1.0f, 1.0f, 700);
            } else {
                const int lost = g_watch.hp - hp;
                pulse(1.0f, 0.8f, static_cast<uint32_t>(std::clamp(140 + lost, 160, 420)));
            }
        }
        // Golden gem collected.
        else if (gem && !g_watch.gem) {
            pulse(0.6f, 1.0f, 450);
        }
        // Landing after a long fall.
        else if (airborne(g_watch.state) && !airborne(state) &&
                 g_watch.fall_speed >= kHardLanding && g_watch.since_hit > 45) {
            const float speed = static_cast<float>(g_watch.fall_speed) / 65536.0f;
            pulse(std::clamp(speed / 10.0f, 0.35f, 1.0f), 0.2f,
                  static_cast<uint32_t>(std::clamp(speed * 18.0f, 70.0f, 200.0f)));
        }
        // Jet dash start.
        else if ((state == kGroundJetState || state == kAirJetState) &&
                 state != g_watch.state && g_watch.state != kGroundJetState &&
                 g_watch.state != kAirJetState) {
            pulse(0.15f, 0.55f, 90);
        }
    }

    g_watch.fall_speed = airborne(state)
        ? std::max(airborne(g_watch.state) ? g_watch.fall_speed : 0, -vy)
        : 0;
    g_watch.since_hit = std::min(g_watch.since_hit + 1, 1000);
    g_watch.valid = true;
    g_watch.hp = hp;
    g_watch.state = state;
    g_watch.row = row;
    g_watch.gem = gem;
}

} // namespace mm::rumble
