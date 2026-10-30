#include "player2_mount.h"

#include <algorithm>
#include <cstdio>
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
constexpr uint16_t kFallState = 0x16;                // Marina's fall (low byte of +0xD0)
constexpr uint16_t kRideState = 0x03;                // Marina on a mount
constexpr uint16_t kOnCat = 0x0100;                  // Marina's +0x94: on the cat (set by the cat)
constexpr uint32_t kPlayerState = 0x80137458u;       // Marina's player-state word
constexpr uint32_t kOnLoop = 0x10000;                // ... carried by 3-12's loop (set by the cat)
constexpr uint16_t kMissile = 0x0F09;
constexpr uint16_t kMissileFlying = 1, kMissileRidden = 4;
constexpr uint32_t kMissileHit = 0x200;             // +0x98: hit, about to blow up
constexpr uint32_t kMissileLetGo = 0x1;             // +0x98: its rider is gone (what the cat sets when hit while riding)
constexpr uint16_t kCatLanding = 0x58, kCatRiding = 0x59;
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
};
constexpr Rider kP1{false, kPlayer1CustomIndex, 0x807F0680u, 0x807F0684u, 0, 0, 0x807F0688u};
constexpr Rider kP2{true, kCustomIndex, 0x807F0418u, 0x807F041Cu, 0x807F0670u, 0x807F0678u, 0x807F068Cu};

// Outside 3-12 the cat is on ground it does not know: its code keeps it on
// the loop's flat floor, at a fixed height on the screen (the controller
// rests at y -36, its feet 10 px lower). So for its update the cat, its
// parts and its rider are moved down by "lift", what puts the ground under
// it at that floor, and back up after: the cat runs on hills, steps and
// blocks. The lift follows the ground up at once and down no faster than a
// fall, so the cat drops off ledges instead of sinking through them. Not
// while it rides a missile (the missile carries it, in the same terms).
constexpr int kCatRest = -36 - 10;    // the loop's floor, under the controller at rest
constexpr int kCatFeet = 10;          // feet below the controller
constexpr int kCatFall = 8;           // px a frame the floor may drop
constexpr int kGroundSearch = 240;    // px searched below the feet
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

void remove(uint8_t* rdram, const Rider& r, int first, int span) {
    let_go(rdram, r);   // (else her missile went on writing player 1's message)
    // Still on it (the cheat went off): she falls from where she sat, as
    // from a jump, instead of sitting on nothing. And what the cat set on
    // her when she got on goes (its first update, func_801A23C4's state 0):
    // with the loop's mark in her player state her own update carried her
    // on with the loop's 10 px a frame, cat or not.
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
    for (int i = first; i < first + span; ++i) {
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(i)) = 0;
    }
    MEM_W(0, addr(r.mount)) = 0;
    MEM_W(0, addr(r.type)) = 0;
    if (r.globals != 0) {
        for (uint32_t i = 0; i < kCatGlobalsSize; ++i) MEM_B(i, addr(r.globals)) = 0;
    }
}

void spawn(uint8_t* rdram, const Rider& r, const Mount& m) {
    // Below 0x90 like the stage's own, so the parts take part in collisions
    // as its parts do; else anywhere below the particles (0xC0), where a
    // busy stage has room.
    int first = mm::actors::free_run(rdram, m.span, 0x8F);
    if (first < 0) first = mm::actors::free_run(rdram, m.span, 0xBF);
    if (first < 0) return;
    // A new one at the rider's feet; its first update (state 0, run as the
    // stage's slot) builds the parts after it and takes the rider up, as
    // the stage's did for Marina.
    const int c = character_slot(rdram, r);
    const int dx = (MEM_W(0x88, slot_addr(c)) - MEM_W(0x88, slot_addr(0))) >> 16;
    const int dy = (MEM_W(0x8C, slot_addr(c)) - MEM_W(0x8C, slot_addr(0))) >> 16;
    mm::actors::spawn_in(rdram, first, m.type, dx, dy);
    MEM_H(0xD2, slot_addr(first)) = static_cast<int16_t>(mm::custom_actors::type_id(r.custom));
    MEM_W(0, addr(r.mount)) = first;
    MEM_W(0, addr(r.type)) = m.type;
    MEM_W(0, addr(r.lift)) = 0;
    request(rdram, r, m.request);   // and she gets on (her riding state), as Marina was told to
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
// kNoGround.
constexpr int kNoGround = -100000;
int ground_under(uint8_t* rdram, int x, int y) {
    int best = kNoGround;
    for (int px : {x - 12, x, x + 12}) {
        for (int py = y + 8; py >= y - kGroundSearch; --py) {
            if (terrain_solid(rdram, px, py)) {
                best = std::max(best, py + 1);
                break;
            }
        }
    }
    return best;
}

// Ground or a wall in the cat's way: its front (kCatFront px ahead) from
// above its feet to its back, camera-relative px. Or the screen's edge
// (kCatEdge from the middle; the camera stops at a stage's ends).
constexpr int kCatFront = 28, kCatStep = 6, kCatBack = 44, kCatEdge = 140;
bool cat_blocked(uint8_t* rdram, int x, int feet, int dir) {
    if (dir * x > kCatEdge) return true;
    for (int y = feet + kCatStep; y <= feet + kCatBack; y += 6) {
        if (terrain_solid(rdram, x + dir * kCatFront, y)) return true;
    }
    return false;
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
};
MissileCall g_call;

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
    g_call = {};
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
            g_call.on = true;
            present(rdram, p1_cat);
            return;
        }
        if (p2_cat <= 0 || !cat_lands(rdram, slot_addr(p2_cat), m, MEM_HU(0, hers))) return;
        g_call.on = g_call.hers = g_call.landing_p2 = true;
        present(rdram, p2_cat);
    }
    g_call.p1_message = MEM_HU(0, msg);
    MEM_H(0, msg) = MEM_H(0, hers);
}

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
        remove(rdram, *r, slot, m->span);
        return;
    }
    // The game's mount code on the copy in the stage's slots (whatever is
    // in them kept aside meanwhile: the stage's own cat, or other objects
    // where the stage has none), with its package in place (another
    // stage's package for the cheat, foreign_actors.h).
    // Another stage's package (decided before it is swapped in, when it
    // looks loaded).
    const bool foreign = !mm::foreign_actors::native(rdram, m->type);
    mm::foreign_actors::with_package(rdram, m->type, [ctx, m, r, slot, foreign](uint8_t* rdram) {
        // (A call like the actor loop's own: the game's context, $a0 = the
        // slot.) The camera is not the copy's to drive: the stage's mount
        // scrolls it (3-12 keeps one shared camera) or the stage's own
        // camera follows the rider, so what the copy does to the camera's
        // words is put back.
        auto ride = [ctx, m, r, slot, foreign](uint8_t* rdram) {
            std::vector<uint8_t> theirs(static_cast<size_t>(m->span) * (kStride - 0x80));
            for (int i = 0; i < m->span; ++i) std::memcpy(theirs.data() + i * (kStride - 0x80), slot_ptr(rdram, m->slot + i) + 0x80, kStride - 0x80);
            copy_records(rdram, m->slot, slot, m->span);
            MEM_H(0xD2, slot_addr(m->slot)) = static_cast<int16_t>(m->type);
            // Outside its stage: the ground under it as its floor (kCatRest).
            int32_t lift = 0, across = 0, pace = 0;
            int ground = 0;
            if (m->type == kCat && foreign) {
                const gpr cat = slot_addr(m->slot);
                const uint16_t state = MEM_HU(0xD0, cat);
                if (state != kCatLanding && state != kCatRiding) {
                    const int32_t prev = MEM_W(0, addr(r->lift));
                    ground = ground_under(rdram, MEM_H(0x88, cat), MEM_H(0x8C, cat) - kCatFeet);
                    const int32_t want = ground == kNoGround ? prev - kCatFall : ground - kCatRest;
                    lift = std::max(want, prev - kCatFall);
                }
                MEM_W(0, addr(r->lift)) = lift;
                pace = MEM_W(0xEC, cat);
                MEM_W(0xEC, cat) = kLoopScroll;
                across = MEM_W(0x88, cat) - (kCatHomeX << 16);
                if (state == kCatLanding || state == kCatRiding) across = 0;
                for (int i = 0; i < m->span; ++i) {
                    MEM_W(0x88, slot_addr(m->slot + i)) -= across;
                    MEM_W(0x8C, slot_addr(m->slot + i)) -= lift << 16;
                }
                MEM_W(0x88, slot_addr(0)) -= across;
                MEM_W(0x8C, slot_addr(0)) -= lift << 16;
            }
            const bool own_globals = r->globals != 0 && m->type == kCat;
            if (own_globals) swap_bytes(rdram, kCatGlobals, r->globals, kCatGlobalsSize);   // her cat's own state
            uint8_t camera[kCameraSize];
            std::memcpy(camera, rdram + (kCamera - 0x80000000u), kCameraSize);
            const int32_t rider_x = MEM_W(0x88, slot_addr(0)), rider_y = MEM_W(0x8C, slot_addr(0));
            const int32_t world_x = MEM_W(0, addr(kMarinaWorldX)), world_y = MEM_W(0, addr(kMarinaWorldY));
            const gpr a0 = ctx->r4;
            ctx->r4 = static_cast<gpr>(m->slot);
            func_80016E70(rdram, ctx);
            ctx->r4 = a0;
            // The cat moves its rider on the screen (func_801A22AC) and
            // pushes her world position on by the loop's scroll (10 px a
            // frame: the camera follows her, so 3-12 scrolls). Elsewhere
            // her world position goes only where the cat took her on the
            // screen, or the camera ran away to the right.
            if (foreign) {
                MEM_W(0, addr(kMarinaWorldX)) = world_x + (MEM_W(0x88, slot_addr(0)) - rider_x);
                MEM_W(0, addr(kMarinaWorldY)) = world_y + (MEM_W(0x8C, slot_addr(0)) - rider_y);
            }
            std::memcpy(rdram + (kCamera - 0x80000000u), camera, kCameraSize);
            if (own_globals) swap_bytes(rdram, kCatGlobals, r->globals, kCatGlobalsSize);
            if (m->type == kCat && foreign) {
                const gpr cat = slot_addr(m->slot);
                const uint16_t held = MEM_HU(0, addr(kHold));
                const int32_t want = (held & MEM_HU(0, addr(kRightMask))) ? kCatSpeed : (held & MEM_HU(0, addr(kLeftMask))) ? -kCatSpeed : 0;
                pace = want > pace ? std::min(want, pace + kCatAccel) : std::max(want, pace - kCatAccel);
                const int x = (MEM_W(0x88, cat) + across) >> 16, feet = (MEM_W(0x8C, cat) >> 16) + lift - kCatFeet;
                if (pace != 0 && cat_blocked(rdram, x + (pace >> 16), feet, pace > 0 ? 1 : -1)) pace = 0;
                MEM_W(0xEC, cat) = pace;
            }
            if (lift != 0 || across != 0) {
                for (int i = 0; i < m->span; ++i) {
                    MEM_W(0x88, slot_addr(m->slot + i)) += across;
                    MEM_W(0x8C, slot_addr(m->slot + i)) += lift << 16;
                }
                MEM_W(0x88, slot_addr(0)) += across;
                MEM_W(0x8C, slot_addr(0)) += lift << 16;
            }
            copy_records(rdram, slot, m->slot, m->span);
            MEM_H(0xD2, slot_addr(slot)) = static_cast<int16_t>(mm::custom_actors::type_id(r->custom));
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

bool riding(uint8_t* rdram) {
    return stage_mount(rdram) != nullptr || MEM_W(0, addr(kP1.mount)) > 0;
}

void tick(uint8_t* rdram) {
    for (const Rider* r : kRiders) {
        const int have = MEM_W(0, addr(r->mount));
        const Mount* want = wanted(rdram, *r);
        if (have > 0) {
            const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(r->type))));
            const bool still = MEM_HU(0xD2, slot_addr(have)) == mm::custom_actors::type_id(r->custom);
            if (!still) {   // a new scene cleared the slots under it
                MEM_W(0, addr(r->mount)) = 0;
                MEM_W(0, addr(r->type)) = 0;
                continue;
            }
            if (want != m) {   // its update takes it away; when it does not run (paused), here
                if (m != nullptr) remove(rdram, *r, have, m->span);
                continue;
            }
            // Put down after a carry the rider is in the fall state, which
            // only ends on ground (the loop has none under the cats): she
            // fell in place for seconds, unable to jump. Asked to get on
            // again, as when her mount was made.
            const int c = character_slot(rdram, *r);
            if (m != nullptr && (MEM_HU(0xD0, slot_addr(c)) & 0xFF) == kFallState && requested(rdram, *r) == 0) {
                request(rdram, *r, m->request);
            }
            continue;
        }
        if (want != nullptr) spawn(rdram, *r, *want);
    }
}

} // namespace mm::player2_mount
