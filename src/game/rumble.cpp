#include "rumble.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "cheats.h"
#include "netplay.h"
#include "player2.h"
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
// Screen shake: the renderer offsets the view by these Q16.16 words while the
// game shakes the screen (the Sea of Lava eruption cycles 0,3,2,1 px across
// and 0,2,-1,1 px down every frame); both are zero otherwise.
constexpr uint32_t kShakeX = 0x800BE590u;
constexpr uint32_t kShakeY = 0x800BE594u;

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

long g_frame = 0;
// Per person (index 0: player 1's person, 1: player 2's).
long g_pulse_until[2] = {};      // frame the last event pulse ends
float g_pulse_strength[2] = {};

// The controller on this machine that a person (1 or 2) holds: online, this
// machine's own person plays on its first controller; locally, person 2 on
// the second (co-op). -1: not here.
int local_pad(int person) {
    if (mm::netplay::active()) return person == mm::netplay::player() ? 0 : -1;
    return person == 1 ? 0 : (mm::cheats::coop_enabled() ? 1 : -1);
}

// strength 0..1 for each motor, scaled by the user's setting; person 1 or 2,
// or 0 for everyone here (explosions, screen shakes, a golden gem).
void pulse(int person, float low, float high, uint32_t ms) {
    if (person == 0) {
        pulse(1, low, high, ms);
        pulse(2, low, high, ms);
        return;
    }
    const int pad = local_pad(person);
    if (pad < 0) return;
    long& until = g_pulse_until[person - 1];
    float& strength = g_pulse_strength[person - 1];
    // A weaker pulse never cuts a stronger one short.
    if (g_frame < until && std::max(low, high) < strength) return;
    until = g_frame + static_cast<long>(ms) * 60 / 1000;
    strength = std::max(low, high);
    static const float kScale[] = {0.0f, 0.35f, 0.65f, 1.0f};
    const int level = std::clamp(mm::cheats::rumble_level(), 0, 3);
    const float scale = kScale[level];
    if (scale <= 0.0f) return;
    const auto motor = [scale](float v) {
        return static_cast<uint16_t>(std::clamp(v * scale, 0.0f, 1.0f) * 65535.0f);
    };
    mm_audio_input::request_rumble(motor(low), motor(high), ms, pad);
}

// Sound effects that stand for impacts (ids passed to Sound_AddSfx), found by
// logging sounds while the events happen. Volume is the game's own (quieter
// for things far away), so distant explosions rumble less.
struct SfxRumble {
    uint16_t id;
    float low, high;
    uint16_t ms;
};
constexpr SfxRumble kSfxRumble[] = {
    {0x43, 1.0f, 0.7f, 280},     // bomb explosion
    {0x93, 0.6f, 0.4f, 110},     // red block blown apart (chain reactions)
};

bool g_gameplay = false;

// One person's character (hits, landings, jets).
struct Watch {
    bool valid = false;
    int hp = 0;
    uint16_t state = 0;
    int32_t fall_speed = 0;      // fastest downward speed this time in the air
    int since_hit = 1000;        // frames since the last damage pulse
};
// The stage (golden gem, screen shake), shared.
struct Shared {
    bool valid = false;
    int row = -1;
    bool gem = false;
    uint32_t load_generation = 0;
    int shake_frames = 0;        // frames left in the current shake window
    int shake_peak = 0;          // largest offset seen in it, Q16.16
};
Watch g_watch[2];
Shared g_shared;

// The record of the character a person (1 or 2) plays: slot 0, or player 2's
// slot (the lead swap trades them), 0 when there is none.
uint32_t character(uint8_t* rdram, int person) {
    const bool slot0 = (person == 1) != mm::player2::lead_swapped(rdram);
    if (slot0) return kMarina;
    const int p2 = mm::player2::slot(rdram);
    return p2 > 0 ? kMarina + static_cast<uint32_t>(p2) * 0x198u : 0;
}

void watch_character(uint8_t* rdram, int person) {
    Watch& w = g_watch[person - 1];
    const uint32_t rec = character(rdram, person);
    if (rec == 0) {
        w.valid = false;
        return;
    }
    const gpr a = addr(rec);
    const int hp = MEM_H(0xE0, a);
    const uint16_t state = MEM_HU(0xD0, a);
    const int32_t vy = MEM_W(0xF0, a);
    if (w.valid && g_shared.valid) {
        // Damage, scaled by the hit (a lava touch takes 100 of 1000).
        if (hp < w.hp) {
            w.since_hit = 0;
            if (hp <= 0) {
                pulse(person, 1.0f, 1.0f, 700);
            } else {
                const int lost = w.hp - hp;
                pulse(person, 1.0f, 0.8f, static_cast<uint32_t>(std::clamp(140 + lost, 160, 420)));
            }
        }
        // Landing after a long fall.
        else if (airborne(w.state) && !airborne(state) && w.fall_speed >= kHardLanding && w.since_hit > 45) {
            const float speed = static_cast<float>(w.fall_speed) / 65536.0f;
            pulse(person, std::clamp(speed / 10.0f, 0.35f, 1.0f), 0.2f,
                  static_cast<uint32_t>(std::clamp(speed * 18.0f, 70.0f, 200.0f)));
        }
        // Jet dash start.
        else if ((state == kGroundJetState || state == kAirJetState) && state != w.state &&
                 w.state != kGroundJetState && w.state != kAirJetState) {
            pulse(person, 0.15f, 0.55f, 90);
        }
    }
    w.fall_speed = airborne(state) ? std::max(airborne(w.state) ? w.fall_speed : 0, -vy) : 0;
    w.since_hit = std::min(w.since_hit + 1, 1000);
    w.valid = true;
    w.hp = hp;
    w.state = state;
}

} // namespace

void tick(uint8_t* rdram) {
    ++g_frame;
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    const bool gameplay = (game_state == 6 || game_state == 10) &&
                          MEM_HU(0, addr(kGamePaused)) == 0;
    g_gameplay = gameplay;
    const uint32_t generation = mm::savestate::load_generation();
    if (!gameplay || mm::cheats::rumble_level() == 0 || generation != g_shared.load_generation) {
        g_shared.valid = false;
        g_watch[0].valid = g_watch[1].valid = false;
        g_shared.load_generation = generation;
        return;
    }

    const int row = MEM_HU(0, addr(kCurrentStage));
    const bool gem = gold_gem(rdram, row);
    if (g_shared.valid && row != g_shared.row) g_shared.valid = false;   // a new stage
    watch_character(rdram, 1);
    watch_character(rdram, 2);
    // Golden gem collected (for both).
    if (g_shared.valid && gem && !g_shared.gem) pulse(0, 0.6f, 1.0f, 450);

    // Screen shake: a steady low rumble while it lasts, stronger for bigger
    // shakes. Re-issued in short overlapping pulses so it stops promptly when
    // the shake ends, and never over a stronger event pulse still running.
    const int shake = std::max(std::abs(static_cast<int>(MEM_W(0, addr(kShakeX)))),
                               std::abs(static_cast<int>(MEM_W(0, addr(kShakeY)))));
    if (shake != 0) {
        g_shared.shake_peak = std::max(g_shared.shake_peak, shake);
        g_shared.shake_frames = 8;
    }
    if (g_shared.shake_frames > 0) {
        --g_shared.shake_frames;
        const float amount = std::clamp(static_cast<float>(g_shared.shake_peak) / 65536.0f / 4.0f, 0.3f, 1.0f);
        if ((g_frame % 6) == 0) {
            pulse(0, amount, amount * 0.4f, 160);
            g_pulse_until[0] = g_pulse_until[1] = g_frame;   // shake pulses never block events
        }
        if (g_shared.shake_frames == 0) g_shared.shake_peak = 0;
    }

    g_shared.valid = true;
    g_shared.row = row;
    g_shared.gem = gem;
}

} // namespace mm::rumble

// Generated hook at Sound_AddSfx's entry (troublemakers.us1.toml): every sound
// effect passes through it with its id in $a0 and volume (0..255) at 0x10($sp).
extern "C" int mm_far_in_shadow(void);

extern "C" void mm_rumble_sfx(uint8_t* rdram, uint32_t id, uint32_t volume) {
    (void)rdram;
    if (mm_far_in_shadow()) return;
    if (!mm::rumble::g_gameplay) return;
    for (const auto& s : mm::rumble::kSfxRumble) {
        if (s.id == (id & 0xFFFFu)) {
            const float v = std::clamp(static_cast<float>(volume & 0xFFu) / 255.0f, 0.25f, 1.0f);
            mm::rumble::pulse(0, s.low * v, s.high * v, s.ms);
            return;
        }
    }
}
