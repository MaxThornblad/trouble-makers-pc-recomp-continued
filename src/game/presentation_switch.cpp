#include "presentation_switch.h"

#include <atomic>

#include "mm_audio_input.hpp"
#include "ultramodern/ultramodern.hpp"

namespace mm::presentation {
namespace {
std::atomic<bool> g_live{true};
}

void set_live(bool on) {
    if (g_live.exchange(on) == on) return;
    ultramodern::set_presentation(on);
    mm_audio_input::set_audio_muted(!on);
    mm_audio_input::set_rumble_muted(!on);
}

bool live() { return g_live.load(std::memory_order_relaxed); }

} // namespace mm::presentation
