// What the game's camera follows with player 2 in the game: the players'
// midpoint on one shared screen, player 1 alone when far apart is allowed
// (far_coop.h), and player 2 in her machine's own picture. See player2.h.
#include "player2.h"

#include <cstdint>

#include "cheats.h"
#include "far_coop.h"

// Shared camera. Translated func_80012830 (camera update) calls these at its
// entry and before its return (troublemakers.us1.toml). While co-op is on,
// Marina's position reads as the midpoint between the two players for the
// camera's sake, so it frames both; her real position comes back after,
// carrying any shift the camera code applied meanwhile.
namespace {
bool g_camera_swapped = false;
bool g_stood_in = false;
int32_t g_saved_x, g_saved_y, g_saved_wx, g_saved_wy, g_mid_x, g_mid_y;
}

extern "C" void mm_camera_begin(uint8_t* rdram) {
    using namespace mm::player2;
    g_camera_swapped = false;
    g_stood_in = false;
    if (mm::faraway::secondary_pass()) return;   // the second camera: its target is set up already
    if (!mm::cheats::coop_enabled()) return;
    const int p2 = slot(rdram);
    if (p2 < 0) return;
    // Far apart (far_coop.h): the game's camera follows player 1, or player
    // 2 in her machine's shadow frame; no midpoint.
    const bool far_apart = mm::faraway::active(rdram);
    if (far_apart && !mm::faraway::swapped()) return;
    if (far_apart) {
        // Player 2's own picture (the shadow frame): the camera follows her,
        // all of her (which way she faces and moves), as it follows Marina.
        stand_in_begin(rdram);
        g_stood_in = true;
        return;
    }
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    const gpr b = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
    g_saved_x = MEM_W(0x88, m);
    g_saved_y = MEM_W(0x8C, m);
    g_saved_wx = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u)));
    g_saved_wy = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu)));
    g_mid_x = far_apart ? MEM_W(0x88, b) : static_cast<int32_t>((static_cast<int64_t>(g_saved_x) + MEM_W(0x88, b)) / 2);
    g_mid_y = far_apart ? MEM_W(0x8C, b) : static_cast<int32_t>((static_cast<int64_t>(g_saved_y) + MEM_W(0x8C, b)) / 2);
    MEM_W(0x88, m) = g_mid_x;
    MEM_W(0x8C, m) = g_mid_y;
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u))) = g_saved_wx + (g_mid_x - g_saved_x);
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu))) = g_saved_wy + (g_mid_y - g_saved_y);
    g_camera_swapped = true;
}

extern "C" void mm_camera_end(uint8_t* rdram) {
    if (g_stood_in) {
        g_stood_in = false;
        mm::player2::stand_in_end(rdram);
        return;
    }
    if (!g_camera_swapped) return;
    g_camera_swapped = false;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    MEM_W(0x88, m) = g_saved_x + (MEM_W(0x88, m) - g_mid_x);
    MEM_W(0x8C, m) = g_saved_y + (MEM_W(0x8C, m) - g_mid_y);
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u))) = g_saved_wx;
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu))) = g_saved_wy;
}
