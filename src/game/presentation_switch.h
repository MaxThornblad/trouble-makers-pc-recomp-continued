// Presentation on or off (netcode-v2 phase 3, docs/NETCODE-V2.md).
//
// A frame of the game can run without anyone seeing or hearing it: the
// graphics task is completed without reaching the renderer, audio samples
// are counted but not played, rumble is dropped. A rollback runs the frames
// it replays this way; the game itself cannot tell the difference, and the
// state it produces is identical (the replay fingerprints prove it).
#pragma once

namespace mm::presentation {

void set_live(bool on);   // game thread
bool live();

} // namespace mm::presentation
