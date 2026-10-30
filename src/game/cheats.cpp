#include "cheats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include "gem_compass.h"
#include "grab_blocks.h"
#include "far_coop.h"
#include "player2.h"
#include "state_registry.h"


extern "C" int mm_far_in_shadow(void);
#include "inspector.h"
#include "mm_audio_input.hpp"
#include "recomp.h"
#include "savestate.h"

namespace mm::cheats {
namespace {

struct CheatInfo {
    const char* key;          // cheats.cfg key
    const char* name;
    const char* description;
};

constexpr std::array<CheatInfo, kCheatCount> kCheats{{
    { "endless_jet", "Endless jet",
      "Marina's jet keeps going at full speed while you hold a direction. "
      "Jump and double-tap in the air to fly: hold Up to climb, Down to "
      "descend." },
    { "moon_jump", "Moon jump",
      "Hold jump to go about two and a half times higher, then float down "
      "slowly under weak gravity." },
    { "no_clip", "No clip",
      "Marina ignores floors and walls. Steer freely with the D-pad or stick "
      "(hold Up/Down to rise/sink). Careful: falling out of the bottom of a "
      "stage can still cost a life." },
    { "gem_magnet", "Gem magnet",
      "Gems near Marina fly to her and are collected: red, blue, green and "
      "golden gems placed in the stage as well as gems knocked out of "
      "enemies." },
    { "grab_blocks", "Grabbable blocks",
      "Marina can grab stage blocks (square, wall, small, big cylinder and slope "
      "blocks) like any other object: carry them, throw them or set them down "
      "anywhere; they stay solid to stand on and stack. Red blocks still shatter." },
    { "mortal_npcs", "Mortal NPCs",
      "Characters the game keeps from dying (the hopping Clancers in 1-2 and "
      "1-3 and others like them) can be beaten like enemies: hits and throws "
      "take their health, and they go down when it runs out." },
    { "charged_throw", "Charged throw",
      "While carrying something, hold the throw button (and a direction) to "
      "charge the throw; let go to throw. The longer you hold, the harder it "
      "flies, up to absurd strength after about four seconds. A bar over the "
      "player shows the charge. Works with the throw strength sliders." },
    { "cat_ride", "Ride the cat",
      "Both players ride the cat from 3-12 (CERBERUS) in any stage. Jump onto "
      "missiles to ride them too (the Spawn page has Lunar's missile). Turn it "
      "off to get off." },
}};

std::array<std::atomic<bool>, kCheatCount> g_enabled{};
std::atomic<int> g_fast_forward_speed{3};
std::atomic<bool> g_rank_timer{false};
std::atomic<bool> g_gem_hints{true};
std::atomic<bool> g_gem_compass{false};
std::atomic<int> g_rumble_level{2};
std::atomic<bool> g_enemy_bars{true};
std::atomic<bool> g_damage_numbers{true};
std::atomic<bool> g_hold_dash{true};
std::atomic<bool> g_coop{false};
std::atomic<bool> g_active{false};
std::array<std::atomic<int>, 2> g_throw_strength{kThrowStrengthDefault, kThrowStrengthDefault};   // steps
std::array<std::atomic<int>, 2> g_session_throw{kThrowStrengthDefault, kThrowStrengthDefault};
std::filesystem::path g_path;
std::mutex g_file_mutex;

// The settings in force this frame (set_session_settings, from the latched
// frame: frame_input.h). Locally they are the player's own choices as of the
// frame point; online the host's, so both machines run the same rules.
std::atomic<bool> g_session{false};
std::atomic<uint32_t> g_session_cheats{0};
std::atomic<bool> g_session_active{false};
std::atomic<bool> g_session_hold_dash{true};
std::atomic<bool> g_session_gem_hints{true};
std::atomic<bool> g_session_coop{false};
std::atomic<bool> g_split{false};
std::atomic<bool> g_session_split{false};
std::atomic<bool> g_session_follow{false};   // this machine is the guest: the host's settings rule

// The guest's menu cannot change what the host decides for the session.
bool held_by_host(const char* what) {
    if (!g_session.load(std::memory_order_acquire) || !g_session_follow.load(std::memory_order_relaxed)) return false;
    std::fprintf(stderr, "[cheats] %s is set by the host during online play\n", what);
    return true;
}

bool active_now() {
    return g_session.load(std::memory_order_acquire) ? g_session_active.load(std::memory_order_relaxed)
                                                     : g_active.load(std::memory_order_relaxed);
}

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

void save() {
    if (g_path.empty() || std::getenv("MM_TEST_CHEATS") || std::getenv("MM_TEST_TOGGLE") || std::getenv("MM_NET_TOGGLE")) return;   // (tests share a home)
    std::lock_guard lock{g_file_mutex};
    std::ofstream f(g_path, std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[cheats] could not write %s\n", g_path.string().c_str());
        return;
    }
    for (int i = 0; i < kCheatCount; ++i) {
        f << kCheats[i].key << '=' << (g_enabled[i].load() ? 1 : 0) << '\n';
    }
    f << "fast_forward_speed=" << g_fast_forward_speed.load() << '\n';
    f << "rank_timer=" << (g_rank_timer.load() ? 1 : 0) << '\n';
    f << "gem_hints=" << (g_gem_hints.load() ? 1 : 0) << '\n';
    f << "gem_compass=" << (g_gem_compass.load() ? 1 : 0) << '\n';
    f << "rumble=" << g_rumble_level.load() << '\n';
    f << "enemy_bars=" << (g_enemy_bars.load() ? 1 : 0) << '\n';
    f << "damage_numbers=" << (g_damage_numbers.load() ? 1 : 0) << '\n';
    f << "hold_dash=" << (g_hold_dash.load() ? 1 : 0) << '\n';
    f << "coop=" << (g_coop.load() ? 1 : 0) << '\n';
    f << "split_screen=" << (g_split.load() ? 1 : 0) << '\n';
    f << "throw_objects=" << kThrowStrengths[g_throw_strength[0].load()] << '\n';
    f << "throw_players=" << kThrowStrengths[g_throw_strength[1].load()] << '\n';
}

// --- Endless jet -----------------------------------------------------------
//
// Marina is actor 0. Her state word (+0xD0, upper half) is 0x020B while she
// jets along the ground and 0x0110 while she jets in the air. Both use a timer
// at +0x150 that counts down from 15 each frame; on the ground full speed holds
// while it is 8 or more, then she slows and the jet ends at 0. Double-tapping
// again mid-jet makes the game itself reset that timer to 15 along with two
// animation step counters (+0x170 upper half -> 0x0100, +0x180 top byte -> 1).
// Endless jet performs that same re-trigger just before the slowdown, but only
// while the player keeps holding a direction, so letting go ends the jet
// normally.
//
// In the air the game also bends the jet into an arc: vertical velocity
// (+0xF0, 16.16) loses the value at +0x158 every frame, and +0x158 grows by
// 0x600 per frame. With the cheat on, the air jet flies level instead, and
// holding Up or Down climbs or descends - free flight.
constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kMarina = 0x800EF510u;
constexpr uint32_t kMarinaHold = 0x801370CCu;
constexpr uint16_t kUp = 0x0800u;              // N64 D-pad bits the game maps directions to
constexpr uint16_t kDown = 0x0400u;
constexpr uint16_t kDirectionMask = 0x0F00u;
constexpr uint16_t kGroundJetState = 0x020Bu;
constexpr uint16_t kAirJetState = 0x0110u;
constexpr uint32_t kVelocityY = 0xF0u;
constexpr uint32_t kGravityStep = 0x158u;
constexpr uint32_t kJetTimer = 0x150u;
constexpr uint32_t kJetAnimStep = 0x170u;
constexpr uint32_t kJetAnimFrame = 0x180u;
constexpr uint32_t kRetriggerAt = 9;           // last full-speed tick
constexpr uint32_t kJetDuration = 15;
constexpr int32_t kClimbSpeed = 0x18000;       // 1.5 px/frame

void apply_endless_jet(uint8_t* rdram) {
    if (MEM_HU(0, addr(kGameState)) != 6) return;           // gameplay only
    const gpr marina = addr(kMarina);
    const uint16_t action = MEM_HU(0xD0, marina) & 0xFF;     // high byte varies (0x810B: jet left)
    const bool ground = action == (kGroundJetState & 0xFF);
    const bool air = action == (kAirJetState & 0xFF);
    if (!ground && !air) return;
    const uint16_t hold = MEM_HU(0, addr(kMarinaHold));
    if ((hold & kDirectionMask) == 0) return;

    if (air) {
        int32_t vy = 0;
        if (hold & kUp) vy = kClimbSpeed;
        else if (hold & kDown) vy = -kClimbSpeed;
        MEM_W(kVelocityY, marina) = vy;
        MEM_W(kGravityStep, marina) = 0;
    }

    const uint32_t timer = static_cast<uint32_t>(MEM_W(kJetTimer, marina));
    if (timer == 0 || timer > kRetriggerAt) return;
    MEM_W(kJetTimer, marina) = kJetDuration;
    MEM_H(kJetAnimStep, marina) = 0x0100;
    MEM_B(kJetAnimFrame, marina) = 1;
}

// --- Hold to dash ------------------------------------------------------------
//
// The same re-trigger as endless jet, on the ground and in the air, but only
// while the held direction is the way the jet goes (the game's own D-pad
// masks at 0x800BE50C / 0x800BE510), and
// without endless jet's free flight: an air jet still arcs down.
constexpr uint32_t kButtonLeft = 0x800BE50Cu;
constexpr uint32_t kButtonRight = 0x800BE510u;

void apply_hold_dash(uint8_t* rdram) {
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    if (game_state != 6 && game_state != 10) return;
    const gpr marina = addr(kMarina);
    // Low byte = action (high byte: phase / variant; 0x810B is the ground
    // jet to the left in 1-9).
    const uint16_t action = MEM_HU(0xD0, marina) & 0xFF;
    if (action != (kGroundJetState & 0xFF) && action != (kAirJetState & 0xFF)) return;
    const uint16_t hold = MEM_HU(0, addr(kMarinaHold));
    // Which way the jet goes: its horizontal velocity (+0xEC). (Flag 0x20,
    // facing left when standing, is not set during a left jet.)
    const int32_t vx = MEM_W(0xEC, marina);
    if (vx == 0) return;
    const uint16_t forward = MEM_HU(0, addr(vx < 0 ? kButtonLeft : kButtonRight));
    if ((hold & forward) == 0) return;
    const uint32_t timer = static_cast<uint32_t>(MEM_W(kJetTimer, marina));
    if (timer == 0 || timer > kRetriggerAt) return;
    MEM_W(kJetTimer, marina) = kJetDuration;
    MEM_H(kJetAnimStep, marina) = 0x0100;
    MEM_B(kJetAnimFrame, marina) = 1;
}

// --- Moon jump -------------------------------------------------------------
//
// Jumping enters state 0x0116: Marina rises at 3 px/frame while jump is held
// and a timer at +0x150 counts down from 19; at 9 the rise starts halving
// each frame, then she falls in state 0x0216, where the game takes
// 0.375 px/frame (0x6000 in 16.16) off her vertical velocity every frame.
// Moon jump re-arms the full-speed rise up to three more times while jump is
// held and gives back two thirds of the fall gravity so she floats down
// (measured: a full jump peaks at +174 px instead of +70, hang time 2 s).
constexpr uint16_t kRiseState = 0x0116u;
constexpr uint16_t kFallState = 0x0216u;
constexpr uint16_t kJumpButton = 0x8000u;      // N64 A
constexpr uint32_t kRiseRearmAt = 10;          // last full-speed rise tick
constexpr uint32_t kRiseDuration = 19;
constexpr int kExtraRises = 3;
constexpr int32_t kFallGravity = 0x6000;
constexpr int32_t kMoonGravityRefund = 0x4000;

// Rises left in this jump, one word per player in game memory (player 2's
// block, player2.cpp's layout), so both machines of an online game agree.
constexpr uint32_t kRisesLeft[2] = {0x807F0610u, 0x807F0614u};

void apply_moon_jump(uint8_t* rdram, int player) {
    if (MEM_HU(0, addr(kGameState)) != 6) return;
    const gpr marina = addr(kMarina);
    const gpr rises = addr(kRisesLeft[player]);
    const uint16_t state = MEM_HU(0xD0, marina);
    if (state != kRiseState) MEM_W(0, rises) = kExtraRises;
    if (state == kRiseState) {
        const bool held = (MEM_HU(0, addr(kMarinaHold)) & kJumpButton) != 0;
        if (held && MEM_W(0, rises) > 0 &&
            static_cast<uint32_t>(MEM_W(kJetTimer, marina)) == kRiseRearmAt) {
            MEM_W(kJetTimer, marina) = kRiseDuration;
            MEM_W(0, rises) = MEM_W(0, rises) - 1;
        }
    }
    else if (state == kFallState) {
        MEM_W(kVelocityY, marina) = MEM_W(kVelocityY, marina) + kMoonGravityRefund;
    }
}

// --- No clip ---------------------------------------------------------------
//
// The game runs terrain collision per actor through func_800160EC(index); the
// generated hook there asks mm_cheat_skip_terrain() and returns early for
// Marina (index 0). With no floor under her she drops into the fall state, so
// no clip also drives her velocity from the D-pad each frame and cancels the
// fall state's gravity.
constexpr int32_t kNoClipSpeed = 0x30000;      // 3 px/frame

void apply_no_clip(uint8_t* rdram) {
    if (MEM_HU(0, addr(kGameState)) != 6) return;
    const gpr marina = addr(kMarina);
    const uint16_t hold = MEM_HU(0, addr(kMarinaHold));
    int32_t vx = 0, vy = 0;
    if (hold & 0x0100u) vx = kNoClipSpeed;        // right
    else if (hold & 0x0200u) vx = -kNoClipSpeed;  // left
    if (hold & kUp) vy = kNoClipSpeed;
    else if (hold & kDown) vy = -kNoClipSpeed;
    if (MEM_HU(0xD0, marina) == kFallState) vy += kFallGravity;
    MEM_W(0xEC, marina) = vx;
    MEM_W(kVelocityY, marina) = vy;
    MEM_W(kGravityStep, marina) = 0;
}

// --- Gem magnet ------------------------------------------------------------
//
// Stage gems are not actors. Each frame func_800451E4 walks the stage's item
// list: 6-byte records {u16 kind, u16 x, u16 y} in world pixels, ending at a
// zero kind. Bits 0xC000 of kind mark a record as gone (collected), bits
// 0xFFF0 >> 4 pick a handler from the table at 0x800D37BC, the low nibble is
// the gem colour. The handler for gems (func_800446F0) copies the record to
// a draw slot at world - camera and tests that box (+/-6 px) against
// Marina's hitbox (actor +0xAA..+0xB0, relative to her +0x88/+0x8C).
// The generated hook at func_800451E4's entry calls mm_cheat_gem_magnet with
// the list, which moves gem records near a player toward the centre of the
// nearer one's hitbox (Marina's or player 2's); the game, or player 2's
// pickup pass (player2.cpp collect_pickups), then collects them.
//
// Gems knocked out of enemies are actors of type 8 (SpawnGemActor); the
// magnet pulls those too, in camera-relative 16.16 coordinates.
constexpr uint32_t kCameraX = 0x800BE558u;
constexpr uint32_t kCameraY = 0x800BE55Cu;
constexpr uint32_t kItemHandlers = 0x800D37BCu;
constexpr uint32_t kGemHandler = 0x800446F0u;
constexpr uint32_t kActorCount = 208;
constexpr uint32_t kActorStride = 0x198;
constexpr uint16_t kGemActorType = 8;
constexpr int kMagnetRadius = 96;              // px
constexpr int kMaxItems = 512;                 // safety bound on the list walk

bool magnet_live(uint8_t* rdram) {
    return active_now() && enabled(Cheat::GemMagnet) && MEM_HU(0, addr(kGameState)) == 6 &&
           MEM_W(0x80, addr(kMarina)) != 0;
}

// Centres of the players' hitboxes, camera-relative pixels: Marina's and
// player 2's (online the host's cheats are both players'). Returns how many.
int player_centres(uint8_t* rdram, int xs[2], int ys[2]) {
    int n = 0;
    const int p2 = mm::player2::slot(rdram);
    for (const int slot : {0, p2}) {
        if (slot < 0 || (n > 0 && slot == 0)) continue;
        const gpr m = addr(kMarina + static_cast<uint32_t>(slot) * kActorStride);
        if (MEM_W(0x80, m) == 0) continue;
        xs[n] = MEM_H(0x88, m) + (MEM_H(0xAA, m) + MEM_H(0xAC, m)) / 2;
        ys[n] = MEM_H(0x8C, m) + (MEM_H(0xAE, m) + MEM_H(0xB0, m)) / 2;
        ++n;
    }
    return n;
}

// Towards the nearer player: the offset to her centre.
void nearest(const int xs[2], const int ys[2], int n, int x, int y, int& dx, int& dy) {
    dx = xs[0] - x;
    dy = ys[0] - y;
    for (int i = 1; i < n; ++i) {
        const int ex = xs[i] - x, ey = ys[i] - y;
        if (ex * ex + ey * ey < dx * dx + dy * dy) { dx = ex; dy = ey; }
    }
}

// One step of the pull: fast enough to feel like suction, accelerating as the
// gem closes in. Returns false when (dx, dy) is outside the radius.
bool pull_step(int dx, int dy, int& sx, int& sy) {
    const int dist2 = dx * dx + dy * dy;
    if (dist2 > kMagnetRadius * kMagnetRadius) return false;
    int dist = 0;
    while ((dist + 1) * (dist + 1) <= dist2) ++dist;
    if (dist == 0) { sx = sy = 0; return true; }
    const int speed = std::min(dist, std::max(3, (kMagnetRadius - dist) / 6 + 3));
    sx = dx * speed / dist;
    sy = dy * speed / dist;
    return true;
}

void pull_gem_actors(uint8_t* rdram) {
    int xs[2], ys[2];
    const int n = player_centres(rdram, xs, ys);
    if (n == 0) return;
    for (uint32_t slot = 1; slot < kActorCount; ++slot) {
        const gpr a = addr(kMarina + slot * kActorStride);
        if (MEM_W(0x80, a) == 0 || MEM_HU(0xD2, a) != kGemActorType) continue;
        int dx, dy, sx, sy;
        nearest(xs, ys, n, MEM_H(0x88, a), MEM_H(0x8C, a), dx, dy);
        if (!pull_step(dx, dy, sx, sy)) continue;
        MEM_W(0x88, a) = MEM_W(0x88, a) + (sx << 16);
        MEM_W(0x8C, a) = MEM_W(0x8C, a) + (sy << 16);
        MEM_W(0xEC, a) = 0;
        MEM_W(0xF0, a) = 0;
    }
}

} // namespace

void gem_magnet(uint8_t* rdram, uint32_t list) {
    if (!magnet_live(rdram)) return;
    int xs[2], ys[2];
    const int n = player_centres(rdram, xs, ys);
    if (n == 0) return;
    const int cam_x = MEM_H(0, addr(kCameraX));
    const int cam_y = MEM_H(0, addr(kCameraY));
    for (int i = 0; i < n; ++i) {                  // to world pixels
        xs[i] += cam_x;
        ys[i] += cam_y;
    }
    for (int i = 0; i < kMaxItems; ++i, list += 6) {
        const gpr rec = addr(list);
        const uint16_t kind = MEM_HU(0, rec);
        if (kind == 0) break;
        if (kind & 0xC000u) continue;
        const uint32_t handler = static_cast<uint32_t>(
            MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers)));
        if (handler != kGemHandler) continue;
        const int x = MEM_HU(2, rec), y = MEM_HU(4, rec);
        int dx, dy, sx, sy;
        nearest(xs, ys, n, x, y, dx, dy);
        if (!pull_step(dx, dy, sx, sy)) continue;
        MEM_H(2, rec) = static_cast<uint16_t>(x + sx);
        MEM_H(4, rec) = static_cast<uint16_t>(y + sy);
    }
}

} // namespace mm::cheats

// --- Mortal NPCs ------------------------------------------------------------
//
// func_8006C1AC is the common update for Clancer-like characters (called
// through func_8007325C by many stage types). Once their health is 0 or less
// (state >= 0x51) it sets it back to 0x4000 if bit 0x80 is set in +0x150:
// that bit is what makes a character immortal (the 1-3 hoppers have 1 health
// and the bit). Clearing it before the check lets them die normally.
// Each slot whose bit the cheat cleared keeps the type it had then, so
// turning the cheat off gives those characters their bit back (they stayed
// mortal). Registered, as the bit itself is game memory.
namespace mm::cheats {
namespace {
struct MadeMortal {
    std::array<uint16_t, 208> type{};   // 0: not cleared by the cheat
};
mm::state::Pod<MadeMortal> g_made_mortal{"mortal-npcs-bit", 1};
constexpr uint32_t kImmortalBit = 0x80;   // +0x150
} // namespace
} // namespace mm::cheats

extern "C" void mm_mortal_hook(uint8_t* rdram, uint32_t slot) {
    if (mm_far_in_shadow()) return;   // its record of health is host state (far_coop.h)
    if (!mm::cheats::g_active_for_hooks() || !mm::cheats::enabled(mm::cheats::Cheat::MortalNpcs)) return;
    slot &= 0xFFFF;
    if (slot == 0 || slot >= 208) return;
    const gpr a = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + slot * 0x198u));
    if ((MEM_W(0x150, a) & mm::cheats::kImmortalBit) == 0) return;
    MEM_W(0x150, a) = MEM_W(0x150, a) & ~mm::cheats::kImmortalBit;
    mm::cheats::g_made_mortal->type[slot] = MEM_HU(0xD2, a);
}

// The other kind of immortal: characters whose update puts their health
// back every frame, so a hit only shows as one frame of lower health. The
// 1-1 tutorial cast (type 0x1806) is held at 10000 (func_801A9880_710B10 and
// others); the 3-11 cats (type 0x2801) at 5 (a throw shows 5 -> -45 -> 5).
// With the cheat on such a character is recognised either by health of 5000
// or more, or by a drop that is undone on the very next frame, and gets a
// shadow health (kShadowHealth, or kLowShadowHealth when found by an
// undone drop) that each drop takes from. When it runs
// out it gets the game's own Clancer death (func_8006CFDC: turns the actor
// into the rising-spirit type 0x63, which then vanishes).
extern "C" void func_8006CFDC(uint8_t* rdram, recomp_context* ctx);

namespace mm::cheats {
namespace {

constexpr int kRefillHealth = 5000;              // at or above: kept alive by a refill
constexpr int kShadowHealth = 150;               // high-health refillers: about three throws (-50 each)
constexpr int kLowShadowHealth = 50;            // low-health ones (the 5-hp cats): one throw

struct Shadow {
    uint16_t type = 0;
    int16_t last = 0;                            // health seen last frame
    int16_t full = 0;                            // the health it is held at (0: not known yet)
    int pending = 0;                             // drop seen last frame, not yet classed
    int health = 0;
};
// Registered: a save state, a joiner and a rollback all carry how hurt each
// NPC is (state_registry.h); it used to reset on every load.
struct Shadows {
    std::array<Shadow, 0x90> slots;
};
mm::state::Pod<Shadows> g_shadows{"mortal-npcs", 1};

void beat(uint8_t* rdram, int slot, uint16_t type) {
    recomp_context ctx{};
    ctx.r29 = addr(0x8045FF00u);                 // scratch stack (host-only expansion memory)
    ctx.r4 = static_cast<gpr>(slot);
    func_8006CFDC(rdram, &ctx);
    std::fprintf(stderr, "[cheats] mortal NPCs: slot %d (type %04X) beaten\n", slot, type);
}

// The cheat off: the characters it made mortal (still the same actor, not
// beaten) are immortal again.
void restore_immortals(uint8_t* rdram) {
    int n = 0;
    for (int slot = 1; slot < 208; ++slot) {
        uint16_t& was = g_made_mortal->type[slot];
        if (was == 0) continue;
        const gpr a = addr(0x800EF510u + static_cast<uint32_t>(slot) * 0x198u);
        if (MEM_W(0x80, a) != 0 && MEM_HU(0xD2, a) == was) {
            MEM_W(0x150, a) = MEM_W(0x150, a) | kImmortalBit;
            ++n;
        }
        was = 0;
    }
    if (n > 0) std::fprintf(stderr, "[cheats] mortal NPCs off: %d immortal again\n", n);
}

void apply_mortal_refills(uint8_t* rdram) {
    for (int slot = 1; slot < 0x90; ++slot) {
        const gpr a = addr(0x800EF510u + static_cast<uint32_t>(slot) * 0x198u);
        Shadow& sh = g_shadows->slots[slot];
        const uint16_t type = MEM_W(0x80, a) != 0 ? MEM_HU(0xD2, a) : 0;
        if (type == 0 || (type >> 8) == 0x7F || type == 0x63) {
            sh = {};
            continue;
        }
        const int16_t hp = MEM_H(0xE0, a);
        if (sh.type != type) {                   // new actor in this slot
            sh = {};
            sh.type = type;
            sh.last = hp;
            if (hp >= kRefillHealth) {
                sh.full = hp;
                sh.health = kShadowHealth;
            }
            continue;
        }
        int damage = 0;
        if (sh.full != 0) {
            if (hp < sh.full) damage = sh.full - hp;   // known refiller: every drop counts
        } else if (sh.pending > 0) {
            if (hp == sh.last + sh.pending) {          // last frame's drop was undone: a refiller
                sh.full = hp;
                sh.health = kLowShadowHealth;
                damage = sh.pending;
            }
            sh.pending = 0;
        } else if (hp < sh.last && hp > -0x4000) {
            sh.pending = sh.last - hp;                 // see next frame whether it sticks
        }
        sh.last = hp;
        if (damage <= 0) continue;
        sh.health -= damage;
        if (sh.health > 0) continue;
        beat(rdram, slot, type);
        sh = {};
    }
}

} // namespace
} // namespace mm::cheats

extern "C" void mm_item_list_hook(uint8_t* rdram, uint32_t list) {
    if (mm_far_in_shadow()) return;
    mm::gem_compass::scan_items(rdram, list);
    mm::grab_blocks::on_item_list(rdram, list);
    mm::faraway::hide_collected_golden_gems(rdram, list);
    mm::player2::collect_pickups(rdram, list);
    mm::inspector::note_item_list(list);
    mm::cheats::gem_magnet(rdram, list);
}

namespace mm::cheats {

extern "C" int mm_cheat_skip_terrain(uint8_t* rdram, uint32_t actor_index) {
    return (actor_index & 0xFFFF) == 0 && active_now() &&
           enabled(Cheat::NoClip) &&
           MEM_HU(0, addr(kGameState)) == 6;
}

bool g_active_for_hooks() {
    return active_now();
}

bool grab_blocks_live() {
    return active_now() && enabled(Cheat::GrabBlocks);
}

const char* name(Cheat cheat) { return kCheats[static_cast<int>(cheat)].name; }
const char* description(Cheat cheat) { return kCheats[static_cast<int>(cheat)].description; }

bool enabled(Cheat cheat) {
    if (g_session.load(std::memory_order_acquire)) {
        return (g_session_cheats.load(std::memory_order_relaxed) >> static_cast<int>(cheat)) & 1u;
    }
    return g_enabled[static_cast<int>(cheat)].load(std::memory_order_acquire);
}

void toggle(Cheat cheat) {
    if (held_by_host(name(cheat))) return;
    auto& flag = g_enabled[static_cast<int>(cheat)];
    const bool now = !flag.load();
    flag.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] %s %s\n", name(cheat), now ? "on" : "off");
    save();
}

namespace {

// One "key=value" line of cheats.cfg (or of MM_TEST_CHEATS).
void apply_setting(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();  // CRLF files
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return;
    const std::string key = line.substr(0, eq);
    const bool on = line.substr(eq + 1) == "1";
    if (key == "hold_dash") {
        g_hold_dash.store(on);
        return;
    }
    if (key == "split_screen") {
        g_split.store(on);
        return;
    }
    if (key == "coop") {
        g_coop.store(on);
        mm_audio_input::set_player2_input(on);
        return;
    }
    if (key == "damage_numbers") {
        g_damage_numbers.store(on);
        return;
    }
    if (key == "enemy_bars") {
        g_enemy_bars.store(on);
        return;
    }
    if (key == "rumble") {
        g_rumble_level.store(std::clamp(std::atoi(line.substr(eq + 1).c_str()), 0, 3));
        return;
    }
    if (key == "gem_compass") {
        g_gem_compass.store(on);
        return;
    }
    if (key == "gem_hints") {
        g_gem_hints.store(on);
        return;
    }
    if (key == "rank_timer") {
        g_rank_timer.store(on);
        return;
    }
    if (key == "throw_objects" || key == "throw_players") {
        const int value = std::atoi(line.substr(eq + 1).c_str());
        for (int i = 0; i < kThrowStrengthCount; ++i) {
            if (kThrowStrengths[i] == value) g_throw_strength[key == "throw_players" ? 1 : 0].store(i);
        }
        return;
    }
    if (key == "fast_forward_speed") {
        const int value = std::atoi(line.substr(eq + 1).c_str());
        for (int speed : kFastForwardSpeeds) {
            if (speed == value) g_fast_forward_speed.store(value);
        }
        return;
    }
    for (int i = 0; i < kCheatCount; ++i) {
        if (key == kCheats[i].key) g_enabled[i].store(on);
    }
}

} // namespace

void configure(const std::filesystem::path& folder) {
    g_path = folder / "cheats.cfg";
    std::ifstream f(g_path);
    std::string line;
    while (std::getline(f, line)) apply_setting(line);
    // Tests (bot runs share one home): settings on top of the file,
    // "cat_ride=1,coop=0", never written back (nor are test toggles).
    if (const char* test = std::getenv("MM_TEST_CHEATS")) {
        std::string all = test;
        for (size_t at = 0; at <= all.size();) {
            const size_t comma = std::min(all.find(',', at), all.size());
            apply_setting(all.substr(at, comma - at));
            at = comma + 1;
        }
    }
    for (int i = 0; i < kCheatCount; ++i) {
        if (g_enabled[i].load()) std::fprintf(stderr, "[cheats] %s on\n", kCheats[i].name);
    }
}

int fast_forward_speed() {
    return g_fast_forward_speed.load(std::memory_order_acquire);
}

void cycle_fast_forward_speed() {
    const int current = g_fast_forward_speed.load();
    constexpr int count = static_cast<int>(sizeof kFastForwardSpeeds / sizeof kFastForwardSpeeds[0]);
    int next = kFastForwardSpeeds[0];
    for (int i = 0; i < count; ++i) {
        if (kFastForwardSpeeds[i] == current) next = kFastForwardSpeeds[(i + 1) % count];
    }
    g_fast_forward_speed.store(next, std::memory_order_release);
    std::fprintf(stderr, "[cheats] fast-forward speed %dx\n", next);
    save();
}

bool rank_timer_enabled() {
    return g_rank_timer.load(std::memory_order_acquire);
}

void toggle_rank_timer() {
    const bool now = !g_rank_timer.load();
    g_rank_timer.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] rank timer %s\n", now ? "on" : "off");
    save();
}

bool gem_hints_enabled() {
    if (g_session.load(std::memory_order_acquire)) return g_session_gem_hints.load(std::memory_order_relaxed);
    return g_gem_hints.load(std::memory_order_acquire);
}

void toggle_gem_hints() {
    if (held_by_host("gem hints")) return;
    const bool now = !g_gem_hints.load();
    g_gem_hints.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] gem hints %s\n", now ? "on" : "off");
    save();
}

bool gem_compass_enabled() {
    return g_gem_compass.load(std::memory_order_acquire);
}

void toggle_gem_compass() {
    const bool now = !g_gem_compass.load();
    g_gem_compass.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] golden gem compass %s\n", now ? "on" : "off");
    save();
}

bool enemy_bars_enabled() {
    return g_enemy_bars.load(std::memory_order_acquire);
}

void toggle_enemy_bars() {
    const bool now = !g_enemy_bars.load();
    g_enemy_bars.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] enemy health bars %s\n", now ? "on" : "off");
    save();
}

bool coop_enabled() {
    if (g_session.load(std::memory_order_acquire)) return g_session_coop.load(std::memory_order_relaxed);
    return g_coop.load(std::memory_order_acquire);
}

bool split_screen_enabled() {
    const bool on = g_session.load(std::memory_order_acquire) ? g_session_split.load(std::memory_order_relaxed)
                                                              : g_split.load(std::memory_order_acquire);
    return on && coop_enabled();
}

void toggle_split_screen() {
    const bool now = !g_split.load();
    g_split.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] split screen %s\n", now ? "on" : "off");
    save();
}

void toggle_coop() {
    const bool now = !g_coop.load();
    g_coop.store(now, std::memory_order_release);
    mm_audio_input::set_player2_input(now);
    std::fprintf(stderr, "[cheats] co-op %s\n", now ? "on" : "off");
    save();
}

bool hold_dash_enabled() {
    if (g_session.load(std::memory_order_acquire)) return g_session_hold_dash.load(std::memory_order_relaxed);
    return g_hold_dash.load(std::memory_order_acquire);
}

void toggle_hold_dash() {
    if (held_by_host("hold to dash")) return;
    const bool now = !g_hold_dash.load();
    g_hold_dash.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] hold to dash %s\n", now ? "on" : "off");
    save();
}

void apply_settings(uint8_t* rdram) {
    // Endless jet (a cheat) already covers this, and adds flight.
    if (active_now() && (enabled(Cheat::EndlessJet) || enabled(Cheat::NoClip))) return;
    if (!hold_dash_enabled()) return;
    apply_hold_dash(rdram);
    if (!mm_far_in_shadow()) mm::player2::as_marina(rdram, [](uint8_t* r) { apply_hold_dash(r); });   // and player 2's
}

bool damage_numbers_enabled() {
    return g_damage_numbers.load(std::memory_order_acquire);
}

void toggle_damage_numbers() {
    const bool now = !g_damage_numbers.load();
    g_damage_numbers.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] damage numbers %s\n", now ? "on" : "off");
    save();
}

int rumble_level() {
    return g_rumble_level.load(std::memory_order_relaxed);
}

const char* rumble_level_name() {
    static const char* const kNames[] = {"Off", "Low", "Medium", "High"};
    return kNames[std::clamp(rumble_level(), 0, 3)];
}

void cycle_rumble_level() {
    const int next = (g_rumble_level.load() + 1) % 4;
    g_rumble_level.store(next, std::memory_order_relaxed);
    std::fprintf(stderr, "[cheats] rumble %s\n", rumble_level_name());
    save();
    if (next > 0) {
        // A sample pulse at the new strength.
        static const float kScale[] = {0.0f, 0.35f, 0.65f, 1.0f};
        const auto v = static_cast<uint16_t>(kScale[next] * 65535.0f);
        mm_audio_input::request_rumble(v, v, 250);
    }
}

SessionSettings capture_session_settings() {
    SessionSettings out;
    for (int i = 0; i < kCheatCount; ++i) {
        if (g_enabled[i].load()) out.cheats |= 1u << i;
    }
    out.active = g_active.load() ? 1 : 0;
    out.hold_dash = g_hold_dash.load() ? 1 : 0;
    out.gem_hints = g_gem_hints.load() ? 1 : 0;
    out.coop = g_coop.load() ? 1 : 0;
    out.split = g_split.load() ? 1 : 0;
    out.throw_objects = static_cast<uint8_t>(g_throw_strength[0].load() + 1);
    out.throw_players = static_cast<uint8_t>(g_throw_strength[1].load() + 1);
    return out;
}

int throw_strength(ThrowKind kind) {
    if (!active_now()) return 100;
    const int k = static_cast<int>(kind);
    const int step = g_session.load(std::memory_order_acquire) ? g_session_throw[k].load(std::memory_order_relaxed)
                                                               : g_throw_strength[k].load(std::memory_order_relaxed);
    return kThrowStrengths[std::clamp(step, 0, kThrowStrengthCount - 1)];
}

int throw_strength_setting(ThrowKind kind) {
    return kThrowStrengths[g_throw_strength[static_cast<int>(kind)].load()];
}

void adjust_throw_strength(ThrowKind kind, int delta) {
    if (held_by_host("Throw strength")) return;
    auto& step = g_throw_strength[static_cast<int>(kind)];
    const int now = std::clamp(step.load() + delta, 0, kThrowStrengthCount - 1);
    step.store(now);
    std::fprintf(stderr, "[cheats] throw strength (%s) %d%%\n", kind == ThrowKind::Players ? "players" : "objects",
                 kThrowStrengths[now]);
    save();
}

void set_session_settings(const SessionSettings* settings, bool from_host) {
    if (settings == nullptr) {
        g_session.store(false, std::memory_order_release);
        g_session_follow.store(false, std::memory_order_relaxed);
        return;
    }
    g_session_follow.store(from_host, std::memory_order_relaxed);
    g_session_cheats.store(settings->cheats, std::memory_order_relaxed);
    g_session_active.store(settings->active != 0, std::memory_order_relaxed);
    g_session_hold_dash.store(settings->hold_dash != 0, std::memory_order_relaxed);
    g_session_gem_hints.store(settings->gem_hints != 0, std::memory_order_relaxed);
    g_session_coop.store(settings->coop != 0, std::memory_order_relaxed);
    g_session_split.store(settings->split != 0, std::memory_order_relaxed);
    // (0 from a build without the sliders: the default.)
    g_session_throw[0].store(settings->throw_objects ? settings->throw_objects - 1 : kThrowStrengthDefault, std::memory_order_relaxed);
    g_session_throw[1].store(settings->throw_players ? settings->throw_players - 1 : kThrowStrengthDefault, std::memory_order_relaxed);
    g_session.store(true, std::memory_order_release);
}

bool settings_from_host() {
    return g_session.load(std::memory_order_acquire) && g_session_follow.load(std::memory_order_relaxed);
}

void set_active(bool active) {
    g_active.store(active, std::memory_order_relaxed);
}

void apply(uint8_t* rdram) {
    if (g_active_for_hooks() && enabled(Cheat::MortalNpcs)) {
        apply_mortal_refills(rdram);
    } else if (!mm_far_in_shadow()) {
        restore_immortals(rdram);
        g_shadows->slots = {};   // the refillers' shadow health starts over next time
    }
    if (enabled(Cheat::NoClip)) {
        apply_no_clip(rdram);
        if (magnet_live(rdram)) pull_gem_actors(rdram);
        return;  // no clip owns Marina's movement
    }
    if (magnet_live(rdram)) pull_gem_actors(rdram);
    // Marina's movement cheats, for player 1 and (as Marina for a moment)
    // player 2: online the host's cheats are both players'.
    if (enabled(Cheat::EndlessJet)) apply_endless_jet(rdram);
    if (enabled(Cheat::MoonJump)) apply_moon_jump(rdram, 0);
    // Not in player 2's picture pass (far_coop.cpp): that frame only draws.
    if (!mm_far_in_shadow()) mm::player2::as_marina(rdram, [](uint8_t* r) {
        if (enabled(Cheat::EndlessJet)) apply_endless_jet(r);
        if (enabled(Cheat::MoonJump)) apply_moon_jump(r, 1);
    });
}

} // namespace mm::cheats
