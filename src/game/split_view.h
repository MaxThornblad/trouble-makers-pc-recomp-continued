// Split screen views for overlays (far_coop.cpp draws the two pictures).
//
// Overlays (player tags, enemy health bars, throw charge bars, the gem
// compass) work in camera-relative N64 pixels of the game's camera and map
// them to the window. In local split screen the window holds two views:
// view 0, the game's camera (the real frame), and view 1, the second camera
// (player 2's), each in its own half. An overlay publishes its positions
// once, in view 0's terms, and draws them once per view: in view 1 a
// position is shifted by the offset between the two cameras.
#pragma once

#include <cstdint>

namespace mm::split_view {

// Game thread, once a frame from the real frame's tick.
void publish(uint8_t* rdram);

struct State {
    bool on = false;            // two views
    bool second_on_top = false; // view 1 in the top half (player 2 leads)
    float dx = 0, dy = 0;       // view 1's camera minus view 0's, N64 px, +y up
};
State current();   // any thread

// Where a view sits in a window of display_w x display_h, and how its
// camera-relative N64 pixels map onto it: window x = cx + rel_x * scale,
// window y = cy - rel_y * scale. half_w / half_h: the view's visible extent
// in N64 px either side of its centre.
struct Mapping {
    float x0, y0, x1, y1;       // the view's rectangle in the window
    float cx, cy, scale;
    float half_w, half_h;
    float dx, dy;               // subtract from a view-0 position for this view
};
int view_count();   // 1, or 2 in split screen
Mapping mapping(int view, float display_w, float display_h, bool widescreen);

} // namespace mm::split_view
