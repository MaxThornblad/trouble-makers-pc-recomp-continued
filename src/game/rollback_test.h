// netcode-v2 phase 0 measurement: see rollback_test.cpp. Test only.
#pragma once

#include "recomp.h"

namespace mm::rollback_test {

// Game thread, from replay::on_frame while a replay plays, after its
// fingerprint, with the replay frame the game is at.
void on_frame(uint8_t* rdram, recomp_context* ctx, long frame);
// The replay ended: print the totals.
void on_stop();

} // namespace mm::rollback_test
