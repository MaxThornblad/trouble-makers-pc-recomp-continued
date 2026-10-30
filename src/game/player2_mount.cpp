#include "player2_mount.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "far_coop.h"
#include "foreign_actors.h"
#include "player2.h"
#include "player2_health.h"

extern "C" void func_80016E70(uint8_t* rdram, recomp_context* ctx);   // banked actor update, $a0 = slot
extern "C" void func_80012AB4(uint8_t* rdram, recomp_context* ctx);   // terrain cell at a camera-relative point

namespace mm::player2_mount {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint32_t kGameState = 0x800BE4F0u;       // 6 and 10 = stage gameplay
constexpr uint32_t kMarinaRequest = 0x800BE5F4u;   // Marina's request word (scene scripts)
constexpr uint32_t kMarinaWorldX = 0x800BE5D8u;    // Marina's world position (the camera follows it)
constexpr uint32_t kMarinaWorldY = 0x800BE5DCu;
// The game's camera words (far_coop.cpp's camera set and the look-at
// target after it, 0x800BE5A8..B4, which LookAt_Update reads).
constexpr uint32_t kCamera = 0x800BE544u;
constexpr uint32_t kCameraSize = 0x74u;
constexpr uint32_t kCameraX = 0x800BE558u, kCameraY = 0x800BE55Cu;   // s32 16.16: what screen positions are relative to

// The cat (3-12) keeps state outside its record, in the stage package's
// globals 0x801A80E6..0x801A80EB, which any cat code reads and writes. Last
// of them, 0x801A80EA, is a message from Lunar's missiles (type 0x0F09,
// func_801A1178): a missile the cat lands on (its flying state checks the
// cat in slot 96, falling, against its box) or carries (its ridden state,
// every frame) writes its slot there, the cat rides while it reads one and
// clears it at the end of its update (func_801A23C4). Player 2's cat ran the
// same code: it read player 1's message (her cat updates after the
// missiles) and cleared it, so neither cat could ride a missile.
//
// So her cat has its own copy of those globals (in player 2's block),
// swapped in for its update, and the missiles talk to her copy for her: a
// missile she rides (her rider's missile word) writes her message, and a
// flying one that player 1's cat is not landing on sees her cat in slot
// 96's place for its update when hers is (cat_lands). Player 1's cat uses
// the package's own globals. All game memory.
constexpr uint32_t kCatGlobals = 0x801A80E6u;
constexpr uint32_t kCatGlobalsSize = 6;
constexpr uint32_t kMissileMessage = 0x801A80EAu;   // u16: the missile's slot, 0 = none
constexpr uint16_t kCat = 0x0F01;
constexpr uint16_t kBee = 0x1F02;
constexpr uint16_t kFallState = 0x16;                // Marina's fall (low byte of +0xD0)
constexpr uint16_t kRideState = 0x03;                // Marina on a mount
constexpr uint16_t kOnCat = 0x0100;                  // Marina's +0x94: on the cat (set by the cat)
constexpr uint32_t kPlayerState = 0x80137458u;       // Marina's player-state word
constexpr uint32_t kOnLoop = 0x10000;                // ... carried by 3-12's loop (set by the cat)
constexpr uint32_t kRiderHit = 0x80137444u;          // player-state word: bits 0x2 / 0x10000, she was hit (func_801A23C4)
constexpr uint32_t kRiderHitBits = 0x10002;
constexpr uint16_t kMissile = 0x0F09;
constexpr uint16_t kMissileFlying = 1, kMissileRidden = 4;
constexpr uint32_t kMissileHit = 0x200;             // +0x98: hit, about to blow up
constexpr uint32_t kMissileLetGo = 0x1;             // +0x98: its rider is gone (what the cat sets when hit while riding)
constexpr uint16_t kCatLanding = 0x58, kCatRiding = 0x59;
constexpr uint16_t kCatFalling = 0x22;              // what the missile's landing test wants (0x21 / 0x22)
// The mount button (cheat cats outside 3-12): N64 Z, which does nothing in
// play (the game reads it only to skip cutscenes; Marina's own input strips
// it). Pressed with a flying missile within kBoardReach of the cat, the cat
// is put just above it, falling, as 3-12's loop brings it there: the
// missile's own landing test (func_801A1178, flying state) then lands it,
// and the cat's code rides it as in 3-12. Pressed while riding, the missile
// is told its rider is gone (let_go), as the cat's own code does when hit.
constexpr uint16_t kMountButton = 0x2000;
constexpr uint32_t kMarinaPress = 0x800BE4FCu;      // u16: the buttons slot 0's character presses this frame
constexpr int kBoardReach = 96;                     // px from the cat to the missile
constexpr int kBoardFrames = 12;                    // frames the missile is kept from blowing up meanwhile
constexpr int kCallAbove = 24;                      // px over the cat a called missile appears (made at its feet, the cat landed on the ground instead and the missile flew off alone, or blew up on the ground)
constexpr int kBoardAbove = 8;                      // px: the cat this far above the missile (its test: -4..+19)
constexpr uint32_t kRiderStruck = 0x2E;             // a rider's +0x98: what a missile touching her sets
constexpr uint32_t kMissileHitBits = 0x201;         // +0x98 in flight: hit (0x1), about to blow up (0x200)
constexpr int kRideReach = 64;                      // px: a cat this far from its missile is not on it

// A mount: the stage's type, the slot its code answers to, and how many
// slots it spans (controller first, then the part sprites it moves, which
// it finds in the slots after its own).
struct Mount {
    uint16_t type;
    int slot;
    int span;
    uint32_t request;   // what the scene script asks of Marina at the start (her request word): get on
    const char* name;
};
constexpr Mount kMounts[] = {
    {kCat, 96, 14, 5, "cat"},   // 3-12 CERBERUS: the cat Marina rides (func_801A23C4; parts in 97..109)
    {kBee, 48, 24, 5, "bee"},   // 5-3: the bee Marina rides (func_801A89C0; parts in 49..71)
};
constexpr const Mount& kCheatMount = kMounts[0];   // the "Ride the cat" cheat

// Who rides a mount of their own (a custom actor holding a copy of the
// stage's): player 2 where the stage gives Marina one, and either player
// with the cheat where it does not. Their words are in player 2's block
// (player2.cpp's layout).
struct Rider {
    bool p2;
    uint8_t custom;        // custom actor index of the copy
    uint32_t mount;        // s32: controller slot of the copy, 0 = none
    uint32_t type;         // s32: the stage type it copies
    uint32_t globals;      // her own copy of the cat's globals (player 2 only)
    uint32_t missile;      // s32: the missile her cat rides (player 2 only)
    uint32_t lift;         // s32: the cat's floor outside 3-12 (see kCatRest)
    uint32_t facing;       // s32: 1 while the cat outside 3-12 faces left (see mirror_cat)
    uint32_t high;         // s32: 1 once the stage took her mount's slots (spawn)
    uint32_t pace;         // s32: the cat's speed outside 3-12 (16.16), kept through a frame a wall stops it
    uint32_t boarding;     // s32: the missile the mount button put the cat onto (slot), 0 = none
    uint32_t boarding_left;// s32: frames that boarding still waits for the missile's landing
    uint32_t boarding_health; // s32: the rider's health when she pressed (the missile does not hurt her getting on)
};
constexpr Rider kP1{false, kPlayer1CustomIndex, 0x807F0680u, 0x807F0684u, 0, 0, 0x807F0688u, 0x807F0690u, 0x807F0698u,
                    0x807F06A0u, 0x807F06A8u, 0x807F06B0u, 0x807F06B8u};
constexpr Rider kP2{true, kCustomIndex, 0x807F0418u, 0x807F041Cu, 0x807F0670u, 0x807F0678u, 0x807F068Cu, 0x807F0694u, 0x807F069Cu,
                    0x807F06A4u, 0x807F06ACu, 0x807F06B4u, 0x807F06BCu};

// Outside 3-12 the cat is on ground it does not know: its code keeps it on
// the loop's flat floor, at a fixed height on the screen (the controller
// rests at y -36, its feet 10 px lower). So for its update the cat, its
// parts and its rider are moved down by "lift", what puts the ground under
// it at that floor, and back up after: the cat runs on hills, steps and
// blocks. The lift follows the ground up no faster than kCatRise (it steps
// up a tall step over a few frames: snapped up 22 px at once, the cat and
// its rider jumped) and down no faster than a fall, so the cat drops off
// ledges instead of sinking through them. Not
// while it rides a missile (the missile carries it, in the same terms).
constexpr int kCatRest = -36 - 10;    // the loop's floor, under the controller at rest
constexpr int kCatFeet = 10;          // feet below the controller
constexpr int kCatFall = 8;           // px a frame the floor may drop
constexpr int kCatRise = 8;           // px a frame the floor may rise
constexpr int kGroundSearch = 240;    // px searched below the feet
constexpr int kPitY = -300;           // camera-relative: far below the screen (it shows about -120..120)
// The cat's own body (its controller's box, +-16 around it; the rider sits
// above it and is not the cat's to fit): what it steps up onto and what
// stops it. kCatStep: how far above its feet ground still counts as ground,
// walking (a house's 18 px floor step in 1-1) and landing from a jump
// alike (its body over a ledge's edge, feet up to that far below the top).
// Anything taller is a wall, up to its body's top; above that only the
// rider is in the way, and she passes (a 50 px door with the cat's 26 px).
constexpr int kCatStep = 22;          // px above the feet
constexpr int kCatTop = 26;           // px above the feet: the body's top
constexpr int kCatFront = 16;         // px ahead of its middle: the body's front
constexpr int kRiderTop = 44;         // px above the feet: the rider's head (she sits ~19 px over the controller)
// Across, 3-12's camera scrolls by 10 px a frame and the cat runs at 10 px
// (13 holding right, 7 left), so it stays on the screen. Elsewhere the
// camera follows its rider: the cat ran off to the right with it. So
// outside 3-12 its code sees it run at the loop's speed, and what moves it
// is its rider's left / right (kCatSpeed px a frame either way, reached
// kCatAccel a frame; it stands without), up to walls (cat_blocked). For its update it is also put at
// its spot on the screen (x 33), as the loop keeps it, and what it moves
// from there is how far it goes.
constexpr int32_t kLoopScroll = 10 << 16;
constexpr int32_t kCatSpeed = 6 << 16;
constexpr int32_t kCatAccel = 1 << 15;
constexpr uint32_t kHold = 0x800BE4F8u;             // u16: the rider's buttons held (her update's)
constexpr uint32_t kRightMask = 0x800BE510u, kLeftMask = 0x800BE50Cu;   // u16: what counts as right / left
constexpr int kCatHomeX = 33;
constexpr const Rider* kRiders[] = {&kP1, &kP2};

gpr addr(uint32_t vaddr) { return static_cast<gpr>(static_cast<int32_t>(vaddr)); }
gpr slot_addr(int slot) { return addr(kActors + static_cast<uint32_t>(slot) * kStride); }
uint8_t* slot_ptr(uint8_t* rdram, int slot) { return rdram + (kActors - 0x80000000u) + static_cast<uint32_t>(slot) * kStride; }

bool gameplay(uint8_t* rdram) {
    const uint16_t s = MEM_HU(0, addr(kGameState));
    return s == 6 || s == 10;
}

// The stage's own mount (its code answers to the slot the stage put it in).
const Mount* stage_mount(uint8_t* rdram) {
    for (const Mount& m : kMounts) {
        if (MEM_W(0x80, slot_addr(m.slot)) != 0 && MEM_HU(0xD2, slot_addr(m.slot)) == m.type) return &m;
    }
    return nullptr;
}

const Mount* mount_of_type(uint16_t type) {
    for (const Mount& m : kMounts) {
        if (m.type == type) return &m;
    }
    return nullptr;
}

bool cheat_on() {
    return mm::cheats::g_active_for_hooks() && mm::cheats::enabled(mm::cheats::Cheat::CatRide);
}

// The mount a rider should have now (nullptr: none).
const Mount* wanted(uint8_t* rdram, const Rider& r) {
    if (!gameplay(rdram)) return nullptr;
    const Mount* stage = stage_mount(rdram);
    if (r.p2) {
        if (mm::player2::slot(rdram) <= 0 || !mm::cheats::coop_enabled() || mm::player2::health::is_down(rdram)) return nullptr;
        if (stage != nullptr) return stage;
    } else if (stage != nullptr) {
        return nullptr;   // player 1 rides the stage's own
    }
    if (!cheat_on() || !mm::foreign_actors::known(kCheatMount.type)) return nullptr;
    return &kCheatMount;
}

int character_slot(uint8_t* rdram, const Rider& r) {
    return r.p2 ? mm::player2::slot(rdram) : 0;
}

uint32_t requested(uint8_t* rdram, const Rider& r) {
    return r.p2 ? mm::player2::requested(rdram) : static_cast<uint32_t>(MEM_W(0, addr(kMarinaRequest)));
}

void request(uint8_t* rdram, const Rider& r, uint32_t word) {
    if (r.p2) {
        mm::player2::request(rdram, word);
    } else {
        MEM_W(0, addr(kMarinaRequest)) = static_cast<int32_t>(word);
    }
}

const Rider* rider_of(uint16_t type) {
    for (const Rider* r : kRiders) {
        if (type == mm::custom_actors::type_id(r->custom)) return r;
    }
    return nullptr;
}

// The records' state (+0x80 on; 0x00-0x7F are the renderer's matrices).
void copy_records(uint8_t* rdram, int to, int from, int count) {
    for (int i = 0; i < count; ++i) std::memcpy(slot_ptr(rdram, to + i) + 0x80, slot_ptr(rdram, from + i) + 0x80, kStride - 0x80);
}

void swap_bytes(uint8_t* rdram, uint32_t a, uint32_t b, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t t = MEM_BU(i, addr(a));
        MEM_B(i, addr(a)) = static_cast<int8_t>(MEM_BU(i, addr(b)));
        MEM_B(i, addr(b)) = static_cast<int8_t>(t);
    }
}

// Her ride is over: the missile is told its rider is gone, as the cat's own
// code tells it when hit while riding. (A ridden missile otherwise stays
// ridden: it went on writing her message, and her cat, back on its feet
// elsewhere, took every jump for a landing on it: no jumps, and a missile
// at each press.)
void let_go(uint8_t* rdram, const Rider& r) {
    if (r.missile == 0) return;
    const int missile = MEM_W(0, addr(r.missile));
    MEM_W(0, addr(r.missile)) = 0;
    if (missile <= 0 || missile >= 0xD0) return;
    const gpr m = slot_addr(missile);
    if (MEM_W(0x80, m) == 0 || MEM_HU(0xD2, m) != kMissile || MEM_HU(0xD0, m) != kMissileRidden) return;
    MEM_W(0x98, m) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, m)) | kMissileLetGo);
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] her cat leaves missile %d\n", missile);
}

// Her mount is gone. Still on it (the cheat went off, the cat was taken
// away): she falls from where she sat, as from a jump, instead of sitting on
// nothing. And what the cat set on her when she got on goes (its first
// update, func_801A23C4's state 0): with the loop's mark in her player state
// her own update carried her on with the loop's 10 px a frame, cat or not.
void dismount(uint8_t* rdram, const Rider& r) {
    auto get_off = [](uint8_t* rdram) {
        if ((MEM_HU(0xD0, slot_addr(0)) & 0xFF) != kRideState) return;
        MEM_H(0xD0, slot_addr(0)) = static_cast<int16_t>(kFallState);
        MEM_H(0x94, slot_addr(0)) = static_cast<int16_t>(MEM_HU(0x94, slot_addr(0)) & ~kOnCat);
        MEM_W(0, addr(kPlayerState)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0, addr(kPlayerState))) & ~kOnLoop);
    };
    if (r.p2) {
        mm::player2::as_marina(rdram, get_off);
    } else {
        get_off(rdram);
    }
    MEM_W(0, addr(r.mount)) = 0;
    MEM_W(0, addr(r.type)) = 0;
    if (r.globals != 0) {
        for (uint32_t i = 0; i < kCatGlobalsSize; ++i) MEM_B(i, addr(r.globals)) = 0;
    }
}

// The copy's slots that are still the mount's: its controller (the rider's
// custom type), and its part sprites: type 0x0021 (the cat's 13 and the
// bee's 23 alike) and near the controller (they sit within ~20 px of it).
// The stage's own objects may have taken some of the run since, as 3-6's
// do every few seconds, and 3-6's many-part creature has 0x0021 parts too:
// those stay.
constexpr uint16_t kPartType = 0x0021;
constexpr int kPartReach = 96;   // px from the controller
// (cx, cy: the controller's position, taken before any slot is cleared.)
bool mounts_record(uint8_t* rdram, const Rider& r, int first, int s, int cx, int cy) {
    const gpr a = slot_addr(s);
    const uint16_t type = MEM_HU(0xD2, a);
    if (s == first) return type == mm::custom_actors::type_id(r.custom);
    if (MEM_W(0x80, a) == 0 || type != kPartType) return false;
    return std::abs(MEM_H(0x88, a) - cx) <= kPartReach && std::abs(MEM_H(0x8C, a) - cy) <= kPartReach;
}

void remove(uint8_t* rdram, const Rider& r, const Mount& m, int first) {
    let_go(rdram, r);   // (else her missile went on writing player 1's message)
    const int cx = MEM_H(0x88, slot_addr(first)), cy = MEM_H(0x8C, slot_addr(first));
    for (int i = first; i < first + m.span; ++i) {
        if (!mounts_record(rdram, r, first, i, cx, cy)) continue;
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(i)) = 0;
    }
    dismount(rdram, r);
}

void spawn(uint8_t* rdram, const Rider& r, const Mount& m) {
    // Below 0x90 like the stage's own, so the parts take part in collisions
    // as its parts do; else anywhere below the particles (0xC0), where a
    // busy stage has room.
    // Not over the stage slots its update borrows (m.slot on): there the
    // copy in and out of its own records overlapped and mixed them (her cat
    // in 99..112 crashed on its first update).
    auto run_below = [rdram, &m](int last) {
        for (;;) {
            const int first = mm::actors::free_run(rdram, m.span, last);
            if (first < 0 || first + m.span <= m.slot || first >= m.slot + m.span) return first;
            last = first + m.span - 2;   // the next run down
        }
    };
    // Once the stage took the low run for its own objects (3-6: they
    // overwrote her cat every few seconds, and a new one each time jolted
    // her about), above it.
    const bool high = MEM_W(0, addr(r.high)) != 0;
    int first = run_below(high ? 0xBF : 0x8F);
    if (first < 0) first = run_below(high ? 0x8F : 0xBF);
    if (first < 0) return;
    // A new one at the rider's feet; its first update (state 0, run as the
    // stage's slot) builds the parts after it and takes the rider up, as
    // the stage's did for Marina.
    const int c = character_slot(rdram, r);
    // Seated from here: whatever she was flying with (a throw) goes, or it
    // carried her off her new seat for a few frames.
    MEM_W(0xEC, slot_addr(c)) = 0;
    MEM_W(0xF0, slot_addr(c)) = 0;
    const int dx = (MEM_W(0x88, slot_addr(c)) - MEM_W(0x88, slot_addr(0))) >> 16;
    const int dy = (MEM_W(0x8C, slot_addr(c)) - MEM_W(0x8C, slot_addr(0))) >> 16;
    mm::actors::spawn_in(rdram, first, m.type, dx, dy);
    MEM_H(0xD2, slot_addr(first)) = static_cast<int16_t>(mm::custom_actors::type_id(r.custom));
    MEM_W(0, addr(r.mount)) = first;
    MEM_W(0, addr(r.type)) = m.type;
    // Its floor starts where it is made, under her (not on the loop's: a
    // cat made under a player in the air snapped down to the ground at once,
    // 86 px in 4-11), and drops from there (CatOnLoop::enter).
    MEM_W(0, addr(r.lift)) = (MEM_H(0x8C, slot_addr(c)) - kCatFeet) - kCatRest;
    MEM_W(0, addr(r.facing)) = 0;
    MEM_W(0, addr(r.pace)) = 0;
    if (m.request != 0) request(rdram, r, m.request);   // and she gets on (her riding state), as Marina was told to
    if (!mm::faraway::in_shadow()) {
        std::fprintf(stderr, "[mount] %s's own %s (slots %d..%d)\n", r.p2 ? "player 2" : "player 1", m.name, first, first + m.span - 1);
    }
}

bool terrain_solid(uint8_t* rdram, int x, int y) {
    recomp_context ctx{};
    ctx.r29 = addr(0x8045FF00u);   // the host's scratch stack
    ctx.r4 = static_cast<gpr>(static_cast<int16_t>(x));
    ctx.r5 = static_cast<gpr>(static_cast<int16_t>(y));
    func_80012AB4(rdram, &ctx);
    return (ctx.r2 & 0xC0) != 0;
}

// The top of the ground under a point (camera-relative px, +y up), or
// kNoGround: terrain, or the top of an object players stand on (solid or
// platform boxes of slots 0x00-0x8F, as func_8001F88C lists them; 2-11's
// arena floor is one, and the cats fell through it). Not the cats' own
// records: the one updating (in the slots it borrows) and each rider's.
constexpr int kNoGround = -100000;
constexpr uint32_t kSolidBox = 0x00040000u, kPlatformBox = 0x00002000u;
bool cats_record(uint8_t* rdram, int s, const Mount& m) {
    if (s >= m.slot && s < m.slot + m.span) return true;
    for (const Rider* r : kRiders) {
        const int first = MEM_W(0, addr(r->mount));
        if (first > 0 && s >= first && s < first + m.span) return true;
    }
    return false;
}

int ground_under(uint8_t* rdram, int x, int y, const Mount& m) {
    int best = kNoGround;
    for (int px : {x - 12, x, x + 12}) {
        for (int py = y + kCatStep; py >= y - kGroundSearch; --py) {
            if (terrain_solid(rdram, px, py)) {
                best = std::max(best, py + 1);
                break;
            }
        }
    }
    for (int s = 1; s < 0x90; ++s) {
        const gpr o = slot_addr(s);
        if ((static_cast<uint32_t>(MEM_W(0x80, o)) & (kSolidBox | kPlatformBox)) == 0 || cats_record(rdram, s, m)) continue;
        const int ox = MEM_H(0x88, o), top = MEM_H(0x8C, o) + MEM_H(0xAE, o);
        if (x + 12 < ox + MEM_H(0xAA, o) || x - 12 > ox + MEM_H(0xAC, o)) continue;
        if (top <= y + kCatStep && top >= y - kGroundSearch) best = std::max(best, top);
    }
    return best;
}

// Ground or a wall in the cat's way: its front (kCatFront px ahead) from
// above its feet to its back, camera-relative px. Or, on a shared screen,
// its edge (kCatEdge from the middle; the camera stops at a stage's ends).
// (Asked before a cat's update: it runs in the slots it borrows, player 2's
// record among them (3-12's 96..109), and far_coop, not finding her there,
// would say the screen is shared.)
constexpr int kCatEdge = 140;
bool shared_screen(uint8_t* rdram) {
    return !mm::faraway::active(rdram);
}

bool cat_blocked(uint8_t* rdram, int x, int feet, int dir, bool shared) {
    if (shared && dir * x > kCatEdge) return true;
    for (int y = feet + kCatStep + 1; y <= feet + kCatTop; y += 2) {
        if (terrain_solid(rdram, x + dir * kCatFront, y)) return true;
    }
    return false;
}

// 3-12's cat only ever runs right. Running left outside 3-12 it is shown
// turned round: its parts (the slots after the controller) mirrored about
// its seat (the first part, where the rider sits: she stays put) and their
// pictures flipped (+0x80 bit 0x20, as Marina's facing). Undone before its
// update, so its code always sees itself as it left itself.
constexpr uint32_t kFacingLeft = 0x20;
constexpr uint32_t kGrabTarget = 0x1000;   // +0x80: the collision pass offers it to a grab
void mirror_cat(uint8_t* rdram, const Mount& m) {
    const int32_t seat = MEM_W(0x88, slot_addr(m.slot + 1));
    for (int i = 1; i < m.span; ++i) {
        const gpr part = slot_addr(m.slot + i);
        MEM_W(0x88, part) = 2 * seat - MEM_W(0x88, part);
        MEM_W(0x80, part) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, part)) ^ kFacingLeft);
    }
}

// The missile's landing test (func_801A1178, flying state): a cat falling
// (state 0x21 / 0x22) whose box meets the missile's, with no message out.
bool cat_lands(uint8_t* rdram, gpr cat, gpr missile, uint16_t message) {
    if (message != 0) return false;
    const uint16_t state = MEM_HU(0xD0, cat);
    if (state != 0x21 && state != 0x22) return false;
    const int cx = MEM_H(0x88, cat), cy = MEM_H(0x8C, cat);
    const int mx = MEM_H(0x88, missile), my = MEM_H(0x8C, missile);
    return cx + 16 >= mx - 12 && cx - 16 < mx + 12 && cy - 2 >= my - 6 && cy - 14 < my + 6;
}

// Her cat is off its missile: not landing or riding, and away from it.
bool ride_over(uint8_t* rdram, gpr cat, gpr missile) {
    const uint16_t state = MEM_HU(0xD0, cat);
    if (state == kCatLanding || state == kCatRiding) return false;
    const int dx = MEM_H(0x88, cat) - MEM_H(0x88, missile), dy = MEM_H(0x8C, cat) - MEM_H(0x8C, missile);
    return dx > kRideReach || dx < -kRideReach || dy > kRideReach || dy < -kRideReach;
}

// A missile's update with a copied cat in it (missile_begin .. missile_end).
struct MissileCall {
    bool on = false;
    int slot = 0;
    bool hers = false;          // player 2's message swapped in
    bool presented = false;     // a copied cat stood in for slot 96's
    bool landing_p2 = false;
    uint32_t message = 0;       // where the message lives now
    uint16_t p1_message = 0;
    uint16_t cat_state = 0;     // slot 96's, put back after
    int32_t cat_x = 0, cat_y = 0;
    int boarded = 0;            // the missile being boarded (the mount button)
    int rider = 0;              // and its rider's slot
    const Rider* who = nullptr; // and the rider
};
MissileCall g_call;

// --- The mount button (kMountButton) ---------------------------------

uint16_t rider_press(uint8_t* rdram, const Rider& r) {
    return r.p2 ? mm::player2::pressed_now(rdram) : MEM_HU(0, addr(kMarinaPress));
}

// The missile in `state` nearest to a point (camera-relative px) within
// kBoardReach, or 0. A flying one about to blow up does not count.
int nearest_missile(uint8_t* rdram, int x, int y, uint16_t state) {
    int best = 0, best_d = kBoardReach + 1;
    for (int s = 1; s < 0xD0; ++s) {
        const gpr m = slot_addr(s);
        if (MEM_W(0x80, m) == 0 || MEM_HU(0xD2, m) != kMissile || MEM_HU(0xD0, m) != state) continue;
        if (state == kMissileFlying && (static_cast<uint32_t>(MEM_W(0x98, m)) & kMissileHit) != 0) continue;
        const int d = std::max(std::abs(MEM_H(0x88, m) - x), std::abs(MEM_H(0x8C, m) - y));
        if (d < best_d) {
            best = s;
            best_d = d;
        }
    }
    return best;
}

const char* rider_name(const Rider& r) { return r.p2 ? "player 2" : "player 1"; }

// Pressed with the rider's cat (a copy outside 3-12) in its slots from
// `first`: onto the nearest flying missile, or off the one it rides.
// Onto a flying missile: the cat, its parts and its rider just above it,
// the cat falling, where 3-12's loop brings it before its landing test.
void board(uint8_t* rdram, const Rider& r, const Mount& m, int first, int ms) {
    const gpr cat = slot_addr(first);
    const gpr mis = slot_addr(ms);
    const int32_t dx = MEM_W(0x88, mis) - MEM_W(0x88, cat);
    const int32_t dy = MEM_W(0x8C, mis) + (kBoardAbove << 16) - MEM_W(0x8C, cat);
    for (int i = first; i < first + m.span; ++i) {
        MEM_W(0x88, slot_addr(i)) += dx;
        MEM_W(0x8C, slot_addr(i)) += dy;
    }
    const gpr rider = slot_addr(character_slot(rdram, r));
    MEM_W(0x88, rider) += dx;
    MEM_W(0x8C, rider) += dy;
    MEM_H(0xD0, cat) = static_cast<int16_t>(kCatFalling);
    MEM_W(0xF0, cat) = 0;
    MEM_W(0x98, mis) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, mis)) & ~kMissileHitBits);
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] %s's cat boards missile %d\n", rider_name(r), ms);
}

// A missile of the cat's own, over it (Lunar's, 3-12's code brought in
// as the Spawn page does): made waiting on its launcher with the wait
// already over (its +0x114 timer below zero), so its first update launches
// it (dispatch keeps it where it was made); the cat boards it the frame
// after, when it flies (boarding_tick). Or 0: no slot for it.
int make_missile(uint8_t* rdram, const Rider& r, int first) {
    const gpr cat = slot_addr(first);
    const gpr marina = slot_addr(0);
    const int dx = MEM_H(0x88, cat) - MEM_H(0x88, marina);
    const int dy = MEM_H(0x8C, cat) - MEM_H(0x8C, marina) + kCallAbove;
    const int ms = mm::actors::spawn_now(rdram, kMissile, dx, dy);
    if (ms <= 0) return 0;
    MEM_W(0x114, slot_addr(ms)) = static_cast<int32_t>(0xBF800000u);   // -1.0f
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] %s's cat calls missile %d\n", rider_name(r), ms);
    return ms;
}

void mount_button(uint8_t* rdram, const Rider& r, const Mount& m, int first) {
    const gpr cat = slot_addr(first);
    const uint16_t state = MEM_HU(0xD0, cat);
    const int cx = MEM_H(0x88, cat), cy = MEM_H(0x8C, cat);
    if (state == kCatLanding || state == kCatRiding) {
        // Off: the missile is told its rider is gone, as the cat's own
        // code tells it when hit while riding; the cat drops off it.
        const int ms = nearest_missile(rdram, cx, cy, kMissileRidden);
        if (ms > 0) {
            MEM_W(0x98, slot_addr(ms)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, slot_addr(ms))) | kMissileLetGo);
            if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] %s's cat gets off missile %d\n", rider_name(r), ms);
        }
        if (r.missile != 0) MEM_W(0, addr(r.missile)) = 0;
        // And it falls from there (left in its running state it stood on
        // the air at the missile's height for good).
        MEM_H(0xD0, cat) = static_cast<int16_t>(kCatFalling);
        MEM_W(0xF0, cat) = 0;
        MEM_W(0, addr(r.boarding)) = 0;
        MEM_W(0, addr(r.boarding_left)) = 0;
        return;
    }
    if (MEM_W(0, addr(r.boarding_left)) > 0) return;   // one on its way
    const gpr rider = slot_addr(character_slot(rdram, r));
    // On: a flying missile near, else one of the cat's own.
    int ms = nearest_missile(rdram, cx, cy, kMissileFlying);
    if (ms > 0) {
        board(rdram, r, m, first, ms);
    } else {
        ms = make_missile(rdram, r, first);
        if (ms <= 0) return;
    }
    MEM_W(0, addr(r.boarding)) = ms;
    MEM_W(0, addr(r.boarding_left)) = kBoardFrames;
    MEM_W(0, addr(r.boarding_health)) = MEM_H(0xE0, rider);
}

// Once a frame: the boarding window runs out, or ends with the landing.
void boarding_tick(uint8_t* rdram, const Rider& r) {
    const int left = MEM_W(0, addr(r.boarding_left));
    if (left <= 0) return;
    // Touching the missile on the way down hurt her (her hit bits are
    // cleared as the missile updates, see missile_end; the damage was
    // already done): she has what she had when she pressed.
    const gpr rider = slot_addr(character_slot(rdram, r));
    const int32_t health = MEM_W(0, addr(r.boarding_health));
    if (MEM_H(0xE0, rider) < health) MEM_H(0xE0, rider) = static_cast<int16_t>(health);
    const int ms = MEM_W(0, addr(r.boarding));
    const gpr mis = ms > 0 && ms < 0xD0 ? slot_addr(ms) : 0;
    const uint16_t mstate = mis != 0 && MEM_HU(0xD2, mis) == kMissile ? MEM_HU(0xD0, mis) : 0xFFFF;
    // A called missile has launched: onto it (the cat is not yet falling
    // onto it, nor on it).
    const int first = MEM_W(0, addr(r.mount));
    const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(r.type))));
    if (mstate == kMissileFlying && first > 0 && m != nullptr && m->type == kCat) {
        const uint16_t cstate = MEM_HU(0xD0, slot_addr(first));
        if (cstate != kCatFalling && cstate != kCatLanding && cstate != kCatRiding) board(rdram, r, *m, first, ms);
    }
    const bool landed = mstate == kMissileRidden;
    MEM_W(0, addr(r.boarding_left)) = landed ? 0 : left - 1;
    if (landed || left - 1 <= 0) MEM_W(0, addr(r.boarding)) = 0;
}

void present(uint8_t* rdram, int cat_slot) {
    const gpr slot96 = slot_addr(kCheatMount.slot);
    const gpr mine = slot_addr(cat_slot);
    g_call.presented = true;
    g_call.cat_state = MEM_HU(0xD0, slot96);
    g_call.cat_x = MEM_W(0x88, slot96);
    g_call.cat_y = MEM_W(0x8C, slot96);
    MEM_H(0xD0, slot96) = MEM_H(0xD0, mine);
    MEM_W(0x88, slot96) = MEM_W(0x88, mine);
    MEM_W(0x8C, slot96) = MEM_W(0x8C, mine);
}

} // namespace

void missile_end(uint8_t* rdram) {
    // Being boarded: the rider touching the missile on the way down was
    // hit (her hit bits: the cat threw her off) and hurt (see boarding_tick).
    if (g_call.boarded != 0) {
        const gpr rider = slot_addr(g_call.rider);
        MEM_W(0x98, rider) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, rider)) & ~kRiderStruck);
        // And her health, here, before the frame is drawn (put back only
        // next frame it showed a dip for one frame).
        if (g_call.who != nullptr) {
            const int32_t health = MEM_W(0, addr(g_call.who->boarding_health));
            if (MEM_H(0xE0, rider) < health) MEM_H(0xE0, rider) = static_cast<int16_t>(health);
        }
        g_call.boarded = 0;
    }
    if (!g_call.on) return;
    g_call.on = false;
    if (g_call.hers) {
        const gpr msg = addr(g_call.message);
        const gpr hers = addr(kP2.globals + (kMissileMessage - kCatGlobals));
        MEM_H(0, hers) = MEM_H(0, msg);
        MEM_H(0, msg) = static_cast<int16_t>(g_call.p1_message);
    }
    if (g_call.presented) {
        const gpr cat = slot_addr(kCheatMount.slot);
        MEM_H(0xD0, cat) = static_cast<int16_t>(g_call.cat_state);
        MEM_W(0x88, cat) = g_call.cat_x;
        MEM_W(0x8C, cat) = g_call.cat_y;
    }
    if (g_call.landing_p2 && MEM_HU(0xD0, slot_addr(g_call.slot)) == kMissileRidden) {
        MEM_W(0, addr(kP2.missile)) = g_call.slot;
        if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] her cat lands on missile %d\n", g_call.slot);
    }
}

void missile_begin(uint8_t* rdram, int actor) {
    missile_end(rdram);
    if (actor <= 0 || actor >= 0xD0) return;
    const gpr m = slot_addr(actor);
    if (MEM_W(0x80, m) == 0 || MEM_HU(0xD2, m) != kMissile) return;
    g_call = {};
    // Being boarded (the mount button): the cat or its rider touching it
    // on the way down does not blow it up.
    for (const Rider* r : kRiders) {
        if (MEM_W(0, addr(r->boarding)) == actor && MEM_W(0, addr(r->boarding_left)) > 0 && MEM_HU(0xD0, m) == kMissileFlying) {
            MEM_W(0x98, m) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, m)) & ~kMissileHitBits);
            g_call.boarded = actor;
            g_call.who = r;
            g_call.rider = character_slot(rdram, *r);
        }
    }
    // Whose cats are copies: player 1's when the stage has no cat (the
    // cheat), player 2's always.
    const bool stage_cat = stage_mount(rdram) != nullptr;
    const int p1_cat = stage_cat ? 0 : MEM_W(0, addr(kP1.mount));
    const int p2_cat = MEM_W(0, addr(kP2.mount)) > 0 && MEM_W(0, addr(kP2.type)) == kCat ? MEM_W(0, addr(kP2.mount)) : 0;
    if (!stage_cat && p1_cat <= 0 && p2_cat <= 0) return;
    const uint32_t message = mm::foreign_actors::live(rdram, kMissile, kMissileMessage);
    const gpr msg = addr(message);
    const gpr hers = addr(kP2.globals + (kMissileMessage - kCatGlobals));
    const uint16_t state = MEM_HU(0xD0, m);
    g_call.slot = actor;
    g_call.message = message;
    if (p2_cat > 0 && MEM_W(0, addr(kP2.missile)) == actor) {
        if (state != kMissileRidden) {   // blown up, or done
            MEM_W(0, addr(kP2.missile)) = 0;
            return;
        }
        if (ride_over(rdram, slot_addr(p2_cat), m)) {
            let_go(rdram, kP2);
            return;
        }
        g_call.on = g_call.hers = true;
    } else {
        if (state != kMissileFlying || (static_cast<uint32_t>(MEM_W(0x98, m)) & kMissileHit) != 0) return;
        // Player 1's cat first, then hers.
        const gpr p1 = stage_cat ? slot_addr(kCheatMount.slot) : (p1_cat > 0 ? slot_addr(p1_cat) : 0);
        if (p1 != 0 && cat_lands(rdram, p1, m, MEM_HU(0, msg))) {
            if (stage_cat) return;   // already in slot 96
            if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] player 1's cat lands on missile %d\n", actor);
            g_call.on = true;
            present(rdram, p1_cat);
            return;
        }
        if (p2_cat <= 0 || !cat_lands(rdram, slot_addr(p2_cat), m, MEM_HU(0, hers))) return;
        if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[mount] her cat lands on missile %d\n", actor);
        g_call.on = g_call.hers = g_call.landing_p2 = true;
        present(rdram, p2_cat);
    }
    g_call.p1_message = MEM_HU(0, msg);
    MEM_H(0, msg) = MEM_H(0, hers);
}

// A cat outside 3-12, around its update: what its code sees is 3-12's
// loop (its floor under it, its spot on the screen, the loop's speed, facing
// right), what it does is turned back into the stage's terms after.
struct CatOnLoop {
    bool shared;   // one screen for both (shared_screen)
    int32_t lift = 0, across = 0, pace = 0;

    void enter(uint8_t* rdram, const Rider& r, const Mount& m) {
        if (MEM_W(0, addr(r.facing)) != 0) mirror_cat(rdram, m);   // (as its code left it)
        const gpr cat = slot_addr(m.slot);
        const uint16_t state = MEM_HU(0xD0, cat);
        const bool on_missile = state == kCatLanding || state == kCatRiding;   // (the missile carries it, in the same terms)
        if (!on_missile) {
            const int32_t prev = MEM_W(0, addr(r.lift));
            const int ground = ground_under(rdram, MEM_H(0x88, cat), MEM_H(0x8C, cat) - kCatFeet, m);
            const int32_t want = ground == kNoGround ? prev - kCatFall : ground - kCatRest;
            lift = std::min(std::max(want, prev - kCatFall), prev + kCatRise);
            // Left behind by the camera (it follows the other player) past
            // the screen's edge: dragged along at the edge, as the shared
            // screen keeps a player on foot (she was at -257 on her cat, and
            // snapped to -180 when she got off). Far apart each has a camera
            // of her own: no shared screen.
            const int x = MEM_H(0x88, cat);
            const int drag = !shared ? 0 : x < -kCatEdge ? -kCatEdge - x : x > kCatEdge ? kCatEdge - x : 0;
            if (drag != 0) {
                move(rdram, m, drag << 16, 0);
                MEM_W(0, addr(kMarinaWorldX)) += drag << 16;
            }
            across = MEM_W(0x88, cat) - (kCatHomeX << 16);
        }
        MEM_W(0, addr(r.lift)) = lift;
        pace = MEM_W(0, addr(r.pace));
        MEM_W(0xEC, cat) = kLoopScroll;
        move(rdram, m, -across, -(lift << 16));
    }

    void leave(uint8_t* rdram, const Rider& r, const Mount& m) {
        const gpr cat = slot_addr(m.slot);
        const uint16_t held = MEM_HU(0, addr(kHold));
        const int32_t want = (held & MEM_HU(0, addr(kRightMask))) ? kCatSpeed : (held & MEM_HU(0, addr(kLeftMask))) ? -kCatSpeed : 0;
        pace = want > pace ? std::min(want, pace + kCatAccel) : std::max(want, pace - kCatAccel);
        const int x = (MEM_W(0x88, cat) + across) >> 16, feet = (MEM_W(0x8C, cat) >> 16) + lift - kCatFeet;
        // A wall stops it this frame, its speed stays: a jump that meets a
        // ledge's face goes on over it once its body clears the top, as
        // Marina's does (it stopped dead and went straight up).
        const bool blocked = pace != 0 && cat_blocked(rdram, x + (pace >> 16), feet, pace > 0 ? 1 : -1, shared);
        MEM_W(0, addr(r.pace)) = pace;
        MEM_W(0xEC, cat) = blocked ? 0 : pace;
        if (want != 0) MEM_W(0, addr(r.facing)) = want < 0 ? 1 : 0;   // (the way last asked)
        move(rdram, m, across, lift << 16);
        if (MEM_W(0, addr(r.facing)) != 0) mirror_cat(rdram, m);
        // A ceiling: nothing is ever over 3-12's loop, so the cat's jump
        // knows none, and it rose through a block house's roof with its
        // rider. Rising into one, they stop just under it and come down.
        if (MEM_W(0xF0, cat) > 0) {
            const int wx = MEM_H(0x88, cat), wfeet = MEM_H(0x8C, cat) - kCatFeet;
            int lowest = 0x7FFFFFFF;
            for (int px : {wx - 8, wx, wx + 8}) {
                for (int py = wfeet + kCatStep + 1; py <= wfeet + kRiderTop; py += 2) {
                    if (terrain_solid(rdram, px, py)) {
                        lowest = std::min(lowest, py);
                        break;
                    }
                }
            }
            if (lowest != 0x7FFFFFFF) {
                const int dy = (lowest - 1 - kRiderTop) - wfeet;
                if (dy < 0) move(rdram, m, 0, dy << 16);
                MEM_W(0xF0, cat) = 0;
            }
        }
        // Far apart her camera follows her: the cat's "carried by the loop"
        // bit would have it scroll by the loop's speed, holding her ~135 px
        // from the other player. On a shared screen the rider's own code
        // needs the bit (without it she jitters on and off the seat).
        if (!shared)
            MEM_W(0, addr(kPlayerState)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0, addr(kPlayerState))) & ~kOnLoop);
    }

    // The cat, its parts and its rider (16.16 px).
    static void move(uint8_t* rdram, const Mount& m, int32_t dx, int32_t dy) {
        if (dx == 0 && dy == 0) return;
        for (int i = 0; i < m.span; ++i) {
            MEM_W(0x88, slot_addr(m.slot + i)) += dx;
            MEM_W(0x8C, slot_addr(m.slot + i)) += dy;
        }
        MEM_W(0x88, slot_addr(0)) += dx;
        MEM_W(0x8C, slot_addr(0)) += dy;
    }
};

void update(uint8_t* rdram, recomp_context* ctx, int slot) {
    const Rider* r = rider_of(MEM_HU(0xD2, slot_addr(slot)));
    const Mount* m = r ? mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(r->type)))) : nullptr;
    if (r == nullptr || m == nullptr || MEM_W(0, addr(r->mount)) != slot) {   // not a rider's after all (a reused slot)
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(slot)) = 0;
        return;
    }
    // Down (player2_health.h), her mount goes: it ran on without a rider
    // and carried her body off the top of the screen, and she fell back
    // from there seconds later. She gets a new one at her feet when she is
    // back (tick). The same when the cheat goes off or the stage changes.
    if (wanted(rdram, *r) != m) {
        remove(rdram, *r, *m, slot);
        return;
    }
    // The game's mount code on the copy in the stage's slots (whatever is
    // in them kept aside meanwhile: the stage's own cat, or other objects
    // where the stage has none), with its package in place (another
    // stage's package for the cheat, foreign_actors.h).
    // Another stage's package (decided before it is swapped in, when it
    // looks loaded).
    const bool foreign = !mm::foreign_actors::native(rdram, m->type);
    const bool shared = shared_screen(rdram);
    mm::foreign_actors::with_package(rdram, m->type, [ctx, m, r, slot, foreign, shared](uint8_t* rdram) {
        // (A call like the actor loop's own: the game's context, $a0 = the
        // slot.) The camera is not the copy's to drive: the stage's mount
        // scrolls it (3-12 keeps one shared camera) or the stage's own
        // camera follows the rider, so what the copy does to the camera's
        // words is put back.
        auto ride = [ctx, m, r, slot, foreign, shared](uint8_t* rdram) {
            std::vector<uint8_t> theirs(static_cast<size_t>(m->span) * (kStride - 0x80));
            for (int i = 0; i < m->span; ++i) std::memcpy(theirs.data() + i * (kStride - 0x80), slot_ptr(rdram, m->slot + i) + 0x80, kStride - 0x80);
            copy_records(rdram, m->slot, slot, m->span);
            MEM_H(0xD2, slot_addr(m->slot)) = static_cast<int16_t>(m->type);
            // Outside its stage the cat is run as if on 3-12's loop (CatOnLoop).
            CatOnLoop loop{shared};
            const bool on_loop = m->type == kCat && foreign;
            if (on_loop) loop.enter(rdram, *r, *m);
            const bool own_globals = r->globals != 0 && m->type == kCat;
            if (own_globals) swap_bytes(rdram, kCatGlobals, r->globals, kCatGlobalsSize);   // her cat's own state
            uint8_t camera[kCameraSize];
            std::memcpy(camera, rdram + (kCamera - 0x80000000u), kCameraSize);
            const gpr a0 = ctx->r4;
            ctx->r4 = static_cast<gpr>(m->slot);
            // Its first update builds it, unless the rider was just hit
            // (player-state bits the stage's cat checks first: it goes
            // straight to throwing her off, unbuilt, and crashed; a state
            // loaded on such a frame did it). Built first, it sees them next
            // frame.
            const bool building = m->type == kCat && MEM_HU(0xD0, slot_addr(m->slot)) == 0;
            const uint32_t hit = static_cast<uint32_t>(MEM_W(0, addr(kRiderHit))) & kRiderHitBits;
            if (building) MEM_W(0, addr(kRiderHit)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0, addr(kRiderHit))) & ~kRiderHitBits);
            func_80016E70(rdram, ctx);
            if (building) MEM_W(0, addr(kRiderHit)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0, addr(kRiderHit))) | hit);
            ctx->r4 = a0;
            // The cat moves its rider on the screen (func_801A22AC) and
            // pushes her world position on by the loop's scroll (10 px a
            // frame: the camera follows her, so 3-12 scrolls), and so does
            // her own update while the cat's "on the loop" mark is on her.
            // Elsewhere her world position is where she sits (the camera
            // now loaded plus her place on the screen, as func_8004ED10
            // has it), or the camera ran away to the right (far apart, her
            // own camera, holding her in its walls).
            // (Once she is back in her place: the loop pinned her cat.)
            std::memcpy(rdram + (kCamera - 0x80000000u), camera, kCameraSize);
            if (own_globals) swap_bytes(rdram, kCatGlobals, r->globals, kCatGlobalsSize);
            if (on_loop) loop.leave(rdram, *r, *m);
            if (foreign) {
                MEM_W(0, addr(kMarinaWorldX)) = MEM_W(0x88, slot_addr(0)) + MEM_W(0, addr(kCameraX));
                MEM_W(0, addr(kMarinaWorldY)) = MEM_W(0x8C, slot_addr(0)) + MEM_W(0, addr(kCameraY));
            }
            copy_records(rdram, slot, m->slot, m->span);
            MEM_H(0xD2, slot_addr(slot)) = static_cast<int16_t>(mm::custom_actors::type_id(r->custom));
            // Not to be picked up: the collision pass (func_8001EC1C) takes
            // any actor with +0x80 bit 0x1000 and a hit box as a thing to
            // grab, and the cat's parts have the bit (in 3-12 no one is on
            // foot to reach them). A player who grabbed one was pulled
            // between her hands and the cat's animation every frame.
            for (int i = 0; i < m->span; ++i)
                MEM_W(0x80, slot_addr(slot + i)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, slot_addr(slot + i))) & ~kGrabTarget);
            for (int i = 0; i < m->span; ++i) std::memcpy(slot_ptr(rdram, m->slot + i) + 0x80, theirs.data() + i * (kStride - 0x80), kStride - 0x80);
        };
        // Player 2 stands in for Marina first (her own record may be one of
        // the slots the copy borrows: elsewhere than 3-12 they are anyone's).
        if (r->p2) {
            mm::player2::as_marina(rdram, ride);
        } else {
            ride(rdram);
        }
    });
}

// A camera-relative y on the rider's own screen (far apart player 2 has a
// camera of her own).
int view_y(uint8_t* rdram, const Rider& r, int y) {
    int32_t dx = 0, dy = 0;
    if (r.p2 && mm::faraway::active(rdram) && mm::faraway::p2_camera_offset(rdram, dx, dy)) return y - (dy >> 16);
    return y;
}

bool riding(uint8_t* rdram) {
    return stage_mount(rdram) != nullptr || MEM_W(0, addr(kP1.mount)) > 0;
}

void tick(uint8_t* rdram) {
    for (const Rider* r : kRiders) {
        const int have = MEM_W(0, addr(r->mount));
        const Mount* want = wanted(rdram, *r);
        if (have > 0) {
            const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(r->type))));
            // A new scene cleared the slots under it, or something took it
            // away (its record off, +0x80 clear: player 1's cat went while
            // she threw player 2, and she sat on nothing, carried on at the
            // loop's speed). Not its slots: they may be another's now.
            const bool still = MEM_W(0x80, slot_addr(have)) != 0 &&
                               MEM_HU(0xD2, slot_addr(have)) == mm::custom_actors::type_id(r->custom);
            if (!still) {
                if (have < 0x90 && MEM_W(0x80, slot_addr(have)) != 0) MEM_W(0, addr(r->high)) = 1;   // (an object of the stage's there now)
                dismount(rdram, *r);
                continue;
            }
            if (want != m) {   // its update takes it away; when it does not run (paused), here
                if (m != nullptr) remove(rdram, *r, *m, have);
                continue;
            }
            // Fallen far below the screen (no ground under it: 2-11's lava,
            // where the stage puts a player back on safe ground but the
            // cat fell on for ever with its rider): it goes, and she falls
            // as herself, to whatever the stage does with a fall. A new one
            // when she is back on the screen.
            // (Only a cat away from 3-12, on its own floor; on her own
            // screen: far apart player 2's is not the main camera's.)
            const bool own_floor = m != nullptr && m->type == kCat && !mm::foreign_actors::native(rdram, kCat);
            if (own_floor && view_y(rdram, *r, MEM_H(0x8C, slot_addr(have))) < kPitY) {
                remove(rdram, *r, *m, have);
                continue;
            }
            // Put down after a carry the rider is in the fall state, which
            // only ends on ground (the loop has none under the cats): she
            // fell in place for seconds, unable to jump. Asked to get on
            // again, as when her mount was made.
            const int c = character_slot(rdram, *r);
            if (m != nullptr && m->request != 0 && (MEM_HU(0xD0, slot_addr(c)) & 0xFF) == kFallState && requested(rdram, *r) == 0) {
                request(rdram, *r, m->request);
            }
            // The mount button: onto a missile and off it (a cat away from
            // 3-12; there the loop brings missiles under the stage's cat).
            boarding_tick(rdram, *r);
            if (own_floor && (rider_press(rdram, *r) & kMountButton) != 0) mount_button(rdram, *r, *m, have);
            continue;
        }
        // (Not over nothing: a cat made there fell away again at once, and
        // a new one was made under her each frame.)
        if (want != nullptr) {
            const gpr c = slot_addr(character_slot(rdram, *r));
            const bool own_floor = want->type == kCat && !mm::foreign_actors::native(rdram, kCat);
            if (!own_floor ||
                (view_y(rdram, *r, MEM_H(0x8C, c)) >= kPitY && ground_under(rdram, MEM_H(0x88, c), MEM_H(0x8C, c), *want) != kNoGround)) {
                spawn(rdram, *r, *want);
            }
        }
    }
}

} // namespace mm::player2_mount
