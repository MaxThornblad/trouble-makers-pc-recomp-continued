#include "split_view.h"

#include <algorithm>
#include <mutex>

#include "far_coop.h"
#include "player2.h"

namespace mm::split_view {
namespace {

constexpr float kMaxAspect = 8.0f / 3.0f;   // as far_coop.cpp asks the renderer for (kSplitMaxAspect)
std::mutex g_mutex;
State g_state;

} // namespace

void publish(uint8_t* rdram) {
    State s;
    int32_t dx = 0, dy = 0;
    if (mm::faraway::split_local() && mm::faraway::shows_second_view(rdram) &&
        mm::faraway::p2_camera_offset(rdram, dx, dy)) {
        s.on = true;
        s.second_on_top = mm::player2::lead_swapped(rdram);
        s.dx = static_cast<float>(dx) / 65536.0f;
        s.dy = static_cast<float>(dy) / 65536.0f;
    }
    std::lock_guard lock{g_mutex};
    g_state = s;
}

State current() {
    std::lock_guard lock{g_mutex};
    return g_state;
}

int view_count() {
    return current().on ? 2 : 1;
}

Mapping mapping(int view, float w, float h, bool widescreen) {
    const State s = current();
    Mapping m{};
    m.x0 = 0;
    m.x1 = w;
    m.y0 = 0;
    m.y1 = h;
    float aspect = widescreen ? std::max(w / std::max(h, 1.0f), 4.0f / 3.0f) : 4.0f / 3.0f;
    if (s.on) {
        const bool top = (view == 1) == s.second_on_top;
        const float half = h * 0.5f;
        m.y0 = top ? 0 : half;
        m.y1 = top ? half : h;
        aspect = widescreen ? std::clamp(w / std::max(half, 1.0f), 4.0f / 3.0f, kMaxAspect) : 4.0f / 3.0f;
        if (view == 1) {
            m.dx = s.dx;
            m.dy = s.dy;
        }
    }
    const float rw = m.x1 - m.x0, rh = m.y1 - m.y0;
    // The renderer fits the 240-line picture of that aspect into the
    // rectangle (pillar- or letterboxed).
    m.scale = (rw / std::max(rh, 1.0f) > aspect) ? rh / 240.0f : rw / (240.0f * aspect);
    m.cx = (m.x0 + m.x1) * 0.5f;
    m.cy = (m.y0 + m.y1) * 0.5f;
    m.half_w = 120.0f * aspect;
    m.half_h = 120.0f;
    return m;
}

} // namespace mm::split_view
