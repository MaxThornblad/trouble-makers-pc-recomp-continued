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
    int partner_person = 1;   // whose character the arrow points at (1 or 2)
    int px = 0, py = 0;
};
Marker current_marker();   // any thread

// The lead swap (player2_lead.cpp) in game memory, next to player 2's block:
// +0x0 lead (1: player 2's person plays slot 0, the game's Marina), +0x4
// frames before the lead may change again, +0x8 frame count, +0xC a probe's
// answer waits to be checked, +0x10 the transition words before that probe.
constexpr uint32_t kLeadBlock = 0x807F0600u;

// Swap the two players' characters between slot 0 and player 2's slot:
// records, Marina's globals, previous buttons, this frame's controller
// entries and (far apart) cameras. player2.cpp.
void exchange_characters(uint8_t* rdram);
// Trade player 1's and player 2's controller entries (this frame's reads).
void route_pads(uint8_t* rdram);
// No buttons or stick on the controller playing player 2's slot this frame.
void mute_second_controller(uint8_t* rdram);
// That controller has a new button down this frame, or the stick up.
bool second_controller_acting(uint8_t* rdram);

} // namespace mm::player2
