# Netplay plan and determinism notes

Goal: built-in online co-op. Both PCs run the whole game and exchange only
controller input each frame; a second Marina (player 2, recoloured, with a
"2P" marker) plays alongside the first.

| Phase | What | Status |
|---|---|---|
| 0 | Identical replays: record input, play it back, and prove game memory matches frame for frame | done |
| 1 | Player 2 locally: a second controller drives a second Marina (context swap), shared camera, co-op rules | done |
| 2 | Lockstep netplay over UDP (ENet): input delay of a few frames, session start from the host's state, desync check and automatic resync, a screen per player | working |
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
   the VI thread stops sending VI/AI messages; when the game thread blocks
   on its VI queue (the main loop does once per frame), `osRecvMesg` delivers
   the next VI right there (patch 0015), sleeping to the real VI time so speed
   stays right; every RSP/RDP task is finished (audio
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

## Phase 1: player 2

`src/game/player2.cpp`. Player 2 is custom actor type 0x7F02. Her update
swaps her record and her own copy of Marina's globals into slot 0, puts
controller 2's buttons where Marina reads hers (0x800BE4F8 / 0x800BE4FC; the
game reads all four pads into 0x8012AD70 and never reads the stick), calls
Marina's update `func_8004ED10(0)` and swaps back. Marina's globals were
found with `MM_TEST_P2PROBE` (snapshot memory, run her update once more, diff,
restore): her input words, the player-state block 0x801373D8..0x80137460,
her world position 0x800BE5D8 / 0x800BE5E8 (read by enemies), 0x800BE5A4,
0x800D57E0, 0x800EF4F0..0x800EF50F, 0x80104090, 0x801069D8; the
0x8016xxxx writes are sound channels and stay shared. P2's copy, her previous
buttons and the co-op timers live at 0x807F0000 (host-only memory inside the
save-state range), so states, replays and netplay carry them.

- Terrain collision runs in the global loop (`func_80016CB4`, slots 0-0xBF), so
  her slot gets it like any actor; Marina's own code keeps her inside the
  camera view, which leashes her to the shared screen.
- Co-op setting (`coop` in cheats.cfg, Cheats page): she joins 45 frames into
  every scene, rejoins next to P1 when her health reaches 0 (before Marina's
  defeat code can run) or after 90 frames more than 250 / 170 px away.
- Shared camera: hooks at `func_80012830`'s entry and before 0x800128CC make
  Marina's position read as the players' midpoint during the camera update.
  Netplay will instead keep this game camera and draw each player's own view.
- Her own pose: Marina's frames are streamed into one shared texture for slot 0
  (func_800821B0 / func_800822B8 unpack slot 0's frame to 0x8027AEE8 / 0x8027BEE8),
  and the draw builder's class-0x5800 branch reads slot 0's record, so at
  first player 2 was drawn in player 1's pose. Hooks at 0x8000A474 and
  0x8000DD18 in `func_80009BE8` put her record in slot 0 for her entry,
  unpack her frame into her own buffers (0x807F2000) and point images 2..5
  at copies of their display lists that sample them (0x807F1000).
- Look: the draw builder's prim-colour tint (+0x9C..+0x9F, `func_80009BE8`)
  adds a cool blue (00 30 5A) while she is not hit-flashing; an overlay tag
  "2P" with her health sits over her head.
- Input: controller 2 is reported to the game only with co-op on (second game
  controller, or T F G H / numpad, Y jump, U grab).
- Verified: 3000-frame co-op sessions with random input for both players
  replay identically, including joins and leash rejoins.

Deterministic mode delivers a VI whenever a game thread blocks on the VI queue
(`osRecvMesg`), wherever the wait is, and reads the VI registration from the
*next* VI state (the current one lags one VI-thread tick after
`osViSetEvent`, which hung fast-forward boots about one time in three).

Not yet covered, for phase 2: host-side settings that change the game (cheats,
grab blocks, hold to dash, spawns, warps) must be synced or disabled in a
session; the debug menu and save states must be host-only while connected.

## Phase 2: netplay

`src/game/netplay.cpp` (header comment for the details and the test
environment variables). Start with `--host [port]` / `--join addr[:port]` or
from the overlay's Session page.

- **Transport**: ENet 1.3.18 (fetched by CMake), one connection, two
  channels (control: hello / start / desync; input and fingerprints), all
  reliable and ordered. Serviced on the game thread at the frame point.
- **Start**: the guest sends a hello with the build's commit (mismatch:
  refused). The host takes a save state, loads it back (as a replay recording
  does), and sends it with its cartridge save and its game-changing settings
  (`cheats::SessionSettings`: cheats and whether they are live, hold to dash,
  gem hints, widescreen). The guest plays in a save sandbox (its own save file
  untouched), adopts the settings for the session and loads the state.
- **Lockstep**: each frame point, each side sends its own input for frame
  f + delay (default 4) and waits until the other side's input for f is in.
  The game's controller polls in frame f are answered with those two inputs
  (host = controller 1 = Marina, guest = controller 2 = player 2).
- **Checks**: every 60 frames both send a memory fingerprint (the replay
  fingerprint folded to 64 bits). A mismatch, or the host loading a state,
  makes the host send a fresh start (a resync, at most every 2 s). The guest
  cannot load states.
- **A screen each**: the game camera stays the shared midpoint (it drives the
  simulation). After `LookAt_Update` builds the view matrices (0x804600C0 /
  0x804700C0), their translation moves by half the distance from the other
  player to this machine's player, so each picture centres on its own player.
  Those two matrices are the only memory that differs between the machines and
  are left out of fingerprints. (A first version moved the eye and target
  before the call instead; the call's stack leftovers then differed too.)
  Overlays drawn at camera positions (the 2P tag, enemy bars, damage numbers)
  follow the same offset.
- **Widescreen tracking** restarts on every state load (`g_ws_tracking_gen`),
  because its gameplay/cinema state machine decides the expanded cull bounds
  the game logic sees, and two processes must track it identically after a load.
- Verified: two Linux processes over localhost, and a Windows build (Wine,
  rendering, own view on) against a Linux build: fingerprints identical every
  second for whole sessions; a forced difference and a host state load each
  resync within a second.
