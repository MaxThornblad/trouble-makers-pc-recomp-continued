#include "split_view.h"
#include "throw_cheats.h"
#include "debug_menu.h"
#include "far_coop.h"
#include "cheats.h"
#include "rank_timer.h"
#include "gem_compass.h"
#include "actors.h"
#include "spawn_catalog.h"
#include "dev_harness.h"
#include "enemy_bars.h"
#include "gem_hints.h"
#include "inspector.h"
#include "crash_handler.h"
#include "rumble.h"
#include "replay.h"
#include "netplay.h"
#include "frame_input.h"
#include "player2.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <string_view>

#include "librecomp/game.hpp"
#include "mm_audio_input.hpp"
#include "telemetry.h"
#include "recomp.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" int mm_debug_warp_prepare(
    unsigned char* rdram, int requested_scene, int requested_stage);

namespace mm::debug_menu {
namespace {

struct WarpEntry {
    int stage;
    int scene;
    const char* label;
    const char* name;
};

// Campaign rows from the ROM's gStageScenes progression table. Demo,
// opening, ending, credits, and unused extra rows are deliberately omitted;
// these are the same 52 playable rows covered by test_widescreen_playable.sh.
constexpr std::array<WarpEntry, 52> kWarps{{
    { 2,  0, "1-1",  "Meet Marina!" },
    { 3, 68, "1-2",  "Meet Calina!" },
    { 4, 53, "1-3",  "Clanball Land" },
    { 5, 55, "1-4",  "Spike Land" },
    { 6, 52, "1-5",  "3 Clancer Kids" },
    { 7, 67, "1-6",  "Blockman Rises" },
    { 8, 56, "1-7",  "Wormin' Up!" },
    { 9, 58, "1-8",  "Crisis: Nepton" },
    {10, 54, "1-9",  "Western World" },
    {11, 57, "1-10", "Volcano!" },

    {12,  1, "2-1",  "Sea of Lava" },
    {13, 69, "2-2",  "Vertigo!" },
    {14, 37, "2-3",  "Sink or Float" },
    {15, 60, "2-4",  "Hot Rush" },
    {16, 38, "2-5",  "Searin' Swing" },
    {17, 59, "2-6",  "Flamb!!" },
    {18, 61, "2-7",  "Tightrope Ride" },
    {19, 70, "2-8",  "Freefall!" },
    {20, 62, "2-9",  "Magma Rafts!" },
    {21, 13, "2-10", "Seasick Climb" },
    {22,  5, "2-11", "Migen Brawl!" },

    {24, 72, "3-1",  "Clanpot Shake" },
    {25, 12, "3-2",  "Clance War" },
    {26, 35, "3-3",  "Missile Surf!" },
    {27, 71, "3-4",  "Clanball Lift" },
    {28, 32, "3-5",  "Go Marzen 64" },
    {29, 31, "3-6",  "Chilly Dog!" },
    {30, 36, "3-7",  "Snowstorm Maze" },
    {31,  9, "3-8",  "LUNAR!" },
    {32, 33, "3-9",  "The Day Before" },
    {33, 18, "3-10", "The Day Of" },
    {34, 29, "3-11", "Cat-astrophe!" },
    {35, 19, "3-12", "CERBERUS" },

    {37, 42, "4-1",  "Rolling Rock!" },
    {38, 39, "4-2",  "Toadly Raw!" },
    {39, 40, "4-3",  "7 Clancer Kids" },
    {40, 23, "4-4",  "Rescue! Act 1" },
    {41, 41, "4-5",  "Rescue! Act 2" },
    {42, 10, "4-6",  "Taurus!" },
    {43, 48, "4-7",  "Ghost Catcher" },
    {44, 24, "4-8",  "Aster's Tryke" },
    {45, 46, "4-9",  "Moley Cow!" },
    {46, 47, "4-10", "Aster's Maze" },
    {47, 25, "4-11", "SASQUATCH beta" },

    {49, 77, "5-1", "Clance War II" },
    {50, 20, "5-2", "Counterattack" },
    {51, 78, "5-3", "Bee's the One" },
    {52, 85, "5-4", "MERCO!" },
    {53, 22, "5-5", "Trapped?" },
    {54, 26, "5-6", "PHOENIX" },
    {56, 79, "5-7", "Inner Struggle" },
    {57, 27, "5-8", "Final Battle" },
}};

constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kGameStateSubState = 0x800BE4F4u;
constexpr uint32_t kCannotPause = 0x800BE4ECu;
constexpr uint32_t kCurrentScene = 0x800BE5D0u;
constexpr uint32_t kCurrentStage = 0x80178162u;
constexpr uint32_t kLoadFlag = 0x800D2908u;
constexpr uint32_t kDisplayListA = 0x8027CEE8u;
constexpr uint32_t kDisplayListB = 0x8027EEE8u;

std::atomic_bool g_enabled{false};
std::atomic_bool g_open{false};
std::atomic_bool g_game_ready{false};
std::atomic_bool g_save_protected{false};
std::atomic_bool g_waiting_for_release{false};
std::atomic<int> g_page{0};
std::atomic<int> g_selected_warp{0};
std::atomic<int> g_selected_cheat{0};
// Pages: 0 Level Warp, 1 Cheats, 2 Spawn, 3 Inspect, 4 Session.
constexpr int kPageWarp = 0;
constexpr int kPageCheats = 1;
constexpr int kPageSpawn = 2;
constexpr int kPageInspect = 3;
constexpr int kPageSession = 4;
constexpr int kPageCount = 5;
constexpr int kSessionRows = 9;   // record, play latest, stop; host, host on Steam, address, join, join on Steam, leave

// Session page: the netplay join address being typed (row 4; Enter starts
// and ends editing, text comes from SDL text input).
std::mutex g_address_mutex;
std::string g_address;
bool g_address_loaded = false;
std::atomic<bool> g_editing_address{false};
// Inspect page rows: the overlay and label toggles, then the active actors
// (nearest to Marina first, from the last snapshot).
constexpr int kInspectToggles = 2;
std::atomic<int> g_selected_inspect{0};
std::atomic<int> g_selected_session{0};
std::atomic<int> g_inspect_rows{kInspectToggles};
std::atomic<int> g_selected_spawn{0};
std::atomic<bool> g_spawn_show_all{false};

// Spawn page rows: the curated catalogue (or every safe shared type when
// "show all" is on), then the show-all toggle and "remove spawned objects".
int spawn_row_count() {
    return (g_spawn_show_all.load(std::memory_order_relaxed)
                ? mm::spawn_catalog::kSafeTypeCount
                : mm::spawn_catalog::kCatalogCount) + 2;
}
std::atomic<int> g_current_stage{-1};
std::atomic<int> g_current_scene{-1};
std::atomic<int> g_current_game_state{-1};

std::array<bool, SDL_CONTROLLER_BUTTON_MAX> g_pad_buttons{};

gpr rdram_gpr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

bool activation_chord_held() {
    return g_pad_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] &&
           g_pad_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] &&
           g_pad_buttons[SDL_CONTROLLER_BUTTON_START];
}

bool navigation_input_released() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const bool keyboard_held =
        keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_ESCAPE] ||
        keys[SDL_SCANCODE_F1] || keys[SDL_SCANCODE_UP] ||
        keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_LEFT] ||
        keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_PAGEUP] ||
        keys[SDL_SCANCODE_PAGEDOWN];
    const bool controller_held =
        g_pad_buttons[SDL_CONTROLLER_BUTTON_A] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_B] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_START];
    return !keyboard_held && !controller_held;
}

void close_overlay() {
    g_open.store(false, std::memory_order_release);
    g_waiting_for_release.store(true, std::memory_order_release);
    std::fprintf(stderr, "[debug-menu] closed\n");
}

void toggle_overlay() {
    if (g_open.load(std::memory_order_acquire)) {
        close_overlay();
        return;
    }
    if (!g_game_ready.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "[debug-menu] toggle ignored: game is not ready\n");
        return;
    }
    g_open.store(true, std::memory_order_release);
    g_waiting_for_release.store(false, std::memory_order_release);
    mm_audio_input::set_input_blocked(true);
    std::fprintf(stderr, "[debug-menu] opened\n");
}

// The Cheats page's throw strength rows: 0 objects, 1 players, -1 another row.
int throw_row(int selected) {
    if (selected == mm::cheats::kCheatCount + 9) return 0;
    if (selected == mm::cheats::kCheatCount + 10) return 1;
    return -1;
}

void change_page(int delta) {
    int page = g_page.load(std::memory_order_relaxed);
    // On a throw strength row, left / right move the slider.
    if (page == kPageCheats) {
        if (const int t = throw_row(g_selected_cheat.load(std::memory_order_relaxed)); t >= 0) {
            mm::cheats::adjust_throw_strength(static_cast<mm::cheats::ThrowKind>(t), delta);
            return;
        }
    }
    page = (page + delta + kPageCount) % kPageCount;
    g_page.store(page, std::memory_order_relaxed);
}

void move_selection(int delta) {
    const int page = g_page.load(std::memory_order_relaxed);
    if (page == kPageCheats) {
        const int count = mm::cheats::kCheatCount + 12;   // + fast-forward speed, rank timer, gem hints, gem compass, rumble, enemy bars, damage numbers, hold to dash, co-op, two throw strength rows, split screen
        int selected = g_selected_cheat.load(std::memory_order_relaxed);
        selected = (selected + delta + count) % count;
        g_selected_cheat.store(selected, std::memory_order_relaxed);
        return;
    }
    if (page == kPageInspect) {
        const int count = std::max(1, g_inspect_rows.load(std::memory_order_relaxed));
        int selected = g_selected_inspect.load(std::memory_order_relaxed);
        selected = (selected + delta + count) % count;
        g_selected_inspect.store(selected, std::memory_order_relaxed);
        return;
    }
    if (page == kPageSession) {
        int selected = g_selected_session.load(std::memory_order_relaxed);
        selected = (selected + delta + kSessionRows) % kSessionRows;
        g_selected_session.store(selected, std::memory_order_relaxed);
        return;
    }
    if (page == kPageSpawn) {
        const int count = spawn_row_count();
        int selected = g_selected_spawn.load(std::memory_order_relaxed);
        selected = (selected + delta + count) % count;
        g_selected_spawn.store(selected, std::memory_order_relaxed);
        return;
    }
    if (page != kPageWarp) {
        return;
    }
    const int count = static_cast<int>(kWarps.size());
    int selected = g_selected_warp.load(std::memory_order_relaxed);
    selected = (selected + delta + count) % count;
    g_selected_warp.store(selected, std::memory_order_relaxed);
}

void activate_selection() {
    const int page = g_page.load(std::memory_order_relaxed);
    if (page == kPageCheats) {
        const int selected = g_selected_cheat.load(std::memory_order_relaxed);
        if (selected == mm::cheats::kCheatCount) {
            mm::cheats::cycle_fast_forward_speed();
        } else if (selected == mm::cheats::kCheatCount + 1) {
            mm::cheats::toggle_rank_timer();
        } else if (selected == mm::cheats::kCheatCount + 2) {
            mm::cheats::toggle_gem_hints();
        } else if (selected == mm::cheats::kCheatCount + 3) {
            mm::cheats::toggle_gem_compass();
        } else if (selected == mm::cheats::kCheatCount + 4) {
            mm::cheats::cycle_rumble_level();
        } else if (selected == mm::cheats::kCheatCount + 5) {
            mm::cheats::toggle_enemy_bars();
        } else if (selected == mm::cheats::kCheatCount + 6) {
            mm::cheats::toggle_damage_numbers();
        } else if (selected == mm::cheats::kCheatCount + 7) {
            mm::cheats::toggle_hold_dash();
        } else if (selected == mm::cheats::kCheatCount + 8) {
            mm::cheats::toggle_coop();
        } else if (selected == mm::cheats::kCheatCount + 11) {
            mm::cheats::toggle_split_screen();
        } else if (const int t = throw_row(selected); t >= 0) {
            // Enter/A steps up, wrapping round to the weakest.
            const auto kind = static_cast<mm::cheats::ThrowKind>(t);
            const bool top = mm::cheats::throw_strength_setting(kind) == mm::cheats::kThrowStrengths[mm::cheats::kThrowStrengthCount - 1];
            mm::cheats::adjust_throw_strength(kind, top ? -mm::cheats::kThrowStrengthCount : 1);
        } else {
            mm::cheats::toggle(static_cast<mm::cheats::Cheat>(selected));
        }
        return;
    }
    if (page == kPageInspect) {
        const int selected = g_selected_inspect.load(std::memory_order_relaxed);
        if (selected == 0) mm::inspector::toggle_overlay();
        if (selected == 1) mm::inspector::toggle_labels();
        return;
    }
    if (page == kPageSession) {
        switch (g_selected_session.load(std::memory_order_relaxed)) {
            case 0: mm::replay::request_record(); close_overlay(); break;
            case 1: mm::replay::request_play_latest(); close_overlay(); break;
            case 2: mm::replay::request_stop(); break;
            case 3: mm::netplay::request_host(); break;
            case 4: mm::netplay::request_host_steam(); break;
            case 5: {
                const bool now = !g_editing_address.load();
                g_editing_address.store(now);
                if (now) SDL_StartTextInput();
                else SDL_StopTextInput();
                break;
            }
            case 6: {
                std::string a;
                {
                    std::lock_guard lock{g_address_mutex};
                    a = g_address;
                }
                if (!a.empty()) mm::netplay::request_join(a);
                break;
            }
            case 7: mm::netplay::request_join_steam(0); break;
            default: mm::netplay::request_leave(); break;
        }
        return;
    }
    if (page == kPageSpawn) {
        const int rows = spawn_row_count();
        const int selected = g_selected_spawn.load(std::memory_order_relaxed);
        if (selected == rows - 2) {
            g_spawn_show_all.store(!g_spawn_show_all.load());
            g_selected_spawn.store(spawn_row_count() - 2, std::memory_order_relaxed);
        } else if (selected == rows - 1) {
            mm::actors::request_clear();
        } else if (g_spawn_show_all.load(std::memory_order_relaxed)) {
            mm::actors::request_spawn(mm::spawn_catalog::kSafeTypes[selected]);
        } else {
            mm::actors::request_spawn(mm::spawn_catalog::kCatalog[selected].type,
                                      mm::spawn_catalog::kCatalog[selected].param);
        }
        return;
    }
    if (page != kPageWarp) {
        return;
    }
    {
        // A command in the input stream (frame_input.h): the same frame in a
        // replay, and online the host's warp reaches both machines.
        const int i = g_selected_warp.load(std::memory_order_relaxed);
        if (i >= 0 && i < static_cast<int>(kWarps.size())) {
            mm::frame_input::Command c;
            c.kind = mm::frame_input::Cmd::Warp;
            c.a = kWarps[i].stage;
            c.b = static_cast<uint32_t>(kWarps[i].scene);
            mm::frame_input::request(c);
        }
    }
    close_overlay();
}

bool handle_navigation_key(SDL_Scancode key) {
    switch (key) {
        case SDL_SCANCODE_ESCAPE:   close_overlay(); return true;
        case SDL_SCANCODE_UP:       move_selection(-1); return true;
        case SDL_SCANCODE_DOWN:     move_selection(1); return true;
        case SDL_SCANCODE_PAGEUP:
        case SDL_SCANCODE_LEFT:     change_page(-1); return true;
        case SDL_SCANCODE_PAGEDOWN:
        case SDL_SCANCODE_RIGHT:    change_page(1); return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: activate_selection(); return true;
        default: return false;
    }
}

bool handle_navigation_button(Uint8 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_B: close_overlay(); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move_selection(-1); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_selection(1); return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: change_page(-1); return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: change_page(1); return true;
        case SDL_CONTROLLER_BUTTON_A: activate_selection(); return true;
        default: return false;
    }
}

} // namespace

void configure(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    std::fprintf(stderr, "[debug-menu] %s\n", enabled ? "enabled" : "disabled");
    if (!enabled) {
        g_open.store(false, std::memory_order_release);
        g_waiting_for_release.store(false, std::memory_order_release);
        mm_audio_input::set_input_blocked(false);
    }
}

bool handle_event(const SDL_Event& event) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return false;
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN ||
        event.type == SDL_CONTROLLERBUTTONUP) {
        const Uint8 button = event.cbutton.button;
        if (button < g_pad_buttons.size()) {
            g_pad_buttons[button] = event.type == SDL_CONTROLLERBUTTONDOWN;
        }
    }

    if (g_waiting_for_release.load(std::memory_order_acquire)) {
        if (navigation_input_released()) {
            g_waiting_for_release.store(false, std::memory_order_release);
            mm_audio_input::set_input_blocked(false);
        }
        return event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
               event.type == SDL_CONTROLLERBUTTONDOWN ||
               event.type == SDL_CONTROLLERBUTTONUP;
    }

    // Typing the netplay address: text and Backspace go to it; Enter / Esc finish.
    if (g_editing_address.load(std::memory_order_acquire) && g_open.load(std::memory_order_acquire)) {
        if (event.type == SDL_TEXTINPUT) {
            std::lock_guard lock{g_address_mutex};
            for (const char* c = event.text.text; *c; ++c) {
                if (g_address.size() < 64 && *c > ' ' && *c < 127) g_address.push_back(*c);
            }
            return true;
        }
        if (event.type == SDL_KEYDOWN) {
            const SDL_Scancode sc = event.key.keysym.scancode;
            if (sc == SDL_SCANCODE_BACKSPACE) {
                std::lock_guard lock{g_address_mutex};
                if (!g_address.empty()) g_address.pop_back();
            } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_ESCAPE) {
                g_editing_address.store(false);
                SDL_StopTextInput();
            } else if (sc == SDL_SCANCODE_V && (SDL_GetModState() & KMOD_CTRL)) {
                if (char* clip = SDL_GetClipboardText()) {
                    std::lock_guard lock{g_address_mutex};
                    for (const char* c = clip; *c; ++c) {
                        if (g_address.size() < 64 && *c > ' ' && *c < 127) g_address.push_back(*c);
                    }
                    SDL_free(clip);
                }
            }
            return true;
        }
        if (event.type == SDL_KEYUP) return true;
    }

    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
        if (event.key.keysym.scancode == SDL_SCANCODE_F1) {
            toggle_overlay();
            return true;
        }
        if (event.key.keysym.scancode == SDL_SCANCODE_F2) {
            mm::inspector::toggle_overlay();     // collision view, also with the menu closed
            return true;
        }
        if (g_open.load(std::memory_order_acquire)) {
            handle_navigation_key(event.key.keysym.scancode);
            return true;
        }
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        if (!g_open.load(std::memory_order_acquire) && activation_chord_held()) {
            toggle_overlay();
            return true;
        }
        if (g_open.load(std::memory_order_acquire)) {
            handle_navigation_button(event.cbutton.button);
            return true;
        }
    }

    // Keep every keyboard/controller event away from the normal hotkey path
    // while the menu owns input, including key-up events.
    if (g_open.load(std::memory_order_acquire)) {
        return event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
               event.type == SDL_CONTROLLERBUTTONDOWN ||
               event.type == SDL_CONTROLLERBUTTONUP ||
               event.type == SDL_CONTROLLERAXISMOTION;
    }
    return false;
}

extern "C" int mm_far_in_shadow(void);

extern "C" void mm_debug_menu_tick(unsigned char* rdram) {
    // Player 2's picture (far_coop.h): only what is drawn over it comes from
    // this frame; everything else here keeps host state.
    if (mm_far_in_shadow()) {
        // (Split screen: overlays publish once, from the real frame, and
        // draw in both views; split_view.h.)
        if (!mm::faraway::split_local()) {
            mm::enemy_bars::publish(rdram);
            mm::throw_cheats::publish(rdram);
        }
        return;
    }
    const int game_state = MEM_HU(0, rdram_gpr(kGameState));
    const int current_stage = MEM_HU(0, rdram_gpr(kCurrentStage));
    const int current_scene = static_cast<int16_t>(
        MEM_HU(0, rdram_gpr(kCurrentScene)));
    mm::telemetry::record_scene(game_state, current_stage, current_scene);
    mm::crash_handler::note_scene(game_state, current_stage, current_scene);
    mm::rank_timer::publish(rdram);
    mm::gem_hints::publish(rdram);
    mm::gem_compass::publish(rdram);
    mm::rumble::tick(rdram);
    mm::cheats::apply_settings(rdram);
    mm::actors::tick(rdram);
    mm::player2::tick(rdram);
    mm::dev_harness::tick(rdram);
    mm::split_view::publish(rdram);
    if (!mm::faraway::shows_second_view(rdram) || mm::faraway::split_local()) {
        mm::enemy_bars::publish(rdram);
        mm::throw_cheats::publish(rdram);
    }
    mm::cheats::set_active(g_enabled.load(std::memory_order_acquire));
    if (g_enabled.load(std::memory_order_acquire)) mm::inspector::publish(rdram);

    // Cheats run while they are live: this player's debug menu is enabled,
    // or a netplay host's is (cheats.h, SessionSettings).
    if (!g_enabled.load(std::memory_order_acquire)) {
        if (mm::cheats::g_active_for_hooks()) mm::cheats::apply(rdram);
        return;
    }
    if (mm::cheats::g_active_for_hooks()) mm::cheats::apply(rdram);

    g_current_game_state.store(game_state, std::memory_order_relaxed);
    g_current_stage.store(current_stage, std::memory_order_relaxed);
    g_current_scene.store(current_scene, std::memory_order_relaxed);
    g_game_ready.store(game_state != 0, std::memory_order_release);

    // Headless harness: MM_TEST_WARP=1-3 warps there once the title is up
    // (about 300 frames in).
    if (static const char* test_warp = std::getenv("MM_TEST_WARP"); test_warp != nullptr) {
        static int frames = 0;
        if (++frames == 300) {
            for (int i = 0; i < static_cast<int>(kWarps.size()); ++i) {
                if (std::string_view{kWarps[i].label} == test_warp) {
                    mm::frame_input::Command c;
                    c.kind = mm::frame_input::Cmd::Warp;
                    c.a = kWarps[i].stage;
                    c.b = static_cast<uint32_t>(kWarps[i].scene);
                    mm::frame_input::request(c);
                }
            }
        }
    }
    if (mm::frame_input::Command cmd; mm::frame_input::take_command(mm::frame_input::Cmd::Warp, &cmd)) {
        WarpEntry warp{cmd.a, static_cast<int>(cmd.b), "?", "warp"};
        for (const WarpEntry& w : kWarps) {
            if (w.stage == warp.stage && w.scene == warp.scene) warp = w;
        }

        // From this point through process exit, EEPROM writes become successful
        // no-ops. Reads still use the original on-disk save buffer, so a debug
        // run cannot leak unlocked stages, gems, times, or completion flags.
        recomp::set_save_writes_enabled(false);
        g_save_protected.store(true, std::memory_order_release);

        const int scene = mm_debug_warp_prepare(rdram, warp.scene, warp.stage);
        MEM_H(0, rdram_gpr(kCurrentScene)) = static_cast<int16_t>(scene);
        MEM_H(0, rdram_gpr(kCannotPause)) = 0;
        MEM_H(0, rdram_gpr(kLoadFlag)) = 1;
        MEM_H(0, rdram_gpr(kGameState)) = 5; // GAMESTATE_LOADING
        MEM_H(0, rdram_gpr(kGameStateSubState)) = 0;

        // Match the established MM_WARP harness: discard stale commands from
        // both display-list arenas before the new stage's loading pass.
        for (int i = 0; i < 0x1000; ++i) {
            MEM_W(i * 4, rdram_gpr(kDisplayListA)) = 0;
        }
        for (int i = 0; i < 0xC00; ++i) {
            MEM_W(i * 4, rdram_gpr(kDisplayListB)) = 0;
        }
        std::fprintf(stderr,
            "[debug-menu] warp %s %s; save writes disabled for this session\n",
            warp.label, warp.name);
        return;
    }
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !g_open.load(std::memory_order_acquire)) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(
        std::min(720.0f, std::max(420.0f, io.DisplaySize.x - 48.0f)),
        std::min(560.0f, std::max(320.0f, io.DisplaySize.y - 48.0f)));
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Trouble Makers Debug Menu", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const int page = g_page.load(std::memory_order_relaxed);
    static const char* const kPageTitles[kPageCount] = {
        "[ Level Warp ]   Cheats     Spawn     Inspect     Session  ",
        "  Level Warp   [ Cheats ]   Spawn     Inspect     Session  ",
        "  Level Warp     Cheats   [ Spawn ]   Inspect     Session  ",
        "  Level Warp     Cheats     Spawn   [ Inspect ]   Session  ",
        "  Level Warp     Cheats     Spawn     Inspect   [ Session ]",
    };
    ImGui::TextUnformatted(kPageTitles[page]);
    ImGui::SameLine();
    ImGui::TextDisabled("  PageUp/PageDown or L/R");
    ImGui::Separator();

    if (page == 0) {
        ImGui::TextWrapped(
            "Choose a campaign stage. Enter/A warps; Esc/B closes. "
            "The first warp disables save writes until the game exits.");
        ImGui::Spacing();
        const int selected = g_selected_warp.load(std::memory_order_relaxed);
        ImGui::BeginChild("##warps", ImVec2(0.0f, -34.0f), true);
        for (int i = 0; i < static_cast<int>(kWarps.size()); ++i) {
            char row[128];
            std::snprintf(row, sizeof row, "%5s   %-24s  (stage %02d / scene %02d)",
                          kWarps[i].label, kWarps[i].name,
                          kWarps[i].stage, kWarps[i].scene);
            ImGui::Selectable(row, i == selected);
            if (i == selected) {
                ImGui::SetScrollHereY(0.5f);
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down: select   Enter/A: warp   Esc/B: close");
    }
    else if (page == kPageCheats) {
        ImGui::TextWrapped(
            "Cheats stay on between sessions until you turn them off. "
            "They only work while the debug menu is enabled in the launcher.");
        if (mm::cheats::settings_from_host()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
            ImGui::TextWrapped(
                "Online: the host's cheats, hold to dash and gem hints are shown and "
                "used for both players; only the host can change them. Your own "
                "settings come back when you leave.");
            ImGui::PopStyleColor();
        } else if (mm::netplay::active()) {
            ImGui::TextWrapped("Online: your changes apply to both players a few frames later.");
        }
        ImGui::Spacing();
        const int selected = g_selected_cheat.load(std::memory_order_relaxed);
        ImGui::BeginChild("##cheats", ImVec2(0.0f, -34.0f), true);
        for (int i = 0; i < mm::cheats::kCheatCount; ++i) {
            const auto cheat = static_cast<mm::cheats::Cheat>(i);
            const bool on = mm::cheats::enabled(cheat);
            char row[96];
            std::snprintf(row, sizeof row, "%-5s %s", on ? "[ON]" : "[  ]",
                          mm::cheats::name(cheat));
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, i == selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (i == selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", mm::cheats::description(cheat));
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        ImGui::Separator();
        {
            const bool row_selected = selected == mm::cheats::kCheatCount;
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Fast-forward speed",
                          (std::to_string(mm::cheats::fast_forward_speed()) + "x").c_str());
            ImGui::Selectable(row, row_selected);
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "How fast the game runs while the fast-forward hotkey (Tab by "
                    "default) is held. Enter/A cycles 2x, 3x, 4x, 5x, 6x, 8x. Works "
                    "even with the debug menu turned off. Very high speeds may be "
                    "limited by how fast your PC can draw frames.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 1;
            const bool on = mm::cheats::rank_timer_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Rank timer", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "Top-right countdown to the best clear-time rank you can still get "
                    "(S, A, B, C, then D) and how long you have left for it. Pauses with "
                    "the stage timer. Works even with the debug menu turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 2;
            const bool on = mm::cheats::gem_hints_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Gem hints when paused", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "When you pause in a stage, shows how to find its golden gem (from "
                    "the Mischief Makers Wiki) and whether you already have it. Works "
                    "even with the debug menu turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 3;
            const bool on = mm::cheats::gem_compass_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Golden gem compass", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "A gold arrow at the screen edge points to the stage's golden gem, "
                    "with its distance; on screen a marker bobs above it. Hidden once "
                    "the gem is collected. Gems that appear later (from an enemy or a "
                    "reward) show a note instead. Works even with the debug menu "
                    "turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 4;
            char row[96];
            std::snprintf(row, sizeof row, "      Rumble: %s", mm::cheats::rumble_level_name());
            ImGui::Selectable(row, row_selected);
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "Controller rumble when Marina is hit, lands from a long fall, "
                    "starts a jet dash or collects a golden gem, when bombs and red "
                    "blocks explode, and while the screen shakes. Enter/A cycles Off, "
                    "Low, Medium and High and plays a sample. Works even with the "
                    "debug menu turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 5;
            const bool on = mm::cheats::enemy_bars_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Enemy health bars", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "A small health bar appears above an enemy once you hit it and it "
                    "survives, and fades a few seconds later; bosses keep theirs until "
                    "beaten. Works even with the debug menu turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 6;
            const bool on = mm::cheats::damage_numbers_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Damage numbers", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "The damage of each hit floats up from whoever took it: yellow "
                    "for enemies, red for Marina. Works even with the debug menu "
                    "turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 7;
            const bool on = mm::cheats::hold_dash_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Hold to dash", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "Double-tap a direction to jet, then keep holding it: the jet keeps "
                    "going at full speed until you let go, no more tapping. In the air "
                    "the jet still arcs down. Works even with the debug menu turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 8;
            const bool on = mm::cheats::coop_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Co-op: player 2", on ? "[ON]" : "[  ]");
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            }
            ImGui::Selectable(row, row_selected);
            if (on) {
                ImGui::PopStyleColor();
            }
            if (row_selected) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "A second Marina (tinted blue, \"2P\" over her head) joins a moment "
                    "into every scene on a second controller, or the keyboard: T F G H "
                    "or the numpad to move, Y jump, U grab. The camera frames you both; "
                    "if she is defeated or left behind she comes back next to player 1. "
                    "Works even with the debug menu turned off.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        for (int t = 0; t < 2; ++t) {
            const auto kind = static_cast<mm::cheats::ThrowKind>(t);
            const bool row_selected = selected == mm::cheats::kCheatCount + 9 + t;
            const int percent = mm::cheats::throw_strength_setting(kind);
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Throw strength: %s", (std::to_string(percent) + "%").c_str(),
                          t == 0 ? "objects" : "players");
            const bool changed = percent != 100;
            if (changed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            ImGui::Selectable(row, row_selected);
            if (changed) ImGui::PopStyleColor();
            // The slider: one notch per step.
            {
                int step = 0;
                for (int i = 0; i < mm::cheats::kThrowStrengthCount; ++i) {
                    if (mm::cheats::kThrowStrengths[i] == percent) step = i;
                }
                const float frac = static_cast<float>(step) / (mm::cheats::kThrowStrengthCount - 1);
                ImGui::Indent();
                ImGui::ProgressBar(frac, ImVec2(220.0f, 6.0f), "");
                ImGui::Unindent();
            }
            if (row_selected) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(t == 0
                    ? "How hard thrown objects fly (blocks, enemies, characters), from 25%% to "
                      "1200%% of the normal throw. Left/Right move the slider; Enter/A steps "
                      "up. A cheat: it works while the debug menu is enabled."
                    : "How hard the players fly when they throw each other, from 25%% to "
                      "1200%%. Left/Right move the slider; Enter/A steps up. A cheat: it "
                      "works while the debug menu is enabled.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        {
            const bool row_selected = selected == mm::cheats::kCheatCount + 11;
            const bool on = mm::cheats::split_screen_enabled();
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Split screen (local co-op)", on ? "[ON]" : "[  ]");
            if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            ImGui::Selectable(row, row_selected);
            if (on) ImGui::PopStyleColor();
            if (row_selected) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "With co-op on, each player gets her own camera and half of the window: "
                    "player 1 on top, player 2 below. The players can go anywhere in the "
                    "stage, as online. Turn Widescreen on in the launcher; each half shows "
                    "a view twice as wide as the original. Not used online (each player "
                    "has her own screen there).");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down: select   Enter/A: change   Left/Right: slider or page   Esc/B: close");
    }
    else if (page == kPageSpawn) {
        const bool show_all = g_spawn_show_all.load(std::memory_order_relaxed);
        ImGui::TextWrapped(
            "Enter/A drops the object a short step in front of Marina, in the "
            "direction she last walked. Up to %d spawned objects at a time; they "
            "disappear when you leave the stage.", mm::actors::kMaxSpawned);
        ImGui::TextDisabled("%s   (%d spawned now)", mm::actors::last_result(),
                            mm::actors::spawned_count());
        ImGui::Spacing();
        const int selected = g_selected_spawn.load(std::memory_order_relaxed);
        const int rows = spawn_row_count();
        ImGui::BeginChild("##spawn", ImVec2(0.0f, -34.0f), true);
        for (int i = 0; i < rows - 2; ++i) {
            char row[128];
            if (show_all) {
                const uint16_t type = mm::spawn_catalog::kSafeTypes[i];
                const char* name = mm::spawn_catalog::name_of(type);
                std::snprintf(row, sizeof row, "0x%02X   %s", type, name ? name : "(unnamed)");
            } else {
                const auto& e = mm::spawn_catalog::kCatalog[i];
                std::snprintf(row, sizeof row, "%-28s 0x%02X", e.name, e.type);
            }
            ImGui::Selectable(row, i == selected);
            if (i == selected) {
                ImGui::SetScrollHereY(0.5f);
                if (!show_all && mm::spawn_catalog::kCatalog[i].note[0] != '\0') {
                    ImGui::Indent();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("%s", mm::spawn_catalog::kCatalog[i].note);
                    ImGui::PopStyleColor();
                    ImGui::Unindent();
                }
            }
        }
        ImGui::Separator();
        {
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Show every shared object type", show_all ? "[ON]" : "[  ]");
            ImGui::Selectable(row, selected == rows - 2);
            if (selected == rows - 2) ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Selectable("      Remove spawned objects", selected == rows - 1);
        if (selected == rows - 1) ImGui::SetScrollHereY(0.5f);
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down: select   Enter/A: spawn   Esc/B: close");
    }
    else if (page == kPageInspect) {
        const auto actors = mm::inspector::actors();
        const int rows = kInspectToggles + static_cast<int>(actors.size());
        g_inspect_rows.store(rows, std::memory_order_relaxed);
        int selected = std::min(g_selected_inspect.load(std::memory_order_relaxed), rows - 1);
        const int selected_actor = selected - kInspectToggles;
        mm::inspector::set_selected_slot(selected_actor >= 0 ? actors[selected_actor].slot : -1);
        ImGui::TextWrapped(
            "Every active object, nearest to Marina first. The collision view "
            "(F2 toggles it any time, menu open or closed) outlines terrain "
            "cells, hit / attack boxes and solid objects; the selected object "
            "stays highlighted after you close the menu.");
        ImGui::Spacing();
        ImGui::BeginChild("##inspect", ImVec2(0.0f, -34.0f), true);
        {
            char row[96];
            std::snprintf(row, sizeof row, "%-5s Collision view (F2)",
                          mm::inspector::overlay_enabled() ? "[ON]" : "[  ]");
            ImGui::Selectable(row, selected == 0);
            if (selected == 0) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(
                    "Terrain: red = solid, cyan top = stand-on only. Objects: green = hit "
                    "box (faint when off), red = attack box, orange fill = solid object, "
                    "yellow top = stand-on object, white = Marina. Purple = gems and blocks "
                    "of the stage.");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
            std::snprintf(row, sizeof row, "%-5s Labels (slot:type)",
                          mm::inspector::labels_enabled() ? "[ON]" : "[  ]");
            ImGui::Selectable(row, selected == 1);
            if (selected == 1) ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Separator();
        ImGui::TextDisabled("slot  type  name                  state  hp     flags     dist");
        for (int i = 0; i < static_cast<int>(actors.size()); ++i) {
            const auto& a = actors[i];
            const std::string name = mm::inspector::type_name(a.type);
            char row[160];
            std::snprintf(row, sizeof row, "%4d  %04X  %-20.20s  %04X  %-5d  %08X  %d",
                          a.slot, a.type, name.empty() ? "-" : name.c_str(), a.state, a.hp,
                          a.flags, a.distance);
            const bool is_selected = selected == kInspectToggles + i;
            ImGui::Selectable(row, is_selected);
            if (is_selected) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Text("pos %d,%d  vel %.2f,%.2f px/frame  image %04X  draw %04X",
                            a.x, a.y, a.vx / 16.0, a.vy / 16.0, a.image, a.draw);
                ImGui::Text("hit box L%d R%d T%d B%d%s   attack box L%d R%d T%d B%d%s",
                            a.hit[0], a.hit[1], a.hit[2], a.hit[3], (a.flags & 0x400u) ? "" : " (off)",
                            a.attack[0], a.attack[1], a.attack[2], a.attack[3], (a.flags & 0x80u) ? "" : " (off)");
                ImGui::Text("signals +98 %08X  +D6 %d  stands on %s%d%s%s",
                            a.signals, a.held, (a.stand_on & 0x8000u) ? "slot " : "",
                            (a.stand_on & 0x8000u) ? (a.stand_on & 0x7FFF) : 0,
                            (a.flags & 0x00040000u) ? "  [solid]" : "", (a.flags & 0x2000u) ? "  [platform]" : "");
                ImGui::PopStyleColor();
                ImGui::Unindent();
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down: select   Enter/A: toggle   F2: collision view   Esc/B: close");
    }
    else {
        ImGui::TextUnformatted("Replays");
        ImGui::TextWrapped(
            "A replay records the game from a save state taken now plus every "
            "controller input, and plays it back exactly. Playback checks game "
            "memory against the recording every frame: the groundwork for netplay. "
            "Replays are saved in the replays folder next to savestates. Cheat or "
            "menu changes made while recording are not part of the replay.");
        const int selected = g_selected_session.load(std::memory_order_relaxed);
        const mm::replay::Mode mode = mm::replay::mode();
        ImGui::Selectable("      Record a replay from here", selected == 0);
        ImGui::Selectable("      Play the latest replay", selected == 1);
        ImGui::Selectable("      Stop recording / playback", selected == 2);
        ImGui::Separator();
        ImGui::TextUnformatted("Online co-op (netplay)");
        ImGui::TextWrapped(
            "The host plays Marina, the other player joins as player 2 from the host's "
            "game. Both need the same build. On Steam (both running Steam, signed in, "
            "friends): the host picks Host on Steam and invites, the friend accepts the "
            "invite or picks Join a friend on Steam. No ports or addresses. Without Steam, "
            "the host's port (UDP 7845) must be reachable: on the same network use the "
            "host's local address; over the internet a VPN like Tailscale is easiest.");
        {
            std::string address;
            {
                std::lock_guard lock{g_address_mutex};
                if (!g_address_loaded) {
                    g_address = mm::netplay::last_address();
                    g_address_loaded = true;
                }
                address = g_address;
            }
            const bool editing = g_editing_address.load();
            ImGui::Selectable("      Host a game (port 7845)", selected == 3);
            ImGui::Selectable("      Host on Steam (no port forwarding)", selected == 4);
            const std::string row = std::string("      Address: ") + (address.empty() ? "(none)" : address) +
                                    (editing ? "_   (type, Enter to finish)" : "   (Enter to edit)");
            ImGui::Selectable(row.c_str(), selected == 5);
            ImGui::Selectable("      Join that address (or steam:<lobby id>)", selected == 6);
            ImGui::Selectable("      Join a friend on Steam (or accept their invite)", selected == 7);
            ImGui::Selectable("      Leave the online game", selected == 8);
            const std::string net = mm::netplay::status_text();
            ImGui::BulletText("%s", net.empty() ? "Not connected" : net.c_str());
        }
        if (mode == mm::replay::Mode::Recording) {
            ImGui::BulletText("Recording: frame %ld", mm::replay::frame());
        } else if (mode == mm::replay::Mode::Playing) {
            const long mismatch = mm::replay::first_mismatch();
            if (mismatch < 0) {
                ImGui::BulletText("Playing: frame %ld, identical to the recording so far", mm::replay::frame());
            } else {
                ImGui::BulletText("Playing: frame %ld, differed from the recording at frame %ld",
                                  mm::replay::frame(), mismatch);
            }
        } else {
            ImGui::BulletText("No replay running");
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Debug session status");
        ImGui::Spacing();
        ImGui::BulletText("Current progression row: %d",
            g_current_stage.load(std::memory_order_relaxed));
        ImGui::BulletText("Current scene: %d",
            g_current_scene.load(std::memory_order_relaxed));
        ImGui::BulletText("Game state: %d",
            g_current_game_state.load(std::memory_order_relaxed));
        if (g_save_protected.load(std::memory_order_acquire)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            ImGui::BulletText("Save protection: ACTIVE (save writes are blocked)");
            ImGui::PopStyleColor();
        }
        else {
            ImGui::BulletText("Save protection: armed; activates on first warp");
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Debug warps change the game's in-memory progression data so the "
            "destination loads with a complete stage-table row. Those changes "
            "are never copied to the save file after protection activates.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "F1 or L+R+Start toggles this overlay. Gameplay input is held "
            "while it is open.");
    }

    ImGui::End();
}
#endif

} // namespace mm::debug_menu
