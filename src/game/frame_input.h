// One input stream (netcode-v2 phase 2, docs/NETCODE-V2.md).
//
// Everything that can change the game in a frame is one value, a Frame:
// both controllers, the settings in force (cheats and options) and any
// commands (a warp, a spawn). It is built once at the frame point by
// whichever source is active and latched; the game's controller reads, the
// cheats and the debug tools all read the latched frame. So a local game, a
// replay and an online session feed the game the same way, a recording
// holds everything that happened, a rollback replays it exactly, and a new
// cheat, option or debug action needs no netplay code of its own.
//
// Sources (who calls latch):
//   local    frame_input::tick, when nothing else latched: the pads sampled
//            now (or the test pad scripts), the local settings, the requests
//            queued by the menu;
//   replay   replay.cpp: the recording's frame (recording: the local frame,
//            written out);
//   netplay  netplay.cpp: the local pad and the partner's for the frame
//            number, the host's settings and commands.
//
// Game thread, except request() and the pad script setters (any thread).
#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

#include "cheats.h"
#include "recomp.h"

namespace mm::frame_input {

struct Pad {
    uint16_t buttons = 0;
    int8_t x = 0, y = 0;   // -127..127
    uint8_t connected = 0;
    uint8_t pad_ = 0;
};

enum class Cmd : uint8_t {
    None = 0,
    Warp = 1,          // a: stage, b: scene (debug_menu.cpp)
    Spawn = 2,         // a: actor type, b: parameter (actors.cpp)
    ClearSpawns = 3,   // actors.cpp
};

struct Command {
    Cmd kind = Cmd::None;
    uint8_t pad_[3]{};
    int32_t a = 0;
    uint32_t b = 0;
};

constexpr int kMaxCommands = 4;

struct Frame {
    Pad pad[2];
    mm::cheats::SessionSettings settings;
    uint8_t command_count = 0;
    uint8_t pad_[3]{};
    Command commands[kMaxCommands];

    void add(const Command& c) {
        if (command_count < kMaxCommands) commands[command_count++] = c;
    }
};
static_assert(std::is_trivially_copyable_v<Frame>, "a frame is plain data (files, packets)");

// The latch. Applies the settings (netplay: from_host says the host decides
// them, so the local menu cannot change them) and keeps the commands for
// the tools that run them this frame.
void latch(const Frame& frame, bool from_host);
const Frame& current();
bool latched_this_frame();
// The frame point: when no source latched a frame since the last tick, the
// local source does. Called last in the frame hook.
void tick(uint8_t* rdram);

// The local source: both pads now (SDL, or the pad script for tests at
// `script_frame`), the local settings, the queued requests (which it takes).
Frame sample_local(long script_frame);
// A request from a menu or a test, for the next local frame. Online only
// the host's reach the game (netplay.cpp).
void request(const Command& c);

// Test pad scripts ("60:,8:r,10:uB,..."): frames count, then button letters
// (A B Z S, d-pad u d l r, stick U D L R). One per controller.
void set_pad_script(int controller, const char* script);
bool pad_script_active(int controller);
Pad pad_from_script(int controller, long frame);

// A command that the tools consume from the current frame.
bool take_command(Cmd kind, Command* out);   // the first of that kind this frame, once

} // namespace mm::frame_input
