// Input backend for ultramodern's input::callbacks_t, backed by SDL2 and the
// persistent two-slot mappings in controls.cpp. The game is single-player, so
// keyboard and the first SDL game controller are combined into N64 port 0.
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

// Controller 2's buttons (N64 bits). The game reads only buttons (never the
// stick), so the left stick also drives the D-pad bits.
uint16_t player2_buttons() {
    uint16_t b = 0;
    if (SDL_GameController* c = g_controller2) {
        auto button = [c](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(c, x) != 0; };
        const float lx = axis_norm(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX));
        const float ly = axis_norm(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
        if (button(SDL_CONTROLLER_BUTTON_A)) b |= BTN_A;
        if (button(SDL_CONTROLLER_BUTTON_B) || button(SDL_CONTROLLER_BUTTON_X)) b |= BTN_B;
        if (button(SDL_CONTROLLER_BUTTON_Y)) b |= BTN_CU;
        if (button(SDL_CONTROLLER_BUTTON_START)) b |= BTN_START;
        if (button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= BTN_L;
        if (button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= BTN_R;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 ||
            SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= BTN_Z;
        if (button(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -kDigitalAxisThreshold) b |= BTN_UP;
        if (button(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > kDigitalAxisThreshold) b |= BTN_DOWN;
        if (button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -kDigitalAxisThreshold) b |= BTN_LEFT;
        if (button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > kDigitalAxisThreshold) b |= BTN_RIGHT;
    }
    // Keyboard: T F G H or the numpad to move, Y / numpad 0 jump (A),
    // U / numpad . grab (B), numpad Enter start.
    if (key(SDL_SCANCODE_T) || key(SDL_SCANCODE_KP_8)) b |= BTN_UP;
    if (key(SDL_SCANCODE_G) || key(SDL_SCANCODE_KP_5) || key(SDL_SCANCODE_KP_2)) b |= BTN_DOWN;
    if (key(SDL_SCANCODE_F) || key(SDL_SCANCODE_KP_4)) b |= BTN_LEFT;
    if (key(SDL_SCANCODE_H) || key(SDL_SCANCODE_KP_6)) b |= BTN_RIGHT;
    if (key(SDL_SCANCODE_Y) || key(SDL_SCANCODE_KP_0)) b |= BTN_A;
    if (key(SDL_SCANCODE_U) || key(SDL_SCANCODE_KP_PERIOD)) b |= BTN_B;
    if (key(SDL_SCANCODE_KP_ENTER)) b |= BTN_START;
    return b;
}

float binding_strength(InputBinding binding) {
    switch (binding.type) {
        case BindingType::None:
            return 0.0f;
        case BindingType::Keyboard:
            return key(static_cast<SDL_Scancode>(binding.id)) ? 1.0f : 0.0f;
        case BindingType::ControllerButton:
            if (g_controller == nullptr) return 0.0f;
            return SDL_GameControllerGetButton(
                       g_controller, static_cast<SDL_GameControllerButton>(binding.id))
                ? 1.0f : 0.0f;
        case BindingType::ControllerAxis: {
            if (g_controller == nullptr || binding.id == 0) return 0.0f;
            const int encoded_axis = std::abs(binding.id);
            const auto axis = static_cast<SDL_GameControllerAxis>(encoded_axis - 1);
            const float direction = binding.id < 0 ? -1.0f : 1.0f;
            return std::max(0.0f,
                axis_norm(SDL_GameControllerGetAxis(g_controller, axis)) * direction);
        }
    }
    return 0.0f;
}

float input_strength(N64Input input) {
    float result = 0.0f;
    constexpr std::array devices{
        ControlDevice::Keyboard, ControlDevice::Controller};
    for (ControlDevice device : devices) {
        for (size_t slot = 0; slot < kBindingsPerInput; ++slot) {
            result = std::max(result, binding_strength(get_binding(device, input, slot)));
        }
    }
    return result;
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

    struct DigitalInput {
        N64Input input;
        uint16_t mask;
    };
    constexpr std::array digital_inputs{
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

    uint16_t b = 0;
    for (const DigitalInput& input : digital_inputs) {
        if (input_strength(input.input) >= kDigitalAxisThreshold) b |= input.mask;
    }

    const float sx = input_strength(N64Input::AnalogRight) -
                     input_strength(N64Input::AnalogLeft);
    const float sy = input_strength(N64Input::AnalogUp) -
                     input_strength(N64Input::AnalogDown);

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

void request_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    if (ms == 0 || (low == 0 && high == 0)) return;
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

bool sample_local_input(uint16_t* buttons, float* x, float* y) {
    return live_input(0, buttons, x, y);
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
