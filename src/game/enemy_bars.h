// Enemy health bars.
//
// Every actor has a health word at +0xE0 (Marina's is the one the rumble
// watches). Enemies lose it when hit, but the game changes it from hundreds of
// places, so instead of hooking each one this watches all 208 actor slots each
// frame. An actor gets a bar once it is hit and survives: its health drops
// while it stays alive. Things that are not real hits are filtered out:
//   - an actor whose health counts down several frames in a row (a timer),
//   - frames where many actors change at once (a stage reset or Marina dying),
//   - actors with fewer than 2 health points (one hit always finishes them).
// The bar shows the share of the health the actor had when it appeared (or
// the most it has had since), sits above its hitbox, and fades a few seconds
// after the last hit; actors with a lot of health (bosses) keep theirs on
// screen until they are beaten. A setting on the Cheats page turns it off.
#pragma once

#include <cstdint>

namespace mm::enemy_bars {

// Game thread, once per frame.
void publish(uint8_t* rdram);

#ifdef MM_HAS_GRAPHICS
void draw_overlay();
#endif

} // namespace mm::enemy_bars
