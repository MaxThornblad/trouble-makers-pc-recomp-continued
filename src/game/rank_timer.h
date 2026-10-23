// Rank timer: an on-screen countdown to the clear-time rank you can still get.
//
// The game grades a stage clear from the stage timer (frames, 60/s) against a
// per-stage par time (func_8001A758; par table at 0x800C8478, indexed by the
// progression row):
//   S  under par
//   A  under par + 30 s
//   B  under par + 2 min
//   C  under par + 5 min and under 10 min total
//   D  anything slower
// The overlay shows the best rank still reachable and how long is left before
// it drops to the next one. It pauses with the stage timer, because it reads
// that timer.
#pragma once

#include <cstdint>

namespace mm::rank_timer {

// Game thread, once per frame: publishes the stage timer and par time.
void publish(uint8_t* rdram);

#ifdef MM_HAS_GRAPHICS
// Draws the countdown (top-right) when enabled and a campaign stage is active.
void draw_overlay();
#endif

} // namespace mm::rank_timer
