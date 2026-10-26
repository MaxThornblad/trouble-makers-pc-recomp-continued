# Netplay plan and determinism notes

Goal: built-in online co-op. Both PCs run the whole game and exchange only
controller input each frame; a second Marina (player 2, recoloured, with a
"2P" marker) plays alongside the first.

| Phase | What | Status |
|---|---|---|
| 0 | Identical replays: record input, play it back, and prove game memory matches frame for frame | done |
| 1 | Player 2 locally: a second controller drives a second Marina (context swap), shared camera, co-op rules | next |
| 2 | Lockstep netplay over UDP (ENet): input delay of a few frames, session start from the host's state, desync check and automatic resync | |
| 3 | Connecting over the internet: Tailscale/ZeroTier first, join-by-code later | |
| 4 | Rollback (optional) | |

## Phase 0: what makes the game reproducible

Replays (`src/game/replay.cpp`, overlay **Session** page, environment
variables in `replay.h`) record a save state plus every controller poll, and
fingerprint game memory (64 KiB blocks) at every frame point. Playback loads
the state, answers the polls from the recording and compares fingerprints.
`tools/replay_diff.py` compares two `hashes.bin` files; `MM_REPLAY_DUMP`
writes full memory at chosen frames for a word-by-word diff.

Verified: 10-minute random-input replays (36 000 frames) from three different
save states (1-3 start, 1-6, 1-9), recorded at 8x speed and played at 4x,
match in every frame; a recording made by the Windows build (under Wine)
plays back identically on the Linux build.

What had to change, in the order it was found:

1. **Host pointers in RDRAM.** `OSThread::context` (8 bytes per game thread)
   is a host pointer; it differs per process and is left out of the
   fingerprint. Two profiling words the game fills from `osGetTime` and never
   reads (0x801374DC, 0x80182018) are left out too.
2. **Event timing (the big one).** The main loop (`Thread_MainProc`) decides
   what to do from how many messages sit in its queues: VI retraces
   (queue 0x8012ABC0, retrace count 1), RSP and RDP completions. Normally host
   threads post those on wall-clock time. *Deterministic event mode*
   (N64ModernRuntime patch 0011, `ultramodern::set_deterministic_events`):
   the VI thread stops sending VI/AI messages; the game thread delivers
   exactly one VI per frame from a hook just before the loop's blocking VI
   wait (`before_vram = 0x80000F5C`, `mm_deterministic_vi`), sleeping to the
   real VI time so speed stays right; every RSP/RDP task is finished (audio
   tasks run right on the game thread) and its completion delivered before
   `osSpTaskStartGo` returns.
3. **AI_LEN.** `Sound_Update` reads the AI_LEN register directly to decide
   how many samples to mix; a host thread mirrors the real audio queue into
   it. In replays a *virtual audio clock* (`register_overlays.cpp`) sets it
   instead: it drains one 60 Hz frame of samples per game frame and fills by
   what the game submits. Fast-forward now uses the same clock: at 8x the real
   queue overflowed and was cleared over and over, AI_LEN swung wildly and the
   game could mix a corrupt audio command list (an aspMain "unhandled jump
   target" exit within a minute or so of 8x play, also without replays).
4. **Audio ucode state.** RSP DMEM 0xF80-0xFFF survives between audio tasks,
   and the game hands the ucode a state buffer at 0x8083FF20, past the 8 MiB
   save states used to hold. Both are now in save states (format version 3).
5. **Start from a load.** The recorder takes its start state and immediately
   loads it back, exactly as playback (or a netplay peer) will, so no live
   host state can differ between the two sides.
6. **Cartridge save.** Not in save states but read by the game; a replay
   carries `eeprom.bin`, and playback runs in a *save sandbox* (patch 0012:
   reads and writes go to a session copy, the player's save file is never
   touched).
7. **Renderer output.** The framebuffers (0x801DA800 and 0x803DA800,
   320x240x16) hold whatever the renderer writes back, so they are left out of
   the fingerprint. The game logic does not read them.

Not yet covered, for phase 2: host-side settings that change the game (cheats,
grab blocks, hold to dash, spawns, warps) must be synced or disabled in a
session; the debug menu and save states must be host-only while connected.
