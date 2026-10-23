#include "cheats.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

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
}};

std::array<std::atomic<bool>, kCheatCount> g_enabled{};
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

} // namespace

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
        for (int i = 0; i < kCheatCount; ++i) {
            if (key == kCheats[i].key) g_enabled[i].store(on);
        }
    }
    for (int i = 0; i < kCheatCount; ++i) {
        if (g_enabled[i].load()) std::fprintf(stderr, "[cheats] %s on\n", kCheats[i].name);
    }
}

void apply(uint8_t* rdram) {
    if (enabled(Cheat::EndlessJet)) apply_endless_jet(rdram);
}

} // namespace mm::cheats
