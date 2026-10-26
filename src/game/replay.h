// Input replays and determinism checking (netplay groundwork, see
// docs/NETPLAY.md).
//
// A replay is a folder holding start.tmstate (a save state taken at the
// frame point, see savestate.h), input.bin (every controller poll the game
// made, in order, tagged with the replay frame it happened in) and
// hashes.bin (a fingerprint of game memory, in 64 KiB blocks, at every
// frame point). Playing a replay loads start.tmstate and answers the game's
// controller polls from input.bin; if the game is deterministic, the
// playback's hashes.bin matches the recording's frame for frame
// (tools/replay_diff.py compares two).
//
// Headless / test use (environment):
//   MM_REPLAY_RECORD=<dir>@<frame>   start recording at that frame point
//   MM_REPLAY_PLAY=<dir>@<frame>     load <dir>/start.tmstate and play back;
//                                    the playback's hashes go to <dir>/play/
//   MM_REPLAY_LENGTH=<frames>        stop after that many replay frames
//   MM_REPLAY_PAD="20:,30:R,5:RA"    synthetic controller 0 input while
//                                    recording (frames:keys; keys A B Z S
//                                    (start), U D L R (stick), u d l r (d-pad))
//   MM_REPLAY_PAD2="..."             the same for controller 2 (player 2)
//   MM_REPLAY_SPEED=<n>              run n times faster (tests; results must not change)
//   MM_REPLAY_DUMP=<frame>[,<frame>] also write all game memory and RSP DMEM
//                                    at those replay frames (dump_/dmem_<frame>.bin)
//   MM_REPLAY_WATCH=<vaddr>          debugging: SIGTRAP after a playback's load
//                                    with mm_watch_ptr at that word (gdb watch)
#pragma once

#include <cstdint>
#include <filesystem>

#include "recomp.h"

namespace mm::replay {

// Folder for replays made from the overlay (<config>/replays).
void configure(const std::filesystem::path& folder);

// Any thread: start recording into a new timestamped folder, play the most
// recent one, or stop. Acted on at the next frame point.
void request_record();
void request_play_latest();
void request_stop();

enum class Mode { Idle, Recording, Playing };
Mode mode();
// Replay frames so far; and, while playing, the first frame whose memory
// differed from the recording (-1: none so far).
long frame();
long first_mismatch();

// Game thread, from mm_savestate_frame_hook.
void on_frame(uint8_t* rdram, recomp_context* ctx);

} // namespace mm::replay
