#include "frame_input.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "mm_audio_input.hpp"

namespace mm::frame_input {
namespace {

Frame g_current;
long g_number = 0;
bool g_latched = false;         // a source latched since the last tick
bool g_taken[kMaxCommands]{};   // commands of the current frame already consumed

std::mutex g_request_mutex;
std::vector<Command> g_requests;   // from any thread, for the next local frame

struct PadStep {
    long frames;
    uint16_t buttons;
    int8_t x, y;
};
std::mutex g_script_mutex;
std::vector<PadStep> g_scripts[2];
bool g_script_set[2]{};

long g_local_frame = 0;   // frames the local source has produced (pad scripts count these)

} // namespace

// The game's controller reads (mm_audio_input::get_input) come here.
bool filter(int controller, uint16_t* buttons, float* x, float* y) {
    if (controller < 0 || controller > 1) {
        *buttons = 0;
        *x = *y = 0.0f;
        return false;
    }
    const Pad& p = g_current.pad[controller];
    *buttons = p.buttons;
    *x = p.x / 127.0f;
    *y = p.y / 127.0f;
    return p.connected != 0;
}

void install_once() {
    static bool done = false;
    if (done) return;
    done = true;
    mm_audio_input::set_input_filter(filter);
}

void latch(const Frame& frame, bool from_host, long number) {
    install_once();
    g_current = frame;
    g_number = number;
    static const bool dbg = std::getenv("MM_FRAME_INPUT_DEBUG") != nullptr;
    if (dbg) {
        static long n = 0;
        if ((n++ % 30) == 0)
            std::fprintf(stderr, "[frame-input] latch %ld: p1 %04X %d,%d c%d  p2 %04X %d,%d c%d  cmds %d from_host %d\n", n,
                         frame.pad[0].buttons, frame.pad[0].x, frame.pad[0].y, frame.pad[0].connected,
                         frame.pad[1].buttons, frame.pad[1].x, frame.pad[1].y, frame.pad[1].connected,
                         frame.command_count, from_host);
    }
    g_latched = true;
    std::memset(g_taken, 0, sizeof g_taken);
    mm::cheats::set_session_settings(&g_current.settings, from_host);
}

const Frame& current() { return g_current; }

long number() { return g_number; }

bool latched_this_frame() { return g_latched; }

void tick(uint8_t* rdram) {
    (void)rdram;
    if (!g_latched) latch(sample_local(g_local_frame), false, g_local_frame);
    g_latched = false;   // for the next frame
    ++g_local_frame;
}

Pad sample_pad(int controller, long script_frame) {
    Pad p;
    if (pad_script_active(controller)) return pad_from_script(controller, script_frame);
    uint16_t b = 0;
    float x = 0, y = 0;
    if (mm_audio_input::sample_local_input(controller, &b, &x, &y)) {
        p.buttons = b;
        p.x = static_cast<int8_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 127.0f));
        p.y = static_cast<int8_t>(std::lround(std::clamp(y, -1.0f, 1.0f) * 127.0f));
        p.connected = 1;
    }
    return p;
}

Frame sample_local(long script_frame) {
    Frame f;
    f.pad[0] = sample_pad(0, script_frame);
    f.pad[1] = sample_pad(1, script_frame);
    f.settings = mm::cheats::capture_session_settings();
    std::lock_guard lock{g_request_mutex};
    for (const Command& c : g_requests) f.add(c);
    g_requests.clear();
    return f;
}

void request(const Command& c) {
    std::lock_guard lock{g_request_mutex};
    if (g_requests.size() < kMaxCommands) g_requests.push_back(c);
}

bool take_command(Cmd kind, Command* out) {
    for (int i = 0; i < g_current.command_count; ++i) {
        if (g_current.commands[i].kind == kind && !g_taken[i]) {
            g_taken[i] = true;
            if (out) *out = g_current.commands[i];
            return true;
        }
    }
    return false;
}

void set_pad_script(int controller, const char* script) {
    if (controller < 0 || controller > 1) return;
    std::lock_guard lock{g_script_mutex};
    std::vector<PadStep>& steps = g_scripts[controller];
    steps.clear();
    g_script_set[controller] = script != nullptr;
    for (const char* p = script; p && *p;) {
        PadStep step{0, 0, 0, 0};
        step.frames = std::strtol(p, const_cast<char**>(&p), 10);
        if (*p == ':') ++p;
        for (; *p && *p != ','; ++p) {
            switch (*p) {
                case 'A': step.buttons |= 0x8000; break;
                case 'B': step.buttons |= 0x4000; break;
                case 'Z': step.buttons |= 0x2000; break;
                case 'S': step.buttons |= 0x1000; break;
                case 'u': step.buttons |= 0x0800; break;
                case 'd': step.buttons |= 0x0400; break;
                case 'l': step.buttons |= 0x0200; break;
                case 'r': step.buttons |= 0x0100; break;
                case 'U': step.y = 127; break;
                case 'D': step.y = -127; break;
                case 'L': step.x = -127; break;
                case 'R': step.x = 127; break;
                default: break;
            }
        }
        if (*p == ',') ++p;
        steps.push_back(step);
    }
}

bool pad_script_active(int controller) {
    if (controller < 0 || controller > 1) return false;
    std::lock_guard lock{g_script_mutex};
    return g_script_set[controller];
}

Pad pad_from_script(int controller, long frame) {
    Pad p;
    p.connected = 1;
    if (controller < 0 || controller > 1) return p;
    std::lock_guard lock{g_script_mutex};
    long at = 0;
    for (const PadStep& s : g_scripts[controller]) {
        if (frame < at + s.frames) {
            p.buttons = s.buttons;
            p.x = s.x;
            p.y = s.y;
            return p;
        }
        at += s.frames;
    }
    return p;   // past the script: nothing pressed
}

} // namespace mm::frame_input
