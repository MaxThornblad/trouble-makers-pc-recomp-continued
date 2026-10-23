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
#include "mm_audio_input.hpp"
#include "recomp.h"

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
}};

std::array<std::atomic<bool>, kCheatCount> g_enabled{};
std::atomic<int> g_fast_forward_speed{3};
std::atomic<bool> g_rank_timer{false};
std::atomic<bool> g_gem_hints{true};
std::atomic<bool> g_gem_compass{false};
std::atomic<int> g_rumble_level{2};
std::atomic<bool> g_active{false};
std::filesystem::path g_path;
std::mutex g_file_mutex;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

void save() {
    if (g_path.empty()) return;
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
    const uint16_t state = MEM_HU(0xD0, marina);
    const bool ground = state == kGroundJetState;
    const bool air = state == kAirJetState;
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

void apply_moon_jump(uint8_t* rdram) {
    static int rises_left = 0;
    if (MEM_HU(0, addr(kGameState)) != 6) return;
    const gpr marina = addr(kMarina);
    const uint16_t state = MEM_HU(0xD0, marina);
    if (state != kRiseState) rises_left = kExtraRises;
    if (state == kRiseState) {
        const bool held = (MEM_HU(0, addr(kMarinaHold)) & kJumpButton) != 0;
        if (held && rises_left > 0 &&
            static_cast<uint32_t>(MEM_W(kJetTimer, marina)) == kRiseRearmAt) {
            MEM_W(kJetTimer, marina) = kRiseDuration;
            --rises_left;
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
// the list, which moves gem records near Marina toward the centre of her
// hitbox; the game then collects them itself.
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
    return g_active.load(std::memory_order_relaxed) && enabled(Cheat::GemMagnet) && MEM_HU(0, addr(kGameState)) == 6 &&
           MEM_W(0x80, addr(kMarina)) != 0;
}

// Centre of Marina's hitbox, camera-relative pixels.
void marina_centre(uint8_t* rdram, int& x, int& y) {
    const gpr m = addr(kMarina);
    x = MEM_H(0x88, m) + (MEM_H(0xAA, m) + MEM_H(0xAC, m)) / 2;
    y = MEM_H(0x8C, m) + (MEM_H(0xAE, m) + MEM_H(0xB0, m)) / 2;
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
    int tx, ty;
    marina_centre(rdram, tx, ty);
    for (uint32_t slot = 1; slot < kActorCount; ++slot) {
        const gpr a = addr(kMarina + slot * kActorStride);
        if (MEM_W(0x80, a) == 0 || MEM_HU(0xD2, a) != kGemActorType) continue;
        int sx, sy;
        if (!pull_step(tx - MEM_H(0x88, a), ty - MEM_H(0x8C, a), sx, sy)) continue;
        MEM_W(0x88, a) = MEM_W(0x88, a) + (sx << 16);
        MEM_W(0x8C, a) = MEM_W(0x8C, a) + (sy << 16);
        MEM_W(0xEC, a) = 0;
        MEM_W(0xF0, a) = 0;
    }
}

} // namespace

void gem_magnet(uint8_t* rdram, uint32_t list) {
    if (!magnet_live(rdram)) return;
    int tx, ty;
    marina_centre(rdram, tx, ty);
    const int cam_x = MEM_H(0, addr(kCameraX));
    const int cam_y = MEM_H(0, addr(kCameraY));
    tx += cam_x;                                   // to world pixels
    ty += cam_y;
    for (int i = 0; i < kMaxItems; ++i, list += 6) {
        const gpr rec = addr(list);
        const uint16_t kind = MEM_HU(0, rec);
        if (kind == 0) break;
        if (kind & 0xC000u) continue;
        const uint32_t handler = static_cast<uint32_t>(
            MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers)));
        if (handler != kGemHandler) continue;
        const int x = MEM_HU(2, rec), y = MEM_HU(4, rec);
        int sx, sy;
        if (!pull_step(tx - x, ty - y, sx, sy)) continue;
        MEM_H(2, rec) = static_cast<uint16_t>(x + sx);
        MEM_H(4, rec) = static_cast<uint16_t>(y + sy);
    }
}

} // namespace mm::cheats

extern "C" void mm_item_list_hook(uint8_t* rdram, uint32_t list) {
    mm::gem_compass::scan_items(rdram, list);
    mm::cheats::gem_magnet(rdram, list);
}

namespace mm::cheats {

extern "C" int mm_cheat_skip_terrain(uint8_t* rdram, uint32_t actor_index) {
    return (actor_index & 0xFFFF) == 0 && g_active.load(std::memory_order_relaxed) &&
           enabled(Cheat::NoClip) &&
           MEM_HU(0, addr(kGameState)) == 6;
}

const char* name(Cheat cheat) { return kCheats[static_cast<int>(cheat)].name; }
const char* description(Cheat cheat) { return kCheats[static_cast<int>(cheat)].description; }

bool enabled(Cheat cheat) {
    return g_enabled[static_cast<int>(cheat)].load(std::memory_order_acquire);
}

void toggle(Cheat cheat) {
    auto& flag = g_enabled[static_cast<int>(cheat)];
    const bool now = !flag.load();
    flag.store(now, std::memory_order_release);
    std::fprintf(stderr, "[cheats] %s %s\n", name(cheat), now ? "on" : "off");
    save();
}

void configure(const std::filesystem::path& folder) {
    g_path = folder / "cheats.cfg";
    std::ifstream f(g_path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const bool on = line.substr(eq + 1) == "1";
        if (key == "rumble") {
            g_rumble_level.store(std::clamp(std::atoi(line.substr(eq + 1).c_str()), 0, 3));
            continue;
        }
        if (key == "gem_compass") {
            g_gem_compass.store(on);
            continue;
        }
        if (key == "gem_hints") {
            g_gem_hints.store(on);
            continue;
        }
        if (key == "rank_timer") {
            g_rank_timer.store(on);
            continue;
        }
        if (key == "fast_forward_speed") {
            const int value = std::atoi(line.substr(eq + 1).c_str());
            for (int speed : kFastForwardSpeeds) {
                if (speed == value) g_fast_forward_speed.store(value);
            }
            continue;
        }
        for (int i = 0; i < kCheatCount; ++i) {
            if (key == kCheats[i].key) g_enabled[i].store(on);
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
    return g_gem_hints.load(std::memory_order_acquire);
}

void toggle_gem_hints() {
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

void set_active(bool active) {
    g_active.store(active, std::memory_order_relaxed);
}

void apply(uint8_t* rdram) {
    if (enabled(Cheat::NoClip)) {
        apply_no_clip(rdram);
        if (magnet_live(rdram)) pull_gem_actors(rdram);
        return;  // no clip owns Marina's movement
    }
    if (magnet_live(rdram)) pull_gem_actors(rdram);
    if (enabled(Cheat::EndlessJet)) apply_endless_jet(rdram);
    if (enabled(Cheat::MoonJump)) apply_moon_jump(rdram);
}

} // namespace mm::cheats
