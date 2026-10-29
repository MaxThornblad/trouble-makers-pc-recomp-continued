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
  controller, or T F G H / numpad, Y jump, U grab); since 2026-10-29 through
  player 2's own bindings (controls.json "player2", launcher Controls page,
  Player 2 tab; her controller defaults to player 1's layout, her stick also
  drives the D-pad bits since only her buttons reach the game).
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
  untouched), adopts the settings and loads the state. From then on the
  host's settings ride with each of its inputs, so menu changes on the host
  apply on both machines on the same frame.
- **Lockstep**: each frame point, each side sends its own input for frame
  f + delay and waits until the other side's input for f is in. The delay
  is each side's own, from the measured round trip (below).
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

## Co-op fixes after the first real session

Found with scripted two-player runs and `MM_P2_TRACE=1` (both players'
positions, states and flags every frame) plus `MM_P2_BISECT` (the first time
player 2 starts a grab without input, rerun her update with parts of memory put
back to the frame before, to find what made her do it):

- **Player 2 copied player 1's buffered actions.** Marina's update reads an
  input history (func_800122B0: the last 64 frames of masked hold / press at
  0x801225F0 / 0x8011DD70) that only player 1's input feeds. When player 1
  pressed grab, player 2 grabbed too, held "slot 0" (herself, in her own
  update) and slid off shaking. She now has her own history, shifted with her
  own input each frame, and starts with an empty one.
- **The players could grab each other.** Marina's grab takes actors flagged
  0x1000 at +0x80. Player 2's slot no longer carries it; while player 2
  reaches out, player 1's flag is off for func_8001EC1C (the contact pass).
- **Leash**: player 2 is pulled back only when she has been out of the camera's
  view for 2.5 s (not when far from player 1: the screen clamp lets them stand
  288 px apart, and the old 250 px leash pulled her back from there).
- `MM_TEST_P1DIFF=from,to` lists every word Marina's real update changes
  outside her slot (catches a flag set and cleared again, which the P2 probe
  cannot).
- Random two-player sessions (3000 frames each, six of them) show no position
  jumps other than leash rejoins and the game's own scripted moves.

## Second round: a screen each that holds together, carrying each other

(The shifted view and the shared screen below were replaced by the third
round's second camera; the code is gone.)

- **Blocks and terrain slid with the player's own view.** Only the 3D objects
  follow the view matrices. Stage blocks (inline in the frame list) and the
  three tile layers (their own lists at 0x80400000..0x80418000) are texture
  rectangles in screen coordinates. Just before the frame's graphics task goes
  to the RSP (Thread_MainProc 0x80000EA4), the frame list is walked and every
  rectangle before the HUD's orthographic projection moves by the view offset
  (12-bit coordinates, the widescreen wings already use the negative half).
  The whole picture moves together; checked with Wine screenshots alternating
  offset 0 / 120 every second (MM_VIEW_TEST=120): Marina stays on her block.
  The view moves across only (the layers draw just 7 rows), and online play is
  always widescreen so the moved edges stay drawn. Both the layer and frame
  lists are render-only and left out of fingerprints.
- **One shared screen.** The players ran apart without limit (the camera follows
  their midpoint, but nothing kept them near it) until the leash teleported
  player 2. Now each stays within 180 px of the camera centre across, pushed
  back with the outward speed removed (`keep_on_screen`).
- **Carrying each other.** Player 1 carrying player 2 works with Marina's own
  code. The other way needed three things: player 1's record stands in player
  2's slot during her update (her held slot points there); player 1's update
  runs after player 2's that frame (`mm_marina_update_hook` on
  func_8004ED10), because the carrier must move first or each anchors to the
  other and both slide with the camera; and the pickup frame, where Marina's
  code moves the carrier 55 px behind what she lifts, is run again with player
  1 placed so the carrier stays put. No grabbing each other from the air
  (Marina hangs on what she grabs mid-air and the pair froze) or within a
  second of being carried (escaping with B grabbed straight back).
- **Defeat while player 1 is down** no longer rejoins every frame; player 2
  rejoins with full health (the most either player has had, kept in game
  memory).

## Third round: anywhere in the stage (src/game/far_coop.cpp)

The game has one camera and keeps the world alive and drawn only near it
(docs/README.md 10.2: prefetch rings, the onscreen-only despawner, about 69
actor families with their own distance checks). Online (or with `MM_FAR=1`)
the game gets a second camera for player 2, so neither is held to the other:

- **A second camera in game memory.** The camera's globals
  (0x800BE544..0x800BE577, 0x800BE61C..0x800BE623) are kept for it at
  0x807F0500. Each frame, after the game's camera, func_80012830 runs again
  with that set loaded and player 2's position in Marina's place, so it
  follows her with the game's own smoothing and stage limits. Scroll locks
  are shared only when she is near the locked camera (same arena).
- **One world, run around both cameras.** Actor positions (+0x88 / +0x8C) are
  camera-relative (func_80016D94 scrolls them). Switching cameras swaps the
  global set and moves every scrolled actor (and what the players carry,
  +0x104 / +0x108) by the difference, so world positions stay. The map actor
  prefetch (func_8001DE30) runs a second time with her camera loaded; the
  onscreen-only despawner spares actors in her view; and in the actor loop
  (func_8001751C) each actor runs with the camera it is nearest to (a carried
  actor with its carrier's), so every distance check in its own code works.
  Player 2 has Marina's walls (+-0x90) around her own camera.
- **Each machine shows its own player.** Player 1's is the game's camera.
  On player 2's machine, just before the frame's graphics task goes out, the
  game's whole frame (Graphics_Begin_Frame) runs once more from a copy of
  memory with the two cameras' roles swapped; its display list, and every
  piece of memory it reads that this changed (found by comparing 4 KB pages),
  is copied above the fingerprinted range (0x80840000+, under 16 MB where the
  renderer still reads), memory is put back exactly, and the real frame's list
  jumps there (its colour image retargeted). Host hooks with state of their
  own sit that frame out (debug tick, mortal NPCs, rumble, item list). The
  picture is that of the next frame with this frame's input, which costs
  nothing in latency. The block actor's scratch moved from host memory into
  its record (it leaked between the two frames, and would have broken a
  resync too).
- **Leash** only for a player 2 out of her own camera's view (fallen, stuck).
- Checked: bot scenarios run each far-apart case twice, with and without
  building player 2's picture, and require identical memory fingerprints on
  every frame; screenshots from the real renderer (lavapipe under Xvfb) show
  player 2 crossing the whole first desert stage on her own with its enemies
  and blocks, while player 1's picture is unchanged.

## Fourth round: one health bar, gems, stars and exits for both

- **One health bar.** Player 2 starts each update with player 1's health
  (the HUD's) and what her update did to it (a hit, a gem) goes back to
  player 1's record: both play on one bar, and the game's own defeat ends it
  for both.
- **Gems for player 2.** Stage gems are item records the game collects
  against Marina's hitbox only (func_800446F0 from func_800451E4), gems from
  enemies are type-8 actors caught the same way. One that player 2 touches
  goes to player 1's hitbox that frame, where the game collects it for the
  team.
- **Marina's request word is per player.** 0x800BE5F4 is a mailbox: what a
  star teleporter asks of whoever holds it, and the flight Marina's update
  then asks of herself, taken up by her next update. Shared, player 1 took
  up player 2's requests (and flew instead of her), and player 2 took up the
  stage's requests to player 1. It is now in player 2's copy of Marina's
  globals (at the end of the list, so earlier offsets keep their place).
- **The lead swap (src/game/player2_lead.cpp).** Some of the game answers
  to slot 0 alone: a star's flight and the camera that follows it carry slot
  0, and stage scripts may check slot 0's position. When player 2 does
  something only the lead can, the two players swap characters: records,
  Marina's globals, this frame's controls, and (far apart) cameras; from
  then on Input_Update reads player 2's controller entry as player 1's. Each
  person keeps playing the same character in the same place, and the tint,
  the "2P" tag, the arrow and each machine's picture follow the person (the
  host's machine builds the second picture while player 2 leads). Triggers,
  all from game memory: player 2's character going into the star flight
  (0x0301); and a probe every 4 frames (every frame while player 2 presses a
  button or holds up) that runs the gameplay frame from a copy with the
  characters swapped and puts memory back exactly; if that frame starts a
  transition (the game state, the scene, the room-transition mode or target)
  and the frame as it is does not (or does only with player 2's controller),
  the two swap before the real frame. The lead stays 1.5 s before it may
  change again, and goes back to player 1 with the next scene. A probe costs
  about 1 ms on a 2.1 GHz Xeon (the game's 4.5 MB and player 2's block are
  copied and compared), so about 0.3 ms a frame on average.
- **Fixes found on the way.** The frame's matrix arena pointer (0x800EF4F4)
  sat in Marina's globals and went stale in player 2's copy; it is no longer
  copied in. When player 2 held something her held object updated with her
  camera, and player 1 came back from the wait with a copy taken under the
  other camera: she jumped 156 px when player 2 grabbed a star; she now comes
  back from her rebased record. A boss's introduction (Migen) looks away from
  both players; player 2 was taken for left behind and rejoined every few
  seconds: the leash now waits while player 1 is out of view as well.
- **Checked:** the star in 1-1's block pyramid flies player 2 (only her)
  while player 1 stays at the start; the 2-3 stars move both players to the
  next area; a boss with both players far-apart rules on (Lunar, 3-8,
  fought and beaten; player 2 away from the arena and back in it); parallax backgrounds in player
  2's picture match player 1's at the same spot; bot scenarios lead-star,
  lead-controls and net-lead (identical fingerprints on both machines across
  a swap).
- **Not done: sounds by each machine's own camera.** The game decides
  whether and how loud a sound plays from the game's camera
  (Sound_PlaySfxAtActor: within +-0x90 x +-0x60 of it, pan and volume from
  Sound_CalculatePanVol), and the sound engine's channels live in game
  memory: the handle goes back to the actor that asked, and later code stops
  or moves the sound by it. A machine deciding by its own camera would change
  game memory on that machine alone. So both machines hear what is near the
  game's camera (the lead's). Doing it per machine needs the sound engine's
  state taken out of the shared memory (its own copy per machine, with the
  game seeing the same handles on both), a bigger change.

## Fixes from the first far-apart session

- **Foreground scenery in player 2's picture.** The repeating trees, cacti
  and dune panels (docs/README.md 12.1) are one copy each that puts itself
  within 256 px of the camera every frame. Run with the camera it was
  nearest to, it stayed by player 1, and player 2's picture had none (in 4:3,
  whole patches of ground missing). They now always run with the game's
  camera: player 1's in the real frame, player 2's in her shadow frame.
- **The camera's look-ahead is per camera.** How far the camera leads
  Marina (0x801782B0/B4, eased towards +-0x40 by her input and speed in
  func_80046498 / func_8004667C) was shared: player 1's camera pulled it back
  while player 2 ran, and hers lagged and settled off her. It is now part of
  each camera's saved set, and both passes (the second camera's, and the
  shadow frame's) run with all of player 2 standing in for Marina (record and
  globals), not just her position.
- **Player 2's health after a throw.** When she threw player 1, player 1's
  next update wrote a value of its own (30) into player 2's health field,
  kept from then on: the tag showed an all but empty bar. Her health is set
  back to the shared bar at the end of each of her updates.

## Fixes from the second session

- **Super speed while carrying.** Marina's carry code adds the camera's step
  this frame (centre 0x800BE558 minus last frame's 0x800BE560, and y) to her
  position, making up for a scroll the game spares slot 0 in that state.
  Player 2 is scrolled as every other actor and rebased between cameras, so
  she got the step twice: ahead of her camera, which followed, and further
  ahead the next frame, up to 10 px a frame (Marina carries at 2). The step
  is zero to her during her update. The per-camera look-ahead had made the
  loop much faster; without it she still carried at 3 to 4 px a frame.
- **The star teleporter flight swaps at once.** The swap waited for a frame
  where pausing was allowed, and the flight's start can hold pausing off:
  player 2 then hung in the flight state in her own slot, unable to move,
  while the flight's camera lock dragged player 1. The flight now swaps
  whatever the game is doing (unless the two carry each other), and once she
  lands the other player joins her there (as through a door).
- **Bot scenario p2-carry-block**: player 2 picks up a stage block and walks
  with it; no frame may move her more than 4 px.

## Fixes from the third session: cheats for both, live from the host's menu

- **Movement cheats for player 2.** Endless jet, moon jump and hold to dash
  rewrote only slot 0 (whoever is Marina on that frame). They now also run
  with player 2 standing in as Marina (`player2::as_marina`), and moon jump's
  rises left are kept in game memory per player (0x807F0610 / 0x807F0614)
  rather than in a host-side static, so save states and both machines agree.
- **Live cheats online.** The host's settings were captured once at the start
  and held all session, while its menu kept changing its own saved choices:
  toggles did nothing until the session ended, and then the changed choices
  showed up "out of nowhere". The guest's menu changed its own saved choices
  too, which came back when it left. Protocol 2 sends the host's settings
  (cheats bitmask, live, hold to dash, gem hints) with every one of its
  inputs; both machines adopt them on the frame those inputs are used, so a
  toggle on the host takes effect on the same frame on both, a few frames
  later. The guest's menu shows the host's settings and refuses to change
  them (rank timer, compass, enemy bars and damage numbers draw only on the
  machine that has them and stay each player's own). Leaving restores each
  machine's own choices, untouched by the session.
- **Bot scenario net-cheats**: the host turns moon jump on and off
  mid-session (`MM_NET_TOGGLE=<cheat>@<frame>`), the guest tries and is
  refused; the change lines and the fingerprints match on both machines.

## Input delay from the measured connection (src/game/net_delay.cpp)

The delay used to be a fixed 4 frames: too much on a LAN, and on a slow
link every late packet froze both games. Now each side measures the round
trip (kPing/kPong on the input channel, four a second) and picks its own
delay: the worst recent sample halved, plus 4 ms of slack, in frames, plus
one; clamped to 2..10. It goes up at once when the trip needs more and
steps down one frame after five seconds of needing less. The choice only
shapes when inputs arrive, never what the game does with them (every input
is tied to a frame number and both sides wait for both inputs of a frame),
so the two sides can differ and change at any time and stay identical. When
the delay grows, the frames in between get the same input; when it shrinks,
nothing is sent until the frames already sent are passed. The host also
measures while the guest connects (and starts a third of a second after the
hello for that), so the session begins at the right delay; samples from the
first second after a (re)start are ignored, since those answers wait behind
the guest loading the start state. `MM_NET_DELAY=<n>` fixes the delay (tests).
The overlay shows "<ping> ms  delay <n>" bottom right while playing, and the
session's end prints how many frames waited over a frame for the partner.

The start message moved to the input channel: the inputs for the new epoch
follow it there and cannot overtake it. On separate channels a small input
packet can arrive before the 1.7 MB start (ENet orders within a channel
only), and a guest not yet started dropped it, then waited for it forever.
The lag simulator found this on its first run.

**Lag simulator** (`src/game/net_lag.cpp`): `MM_NET_LAG="<delay ms>[,<jitter
ms>[,<loss %>]]"` wraps the transport so each side holds its sends back by
the delay plus a random jitter, and now and then holds one back a further
round trip as a lost packet's resend would (later messages on the channel
wait behind it, as with a real reliable channel). Bot scenarios **net-lag**
(60 ±15 ms each way: the delay must settle at 5 or more, no resyncs,
identical fingerprints) and **net-loss** (20 ±5 ms, 1% loss).

## Transports: ENet and Steam

`net_transport.h` is the connection netplay runs over: one partner, two
reliable ordered channels, events Connected / Received / Disconnected /
Failed. `net_enet.cpp` is ENet (address and port); `steam_net.cpp` is Steam,
following coop-anything's playbook/networking-steam-spacewar.md: the flat C
API loaded at run time (no SDK headers), app 480, a friends-only lobby tagged
`troublemakers=1`, ISteamNetworkingMessages with messages over 60000 bytes
split, manual callback dispatch, LobbyCreated handled once though it arrives
twice, sessions accepted only from lobby members (the host) or the host (the
guest), and the partner dropped when it leaves the lobby or the session
fails. The guest finds a friend hosting by walking the friends list
(GetFriendGamePlayed: app 480 with a lobby), or joins by invite or Join Game.
`tools/steam_mock/mock_steam.c` is a stand-in libsteam_api.so for testing
two copies on one Linux machine (`coop_bots.py --steam-mock`, scenario
net-steam). Not yet checked: real Steam between two accounts.

Also fixed: in widescreen the game skips two border rectangles; the frame
list then had another length on a machine with widescreen off, which leaked
into game memory (the list head and stack copies) and made every online
session resync once a second. Two no-ops now take their place.

### Bot tests

`tools/coop_bots.py` runs scripted two-player scenarios in the headless build
(separating, carrying both ways, both grabbing, carry-jump-throw, random fuzz
from eight save states, far apart, the lead swap) and netplay sessions
between two processes, and checks
every frame: no position jumps, both on the shared screen, a carrier always
carries a live object, bounded rejoins; online also identical fingerprints and
no resync. It prints one line per scenario and ALL OK.

## Stage 3-12 (CERBERUS): one shared camera

The owner reported trouble in 3-12 online. The stage is a looping chase:
the screen scrolls on its own at 10 px a frame and pushes Marina along,
and every lap (about 785 px) the game wraps its camera back to the loop's
start. Two things went wrong with a camera per player:

- The second camera did not wrap with the game's, so once a lap it was a
  whole loop away from player 2 and dragged her off (the "jumped" lines in
  the bot traces).
- The second camera never auto-scrolls (the chase drives the game's camera
  only), so player 2 could simply be left behind at the loop's start, a
  part of the world the lap wrap moves out from under her.

So this stage keeps one shared camera even online (far_coop.cpp,
kSharedCameraStages): the screen edge carries both players, as it carries
Marina in the original game; the joiner sees the host's screen there. (A
first fix mirrored any camera jump of a lap's size onto the second camera;
it was dropped, as a lead swap or a star teleporter moves the game's
camera as far and it put player 2 in the wrong place.) Bot scenarios on a state
at the stage's start (slot 9): apart-9, fuzz-9-1/2, far-apart-9,
net-fuzz-9; the trace checks learnt about laps and about steps judged
against a moving camera.

### Player 2's cat (src/game/player2_mount.cpp)

The owner's report from the stage: one player had no cat. The cat is an
actor of its own (type 0x0F01, func_801A23C4) that the scene script spawns
into slot 96 and that moves Marina: its body sprite (the slot after it)
runs by the player's input and the scroll, and each frame Marina is put on
the body (func_801A22AC writes slot 0's position). Its code answers to
slot 96 only (a cat in any other slot is an idle one), and the scene
script asks Marina alone to get on (her request word, value 5). Player 2's
character ran Marina's riding state with nothing under it.

She now gets a cat of her own: custom actor type 0x7F03, spawned fresh at
her feet by player2_mount::tick when the stage's cat exists and she has
none, with the same get-on request in her own request word. Its update
runs the game's cat code: her copy (controller and 13 parts) takes slots
96..109 for the call, she stands in for Marina (as_marina), the game's
func_80016E70 runs slot 96, then the copies go back and player 1's cat and
record are restored. What the code does to the camera words
(0x800BE544..0x800BE5B8; the cat drives the stage's scroll through them,
and two cats scrolled it twice as fast) is put back, and her cat is held
The "invisible host" in the same report: the co-op screen-edge clamp
(180 px) held player 1 at the edge while her cat ran on to ~223 px, so she
stood off her cat behind player 2's. On a mount the clamp and the leash
now leave the riders alone (the stage keeps them in view, out to ~240 px). All of it is game
memory, so states, rollbacks, replays and resyncs carry it. Cerberus still
aims at the stage's cat (it reads slot 96 directly); both players can
still grab its parts.

## Fixes from the fourth session (owner's reports, 2026-10-28)

- Player 2 carrying player 1 walked and slid at half speed once the camera
  moved (normal at a stage's left edge). Her update zeroed the camera's
  step for Marina's carry code (right for her own camera far apart); on
  the shared camera, carrying player 1, the step is the carry's own (the
  camera follows player 1 in her hands) and the code needs it. Bot
  scenario p2-carries-p1-walk.
- Gem magnet pulled toward player 1 only: gems now go toward the nearer
  player (cheats.cpp player_centres); player 2's pickup pass collects them.
- Mortal NPCs stayed mortal after the cheat was turned off: the hook
  cleared each character's immortal bit (+0x150 0x80) for good. It now
  notes which characters it changed (registered "mortal-npcs-bit") and
  gives them the bit back when the cheat goes off. Bot scenario
  net-mortal-off.
- Player 2 (online, far apart) saw golden gems the save already has, and
  taking one gave nothing. The gem handler (func_800446F0) hides such a gem
  only when the game's own camera reaches it, and only after drawing it
  that frame (YellowGem_GetFlag, then +0x4000); player 2's picture runs a
  thrown-away frame from a copy, so there it was drawn, every frame, and
  never gone. far_coop.cpp hide_collected_golden_gems marks them gone from
  the whole list in the real frame.

## Throw cheats (src/game/throw_cheats.cpp, owner's request)

- Two sliders on the Cheats page, "Throw strength: objects" and "Throw
  strength: players", 25% to 1200% (Left/Right on the row; Enter steps
  up). Saved in cheats.cfg (throw_objects / throw_players, percent), sent
  with the host's settings online (SessionSettings throw_objects /
  throw_players: step + 1, 0 = default).
- A "Charged throw" cheat: while carrying, holding the grab button
  (0x800BE514's mask, B by default) charges instead of throwing; letting go
  throws, 100% for a tap rising with the square of the charge to 1500% at
  4 s, the way last aimed if the direction was let go of with the button.
  A bar over the player shows the charge (both players; on the joiner's
  screen from player 2's picture pass).
- How: the throw's velocity comes from a table (func_800554AC(thrower,
  object), into the object's +0xF8/+0xFC); a generated hook at its return
  scales it (players slider when the object is slot 0 or player 2's slot).
  The charge holds the button back from Marina's input words at
  func_8004ED10's entry (mm_marina_update_hook) and keeps its state per
  character in player 2's block (0x620 charge, 0x628 pending strength,
  0x630 aim), so states, rollbacks and both machines agree. Bot scenario
  net-throw.
- Player 2 shaking a character dropped no gems (1-2's lying Clancer).
  Shaking sets the held object's +0x98 bit 0x20000 (func_8004FDF8); the
  object's update reads it (func_8006E4EC drops a gem) and the end of the
  frame clears it (func_8001FF50). Marina shakes before the actor loop;
  player 2 shakes in it, after a lower slot has updated, so the bit came
  and went unseen. update_core notes it (0x807F0634) and held_swap_begin
  gives it to the object's update next frame.

## A health bar each (2026-10-29, src/game/player2_health.cpp)

- **Own health.** Player 2's character keeps its own health (+0xE0 of her
  record) instead of starting each update with player 1's. She joins a scene
  with what she had in the stage's previous scene, else full. Player 1's
  update, when thrown by her, still writes 30 into her health field; hers is
  put back after it.
- **Pickups heal the picker.** Her gems still go to player 1's hit box (the
  game collects them against slot 0 only); the health player 1 gains in the
  next three frames moves to her (`health::credit_to_p2`).
- **Down, then back.** At 0 health a character is "down" for 180 frames: not
  updated, not drawn (+0x80 bits 0x1, 0x80, 0x400 cleared), not hit, glued to
  the other character (so the shared camera and the far-apart rules see one
  player), then it comes back next to the other with half health. The downed
  one is always player 2's slot: when slot 0's character drops to 0 while the
  other is up, the two swap first (the lead swap, `swap_lead_for_down`), at
  GameState_Gameplay's entry, before Marina's update could start her defeat.
  Only both down lets the game's own defeat run. All in player 2's block
  (0x638..0x647), so states, rollback and the second machine carry it.
- **Each view's HUD.** The shadow frame (player 2's picture, and the second
  half in split screen) shows player 2's slot's health in the HUD bar.

## Reconnecting, Steam invites, controllers (2026-10-29)

- **Reconnecting.** A connection that drops mid-session no longer ends it.
  The host's game stalls for the partner's input (as on a slow link) and its
  transport keeps listening (ENet port, Steam lobby); the guest connects
  again to where it joined (address, or the Steam lobby it was in). When the
  guest's hello arrives the host restarts both sides from its game (a new
  epoch, logged "reconnected", not counted as a resync). Given up after a
  minute (the stall's limit). Ending a session on purpose sends kBye
  (protocol 4) so the other side does not wait; ENet now closes gracefully
  so the goodbye goes out first. Test: `MM_NET_TEST_DROP=<frame>` on the
  guest, scenario net-reconnect.
- **Steam without the overlay.** The Session page lists friends online once
  Steam is started; while hosting, picking one sends a lobby invite
  (InviteUserToLobby, no overlay needed); otherwise picking a friend who is
  hosting joins them. The invite window opens only when IsOverlayEnabled.
- **Controllers.** A controller is claimed by pressing a button on it
  (player 1 first, then player 2 with co-op on; a lone controller is player
  1 at once). Hotkeys answer to player 1's controller only. Rumble follows
  the person: hits, landings and jets on a person's character go to their
  controller (online, the local one); explosions, shakes and golden gems to
  everyone (rumble.cpp).
