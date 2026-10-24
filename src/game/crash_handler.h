// Writes a crash report (exception, faulting module + offset, stack) into
// logs/latest.log when the process dies, so a crash on another machine can
// be mapped to code with the matching unstripped build.
#pragma once

namespace mm::crash_handler {

void install();

// Remember what the game was doing, for the report (game thread).
void note_scene(int game_state, int stage, int scene);

} // namespace mm::crash_handler
