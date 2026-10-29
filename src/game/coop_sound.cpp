// Sound for both players when they are far apart (far_coop.h: online, and
// local split screen).
//
// The game places a sound at an actor from the actor's position relative to
// the camera (Sound_PlaySfxAtActor and its variants: silent beyond 144 px
// across or 96 px up or down, panned and quieter towards the edges,
// func_80003540). With a camera each, that is player 1's camera only:
// whatever happened near player 2 was silent. Here a positional sound is
// heard from whichever player's camera is nearer to it: for that call the
// actor's position is taken relative to the second camera when that one is
// nearer, and put back afterwards.
//
// One shared listener, not one per machine: what a sound does to the sound
// player's state is game memory, which both machines must keep identical
// online (and the inputs here are game memory too: positions, the second
// camera). Both players hear both players' surroundings, as in split screen.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "far_coop.h"
#include "recomp.h"

namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint32_t kSlots = 0xD0u;

struct Moved {
    bool on = false;
    uint32_t record = 0;
    int32_t x = 0, y = 0;
};
Moved g_moved;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

// How far a camera-relative position is from the camera, as the game's cull
// sees it (144 px across, 96 up or down: y weighs 1.5 times).
int64_t reach(int32_t x, int32_t y) {
    const int64_t ax = std::llabs(static_cast<int64_t>(x));
    const int64_t ay = std::llabs(static_cast<int64_t>(y)) * 3 / 2;
    return ax > ay ? ax : ay;
}

} // namespace

// Entry of Sound_PlaySfxAtActor / 2 / 3 / Timed ($a1: the actor's slot).
extern "C" void mm_sound_listener_begin(uint8_t* rdram, uint32_t slot) {
    g_moved.on = false;
    slot &= 0xFFFFu;
    static const bool trace_all = std::getenv("MM_SOUND_TRACE") && std::getenv("MM_SOUND_TRACE")[0] == '2';
    if (trace_all) {
        int32_t dx = 0, dy = 0;
        const bool off = mm::faraway::p2_camera_offset(rdram, dx, dy);
        std::fprintf(stderr, "[sound] call slot %u active %d offset %d %d,%d\n", slot, mm::faraway::active(rdram) ? 1 : 0,
                     off ? 1 : 0, dx >> 16, dy >> 16);
    }
    if (slot >= kSlots || !mm::faraway::active(rdram)) return;
    int32_t dx = 0, dy = 0;
    if (!mm::faraway::p2_camera_offset(rdram, dx, dy) || (dx == 0 && dy == 0)) return;
    const uint32_t record = kActors + slot * kStride;
    const gpr a = addr(record);
    const int32_t x = MEM_W(0x88, a), y = MEM_W(0x8C, a);
    if (reach(x - dx, y - dy) >= reach(x, y)) return;   // player 1's camera is as near
    g_moved = {true, record, x, y};
    static const bool trace = std::getenv("MM_SOUND_TRACE") != nullptr;
    if (trace && !mm::faraway::in_shadow()) {
        std::fprintf(stderr, "[sound] slot %u heard from player 2's camera (%d,%d -> %d,%d)\n", slot, x >> 16, y >> 16,
                     (x - dx) >> 16, (y - dy) >> 16);
    }
    MEM_W(0x88, a) = x - dx;
    MEM_W(0x8C, a) = y - dy;
}

// Before their return: the actor where it was.
extern "C" void mm_sound_listener_end(uint8_t* rdram) {
    if (!g_moved.on) return;
    g_moved.on = false;
    const gpr a = addr(g_moved.record);
    MEM_W(0x88, a) = g_moved.x;
    MEM_W(0x8C, a) = g_moved.y;
}
