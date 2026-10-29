// Input backend for ultramodern's input::callbacks_t, backed by SDL2 and the
// persistent two-slot mappings in controls.cpp. Keyboard and the first SDL
// game controller are combined into N64 port 0; with co-op on, player 2's
// keys and the second game controller, through player 2's bindings, into
// port 1.
#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "mm_audio_input.hpp"

namespace mm_audio_input {
namespace {

// N64 controller button bits (libultra PR/os_cont.h layout).
constexpr uint16_t BTN_A      = 0x8000;
constexpr uint16_t BTN_B      = 0x4000;
constexpr uint16_t BTN_Z      = 0x2000; // CONT_G
constexpr uint16_t BTN_START = 0x1000;
constexpr uint16_t BTN_UP     = 0x0800;
constexpr uint16_t BTN_DOWN   = 0x0400;
constexpr uint16_t BTN_LEFT   = 0x0200;
constexpr uint16_t BTN_RIGHT  = 0x0100;
constexpr uint16_t BTN_L      = 0x0020;
constexpr uint16_t BTN_R      = 0x0010;
constexpr uint16_t BTN_CU     = 0x0008; // CONT_E
constexpr uint16_t BTN_CD     = 0x0004; // CONT_D
constexpr uint16_t BTN_CL     = 0x0002; // CONT_C
constexpr uint16_t BTN_CR     = 0x0001; // CONT_F

const Uint8* g_keys = nullptr;
int g_numkeys = 0;
SDL_GameController* g_controller = nullptr;
// Host-side rumble pulse requested by src/game/rumble.cpp, applied on the
// input-polling path that owns g_controller: low motor << 48 | high motor << 32
// | milliseconds.
std::atomic<uint64_t> g_rumble_request{0};
std::atomic<bool> g_rumble_muted{false};   // presentation off
// Last time the player pressed anything (SDL ticks). Rumble is skipped when
// nobody has touched the controls for a minute, so the attract-mode demo on
// the title screen does not shake an idle controller.
std::atomic<uint64_t> g_last_activity{0};
constexpr uint64_t kRumbleIdleMs = 60000;
std::atomic_bool g_input_blocked{false};

constexpr float kDigitalAxisThreshold = 0.5f;

inline bool key(SDL_Scancode s) {
    return g_keys != nullptr && static_cast<int>(s) < g_numkeys && g_keys[s] != 0;
}

// Normalize an SDL axis to [-1.0, 1.0].
inline float axis_norm(Sint16 v) {
    return static_cast<float>(v) / (v < 0 ? 32768.0f : 32767.0f);
}

void open_first_controller() {
    if (g_controller != nullptr) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller != nullptr) {
                SDL_Joystick* joystick = SDL_GameControllerGetJoystick(g_controller);
                char guid[33] = {};
                if (joystick != nullptr) {
                    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid,
                                              static_cast<int>(sizeof(guid)));
                }
                char* mapping = SDL_GameControllerMapping(g_controller);
                const char* name = SDL_GameControllerName(g_controller);
                std::fprintf(stderr,
                    "[input] Controller connected: %s (GUID %s, SDL mapping: %s)\n",
                    name != nullptr ? name : "Unknown controller",
                    guid[0] != '\0' ? guid : "unknown",
                    mapping != nullptr ? "yes" : "no");
                SDL_free(mapping);
                return;
            }
        }
    }
}

// Player 2 (co-op): the second game controller, with a fixed N64-style
// layout, plus keyboard keys that don't clash with player 1's defaults.
SDL_GameController* g_controller2 = nullptr;
std::atomic_bool g_player2_input{false};

void open_second_controller() {
    if (g_controller2 != nullptr || g_controller == nullptr) return;
    SDL_Joystick* first = SDL_GameControllerGetJoystick(g_controller);
    const SDL_JoystickID first_id = first != nullptr ? SDL_JoystickInstanceID(first) : -1;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i) || SDL_JoystickGetDeviceInstanceID(i) == first_id) continue;
        g_controller2 = SDL_GameControllerOpen(i);
        if (g_controller2 != nullptr) {
            const char* name = SDL_GameControllerName(g_controller2);
            std::fprintf(stderr, "[input] Player 2 controller: %s\n", name != nullptr ? name : "Unknown controller");
            return;
        }
    }
}

void refresh_controller_internal() {
    if (g_controller != nullptr && !SDL_GameControllerGetAttached(g_controller)) {
        std::fprintf(stderr, "[input] Controller disconnected.\n");
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
    if (g_controller2 != nullptr && !SDL_GameControllerGetAttached(g_controller2)) {
        std::fprintf(stderr, "[input] Player 2 controller disconnected.\n");
        SDL_GameControllerClose(g_controller2);
        g_controller2 = nullptr;
    }
    open_first_controller();
    if (g_player2_input.load(std::memory_order_relaxed)) open_second_controller();
}

float binding_strength(InputBinding binding, SDL_GameController* controller) {
    switch (binding.type) {
        case BindingType::None:
            return 0.0f;
        case BindingType::Keyboard:
            return key(static_cast<SDL_Scancode>(binding.id)) ? 1.0f : 0.0f;
        case BindingType::ControllerButton:
            if (controller == nullptr) return 0.0f;
            return SDL_GameControllerGetButton(
                       controller, static_cast<SDL_GameControllerButton>(binding.id))
                ? 1.0f : 0.0f;
        case BindingType::ControllerAxis: {
            if (controller == nullptr || binding.id == 0) return 0.0f;
            const int encoded_axis = std::abs(binding.id);
            const auto axis = static_cast<SDL_GameControllerAxis>(encoded_axis - 1);
            const float direction = binding.id < 0 ? -1.0f : 1.0f;
            return std::max(0.0f,
                axis_norm(SDL_GameControllerGetAxis(controller, axis)) * direction);
        }
    }
    return 0.0f;
}

// player 0: the keyboard and the first controller; player 1: the keyboard
// and the second controller, each through that player's bindings.
float input_strength(N64Input input, int player = 0) {
    SDL_GameController* const controller = player == 0 ? g_controller : g_controller2;
    float result = 0.0f;
    constexpr std::array devices{
        ControlDevice::Keyboard, ControlDevice::Controller};
    for (ControlDevice device : devices) {
        for (size_t slot = 0; slot < kBindingsPerInput; ++slot) {
            result = std::max(result, binding_strength(get_binding(device, input, slot, player), controller));
        }
    }
    return result;
}

struct DigitalInput {
    N64Input input;
    uint16_t mask;
};
constexpr std::array kDigitalInputs{
    DigitalInput{N64Input::A, BTN_A},
    DigitalInput{N64Input::B, BTN_B},
    DigitalInput{N64Input::Z, BTN_Z},
    DigitalInput{N64Input::Start, BTN_START},
    DigitalInput{N64Input::DpadUp, BTN_UP},
    DigitalInput{N64Input::DpadDown, BTN_DOWN},
    DigitalInput{N64Input::DpadLeft, BTN_LEFT},
    DigitalInput{N64Input::DpadRight, BTN_RIGHT},
    DigitalInput{N64Input::L, BTN_L},
    DigitalInput{N64Input::R, BTN_R},
    DigitalInput{N64Input::CUp, BTN_CU},
    DigitalInput{N64Input::CDown, BTN_CD},
    DigitalInput{N64Input::CLeft, BTN_CL},
    DigitalInput{N64Input::CRight, BTN_CR},
};

// One player's N64 buttons and stick from their bindings.
uint16_t player_input(int player, float& x, float& y) {
    uint16_t b = 0;
    for (const DigitalInput& input : kDigitalInputs) {
        if (input_strength(input.input, player) >= kDigitalAxisThreshold) b |= input.mask;
    }
    x = std::clamp(input_strength(N64Input::AnalogRight, player) - input_strength(N64Input::AnalogLeft, player), -1.0f, 1.0f);
    y = std::clamp(input_strength(N64Input::AnalogUp, player) - input_strength(N64Input::AnalogDown, player), -1.0f, 1.0f);
    return b;
}

// Player 2's buttons. Her pad goes to the game (and netplay) as buttons
// only, so her stick also drives the D-pad bits, as Marina's own
// controller handling does for player 1's.
uint16_t player2_buttons() {
    float x = 0.0f, y = 0.0f;
    uint16_t b = player_input(1, x, y);
    if (y > kDigitalAxisThreshold) b |= BTN_UP;
    if (y < -kDigitalAxisThreshold) b |= BTN_DOWN;
    if (x < -kDigitalAxisThreshold) b |= BTN_LEFT;
    if (x > kDigitalAxisThreshold) b |= BTN_RIGHT;
    return b;
}

void poll_input() {
    // Refresh the keyboard snapshot. SDL_PumpEvents is idempotent if the host
    // also pumps; calling it here keeps input self-sufficient even if the host
    // loop hasn't pumped yet this frame.
    SDL_PumpEvents();
    g_keys = SDL_GetKeyboardState(&g_numkeys);
    refresh_controller_internal();
    const uint64_t rumble = g_rumble_request.exchange(0, std::memory_order_acq_rel);
    const bool active = SDL_GetTicks64() - g_last_activity.load(std::memory_order_relaxed) < kRumbleIdleMs;
    if (rumble != 0 && active && g_controller != nullptr && SDL_GameControllerHasRumble(g_controller)) {
        SDL_GameControllerRumble(g_controller, static_cast<uint16_t>(rumble >> 48),
                                 static_cast<uint16_t>(rumble >> 32),
                                 static_cast<uint32_t>(rumble & 0xFFFFFFFFu));
    }
}

std::atomic<InputFilter> g_input_filter{nullptr};

bool live_input(int controller_num, uint16_t* buttons, float* x, float* y);

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (buttons == nullptr || x == nullptr || y == nullptr) return false;
    const bool connected = live_input(controller_num, buttons, x, y);
    if (const InputFilter filter = g_input_filter.load(std::memory_order_acquire)) {
        if (!connected) {
            *buttons = 0;
            *x = 0.0f;
            *y = 0.0f;
        }
        return filter(controller_num, buttons, x, y);
    }
    return connected;
}

bool live_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num == 1 && g_player2_input.load(std::memory_order_relaxed)) {
        // While the menu holds input, player 2 is held too.
        *buttons = g_input_blocked.load(std::memory_order_relaxed) ? 0 : player2_buttons();
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }
    if (controller_num != 0) {
        return false;
    }

    if (g_input_blocked.load(std::memory_order_relaxed)) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }

    float sx = 0.0f, sy = 0.0f;
    const uint16_t b = player_input(0, sx, sy);

    if (b != 0 || std::fabs(sx) > 0.3f || std::fabs(sy) > 0.3f) {
        g_last_activity.store(SDL_GetTicks64(), std::memory_order_relaxed);
    }
    *buttons = b;
    *x = std::clamp(sx, -1.0f, 1.0f);
    *y = std::clamp(sy, -1.0f, 1.0f);
    return true;
}

void set_rumble(int controller_num, bool on) {
    if (controller_num != 0 || g_controller == nullptr) return;
    if (g_rumble_muted.load(std::memory_order_relaxed)) return;
    if (!SDL_GameControllerHasRumble(g_controller)) return;
    // N64 rumble is binary; use full-strength low-freq motor. A 0-length pulse
    // stops it; a long pulse sustains it (the runtime re-issues each VI).
    SDL_GameControllerRumble(g_controller, on ? 0xFFFF : 0, on ? 0xFFFF : 0, 100);
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }
    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

} // namespace

void set_rumble_muted(bool muted) {
    g_rumble_muted.store(muted, std::memory_order_relaxed);
}

void request_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    if (ms == 0 || (low == 0 && high == 0)) return;
    if (g_rumble_muted.load(std::memory_order_relaxed)) return;
    g_rumble_request.store((static_cast<uint64_t>(low) << 48) |
                           (static_cast<uint64_t>(high) << 32) | ms,
                           std::memory_order_release);
}

void refresh_controllers() {
    refresh_controller_internal();
}

void set_player2_input(bool on) {
    g_player2_input.store(on, std::memory_order_relaxed);
}

bool sample_local_input(int controller, uint16_t* buttons, float* x, float* y) {
    return live_input(controller, buttons, x, y);
}

void set_input_filter(InputFilter filter) {
    g_input_filter.store(filter, std::memory_order_release);
}

void set_input_blocked(bool blocked) {
    g_input_blocked.store(blocked, std::memory_order_relaxed);
}

// Bring up the game-controller subsystem from init() (audio init lives in
// audio.cpp; this just makes sure input can open pads).
void init_input_subsystem() {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
            fprintf(stderr, "mm_audio_input: SDL_InitSubSystem(GAMECONTROLLER): %s\n", SDL_GetError());
        }
    }
    refresh_controller_internal();
}

ultramodern::input::callbacks_t input_callbacks() {
    return ultramodern::input::callbacks_t{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
}

} // namespace mm_audio_input
