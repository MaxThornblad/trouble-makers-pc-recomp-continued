# netcode-v2: one simulation, rolled back

The branch that reworks how two players share the game, so that each
player's controls answer at once online and new features need no netplay
code of their own. The plan and its background are in
[HANDOVER-netcode-v2.md](HANDOVER-netcode-v2.md); this file is the log of
what was built, what was measured and what was learnt, phase by phase.

## Phase 0: can the game roll back? (measured 2026-10-28)

Rollback means: when the other player's input for a past frame turns out
different from what was predicted, put the game back to the last frame
both inputs were known for and run the frames since again with the right
inputs, all inside one real frame. So three things had to be true:

1. a snapshot and a restore must be cheap enough to do every frame;
2. the frames run again must produce exactly the same game as the first
   time (else the two machines drift apart);
3. a frame must run fast enough that several of them fit in 16.7 ms.

`src/game/rollback_test.cpp` tests all three on a replay:
`MM_TEST_ROLLBACK="<every>:<depth>"` takes an in-memory snapshot
(`savestate::capture_memory`, no file) at every frame point, and every
`every` frames restores the one from `depth` frames back, rewinds the
replay's input cursor (`replay::rewind_to`) and runs those frames again
unpaced (speed multiplier 1000). The replay's per-frame fingerprint check
against the recording then judges the second run, and the harness compares
the fingerprint at the end of it with the first run's.

Numbers on the build container (Intel Xeon 2.1 GHz, 2 cores, headless), a
900-frame replay from the stage 1 save state with both players moving:

| | this container | estimate, owner's 9800X3D |
|---|---|---|
| snapshot (8.5 MB copy + registers, overlays, deferred events, DMEM, audio clock) | 1.3 ms | ~0.5 ms |
| restore | 1.3 ms | ~0.5 ms |
| one game frame run again (logic + audio task, no drawing) | 1.1 ms | ~0.4 ms |
| the replay's fingerprint of 8.25 MB (not needed every frame) | 2.4 ms | ~0.8 ms |
| rollback of 8 frames (restore + 8 frames) | ~10 ms | ~4 ms |

30 rollbacks of 8 frames and 15 of 4 frames: every frame run again
matched the recording, the fingerprints at the end matched the first run.
Verdict: rollback fits, with room for 8 or more frames of prediction on
the owner's PC and even on this slow machine.

What it took to get there, which is the real lesson of the phase:

- **The first runs mismatched.** Not the snapshot: the frames run again
  read a different audio level. The game's `Sound_Update` reads the AI_LEN
  register (`gAudioSamplesLeft` at 0x800C382C = AI_LEN/4), which in replays
  and netplay is driven by a virtual DAC in `register_overlays.cpp`
  (bytes submitted minus bytes drained, one frame's worth per frame
  point). Those counters, and the register word itself (at RDRAM offset
  0x24500004, outside the saved 8.5 MB), were not part of a state, so after
  a restore the game kept reading the level from the moment of the
  rollback. State format 4 now carries the virtual audio clock
  (`audio_clock.h`: submitted, drained, remainder, AI_LEN) and a restore
  puts it back. This is the first entry of the phase 1 list: host-side
  state that the game's behaviour depends on and that a snapshot must
  include.
- **A snapshot must not deliver host events.** The capture used to call
  `deliver_pending_external_messages`, which hands queued host events
  (VI, AI, SP completions, SI) to the game's queues at once; taken every
  frame, that changes when the game sees them. The runtime got
  `park_pending_external_messages()` (patch 0016): queued events move to
  the deferred list, which the state saves, and the game receives them at
  its next dequeue in the same order as without a snapshot.
- Reading the game's registers and memory has no side effects; a snapshot
  every frame leaves a replay matching every frame.

Not yet covered, for phase 3: the frames run again still submit their
audio to the sound device (heard twice) and would, in a graphics build,
still send their display lists to the renderer. Rollback needs a way to run
a frame with no presentation at all. And the replay's fingerprint is far
more expensive than the game frame itself, so rollback's own consistency
checks should hash less, less often, or incrementally.

## Phase 1: one place for host-side state (src/game/state_registry.h)

The rule: anything kept outside game memory that the game's behaviour
depends on is registered, once, as a `mm::state::Pod<T>`; the registry puts
it in every save state (format 5, as named, versioned blobs), so it goes to
a joiner with the host's game, comes back with a rollback, and is folded
into the replay and netplay fingerprint (the last block's hash), so drift
in it shows as a desync instead of hiding. A state that lacks a module (an
older file) resets it. New state should go into game memory instead, where
none of this is needed (player 2's block at 0x807F0000 is the example).

How the candidates were found: not by reading. `coop_bots.py --rollback
30:8` plays every local and far-apart recording back with a rollback every
30 frames, and any state the snapshot lacks shows up as the frames run
again differing from the recording (`play_with_rollback`). Running that
audit over the 33 scenarios is now the check that the registry is complete,
and it stays in the suite.

Converted so far:

- **Mortal NPCs** (cheats.cpp): the per-slot shadow health that decides when
  a refilling NPC is beaten. It reset on every state load, so a joiner (who
  starts from the host's state) and a rollback both forgot the damage dealt
  so far: with the cheat on, the two machines could beat an NPC on
  different frames. Registered as "mortal-npcs".
- **Widescreen mode** (widescreen.cpp): the hysteresis counters that decide
  when the expanded view, and with it the wider actor activity around the
  camera, switches on and off. They were function-local statics reset by a
  generation counter after a load. Registered as "widescreen-mode"; the
  generation counter is gone.

Looked at and left alone, with the reason:

- Intra-frame scratch (set and used within one frame): player 2's stand-in,
  held-swap and contact bookkeeping, far_coop's wall save, draw and camera
  swap flags, lead probing.
- Presentation and output only: markers and tags, gem compass scan, enemy
  bars and damage numbers, rumble (its watch resyncs from game memory on a
  load), the inspector.
- Debug tools that change the game (the spawn tool in actors.cpp): phase 2
  turns those into commands in the input stream.
- Netplay's own session state: not game state.
