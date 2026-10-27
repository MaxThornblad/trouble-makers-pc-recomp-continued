// Player 2's pieces shared between player2.cpp (her update and joining),
// player2_draw.cpp (her own pose, the tag over her head) and
// player2_camera.cpp (what the game's camera follows).
#pragma once

#include <cstdint>

namespace mm::player2 {

// What the overlay draws over the picture (published once a frame).
struct Marker {
    bool visible = false;
    int x = 0, top = 0, health = 0, max_health = 0;   // her tag: camera-relative px, +y up
    // Far apart (far_coop.h): where the other player is, relative to this
    // machine's view (px, +y up), for an arrow at the screen's edge.
    bool partner = false;
    bool partner_is_p1 = false;
    int px = 0, py = 0;
};
Marker current_marker();   // any thread

} // namespace mm::player2
