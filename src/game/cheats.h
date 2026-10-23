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
    Count,
};

constexpr int kCheatCount = static_cast<int>(Cheat::Count);

const char* name(Cheat cheat);
const char* description(Cheat cheat);
bool enabled(Cheat cheat);
void toggle(Cheat cheat);

// Reads <folder>/cheats.cfg; toggles are written back on every change.
void configure(const std::filesystem::path& folder);

// Game thread only.
void apply(uint8_t* rdram);

} // namespace mm::cheats
