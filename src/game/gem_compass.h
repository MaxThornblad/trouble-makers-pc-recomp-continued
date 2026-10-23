// Golden gem compass.
//
// While a campaign stage is played, an arrow at the edge of the screen points
// toward the stage's golden gem; when the gem is on screen, a marker bobs
// above it. Golden gems placed in the stage are records in the item list the
// game walks every frame (see cheats.cpp, gem magnet): a gem handler record
// whose colour bits (kind & 3) are 2. Gems that only appear later (from an
// enemy, a missile, a reward) have no record yet; then a small label says so.
// Collected state comes from the game's gold gem mask, like gem_hints.
#pragma once

#include <cstdint>

namespace mm::gem_compass {

// Game thread, from the item-list hook (func_800451E4 entry) each frame.
void scan_items(uint8_t* rdram, uint32_t list);

// Game thread, once per frame (debug menu tick): publishes Marina, camera and
// collected state, and expires the scan if the item pass did not run.
void publish(uint8_t* rdram);

#ifdef MM_HAS_GRAPHICS
void draw_overlay();
#endif

} // namespace mm::gem_compass
