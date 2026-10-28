#include "player2_mount.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "far_coop.h"
#include "player2.h"

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

void remove(uint8_t* rdram, int first, int span) {
    for (int i = first; i < first + span; ++i) {
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(i)) = 0;
    }
    MEM_W(0, addr(kP2Mount)) = 0;
    MEM_W(0, addr(kP2MountType)) = 0;
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

} // namespace

void update(uint8_t* rdram, recomp_context* ctx, int slot) {
    const int p2 = mm::player2::slot(rdram);
    const Mount* m = mount_of_type(static_cast<uint16_t>(MEM_W(0, addr(kP2MountType))));
    if (m == nullptr || MEM_W(0, addr(kP2Mount)) != slot) {   // not hers after all (a reused slot)
        for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, slot_addr(slot)) = 0;
        return;
    }
    if (p2 <= 0 || !mm::cheats::coop_enabled() || stage_mount(rdram) != m) {
        remove(rdram, slot, m->span);
        return;
    }
    // The game's mount code, on her copy in the stage's slots, with player 2
    // as Marina; player 1's mount kept aside meanwhile.
    std::vector<uint8_t> theirs(static_cast<size_t>(m->span) * (kStride - 0x80));
    for (int i = 0; i < m->span; ++i) std::memcpy(theirs.data() + i * (kStride - 0x80), slot_ptr(rdram, m->slot + i) + 0x80, kStride - 0x80);
    copy_records(rdram, m->slot, slot, m->span);
    MEM_H(0xD2, slot_addr(m->slot)) = static_cast<int16_t>(m->type);
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
        if (MEM_HU(0xD2, slot_addr(have)) == mm::custom_actors::type_id(kCustomIndex)) return;
        MEM_W(0, addr(kP2Mount)) = 0;
        MEM_W(0, addr(kP2MountType)) = 0;
    }
    if (const Mount* m = stage_mount(rdram)) spawn(rdram, *m, p2);
}

} // namespace mm::player2_mount
