// Controller rumble.
//
// Mischief Makers never drives a Rumble Pak, so the port adds rumble from
// gameplay events it can read reliably each frame:
//   - Marina takes damage (her health at actor +0xE0 drops): a strong pulse,
//     longer for bigger hits; losing the last of it gives a long one.
//   - A hard landing after a long fall: a thump scaled by the fall speed.
//   - Starting a jet dash: a short light buzz.
//   - Collecting the stage's golden gem: a long celebratory pulse.
//   - Screen shake (eruptions, quakes, big impacts): a steady low rumble for
//     as long as the game shakes the view, stronger for bigger shakes.
//   - Explosions, from their sound effects: a bomb going off, and each red
//     block it blows apart (quieter, farther ones rumble less).
// Strength is a setting on the debug menu's Cheats page (Off / Low / Medium /
// High, default Medium) and works without the debug menu.
#pragma once

#include <cstdint>

namespace mm::rumble {

// Game thread, once per frame.
void tick(uint8_t* rdram);

} // namespace mm::rumble
