// Cheats, toggled from the debug menu's Cheats page and applied once per frame
// on the game thread (from mm_debug_menu_tick, so only while the debug menu is
// enabled in the launcher). Choices persist in <config>/cheats.cfg.
#pragma once

#include <cstdint>
#include <filesystem>

namespace mm::cheats {

enum class Cheat : int {
    EndlessJet,
    MoonJump,
    NoClip,
    GemMagnet,
    Count,
};

constexpr int kCheatCount = static_cast<int>(Cheat::Count);

const char* name(Cheat cheat);
const char* description(Cheat cheat);
bool enabled(Cheat cheat);
void toggle(Cheat cheat);

// Fast-forward speed (held fast-forward hotkey), stored alongside the cheats.
// Not a cheat: it applies whether or not the debug menu is enabled.
constexpr int kFastForwardSpeeds[] = {2, 3, 4, 5, 6, 8};
int fast_forward_speed();
void cycle_fast_forward_speed();   // next value, wrapping; saved

// Rank timer overlay (src/game/rank_timer.cpp), stored alongside the cheats.
// Like the fast-forward speed it is not a cheat and works without the debug menu.
bool rank_timer_enabled();
void toggle_rank_timer();

// Golden gem hints in the pause menu (src/game/gem_hints.cpp). On by default;
// works without the debug menu.
bool gem_hints_enabled();
void toggle_gem_hints();

// Golden gem compass overlay (src/game/gem_compass.cpp). Off by default;
// works without the debug menu.
bool gem_compass_enabled();
void toggle_gem_compass();

// Enemy health bars (src/game/enemy_bars.cpp). On by default; works without
// the debug menu.
bool enemy_bars_enabled();
void toggle_enemy_bars();

// Controller rumble strength (src/game/rumble.cpp): 0 off, 1 low, 2 medium
// (default), 3 high. Works without the debug menu.
int rumble_level();
const char* rumble_level_name();
void cycle_rumble_level();

// Reads <folder>/cheats.cfg; toggles are written back on every change.
void configure(const std::filesystem::path& folder);

// Game thread only.
void apply(uint8_t* rdram);

// Whether cheats are in effect (the debug menu is enabled). Cheats that act
// from game-code hooks (no clip, gem magnet) check this.
void set_active(bool active);

} // namespace mm::cheats
