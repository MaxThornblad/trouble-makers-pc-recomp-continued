// The virtual audio clock (register_overlays.cpp): in replays and netplay
// the game's AI_LEN register is driven from a virtual DAC, not the host
// audio queue. Its state goes with a save state (savestate.cpp).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mm_audio_clock_state {
    uint64_t submitted;   // bytes the game has handed over (mm_audio_input)
    uint64_t drained;     // bytes the virtual DAC has played
    uint32_t remainder;   // sub-byte carry of the per-frame drain
    uint32_t ai_len;      // the register word the game reads
} mm_audio_clock_state;

void mm_virtual_audio_get(mm_audio_clock_state* out);
void mm_virtual_audio_set(const mm_audio_clock_state* in);

#ifdef __cplusplus
}
#endif
