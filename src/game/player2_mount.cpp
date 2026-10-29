#include "player2_mount.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "far_coop.h"
#include "player2.h"
#include "player2_health.h"

extern "C" void func_80016E70(uint8_t* rdram, recomp_context* ctx);   // banked actor update, $a0 = slot

namespace mm::player2_mount {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
// Player 2's block (player2.cpp): her mount's controller slot (0 = none) and
// the stage type it copies.
constexpr uint32_t kP2Mount = 0x807F0418u;
constexpr uint32_t kP2MountType = 0x807F041Cu;
// The game's camera words (far_coop.cpp's camera set and the look-at
// target after it, 0x800BE5A8..B4, which LookAt_Update reads).
constexpr uint32_t kCamera = 0x800BE544u;
constexpr uint32_t kCameraSize = 0x74u;

// The cat (3-12) keeps state outside its record, in the stage overlay's
// globals 0x801A80E6..0x801A80EB, which any cat code reads and writes. Last
// of them, 0x801A80EA, is a message from Lunar's missiles (type 0x0F09,
// func_801A1178): a missile the cat lands on (its flying state checks the
// cat in slot 96, falling, against its box) or carries (its ridden state,
// every frame) writes its slot there, the cat rides while it reads one and
// clears it at the end of its update (func_801A23C4). Her cat ran the same
// code: it read player 1's message (her cat updates after the missiles) and
// cleared it, so neither cat could ride a missile.
//
// So her cat has its own copy of those globals (in player 2's block),
// swapped in for its update, and the missiles talk to her copy for her: a
// missile she rides (kP2Missile) writes her message, and a flying one that
// player 1's cat is not landing on sees her cat in slot 96's place for its
// update when hers is (cat_lands). All game memory.
constexpr uint32_t kCatGlobals = 0x801A80E6u;
constexpr uint32_t kCatGlobalsSize = 6;
constexpr uint32_t kMissileMessage = 0x801A80EAu;   // u16: the missile's slot, 0 = none
constexpr uint32_t kP2CatGlobals = 0x807F0670u;     // her copy (6 bytes)
constexpr uint32_t kP2Missile = 0x807F0678u;        // s32: the missile her cat rides, 0 = none
constexpr uint16_t kCat = 0x0F01;
constexpr uint16_t kFallState = 0x16;                // Marina's fall (low byte of +0xD0)
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
    {0x0F01, 96, 14, 5, "cat"},   // 3-12 CERBERUS: the cat Marina rides (func_801A23C4; parts in 97..109)
};

gpr addr(uint32_t vaddr) { return static_cast<gpr>(static_cast<int32_t>(vaddr)); }
gpr slot_addr(int slot) { return addr(kActors + static_cast<uint32_t>(slot) * kStride); }
uint8_t* slot_ptr(uint8_t* rdram, int slot) { return rdram + (kActors - 0x80000000u) + static_cast<uint32_t>(slot) * kStride; }

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

// The records' state (+0x80 on; 0x00-0x7F are the renderer's matrices).
void copy_records(uint8_t* rdram, int to, int from, int count) {
    for (int i = 0; i < count; ++i) std::memcpy(slot_ptr(rdram, to + i) + 0x80, slot_ptr(rdram, from + i) + 0x80, kStride - 0x80);
}

void let_go(uint8_t* rdram);

void remove(uint8_t* rdram, int first, int span) {
    let_go(rdram);   // (else her missile went on writing player 1's message)
    for (int i = first; i < first + span; ++i) {
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(i)) = 0;
    }
    MEM_W(0, addr(kP2Mount)) = 0;
    MEM_W(0, addr(kP2MountType)) = 0;
    for (uint32_t i = 0; i < kCatGlobalsSize; ++i) MEM_B(i, addr(kP2CatGlobals)) = 0;
    MEM_W(0, addr(kP2Missile)) = 0;
}

void spawn(uint8_t* rdram, const Mount& m, int p2) {
    // Below 0x90 like the stage's own, so the parts take part in collisions
    // as its parts do.
    const int first = mm::actors::free_run(rdram, m.span, 0x8F);
    if (first < 0) return;
    // A new one at her feet; its first update (state 0, run as the stage's
    // slot) builds the parts after it and takes her up, as the stage's did
    // for Marina.
    const int dx = (MEM_W(0x88, slot_addr(p2)) - MEM_W(0x88, slot_addr(0))) >> 16;
    const int dy = (MEM_W(0x8C, slot_addr(p2)) - MEM_W(0x8C, slot_addr(0))) >> 16;
    mm::actors::spawn_in(rdram, first, m.type, dx, dy);
    MEM_H(0xD2, slot_addr(first)) = static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    MEM_W(0, addr(kP2Mount)) = first;
    MEM_W(0, addr(kP2MountType)) = m.type;
    mm::player2::request(rdram, m.request);   // and she gets on (her riding state), as Marina was told to
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] her own %s (slots %d..%d)\n", m.name, first, first + m.span - 1);
}

void swap_bytes(uint8_t* rdram, uint32_t a, uint32_t b, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t t = MEM_BU(i, addr(a));
        MEM_B(i, addr(a)) = static_cast<int8_t>(MEM_BU(i, addr(b)));
        MEM_B(i, addr(b)) = static_cast<int8_t>(t);
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

// A missile's update with her cat in it (missile_begin .. missile_end).
struct MissileCall {
    bool on = false;
    int slot = 0;
    bool landing = false;       // her cat stood in for slot 96's
    uint16_t p1_message = 0;
    uint16_t cat_state = 0;     // slot 96's, put back after
    int32_t cat_x = 0, cat_y = 0;
};
MissileCall g_call;

// Her ride is over: the missile is told its rider is gone, as the cat's own
// code tells it when hit while riding. (A ridden missile otherwise stays
// ridden: it went on writing her message, and her cat, back on its feet
// elsewhere, took every jump for a landing on it: no jumps, and a missile
// at each press.)
void let_go(uint8_t* rdram) {
    const int missile = MEM_W(0, addr(kP2Missile));
    MEM_W(0, addr(kP2Missile)) = 0;
    if (missile <= 0 || missile >= 0xD0) return;
    const gpr m = slot_addr(missile);
    if (MEM_W(0x80, m) == 0 || MEM_HU(0xD2, m) != kMissile || MEM_HU(0xD0, m) != kMissileRidden) return;
    MEM_W(0x98, m) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, m)) | kMissileLetGo);
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] her cat leaves missile %d\n", missile);
}

// Her cat is off its missile: not landing or riding, and away from it.
bool ride_over(uint8_t* rdram, gpr cat, gpr missile) {
    const uint16_t state = MEM_HU(0xD0, cat);
    if (state == kCatLanding || state == kCatRiding) return false;
    const int dx = MEM_H(0x88, cat) - MEM_H(0x88, missile), dy = MEM_H(0x8C, cat) - MEM_H(0x8C, missile);
    return dx > kRideReach || dx < -kRideReach || dy > kRideReach || dy < -kRideReach;
}

} // namespace

void missile_end(uint8_t* rdram) {
    if (!g_call.on) return;
    g_call.on = false;
    const gpr msg = addr(kMissileMessage);
    const gpr hers = addr(kP2CatGlobals + (kMissileMessage - kCatGlobals));
    MEM_H(0, hers) = MEM_H(0, msg);
    MEM_H(0, msg) = static_cast<int16_t>(g_call.p1_message);
    if (!g_call.landing) return;
    const gpr cat = slot_addr(kMounts[0].slot);
    MEM_H(0xD0, cat) = static_cast<int16_t>(g_call.cat_state);
    MEM_W(0x88, cat) = g_call.cat_x;
    MEM_W(0x8C, cat) = g_call.cat_y;
    if (MEM_HU(0xD0, slot_addr(g_call.slot)) == kMissileRidden) {
        MEM_W(0, addr(kP2Missile)) = g_call.slot;
        if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] her cat lands on missile %d\n", g_call.slot);
    }
}

void missile_begin(uint8_t* rdram, int actor) {
    missile_end(rdram);
    if (actor <= 0 || actor >= 0xD0 || !mm::cheats::coop_enabled()) return;
    const gpr m = slot_addr(actor);
    if (MEM_W(0x80, m) == 0 || MEM_HU(0xD2, m) != kMissile) return;
    const int her_cat = MEM_W(0, addr(kP2Mount));
    const Mount* mount = stage_mount(rdram);
    if (her_cat <= 0 || mount == nullptr || mount->type != kCat || MEM_W(0, addr(kP2MountType)) != kCat) return;
    const gpr msg = addr(kMissileMessage);
    const gpr hers = addr(kP2CatGlobals + (kMissileMessage - kCatGlobals));
    const uint16_t state = MEM_HU(0xD0, m);
    bool landing = false;
    if (MEM_W(0, addr(kP2Missile)) == actor) {
        if (state != kMissileRidden) {   // blown up, or done
            MEM_W(0, addr(kP2Missile)) = 0;
            return;
        }
        if (ride_over(rdram, slot_addr(her_cat), m)) {
            let_go(rdram);
            return;
        }
    } else {
        if (state != kMissileFlying || (static_cast<uint32_t>(MEM_W(0x98, m)) & kMissileHit) != 0) return;
        const gpr cat = slot_addr(mount->slot);
        if (cat_lands(rdram, cat, m, MEM_HU(0, msg))) return;   // player 1's cat first
        if (!cat_lands(rdram, slot_addr(her_cat), m, MEM_HU(0, hers))) return;
        landing = true;
    }
    g_call = {true, actor, landing, MEM_HU(0, msg), 0, 0, 0};
    MEM_H(0, msg) = MEM_H(0, hers);
    if (landing) {
        const gpr cat = slot_addr(mount->slot);
        const gpr mine = slot_addr(her_cat);
        g_call.cat_state = MEM_HU(0xD0, cat);
        g_call.cat_x = MEM_W(0x88, cat);
        g_call.cat_y = MEM_W(0x8C, cat);
        MEM_H(0xD0, cat) = MEM_H(0xD0, mine);
        MEM_W(0x88, cat) = MEM_W(0x88, mine);
        MEM_W(0x8C, cat) = MEM_W(0x8C, mine);
    }
}

void update(uint8_t* rdram, recomp_context* ctx, int slot) {
    const int p2 = mm::player2::slot(rdram);
    const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(kP2MountType))));
    if (m == nullptr || MEM_W(0, addr(kP2Mount)) != slot) {   // not hers after all (a reused slot)
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(slot)) = 0;
        return;
    }
    // Down (player2_health.h), her mount goes: it ran on without a rider
    // and carried her body off the top of the screen, and she fell back
    // from there seconds later. She gets a new one at her feet when she is
    // back (tick).
    if (p2 <= 0 || !mm::cheats::coop_enabled() || stage_mount(rdram) != m || mm::player2::health::is_down(rdram)) {
        remove(rdram, slot, m->span);
        return;
    }
    // The game's mount code, on her copy in the stage's slots, with player 2
    // as Marina; player 1's mount kept aside meanwhile.
    std::vector<uint8_t> theirs(static_cast<size_t>(m->span) * (kStride - 0x80));
    for (int i = 0; i < m->span; ++i) std::memcpy(theirs.data() + i * (kStride - 0x80), slot_ptr(rdram, m->slot + i) + 0x80, kStride - 0x80);
    copy_records(rdram, m->slot, slot, m->span);
    MEM_H(0xD2, slot_addr(m->slot)) = static_cast<int16_t>(m->type);
    const bool cat = m->type == kCat;
    if (cat) swap_bytes(rdram, kCatGlobals, kP2CatGlobals, kCatGlobalsSize);   // her cat's own state
    // (A call like the actor loop's own: the game's context, $a0 = the slot.)
    // The camera is not hers to drive: the stage's mount scrolls it (3-12
    // keeps one shared camera), so what her copy does to the camera's words
    // is put back.
    mm::player2::as_marina(rdram, [ctx, m](uint8_t* r) {
        uint8_t camera[kCameraSize];
        std::memcpy(camera, r + (kCamera - 0x80000000u), kCameraSize);
        ctx->r4 = static_cast<gpr>(m->slot);
        func_80016E70(r, ctx);
        std::memcpy(r + (kCamera - 0x80000000u), camera, kCameraSize);
    });
    if (cat) swap_bytes(rdram, kCatGlobals, kP2CatGlobals, kCatGlobalsSize);
    copy_records(rdram, slot, m->slot, m->span);
    MEM_H(0xD2, slot_addr(slot)) = static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    for (int i = 0; i < m->span; ++i) std::memcpy(slot_ptr(rdram, m->slot + i) + 0x80, theirs.data() + i * (kStride - 0x80), kStride - 0x80);
}

bool riding(uint8_t* rdram) {
    return stage_mount(rdram) != nullptr;
}

void tick(uint8_t* rdram) {
    const int p2 = mm::player2::slot(rdram);
    const int have = MEM_W(0, addr(kP2Mount));
    if (p2 <= 0 || !mm::cheats::coop_enabled()) {
        if (have > 0) {
            const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(kP2MountType))));
            const bool still_hers = MEM_HU(0xD2, slot_addr(have)) == mm::custom_actors::type_id(kCustomIndex);
            remove(rdram, have, still_hers && m ? m->span : 0);
        }
        return;
    }
    if (have > 0) {
        // Her update keeps it or takes it away; a new scene clears the
        // slots under it, and then she may get a new one.
        if (MEM_HU(0xD2, slot_addr(have)) == mm::custom_actors::type_id(kCustomIndex)) {
            // Put down after a carry she is in the fall state, which only
            // ends on ground (the loop has none under the cats): she fell in
            // place for seconds, unable to jump. Asked to get on again, as
            // when her mount was made.
            const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(kP2MountType))));
            if (m != nullptr && (MEM_HU(0xD0, slot_addr(p2)) & 0xFF) == kFallState && mm::player2::requested(rdram) == 0) {
                mm::player2::request(rdram, m->request);
            }
            return;
        }
        MEM_W(0, addr(kP2Mount)) = 0;
        MEM_W(0, addr(kP2MountType)) = 0;
    }
    if (mm::player2::health::is_down(rdram)) return;
    if (const Mount* m = stage_mount(rdram)) spawn(rdram, *m, p2);
}

} // namespace mm::player2_mount
