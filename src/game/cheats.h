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
    GrabBlocks,
    MortalNpcs,
    ChargedThrow,
    Count,
};

constexpr int kCheatCount = static_cast<int>(Cheat::Count);

const char* name(Cheat cheat);
const char* description(Cheat cheat);
bool enabled(Cheat cheat);
void toggle(Cheat cheat);

// Throw strength sliders (src/game/throw_cheats.cpp): how hard thrown
// objects and thrown players fly, in percent of the game's own throw. They
// are cheats (they act only while cheats are live) and online the host's
// values rule. Left / right on the row in the Cheats page.
enum class ThrowKind : int { Objects, Players };
constexpr int kThrowStrengths[] = {25, 50, 75, 100, 150, 200, 300, 500, 800, 1200};
constexpr int kThrowStrengthCount = static_cast<int>(sizeof kThrowStrengths / sizeof kThrowStrengths[0]);
constexpr int kThrowStrengthDefault = 3;   // 100%
int throw_strength(ThrowKind kind);          // percent in force this frame (100 while cheats are not live)
int throw_strength_setting(ThrowKind kind);  // this player's own choice (the menu), percent
void adjust_throw_strength(ThrowKind kind, int delta);   // one step along kThrowStrengths; saved

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

// Floating damage numbers (also src/game/enemy_bars.cpp). On by default;
// works without the debug menu.
bool damage_numbers_enabled();
void toggle_damage_numbers();

// Hold to dash (a setting, not a cheat; on by default; works without the
// debug menu): after a double-tap jet, holding that direction keeps the jet
// going at full speed, as if double-tapping again and again.
// Co-op: player 2 (player2.cpp) joins on controller 2. A setting, saved.
bool coop_enabled();
void toggle_coop();
// Local split screen (src/game/far_coop.cpp, split screen): with co-op on,
// each player gets a camera and half of the window. A setting, saved; never
// online (each player has a screen there).
bool split_screen_enabled();
void toggle_split_screen();
bool hold_dash_enabled();
void toggle_hold_dash();
void apply_settings(uint8_t* rdram);   // every frame, debug menu or not

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

// Grabbable blocks (src/game/grab_blocks.cpp): the cheat is on and cheats are
// live this frame (debug menu enabled).
bool grab_blocks_live();

// Cheats are live this frame (debug menu enabled).
bool g_active_for_hooks();

// Netplay: the host's game-changing settings, sent to the guest. While a
// session's settings are set they replace this player's own (cheats and
// whether they are live, hold to dash, gem hints; co-op is always on), so
// both machines play by the same rules; menus still show and save the
// player's own choices. Null ends the override.
struct SessionSettings {
    uint32_t cheats = 0;      // bit per Cheat
    uint8_t active = 0;       // cheats live (host's debug menu enabled)
    uint8_t hold_dash = 1;
    uint8_t gem_hints = 1;
    uint8_t widescreen = 0;   // filled by netplay (not a cheats.cfg setting)
    uint8_t coop = 0;         // a second player (online: always)
    uint8_t throw_objects = 0;   // throw strength step + 1 (0: the default, 100%)
    uint8_t throw_players = 0;
    uint8_t split = 0;        // local split screen (a gameplay rule: the cameras part)
};
SessionSettings capture_session_settings();
// from_host: this machine is the guest, so its menu cannot change these.
void set_session_settings(const SessionSettings* settings, bool from_host = false);
bool settings_from_host();   // the guest in an online session: the host's settings rule

} // namespace mm::cheats
