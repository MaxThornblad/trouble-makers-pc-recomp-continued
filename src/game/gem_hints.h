// Golden gem hints in the pause menu.
//
// While a campaign stage is paused, a panel shows how to find that stage's
// golden gem and whether it has already been collected. The hints are
// taken or condensed from the community Mischief Makers Wiki
// (https://mischiefmakers.fandom.com, text under CC BY-SA 3.0). Collected state comes from the game's own gold gem mask
// (YellowGem_GetFlag: 64 bits at 0x80171B10, one per progression row).
#pragma once

#include <cstdint>

namespace mm::gem_hints {

// Game thread, once per frame: publishes pause state, stage row and the
// collected flag.
void publish(uint8_t* rdram);

#ifdef MM_HAS_GRAPHICS
void draw_overlay();
#endif

} // namespace mm::gem_hints
