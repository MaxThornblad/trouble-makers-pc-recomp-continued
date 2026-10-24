# Game map: functions, memory and actor types

A running reference of what the modding work has established about the US 1.1
game, so nobody has to rediscover it. Addresses are N64 virtual addresses
(KSEG0). "Q16.16" = 32-bit fixed point, integer part in the upper half.
Each entry says how it was established; anything marked *(guess)* is not yet
confirmed. Keep this file up to date when new things are found.

Related: `docs/README.md` (engine internals, widescreen, rendering).

## Actors

208 slots of `0x198` bytes from `0x800EF510`; slot 0 is Marina.

| Offset | Size | Meaning | Source |
|---:|---|---|---|
| `0x00-0x7F` | | two renderer matrices (one per framebuffer); never copy | docs/README.md 12.1 |
| `0x80` | u32 | flags; 0 = slot free. Bit `0x2` = runs each frame and gets terrain collision, `0x1` = drawn *(guess)*, `0x20` = facing left (Marina), `0x2000` = platform (stand on top), `0x00040000` = solid on all sides (both only for slots below `0x90`, see Collision), `0x80` = attack box active, `0x400` = hit box active, `0x02000000` = Marina is holding something. Grabbable object free `0x00011403`, held `0x00020123`, thrown `0x00021483` | func_8001751C, func_8001EC1C, grab traces |
| `0x84` | u16 | image id for the actor renderer (bank bits `0xF800`) | func_80009BE8, SpawnParticle_Image |
| `0x88`, `0x8C` | Q16.16 | position, camera-relative, +y up | traces |
| `0x90` | | depth / layer *(guess)*; a held brown block copies the holder's `+0x10C` here | SpawnParticle_Image, traces |
| `0xA0` | u8 | contact flags from terrain collision | func_800160EC |
| `0xB4`, `0xB8` | f32 | scale x/y; the brown carry block shrinks to 0.5 while held (the `0x208` sprite path ignores it) | traces, Wine |
| `0x98` | u32 | signal bits from other actors: `0x200` = held this frame (set by the holder every frame; the held actor clears it), `0x400` = thrown | func_800528F4, grab traces |
| `0x94` | u16 | draw mode; `0x208` with image `0x46` at `+0x84` = draw the sprite whose pointer is at `0x18C` (image 0 draws nothing) | func_800441F4 (block knock-out), Wine |
| `0xA2-0xA8` | s16 x4 | attack box: left, right(?), top, bottom relative to position | func_8001E2D0, func_8001EC1C |
| `0xAA-0xB0` | s16 x4 | hit box: left `AA`, right `AC`, top `AE`, bottom `B0` (+y up) | gem pickup func_8002FD48 |
| `0xC8`, `0xCA` | s16 | world x / top of the actor this one stands on | func_800160EC |
| `0xCC` | u16 | `0x8000 \| slot` of the actor this one stands on, else 0 | func_800160EC |
| `0xD0` | u16 | state; low byte = action, high byte = phase | traces |
| `0xD2` | u16 | type (see Actor types) | everywhere |
| `0xD6` | u16 | Marina: slot of the actor she holds (stays set after a throw) | grab traces |
| `0xE0` | s16 | health | Actor_ReduceHealth |
| `0xEC`, `0xF0` | Q16.16 | velocity x / y | Marina movement |
| `0xF8`, `0xFC` | Q16.16 | throw velocity, written by the holder with the `0x400` signal | grab traces |
| `0x104`, `0x108`, `0x10C` | Q16.16 | hold point (camera-relative x, y) and depth, written by the holder each frame | grab traces |
| `0x150` | s32 | per-state timer (jet, jump) | cheats.cpp |
| `0x158` | s32 | gravity accumulator (air jet arc) | cheats.cpp |
| `0x17C` | u32 | private display list (3D actors) | func_80009BE8 |
| `0x184`, `0x188` | | particle start position | SpawnParticle_Image |
| `0x18C` | u32 | sprite pointer for draw mode `0x208` | func_800441F4 |

Marina-specific: hold buttons `0x801370CC`, press `0x801370CE` (D-pad bits come
from `0x800BE50C` left / `0x800BE510` right; up `0x0800`, down `0x0400`, A
`0x8000`, B `0x4000`). States: `0x020B` ground jet, `0x0110` air jet, `0x0116`
jump rise, `0x0216` fall, `0x0401` hurt, `0x0230` held-and-struggling *(guess)*,
`0x011F` grab reach, `0x0122` lift, `0x0004` holding, `0x021A` wind-up, `0x0126` throw.

### Grabbing and throwing

Marina's grab calls `func_800528F4(holder, held)` each frame: it sets
`held+0x98 |= 0x200` and writes the hold point to `held+0x104/0x108`. If the
`0x200` bit is still set on her next check (the held actor did not answer), or
the held actor's health is 0 or less, she lets go. A held actor clears the bit
every frame, enters state `0x?61` (flags `0x00020123`) and moves itself to the
hold point. On a throw (state `0x0126`) she sets `held+0x98 |= 0x400` and the
velocity at `+0xF8/+0xFC`; the actor then flies (state `0x0170` then `0x0171`,
flags `0x00021483`) and the per-actor terrain collision stops it at walls and
floors. Any actor with flags `0x00011403` near her hand can be grabbed this
way (proved with the custom block). Marina's set-down is B while holding still
(the held actor sees the `0x200` bit stop without a `0x400`). Brown carry
blocks also get `+0x98 |= 0x00080000` while held.

### Collision

- **Terrain map**: one byte per 16x16 cell at `0x8010CDF0`, index
  `((x >> 4) & [0x800BE658]) + (((H - y - 1) << [0x800BE660]) & [0x800BE65C])`
  for world pixel (x, y), H = `[0x800BE648]` (1-9: 2048, 512 columns). Bits
  `0xC0` = solid, `0x40` alone = platform *(guess)*. Covers the whole stage.
  Read by `func_80012AB4(x, y)` (camera-relative arguments). Stage blocks are
  solid only through their cells here: hiding a block's item record leaves its
  collision, and moving the record does not move it.
- **Solid actors**: `func_8001F88C` rebuilds two lists each frame from slots
  `0x00-0x8F`: flag `0x00040000` → solid boxes (count `0x800BE4D0`, slots
  `0x8011CF20`, right/left/top/bottom at `0x8011D170/D3D0/D610/D850`), flag
  `0x2000` → stand-on platforms 8 px thick (count `0x800BE4D4`, slots
  `0x8011CDF8`, then `0x8011D048/D290/D4F0/D730`). `func_8001FA78(slot, x, y)`
  tests a point against them (skipping itself and its holder), returns `0xC0`
  or `0x40` and leaves the hit slot in `0x800BE4DC`. Every actor's terrain
  collision (`func_80015B28`, part of `func_800160EC`) uses it, so Marina,
  enemies and blocks stand on such actors; standing sets `+0xCC`.
- The pot (`0x18`, flags `0x0001B003`) is a platform; the brown carry blocks
  (`0x2605`, flags `0x00001402`) are neither.

### Actor code paths

| Function | Purpose | Source |
|---|---|---|
| `func_8001751C` | per-frame update loop over slots 1-207: type < `0x100` calls the shared table, else `func_80016E70` | read |
| `func_80016E70` | banked type dispatch: high byte `0x01-0x2C` picks a stage package table, any other bank does nothing. Hooked: bank `0x7F` = custom types | read; custom_actors.cpp |
| `0x800C7FE0` | shared type table, one function pointer per type `0x00-0x7C`; `0x7D-0x7F` empty | save-state dump |
| `func_8001E2D0` | slot setup for its type: hit/attack boxes, health, flags from per-type tables at `0x800C9700` (7 halfwords per type), `0x800C9DD8` (flags), `0x800C9FCC`; banked types get defaults (32x32 box, 10 health, flags 3) | read |
| `Actor_RangeFindInactive(lo, hi)` | first free slot in a range; gems use `0x7A-0x82`, gem rings `0x90-0xC0` | SpawnGemActor |
| `func_80028528` | finds a free particle slot | SpawnParticle_Image_90C0_16 |
| `func_80009BE8` | actor draw builder; appends `0x17C` display lists | docs/README.md |
| `func_800160EC(slot)` | per-actor terrain collision; hooked for no clip | cheats.cpp |
| `func_80016CB4` | loops actors with flag bit `0x2` through terrain collision | read |
| `func_8001EC1C` | actor-vs-actor box overlap pass (attack vs hit boxes) over slots `0-0x8F` | read |
| `Actor_ReduceHealth(slot, amount)` | health minus amount, returns 1 while alive | read |
| `SpawnParticle_Image(slot, image, x, y, …)` | makes slot a type 9 particle drawing `image` | read |

### Actor types (shared, available in every stage)

| Type | What | Source |
|---:|---|---|
| `0x08` | gem (colour in low bits of its spawn argument; yellow = golden) | SpawnGemActor |
| `0x09` | particle (images, debris, the flying piece of a broken block) | SpawnParticle_Image |
| `0x16` | Marina | slot 0 |
| `0x1C`, `0x3F`, `0x40` | orange clanball (image `0x026E`) | Wine catalogue |
| `0x48` | small clancer | Wine catalogue |
| `0x49-0x52` | purple bomb balls (image `0x0272`) | Wine catalogue |
| `0x54`, `0x55`, `0x56`, `0x5B`, `0x77` | blue clancers | Wine catalogue |
| `0x3D`, `0x76`, `0x7C` | gem variants *(guess)* | Wine catalogue |
| one of `0x0D 0x2E 0x31 0x42` | changes the sky to grey clouds | Wine catalogue |
| `0x18` | the pot from Clanpot Shake; works in other stages; stand-on platform, can't be carried (Marina's grab lets go at once) | spawn test, 1-9 |
| `0x11-0x14`, `0x35`, `0x74` | crash when spawned bare (need parameters or other code) | spawn catalogue |
| one of `0x0B 0x0C 0x0E 0x0F 0x10 0x15`, one of `0x61 0x62 0x64 0x6D 0x6E 0x70 0x72 0x75` | page fault under Wine when spawned bare in a group (0x0B's display list pointer is garbage) | Wine catalogue |

Spawn-catalogue results for every shared type are listed in `src/game/spawn_catalog.cpp`.

Stage-specific types seen: `0x2605` brown carry block in 1-9 (image `0x0084`, draw `0x0109`; shrinks while held, breaks when thrown), `0x0515` Sea of Lava floor block (3D), `0x0508`/`0x0509`
Migen's Shrine rotating wall/platform, `0x180F` 1-9 enemy with 50 health, `0x2604`
health 10000 helpers in 1-9 *(guess: scenery)*.

Custom types (bank `0x7F`, src/game/custom_actors.cpp): `0x7F00` bobbing test block,
`0x7F01` block object (solid, grabbable, throwable; src/game/grab_blocks.cpp turns stage blocks into it).
Spawning: src/game/actors.cpp (`spawn_now`: free slot from `0x79` down to `0x30`,
clear `+0x80..`, set type, call `func_8001E2D0` with a scratch stack at `0x8045FF00`).

## Stage items (gems, blocks)

Not actors: 6-byte records `{u16 kind, u16 x, u16 y}` (world pixels, +y up) in a
list walked every frame by `func_800451E4` (`$a0` = list; `0x800D2978` in the
stages seen), ending at kind 0.

| Field | Meaning |
|---|---|
| `kind & 0xC000` | gone (collected / broken); the game sets `0x8000` or `0x4000` |
| `(kind & 0xFFF0) >> 4` | handler index into `0x800D37BC` |
| `kind & 0xF` | colour (copied to `0x801782BE`) |

Handlers: `0x800446F0` gems (colour `& 3`: 2 = golden), `0x80044390` blocks
(type 1; sprite `0x800D1968[colour]`; colour 1 breaks when `func_80012AB4(x, y)`
reports no `0xC0` bits and spawns its flying piece through `func_800441F4`).
Each processed item gets a draw/collision slot in the pool at `0x804269E0`
(128 x `0x90`, relocated from `0x801069E0`): `+0x80` u16 frame/kind, `+0x84`/`+0x88`
camera-relative x/y, `+0x8C` sprite pointer. Current item index `0x801782C0`,
current record pointer `0x801782BC`. Blocks sit on a 32-px grid
(1-9: x 3440/3472, y 302/334; a block at 3440,334 covers x 3424-3455, y 320-351).
Setting a record's `0x8000` bit stops it drawing; its solidity is in the
terrain map (see Collision). Gem pickup test: `func_8002FD48` (item box
+-6 px against Marina's hit box), collect: `func_8002F93C`.

## Game state and globals

| Address | Meaning |
|---|---|
| `0x800BE4F0` | game state: 6 / 10 stage gameplay, 12 stage transition, 5 stage load |
| `0x800BE4E8` | paused when non-zero |
| `0x800BE5D0` | scene |
| `0x80178162` | progression row (stage) |
| `0x801781E0` | stage timer, frames |
| `0x800C8478` | par time per row (u16 frames) |
| `0x800C4F28` | best time per row |
| `0x80171B10` / `0x80171B14` | golden gem mask, rows 32-63 / 0-31 |
| `0x80178136` | red gem count (capped at 9999 by RedGems_Clamp) |
| `0x800BE558` / `0x800BE55C` | camera centre x / y, Q16.16 |
| `0x800BE590` / `0x800BE594` | screen-shake offset x / y, Q16.16 (zero when calm) |
| `0x800BE4F8` / `0x800BE4FC` | global button hold / press |

Stage view: the 320x240 canvas fills the window height; stages draw 144 px either
side of the camera centre, 100 px above and 90 below (checked against gem sprites).

## Sound effects

All go through `Sound_AddSfx(id, …, volume at 0x10($sp))` at `0x80003020`.

| Id | Sound |
|---:|---|
| `0x43` | bomb explosion |
| `0x93` | red block blown apart |
| `0x4B` | eruption rumble (Sea of Lava), every 8 frames |
| `0x57` | gem collected |
| `0xAA` | Marina hurt |
| `0x25` | landing *(guess)* |
| `0x27` | jump *(guess)* |
| `0x53` | footstep or jump start *(guess)* |
