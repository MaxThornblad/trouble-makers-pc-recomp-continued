// Throw cheats (cheats.h): the throw strength sliders and the charged throw.
//
// How a throw works in the game: Marina's throw looks up the thrown object's
// velocity in a table (func_800554AC(thrower, object): 0x800E3630, by the
// object's weight class at +0xCE and the throw direction at the thrower's
// +0x140) and leaves it in the object's +0xF8/+0xFC; the object takes it as
// its velocity when it leaves her hands (func_800291AC). A generated hook at
// func_800554AC's return scales that velocity: by the objects slider, or by
// the players slider when the object is a player (player 1's or player 2's
// slot), and by the charge of a charged throw.
//
// The charged throw holds the throw button back from Marina's update while
// she carries something (the button that would throw her load), counts the
// frames it is held, and on release gives the update the press it held back,
// with the charge as a strength factor for that one throw. Everything it
// keeps is in game memory (player 2's block), per character, so save states,
// rollbacks, replays and the second machine see the same throws.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace mm::throw_cheats {

// Just before Marina's update runs for a character (0: the game's Marina,
// slot 0; 1: player 2's character standing in for her), with that
// character's input already in Marina's input words.
void before_marina_update(uint8_t* rdram, int character);

// Game thread, once a frame (and in player 2's picture pass, for her view):
// the charge bars to draw.
void publish(uint8_t* rdram);

// Render thread: the bars.
void draw_overlay();

} // namespace mm::throw_cheats
