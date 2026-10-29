# Contributing to Trouble Makers

How to build the game, change it, and find your way around it. Start here,
then keep two references open:

- [`docs/GAME_MAP.md`](docs/GAME_MAP.md): what is known about the game itself:
  actor fields, actor types, input, frame order, items, throwing, shaking,
  memory addresses.
- [`docs/README.md`](docs/README.md): engine internals: overlays, rendering,
  widescreen, the recomp itself.

Co-op and netplay have their own design notes: [`docs/NETPLAY.md`](docs/NETPLAY.md)
(how it works, plus a log of every bug and its cause) and
[`docs/NETCODE-V2.md`](docs/NETCODE-V2.md) (the rollback rework). [`CLAUDE.md`](CLAUDE.md)
lists the build gotchas in short form.

## 1. Build

You need your own US 1.1 Mischief Makers ROM. Never commit it, any game
files or save states.

```sh
# once: the runtime submodule gets the project's patches
git submodule update --init --recursive
git -C lib/N64ModernRuntime am "$(pwd)"/patches/N64ModernRuntime/*.patch
# lib/rt64: clone the pinned fork (commit 23cab603), init its submodules,
# apply patches/rt64/*.patch (see CLAUDE.md)

# the ROM
cp /path/to/your/rom.z64 input/troublemakers.us1.z64

# the recompiler (CMake target N64RecompCLI builds the N64Recomp executable)
cmake -S tools/N64Recomp -B tools/N64Recomp/build && cmake --build tools/N64Recomp/build --target N64RecompCLI RSPRecomp -j

# generate the C from the ROM (again after every change to troublemakers.us1.toml)
tools/N64Recomp/build/N64Recomp troublemakers.us1.toml
tools/N64Recomp/build/RSPRecomp aspMain.us1.rsp.toml

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target troublemakers -j
./build/src/game/troublemakers input/troublemakers.us1.z64
```

Windows builds need clang-cl (MSVC `cl` cannot compile the generated C).
Without a GPU, `cmake -B build_headless -DMM_BUILD_GRAPHICS=OFF` builds a
headless binary for tests. Run it with `MM_HEADLESS_GFX=1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

Turn the debug menu on in the launcher. Most tools and all cheats live there.

## 2. How the game is changed

The game is not emulated: every MIPS function in the ROM became a C function
(`RecompiledFuncs/funcs_*.c`, generated, never edit them). Game memory is a
byte array `rdram`. Our code changes the game in three ways:

1. **Hooks** in [`troublemakers.us1.toml`](troublemakers.us1.toml), under `hook = [`:
   a line of C that N64Recomp pastes into a game function, at its entry or
   before a given instruction:

   ```toml
   { func = "func_800554AC", before_vram = 0x80055524, text = '{ extern void mm_throw_velocity_hook(uint8_t*, recomp_context*); mm_throw_velocity_hook(rdram, ctx); }' },
   ```

   The hook calls an `extern "C"` function in `src/game/`. A hook at the entry
   can `return` to skip the game function. After editing the toml, rerun
   N64Recomp and rebuild.

2. **Reading and writing game memory** from our code with the runtime's
   macros: `MEM_W(offset, address)` (32-bit), `MEM_H` / `MEM_HU` (16-bit),
   `MEM_B` / `MEM_BU`. Addresses are N64 virtual addresses as a sign-extended
   `gpr`. Most files have a little `addr(vaddr)` helper for that:

   ```cpp
   const gpr marina = addr(0x800EF510);            // actor slot 0
   const int x = MEM_W(0x88, marina) >> 16;        // position, Q16.16
   MEM_W(0xEC, marina) = 3 << 16;                  // velocity x: 3 px/frame
   ```

3. **Calling game functions** directly. Give them a context with a scratch
   stack in the unused expansion RAM. In a hook, pass the game's own `ctx`
   instead:

   ```cpp
   recomp_context c{};
   c.r29 = addr(0x8045FF00);   // scratch stack
   c.r4 = slot;                // $a0
   func_8006CFDC(rdram, &c);   // e.g. beat a Clancer
   ```

Find game functions in `symbols/troublemakers.us1.toml` (names and sizes),
read them in `RecompiledFuncs/` (each line carries the original MIPS
instruction as a comment), and write down what you learn in
`docs/GAME_MAP.md`.

## 3. Rules that keep co-op, netplay and save states working

These come from bugs that cost hours. They are cheap to follow:

- **State goes in game memory.** If your feature remembers anything from one
  frame to the next, put it in game memory: an actor field, or a word in the
  mod area at `0x807F0000` (layout in `src/game/player2.cpp`'s header and
  `docs/GAME_MAP.md` "Mod memory"). Then save states, replays, rollbacks and
  the second machine carry it. If it must be a C++ variable, register it:
  `static mm::state::Pod<MyState> g_state{"my-feature", 1};`
  (`src/game/state_registry.h`).
- **Input comes from the latched frame.** Read buttons from the game's own
  input words or from `mm::frame_input::current()`, never straight from SDL, so
  both machines see the same input on the same frame.
- **Settings that change the game ride in `SessionSettings`**
  (`src/game/cheats.h`). Online the host's values rule, and locally they are
  latched each frame.
- **Frames that are thrown away.** Player 2's picture when the players are far
  apart, and the lead-swap probes, run a whole frame from a copy of memory
  and then put it back (`mm::faraway::in_shadow()`). Code that only touches
  game memory can run there. Code that changes host state (a registered Pod,
  a log line, a file) must skip it.
- **Player 2 is a second Marina.** Her update swaps her record into slot 0
  and runs Marina's code (`src/game/player2.cpp`). Code written for "Marina"
  works for her when run through `mm::player2::as_marina()`. Hooks inside
  Marina's code run for both players. Player 2 updates inside the actor
  loop, after lower slots (see "Frame order" in GAME_MAP.md). Signals the
  game passes within one frame may need carrying over (the shake fix).

## 4. Recipes

### Add a cheat (example: Charged throw)

1. `src/game/cheats.h`: append to `enum class Cheat` (before `Count`).
2. `src/game/cheats.cpp`: add `{ "cfg_key", "Menu name", "description" }`
   to `kCheats` in the same position. The menu row, the cheats.cfg entry and
   the online settings bit come with it.
3. Make it act. Either once a frame in `mm::cheats::apply()` (`if
   (enabled(Cheat::X)) ...`), or from a hook that checks
   `g_active_for_hooks() && enabled(Cheat::X)`.
4. Keep any per-frame state in game memory (rule 1).

`src/game/throw_cheats.cpp` is a complete small example: a hook on a game
function, input filtering before Marina's update, state in the mod area, and
an on-screen bar.

### Add a slider or setting

Follow `throw_strength` in `cheats.cpp`: an atomic for the player's own
value, a cheats.cfg key in `save()` / `configure()`, a byte in
`SessionSettings` (filled in `capture_session_settings`, read in
`set_session_settings`), and a menu row in `src/game/debug_menu.cpp`
(Left/Right go to `change_page`, which the throw rows intercept).

### Add an actor type

Custom types live in bank `0x7F` (`src/game/custom_actors.cpp`): add an
index to the enum in `custom_actors.h` and an entry `{name, update, setup}`
to `make_table()`. The game then calls your update every frame for that
type. `src/game/player2_mount.cpp` shows how to run a stage actor's own
code on a copy.

### Draw something over the game

An overlay module publishes positions on the game thread (from
`mm_debug_menu_tick` in `debug_menu.cpp`) and draws with ImGui on the render
thread (`draw_overlays` in `main.cpp`). `src/game/enemy_bars.cpp` and
`throw_cheats.cpp` both map camera-relative pixels to the window this way.

## 5. Finding things out

- **Debug menu → Inspect** and **F2** show actors, their fields and collision
  boxes live.
- **Save states** (Session page / hotkeys) get you back to a spot instantly.
  Tests load them with `MM_TEST_SAVESTATE_SLOT=n MM_TEST_SAVESTATE=load@150`.
- **Test switches** (environment variables, list at the top of
  `src/game/dev_harness.cpp`): `MM_TEST_DUMP=frames` lists every actor,
  `MM_TEST_WORDS=slot@frame` prints a slot's words, `MM_TEST_PEEK=addr:n`
  prints memory, `MM_TEST_SPAWN` spawns a type, `MM_TEST_WARP=1-2` warps to
  a stage. Frames count from the last state load.
- **Who writes this address?** Run under gdb with
  `MM_TEST_WATCH=800F43C0@150`. On that frame the game stops with
  `mm_watch_ptr` set, then `watch -l *(unsigned int*)mm_watch_ptr` and `bt`
  show the game function that writes it. This is how the throw table and
  the shake bit were found.
- **Scripted input**: a replay records a pad script, so you can repeat the
  same moves:

  ```sh
  MM_REPLAY_RECORD="/tmp/rec@200" MM_REPLAY_LENGTH=600 \
  MM_REPLAY_PAD="60:,120:r,8:rA,12:B,20:,3:d,3:,3:d" MM_REPLAY_PAD2="600:" \
  MM_TEST_SAVESTATE_SLOT=8 MM_TEST_SAVESTATE=load@150 ./troublemakers rom.z64
  ```

  Each item is `frames:buttons`, with `A B Z S`, d-pad `u d l r` and stick
  `U D L R`. `MM_REPLAY_PAD2` drives player 2. `MM_P2_TRACE=1` prints both
  players every frame.
- **Bot scenarios** (`tools/coop_bots.py`) script whole co-op and online
  sessions and check them. To test a change, run just the scenarios it
  touches: `--only net-throw,p2-carries`. `--keep DIR` keeps the logs.

## 6. Sending changes

Work on a branch of the fork, keep commits focused, and write in the commit
message what the game does and why the change is right. Update
`docs/GAME_MAP.md` with anything you learned about the game, and
`docs/NETPLAY.md` when you fix a co-op or netplay bug: what broke, why, and
the fix.
