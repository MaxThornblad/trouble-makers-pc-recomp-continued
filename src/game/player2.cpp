#include "player2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>
#include <vector>
#include <cfloat>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "far_coop.h"
#include "netplay.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" void func_8004ED10(uint8_t* rdram, recomp_context* ctx);   // Marina's update(slot)
extern "C" void Trouble_RLE_Type2(uint8_t* rdram, recomp_context* ctx);   // (src, dst, size) unpack

#include "librecomp/game.hpp"

namespace mm::player2 {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint16_t kMarinaType = 0x0016;
constexpr uint32_t kMarinaPaused = 0x800CA230u;
constexpr uint32_t kHoldingFlag = 0x02000000u;   // +0x80: carrying something (slot at +0xD6)   // func_80020024 skips Marina's update while nonzero

// Controller state the game builds in Input_Update (0x80000FE0) and Marina
// copies into her own input words at the start of her update.
constexpr uint32_t kPads = 0x8012AD70u;           // OSContPad[4], 6 bytes each
constexpr uint32_t kHold = 0x800BE4F8u;
constexpr uint32_t kPress = 0x800BE4FCu;

// Marina's own globals: everything her update writes outside her slot that
// is about her (the probe also found sound-effect channels at 0x8016xxxx,
// which are deliberately shared).
struct Range { uint32_t start, size; };
// Marina's input history (func_800122B0, once a frame from GameState_Gameplay:
// the masked hold / press words of the last 64 frames, newest first), which
// her update reads for buffered actions. Player 2 keeps her own.
constexpr uint32_t kHistoryPress = 0x8011DD70u, kHistoryHold = 0x801225F0u;
constexpr uint32_t kHistoryLen = 64;
constexpr std::array<Range, 11> kGlobals{{
    {kHistoryPress, kHistoryLen * 2},
    {kHistoryHold, kHistoryLen * 2},
    {0x800BE5A4u, 4},
    {0x800BE5D8u, 0x18},     // world position pairs read by enemies (0x800BE5D8, 0x800BE5E8)
    {0x800D57E0u, 4},
    {0x800EF4F0u, 0x20},
    {0x80104090u, 4},
    {0x801069D8u, 4},
    {0x801370CCu, 8},        // her hold / press input words and two more
    {0x801373D0u, 0x98},     // player-state block (0x801373D8..0x80137460 seen written)
    {kHold, 8},              // controller words: P2's own while her update runs
}};

constexpr size_t globals_size() {
    size_t n = 0;
    for (const Range& r : kGlobals) n += r.size;
    return n;
}

// Player 2's own copy of those globals and her previous buttons live in
// host-only expansion memory (unused above 0x80480000, docs/README.md 14.1)
// rather than in host variables, so save states, replays and netplay carry
// them with the rest of the game.
// Layout at 0x807F0000 (keep in step): 0x000 globals copy, 0x400 previous
// buttons, 0x404 join timer, 0x408 away timer, 0x40C full health, 0x410 frames
// in her texture buffers (draw), 0x414 grab cooldown, 0x420 frame header,
// 0x1000 display lists, 0x2000 textures, 0x4000 packed frame.
constexpr uint32_t kP2Globals = 0x807F0000u;
constexpr uint32_t kP2PrevButtons = kP2Globals + 0x400u;
constexpr uint32_t kP2JoinTimer = kP2Globals + 0x404u;     // frames P1 has been in play without P2
constexpr uint32_t kP2AwayTimer = kP2Globals + 0x408u;     // frames P2 has been too far from P1
constexpr uint32_t kP2FullHealth = kP2Globals + 0x40Cu;    // the most health either player has had (s16)
constexpr uint32_t kP2GrabCooldown = kP2Globals + 0x414u;  // frames until the players may grab each other again

constexpr uint32_t kGameState = 0x800BE4F0u;   // 6 and 10 = stage gameplay
constexpr uint32_t kCameraX = 0x800BE558u, kCameraY = 0x800BE55Cu;
constexpr uint32_t kWorldX = 0x800BE5D8u, kWorldY = 0x800BE5DCu;   // Marina's world position
constexpr int kJoinDelay = 45;                 // frames into a scene
constexpr int kLeashX = 200, kLeashY = 150;    // px from the camera centre: out of view
constexpr int kLeashFrames = 150;

struct State {
    int slot = -1;        // host-side cache only; slot() checks the slot itself
    bool joined = false;  // a (re)join since the last marker update
};
State g;

struct Marker {
    bool visible = false;
    int x = 0, top = 0, health = 0, max_health = 0;
    // Far apart (far_coop.h): where the other player is, relative to this
    // machine's view (px, +y up), for an arrow at the screen's edge.
    bool partner = false;
    bool partner_is_p1 = false;
    int px = 0, py = 0;
};
std::mutex g_marker_mutex;
Marker g_marker;

uint8_t* at(uint8_t* rdram, uint32_t vaddr) {
    return rdram + (vaddr - 0x80000000u);
}

// MM_P2_TRACE test lines (not from the shadow frame, mm::far).
bool trace_on() {
    static const bool on = std::getenv("MM_P2_TRACE") != nullptr;
    return on && !mm::far::in_shadow();
}

uint8_t* slot_ptr(uint8_t* rdram, int slot) {
    return at(rdram, kActors + static_cast<uint32_t>(slot) * kStride);
}

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

// Where a global's copy sits in player 2's block at kP2Globals.
uint32_t global_offset(uint32_t vaddr) {
    uint32_t off = 0;
    for (const Range& r : kGlobals) {
        if (vaddr >= r.start && vaddr < r.start + r.size) return off + (vaddr - r.start);
        off += r.size;
    }
    return 0;
}

void copy_globals_out(uint8_t* rdram, uint8_t* dst) {
    for (const Range& r : kGlobals) {
        std::memcpy(dst, at(rdram, r.start), r.size);
        dst += r.size;
    }
}

void copy_globals_in(uint8_t* rdram, const uint8_t* src) {
    for (const Range& r : kGlobals) {
        std::memcpy(at(rdram, r.start), src, r.size);
        src += r.size;
    }
}

// Controller 2: the first connected pad other than player 1's, or -1.
int p2_pad_index(uint8_t* rdram) {
    const int p1 = MEM_W(0, addr(0x801370C0u));
    for (int c = 0; c < 4; ++c) {
        if (c == p1) continue;
        if (MEM_BU(4, addr(kPads + static_cast<uint32_t>(c) * 6u)) == 0) return c;
    }
    return -1;
}

// Controller 2's buttons: the second connected pad after player 1's.
uint16_t p2_buttons(uint8_t* rdram) {
    const int p1 = MEM_W(0, addr(0x801370C0u));
    for (int c = 0; c < 4; ++c) {
        if (c == p1) continue;
        const uint32_t pad = kPads + static_cast<uint32_t>(c) * 6u;
        if (MEM_BU(4, addr(pad)) == 0) return MEM_HU(0, addr(pad));   // err_no 0: connected
    }
    return 0;
}

} // namespace

bool controls(uint8_t* rdram, uint32_t& record, uint16_t& hold, uint16_t& press) {
    const int p2 = slot(rdram);
    if (p2 <= 0) return false;
    record = kActors + static_cast<uint32_t>(p2) * kStride;
    hold = MEM_HU(0, addr(kP2Globals + global_offset(0x801370CCu)));
    press = MEM_HU(0, addr(kP2Globals + global_offset(0x801370CEu)));
    return true;
}

int slot(uint8_t* rdram) {
    // The cached slot, else a scan: a loaded state (or a netplay peer's)
    // brings its player 2 along without this process having spawned her.
    auto is_p2 = [rdram](int s) {
        const gpr a = addr(kActors + static_cast<uint32_t>(s) * kStride);
        return MEM_W(0x80, a) != 0 && MEM_HU(0xD2, a) == mm::custom_actors::type_id(kCustomIndex);
    };
    if (g.slot > 0 && is_p2(g.slot)) return g.slot;
    for (int s = 1; s < 0xD0; ++s) {
        if (is_p2(s)) {
            g.slot = s;
            return s;
        }
    }
    g.slot = -1;
    return -1;
}

void setup(uint8_t* rdram, int slot) {
    // Only one player 2 (her globals live in one place): a new one replaces the old.
    if (const int old = mm::player2::slot(rdram); old >= 0 && old != slot) {
        MEM_W(0x80, addr(kActors + static_cast<uint32_t>(old) * kStride)) = 0;
    }
    // A copy of Marina as she is now, a little behind her.
    uint8_t* p = slot_ptr(rdram, slot);
    int32_t x;
    std::memcpy(p, slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    std::memcpy(&x, p + 0x88, 4);
    int dx = (MEM_W(0x80, addr(kActors)) & 0x20) != 0 ? 24 : -24;
    static const char* test_dx = std::getenv("MM_TEST_P2_DX");   // test: join at this offset, facing player 1
    if (test_dx) dx = std::atoi(test_dx);
    x += dx << 16;
    std::memcpy(p + 0x88, &x, 4);
    if (test_dx) {
        uint32_t fl;
        std::memcpy(&fl, p + 0x80, 4);
        fl = dx > 0 ? (fl | 0x20u) : (fl & ~0x20u);   // 0x20: facing left
        std::memcpy(p + 0x80, &fl, 4);
    }
    MEM_W(0x80, addr(kActors + static_cast<uint32_t>(slot) * kStride)) &= ~0x02000000;   // not holding anything
    MEM_H(0xD6, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = 0;
    // Full health, not player 1's present health (which may be almost gone).
    {
        int16_t full = MEM_H(0, addr(kP2FullHealth));
        full = std::max<int16_t>(full, MEM_H(0xE0, addr(kActors)));
        if (full <= 0) full = 1;
        MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = full;
    }
    copy_globals_out(rdram, at(rdram, kP2Globals));
    // Not player 1's recent presses or input words: she starts with none.
    for (uint32_t i = 0; i < kHistoryLen * 2; i += 2) {
        MEM_H(0, addr(kP2Globals + global_offset(kHistoryPress) + i)) = 0;
        MEM_H(0, addr(kP2Globals + global_offset(kHistoryHold) + i)) = 0;
    }
    MEM_W(0, addr(kP2Globals + global_offset(0x801370CCu))) = 0;
    MEM_W(0, addr(kP2Globals + global_offset(kHold))) = 0;
    MEM_W(0, addr(kP2Globals + global_offset(kHold + 4))) = 0;
    MEM_H(0, addr(kP2PrevButtons)) = 0;
    g.slot = slot;
    g.joined = true;
    mm::far::reset(rdram);   // her own camera starts again from the game's
    if (!mm::far::in_shadow()) std::fprintf(stderr, "[player2] joined in slot %d\n", slot);
}

// Put player 2 back next to player 1: a fresh copy of her (as she joins),
// used after a defeat and when she has been left far behind.
void rejoin(uint8_t* rdram, int slot) {
    setup(rdram, slot);
}

// One shared screen. The game has one camera (the players' midpoint, see
// mm_camera_begin), and only what is near it is active and drawn, so the
// players can't leave it: across, each stays within kScreenEdge px of the
// camera centre (360 px apart at most), pushed back with the outward speed
// taken away, as in other shared-screen co-op games. Without it they ran off
// in opposite directions until the leash pulled player 2 back (a teleport).
// Each machine's own view then shows its player at most 180 px off the
// shared centre, well inside the widescreen picture and active area.
constexpr int kScreenEdge = 180;

void clamp_player(uint8_t* rdram, uint32_t record, uint32_t world_x_global) {
    const gpr a = addr(record);
    const int32_t x = MEM_W(0x88, a);
    const int32_t limit = kScreenEdge << 16;
    int32_t want = x;
    if (x > limit) want = limit;
    if (x < -limit) want = -limit;
    if (want == x) return;
    MEM_W(0x88, a) = want;
    const int32_t vx = MEM_W(0xEC, a);
    if ((x > 0 && vx > 0) || (x < 0 && vx < 0)) MEM_W(0xEC, a) = 0;
    if (world_x_global != 0) {
        MEM_W(0, addr(world_x_global)) = MEM_W(0, addr(world_x_global)) + (want - x);
    }
}

void keep_on_screen(uint8_t* rdram, int slot) {
    if (mm::far::active(rdram)) return;   // far apart allowed: each has her own camera (far_coop.h)
    clamp_player(rdram, kActors, kWorldX);
    clamp_player(rdram, kActors + static_cast<uint32_t>(slot) * kStride, kP2Globals + global_offset(kWorldX));
}

void clamp_x(uint8_t* rdram, int32_t centre, int32_t half_width) {
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    const gpr a = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    const int32_t x = MEM_W(0x88, a);
    const int32_t want = std::clamp(x, centre - half_width, centre + half_width);
    if (want == x) return;
    MEM_W(0x88, a) = want;
    const int32_t vx = MEM_W(0xEC, a);
    if ((want < x && vx > 0) || (want > x && vx < 0)) MEM_W(0xEC, a) = 0;
    const uint32_t wx = kP2Globals + global_offset(kWorldX);
    MEM_W(0, addr(wx)) = MEM_W(0, addr(wx)) + (want - x);
}

bool g_in_own_call = false;   // inside a func_8004ED10 call made here
bool g_p1_deferred = false;   // player 1's update was skipped at its usual place this frame

void update_core(uint8_t* rdram, recomp_context* ctx, int slot) {
    g.slot = slot;
    if (MEM_HU(0, addr(kMarinaPaused)) != 0) return;   // the game is not running Marina either
    // Defeated: she comes straight back by player 1 instead of running
    // Marina's own defeat (which would end the game for both).
    if (MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) <= 0) {
        if (MEM_H(0xE0, addr(kActors)) <= 0) {
            // Player 1 is down too: wait (the game's own defeat runs for
            // her); a copy of her now would be defeated at once, every frame.
            MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = 0;
            return;
        }
        if (!mm::far::in_shadow()) std::fprintf(stderr, "[player2] defeated, rejoining\n");
        rejoin(rdram, slot);
    }

    // Player 1 aside.
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, globals_size()> p1_globals;
    std::memcpy(p1_record.data(), slot_ptr(rdram, 0), kStride);
    uint32_t p1_flags;
    std::memcpy(&p1_flags, p1_record.data() + 0x80, 4);   // host-endian word, as MEM_W stores it
    copy_globals_out(rdram, p1_globals.data());

    // Player 2 in.
    std::memcpy(slot_ptr(rdram, 0), slot_ptr(rdram, slot), kStride);
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    (void)p1_flags;
    // Carrying player 1? The game recorded slot 0 as what she holds, but in
    // her own update slot 0 is herself. Player 1's record goes into player
    // 2's slot for the update and the held slot points there, so Marina's
    // carry and throw code moves player 1; it comes back to slot 0 after.
    const bool holding_p1 = (static_cast<uint32_t>(MEM_W(0x80, addr(kActors))) & kHoldingFlag) != 0 &&
                            MEM_HU(0xD6, addr(kActors)) == 0;
    if (holding_p1) {
        std::memcpy(slot_ptr(rdram, slot), p1_record.data(), kStride);
        // As an ordinary object: Marina holding a Marina-type actor pulls
        // herself to it instead of carrying it (she jumped 40 px on the grab
        // and slid away while carrying).
        MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
            static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
        MEM_H(0xD6, addr(kActors)) = static_cast<int16_t>(slot);
        if (const char* t = std::getenv("MM_TEST_HELDX")) {   // experiment: move the held copy
            MEM_W(0x88, addr(kActors + static_cast<uint32_t>(slot) * kStride)) += std::atoi(t) << 16;
        }
        if (trace_on()) {
            const gpr h = addr(kActors + static_cast<uint32_t>(slot) * kStride);
            std::fprintf(stderr, "[p2trace] carry in: p2 x %d vx %d | held x %d y %d fl %08X st %04X D8 %08X\n",
                         MEM_W(0x88, addr(kActors)) >> 16, MEM_W(0xEC, addr(kActors)), MEM_W(0x88, h) >> 16, MEM_W(0x8C, h) >> 16,
                         static_cast<uint32_t>(MEM_W(0x80, h)), MEM_HU(0xD0, h), static_cast<uint32_t>(MEM_W(0xD8, h)));
        }
    }
    copy_globals_in(rdram, at(rdram, kP2Globals));
    const uint16_t buttons = p2_buttons(rdram);
    // Controller 1's raw pad as well (some of Marina's code reads it there):
    // player 2's pad stands in for it during her update.
    uint8_t pad0[6];
    for (int i = 0; i < 6; ++i) pad0[i] = MEM_BU(i, addr(kPads));
    {
        const int p2pad = p2_pad_index(rdram);
        for (int i = 0; i < 6; ++i) {
            MEM_B(i, addr(kPads)) = p2pad >= 0 ? MEM_BU(i, addr(kPads + static_cast<uint32_t>(p2pad) * 6u)) : 0;
        }
    }
    uint16_t hold = buttons, press = static_cast<uint16_t>(buttons & ~MEM_HU(0, addr(kP2PrevButtons)));
    MEM_H(0, addr(kP2PrevButtons)) = static_cast<int16_t>(buttons);
    // As func_800122B0 does for player 1: the input lock mask, then her
    // history moves one frame on with this frame's (action-masked) input.
    if (MEM_HU(0, addr(0x800BE5D4u)) != 0) {
        const uint16_t mask = MEM_HU(0, addr(0x800BE500u));
        hold &= mask;
        press &= mask;
    }
    MEM_H(0, addr(kHold)) = static_cast<int16_t>(hold);
    MEM_H(0, addr(kPress)) = static_cast<int16_t>(press);
    for (uint32_t i = kHistoryLen - 1; i > 0; --i) {
        MEM_H(i * 2, addr(kHistoryHold)) = MEM_H((i - 1) * 2, addr(kHistoryHold));
        MEM_H(i * 2, addr(kHistoryPress)) = MEM_H((i - 1) * 2, addr(kHistoryPress));
    }
    {
        uint16_t m = 0;
        for (uint32_t a : {0x800BE50Cu, 0x800BE510u, 0x800BE504u, 0x800BE508u, 0x800BE514u, 0x800BE518u}) {
            m = static_cast<uint16_t>(m + MEM_HU(0, addr(a)));
        }
        MEM_H(0, addr(kHistoryHold)) = static_cast<int16_t>(hold & m);
        MEM_H(0, addr(kHistoryPress)) = static_cast<int16_t>(press & m);
    }
    if (trace_on() && buttons != 0) {
        std::fprintf(stderr, "[p2trace] p2 input %04X (pads %04X/%02X %04X/%02X, p1 index %d)\n", buttons,
                     MEM_HU(0, addr(kPads)), MEM_BU(4, addr(kPads)), MEM_HU(0, addr(kPads + 6)), MEM_BU(4, addr(kPads + 6)),
                     MEM_W(0, addr(0x801370C0u)));
    }

    if (trace_on()) {
        std::fprintf(stderr, "[p2trace] p2 in: st %04X 98 %08X 94 %04X D4 %04X DA %08X\n", MEM_HU(0xD0, addr(kActors)),
                     static_cast<uint32_t>(MEM_W(0x98, addr(kActors))), MEM_HU(0x94, addr(kActors)), MEM_HU(0xD4, addr(kActors)),
                     static_cast<uint32_t>(MEM_W(0xD8, addr(kActors))));
    }
    // Marina's update, as func_80020024 calls it.
    const gpr saved_a0 = ctx->r4;
    const uint16_t state_before = MEM_HU(0xD0, addr(kActors));
    ctx->r4 = 0;
    const int32_t x_before = MEM_W(0x88, addr(kActors));
    static std::vector<uint8_t> snapshot;
    recomp_context ctx_before;
    if (holding_p1) {
        snapshot.assign(rdram, rdram + 0x800000);
        ctx_before = *ctx;
    }
    g_in_own_call = true;
    func_8004ED10(rdram, ctx);
    g_in_own_call = false;
    if (holding_p1) {
        // Picking player 1 up, Marina's code puts the carrier 55 px behind
        // what she holds (the pickup offset for objects): a jump. Then run
        // her update again from before it, with player 1 placed where that
        // offset leaves the carrier where she was; player 1 comes to her
        // hands instead.
        const int32_t moved = MEM_W(0x88, addr(kActors)) - x_before;
        if (moved > (12 << 16) || moved < -(12 << 16)) {
            std::memcpy(rdram, snapshot.data(), snapshot.size());
            *ctx = ctx_before;
            MEM_W(0x88, addr(kActors + static_cast<uint32_t>(slot) * kStride)) -= moved;
            g_in_own_call = true;
            func_8004ED10(rdram, ctx);
            g_in_own_call = false;
        }
    }
    ctx->r4 = saved_a0;
    if (trace_on() && MEM_HU(0xD0, addr(kActors)) != state_before) {
        std::fprintf(stderr, "[p2trace] p2 state %04X -> %04X (98 %08X, input %04X/%04X, 70CC %08X %08X)\n", state_before,
                     MEM_HU(0xD0, addr(kActors)), static_cast<uint32_t>(MEM_W(0x98, addr(kActors))), MEM_HU(0, addr(kHold)), MEM_HU(0, addr(kPress)),
                     static_cast<uint32_t>(MEM_W(0, addr(0x801370CCu))), static_cast<uint32_t>(MEM_W(0, addr(0x801370D0u))));
    }

    for (int i = 0; i < 6; ++i) MEM_B(i, addr(kPads)) = static_cast<int8_t>(pad0[i]);

    if (holding_p1 && trace_on()) {
        std::fprintf(stderr, "[p2trace] carry: p2 x %d -> %d, p1 x %d -> %d, p1 st %04X\n",
                     MEM_W(0x88, addr(kActors + static_cast<uint32_t>(slot) * kStride)) >> 16, MEM_W(0x88, addr(kActors)) >> 16,
                     static_cast<int32_t>(p1_record[0x88] | p1_record[0x89] << 8 | p1_record[0x8A] << 16 | p1_record[0x8B] << 24) >> 16,
                     MEM_W(0x88, addr(kActors + static_cast<uint32_t>(slot) * kStride)) >> 16, MEM_HU(0xD0, addr(kActors + static_cast<uint32_t>(slot) * kStride)));
    }
    if (holding_p1) {
        // Player 1 as the carry / throw left her.
        std::memcpy(p1_record.data(), slot_ptr(rdram, slot), kStride);
        if (MEM_HU(0xD6, addr(kActors)) == slot) MEM_H(0xD6, addr(kActors)) = 0;
    }

    // Player 2 out, player 1 back.
    std::memcpy(slot_ptr(rdram, slot), slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    // Her colour: a cool blue added over Marina's own (the draw builder's
    // prim-colour tint at +0x9C..+0x9F, func_80009BE8). Marina's update
    // clears those bytes each frame and uses them for her hit flash, so the
    // tint goes on only while she is not flashing.
    {
        const gpr pa = addr(kActors + static_cast<uint32_t>(slot) * kStride);
        if (MEM_BU(0x9C, pa) == 0 && MEM_BU(0x9D, pa) == 0 && MEM_BU(0x9E, pa) == 0) {
            MEM_B(0x9C, pa) = 0x00;
            MEM_B(0x9D, pa) = 0x30;
            MEM_B(0x9E, pa) = 0x5A;
            MEM_B(0x9F, pa) = static_cast<int8_t>(0xFF);
        }
    }
    copy_globals_out(rdram, at(rdram, kP2Globals));
    std::memcpy(slot_ptr(rdram, 0), p1_record.data(), kStride);
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    copy_globals_in(rdram, p1_globals.data());
    keep_on_screen(rdram, slot);
    // Player 1's update, held back this frame because player 2 carries her
    // (mm_marina_update_hook): the holder must move first, as when player 1
    // carries player 2 (Marina's own update runs before the actors'), or
    // each anchors to the other and both slide with the camera.
    if (g_p1_deferred) {
        g_p1_deferred = false;
        const gpr a0 = ctx->r4;
        ctx->r4 = 0;
        g_in_own_call = true;
        func_8004ED10(rdram, ctx);
        g_in_own_call = false;
        ctx->r4 = a0;
    }
}

// Test (MM_P2_BISECT): the first time player 2 starts a grab (0x011F)
// without input, find which game memory changed since the frame before
// makes her do it: rerun her update with parts of memory put back.
void update(uint8_t* rdram, recomp_context* ctx, int slot) {
    static const bool bisect = std::getenv("MM_P2_BISECT") != nullptr;
    if (!bisect) {
        update_core(rdram, ctx, slot);
        return;
    }
    constexpr size_t kSize = 0x800000;
    static std::vector<uint8_t> prev;
    static bool done = false;
    std::vector<uint8_t> cur(rdram, rdram + kSize);
    const gpr me = addr(kActors + static_cast<uint32_t>(slot) * kStride);
    const uint16_t before = MEM_HU(0xD0, me);
    const recomp_context saved_ctx = *ctx;
    update_core(rdram, ctx, slot);
    if (!done && !prev.empty() && before != 0x011F && MEM_HU(0xD0, me) == 0x011F) {
        done = true;
        std::vector<uint8_t> post(rdram, rdram + kSize);
        std::vector<uint32_t> diff;
        for (uint32_t w = 0; w < kSize / 4; ++w) {
            if (reinterpret_cast<uint32_t*>(cur.data())[w] != reinterpret_cast<uint32_t*>(prev.data())[w]) diff.push_back(w);
        }
        auto grabs_with = [&](const std::vector<uint32_t>& revert) {
            std::memcpy(rdram, cur.data(), kSize);
            for (uint32_t w : revert) reinterpret_cast<uint32_t*>(rdram)[w] = reinterpret_cast<uint32_t*>(prev.data())[w];
            *ctx = saved_ctx;
            update_core(rdram, ctx, slot);
            return MEM_HU(0xD0, me) == 0x011F;
        };
        std::fprintf(stderr, "[bisect] %zu words changed; all reverted: grab=%d\n", diff.size(), grabs_with(diff) ? 1 : 0);
        // Shrink: drop chunks whose reversion is not needed to stop the grab.
        std::vector<uint32_t> need = diff;
        for (size_t chunk = need.size() / 2; chunk >= 1; chunk /= 2) {
            for (size_t i = 0; i < need.size();) {
                std::vector<uint32_t> trial;
                trial.insert(trial.end(), need.begin(), need.begin() + static_cast<long>(i));
                trial.insert(trial.end(), need.begin() + static_cast<long>(std::min(need.size(), i + chunk)), need.end());
                if (!grabs_with(trial)) need = trial;
                else i += chunk;
            }
            if (chunk == 1) break;
        }
        for (uint32_t w : need) {
            std::fprintf(stderr, "[bisect] needed %08X: %08X (frame before %08X)\n", 0x80000000u + w * 4,
                         reinterpret_cast<uint32_t*>(cur.data())[w], reinterpret_cast<uint32_t*>(prev.data())[w]);
        }
        std::memcpy(rdram, post.data(), kSize);
        *ctx = saved_ctx;
    }
    prev = std::move(cur);
}

void publish_marker(uint8_t* rdram, int p2) {
    Marker m;
    if (p2 >= 0) {
        const gpr a = addr(kActors + static_cast<uint32_t>(p2) * kStride);
        m.visible = true;
        m.x = MEM_H(0x88, a);
        m.top = MEM_H(0x8C, a) + MEM_H(0xAE, a);
        // On her machine the picture is her own camera's.
        if (int32_t dx, dy; mm::far::shows_p2_view(rdram) && mm::far::p2_camera_offset(rdram, dx, dy)) {
            m.x = (MEM_W(0x88, a) - dx) >> 16;
            m.top = ((MEM_W(0x8C, a) - dy) >> 16) + MEM_H(0xAE, a);
        }
        m.health = MEM_H(0xE0, a);
        g.joined = false;
        m.max_health = MEM_H(0, addr(kP2FullHealth));
        if (int32_t dx, dy; mm::far::p2_camera_offset(rdram, dx, dy)) {
            m.partner = true;
            if (mm::far::shows_p2_view(rdram)) {   // her machine: player 1, seen from her camera
                m.partner_is_p1 = true;
                m.px = (MEM_W(0x88, addr(kActors)) - dx) >> 16;
                m.py = (MEM_W(0x8C, addr(kActors)) - dy) >> 16;
            } else {
                m.px = MEM_W(0x88, a) >> 16;
                m.py = MEM_W(0x8C, a) >> 16;
            }
        }
    }
    std::lock_guard lock{g_marker_mutex};
    g_marker = m;
}

void tick(uint8_t* rdram) {
    const int p2 = slot(rdram);
    // No grabbing each other for a second after either was carried (state
    // 0x2F): escaping from the other's hands with B would grab them back
    // at once, in mid-air, and lock both there.
    {
        int32_t cool = MEM_W(0, addr(kP2GrabCooldown));
        const bool held = (MEM_HU(0xD0, addr(kActors)) & 0xFF) == 0x2F ||
                          (p2 > 0 && (MEM_HU(0xD0, addr(kActors + static_cast<uint32_t>(p2) * kStride)) & 0xFF) == 0x2F);
        cool = held ? 60 : std::max(0, cool - 1);
        MEM_W(0, addr(kP2GrabCooldown)) = cool;
    }
    {
        int16_t full = MEM_H(0, addr(kP2FullHealth));
        full = std::max<int16_t>(full, MEM_H(0xE0, addr(kActors)));
        if (p2 > 0) full = std::max<int16_t>(full, MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(p2) * kStride)));
        MEM_H(0, addr(kP2FullHealth)) = full;
    }
    if (std::getenv("MM_P2_TRACE")) {   // test: both players each frame
        static long n = 0;
        const gpr m = addr(kActors);
        const gpr b = p2 >= 0 ? addr(kActors + static_cast<uint32_t>(p2) * kStride) : m;
        const auto held_ok = [&](gpr who) {
            if ((static_cast<uint32_t>(MEM_W(0x80, who)) & kHoldingFlag) == 0) return 1;
            const uint16_t h = MEM_HU(0xD6, who);
            return h < 0xD0 && MEM_W(0x80, addr(kActors + static_cast<uint32_t>(h) * kStride)) != 0 ? 1 : 0;
        };
        std::fprintf(stderr, "[p2trace] held_ok %d %d hp %d %d\n", held_ok(m), p2 >= 0 ? held_ok(b) : 1,
                     MEM_H(0xE0, m), p2 >= 0 ? MEM_H(0xE0, b) : 0);
        std::fprintf(stderr, "[p2trace] %ld cam %d,%d p1 %d,%d st %04X fl %08X hold %04X | p2(%d) %d,%d st %04X fl %08X hold %04X\n",
                     n++, MEM_W(0, addr(kCameraX)) >> 16, MEM_W(0, addr(kCameraY)) >> 16,
                     MEM_W(0x88, m) >> 16, MEM_W(0x8C, m) >> 16, MEM_HU(0xD0, m), static_cast<uint32_t>(MEM_W(0x80, m)), MEM_HU(0xD6, m),
                     p2, MEM_W(0x88, b) >> 16, MEM_W(0x8C, b) >> 16, MEM_HU(0xD0, b), static_cast<uint32_t>(MEM_W(0x80, b)), MEM_HU(0xD6, b));
    }
    publish_marker(rdram, p2);
    if (!mm::cheats::coop_enabled()) {
        if (p2 >= 0) {
            MEM_W(0x80, addr(kActors + static_cast<uint32_t>(p2) * kStride)) = 0;
            std::fprintf(stderr, "[player2] left (co-op off)\n");
        }
        return;
    }
    const uint16_t state = MEM_HU(0, addr(kGameState));
    const bool p1_playing = (state == 6 || state == 10) && MEM_W(0x80, addr(kActors)) != 0 &&
                            MEM_HU(0xD2, addr(kActors)) == kMarinaType && MEM_HU(0, addr(kMarinaPaused)) == 0;
    if (!p1_playing) {
        MEM_W(0, addr(kP2JoinTimer)) = 0;
        return;
    }
    if (p2 < 0) {
        const int32_t t = MEM_W(0, addr(kP2JoinTimer)) + 1;
        MEM_W(0, addr(kP2JoinTimer)) = t;
        if (t >= kJoinDelay) {
            MEM_W(0, addr(kP2JoinTimer)) = 0;
            mm::actors::spawn_now(rdram, mm::custom_actors::type_id(kCustomIndex), 0, 0);
        }
        return;
    }
    // Leash: out of the (shared) camera's view for a while, i.e. left
    // behind, stuck or fallen. (Not merely far apart: Marina's screen clamp
    // lets the two stand 288 px apart at the screen edges, and a distance
    // leash pulled her back from there.) Positions are camera-relative.
    const gpr a = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    int px = MEM_H(0x88, a), py = MEM_H(0x8C, a);
    // Far apart (netplay): out of her own camera's view instead.
    if (int32_t dx, dy; mm::far::p2_camera_offset(rdram, dx, dy)) {
        px = (MEM_W(0x88, a) - dx) >> 16;
        py = (MEM_W(0x8C, a) - dy) >> 16;
    }
    const bool away = px > kLeashX || px < -kLeashX || py > kLeashY || py < -kLeashY;
    int32_t away_frames = away ? MEM_W(0, addr(kP2AwayTimer)) + 1 : 0;
    if (away_frames >= kLeashFrames) {
        std::fprintf(stderr, "[player2] too far from player 1, rejoining\n");
        rejoin(rdram, p2);
        away_frames = 0;
    }
    MEM_W(0, addr(kP2AwayTimer)) = away_frames;
}

#ifdef MM_HAS_GRAPHICS
// The "2P" marker and her health, over her head (same canvas mapping as the
// enemy bars: 320x240 fills the window height, camera-relative px, +y up).
void draw_overlay() {
    Marker m;
    {
        std::lock_guard lock{g_marker_mutex};
        m = g_marker;
    }
    if (!m.visible) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float scale = io.DisplaySize.y / 240.0f;
    const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    // The other player off this screen (far apart): an arrow at the edge
    // pointing at her, with her tag and how far she is.
    if (m.partner) {
        const float half_w = centre.x / scale - 12.0f, half_h = 120.0f - 14.0f;
        const float fx = static_cast<float>(m.px), fy = static_cast<float>(m.py) + 16.0f;
        if (std::abs(fx) > half_w + 8.0f || std::abs(fy) > half_h + 8.0f) {
            const float k = std::min(half_w / std::max(std::abs(fx), 1.0f), half_h / std::max(std::abs(fy), 1.0f));
            const ImVec2 tip(centre.x + fx * k * scale, centre.y - fy * k * scale);
            const float len = std::sqrt(fx * fx + fy * fy);
            const ImVec2 dir(fx / len, -fy / len), side(-dir.y, dir.x);
            const float as = std::max(8.0f, 7.0f * scale);
            const ImVec2 back(tip.x - dir.x * as, tip.y - dir.y * as);
            const ImU32 col = m.partner_is_p1 ? IM_COL32(220, 60, 60, 230) : IM_COL32(20, 90, 200, 230);
            dl->AddTriangleFilled(tip, ImVec2(back.x + side.x * as * 0.6f, back.y + side.y * as * 0.6f),
                                  ImVec2(back.x - side.x * as * 0.6f, back.y - side.y * as * 0.6f), col);
            char label[32];
            std::snprintf(label, sizeof label, "%s %dm", m.partner_is_p1 ? "1P" : "2P", static_cast<int>(len / 16.0f));
            ImFont* f = ImGui::GetFont();
            const float size = std::max(10.0f, 5.0f * scale);
            const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
            ImVec2 at(back.x - dir.x * as * 0.4f - ts.x * 0.5f, back.y - dir.y * as * 0.4f - ts.y * 0.5f);
            at.x = std::clamp(at.x, 2.0f, io.DisplaySize.x - ts.x - 2.0f);
            at.y = std::clamp(at.y, 2.0f, io.DisplaySize.y - ts.y - 2.0f);
            dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ts.x + 3, at.y + ts.y + 1), col, 3.0f);
            dl->AddText(f, size, at, IM_COL32(255, 255, 255, 255), label);
        }
    }
    float vx, vy;
    view_offset(vx, vy);
    const ImVec2 head(centre.x + (m.x - vx) * scale, centre.y - (m.top - vy + 6.0f) * scale);
    const char* text = "2P";
    ImFont* font = ImGui::GetFont();
    const float size = std::max(10.0f, 6.0f * scale);
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 p0(head.x - ts.x * 0.5f - size * 0.3f, head.y - ts.y - size * 0.6f);
    const ImVec2 p1(head.x + ts.x * 0.5f + size * 0.3f, head.y - size * 0.2f);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 90, 200, 220), size * 0.3f);
    dl->AddText(font, size, ImVec2(head.x - ts.x * 0.5f, p0.y + size * 0.2f), IM_COL32(255, 255, 255, 255), text);
    // Health under the tag.
    const float w = (p1.x - p0.x) * 1.6f, h = std::max(2.0f, size * 0.22f);
    const ImVec2 b0(head.x - w * 0.5f, p1.y + h * 0.6f), b1(head.x + w * 0.5f, p1.y + h * 1.6f);
    dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 180));
    const float frac = m.max_health > 0 ? std::clamp(static_cast<float>(m.health) / m.max_health, 0.0f, 1.0f) : 0.0f;
    dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * frac, b1.y), IM_COL32(70, 170, 255, 255));
}
#endif

} // namespace mm::player2

// Shared camera. Translated func_80012830 (camera update) calls these at its
// entry and before its return (troublemakers.us1.toml). While co-op is on,
// Marina's position reads as the midpoint between the two players for the
// camera's sake, so it frames both; her real position comes back after,
// carrying any shift the camera code applied meanwhile.
namespace {
bool g_camera_swapped = false;
int32_t g_saved_x, g_saved_y, g_saved_wx, g_saved_wy, g_mid_x, g_mid_y;
}

// Her own look. Marina's pose is one of ~800 compressed 64x64 frames that
// the game streams into a single texture: func_800821B0 / func_800822B8
// (main loop) DMA slot 0's frame (+0x84, image class 0x5800) and unpack it
// to 0x8027AEE8 or 0x8027BEE8 (double buffered, index = 0x800BE6A4 +
// 0x800BE700), and the draw builder's class-0x5800 branch (func_80009BE8 at
// 0x8000A474) draws whoever has a Marina image as image 2..5 (the display
// lists in table [0x801376DC] that sample that buffer), with offsets and
// flags read from slot 0, not the actor being drawn. So without help player
// 2 is drawn in player 1's pose. Around her entry in the draw builder
// (0x8000A474 to the loop step at 0x8000DD18) her record stands in for slot
// 0, her own frame is unpacked into her own buffer and images 2..5 point at
// copies of their display lists that sample it. Everything lives in game
// memory at 0x807F1000.. so it is the same on every machine.
namespace {
constexpr uint32_t kDlTable = 0x801376DCu;        // -> display list per class-0 image
constexpr uint32_t kSlotImages = 0x80137480u;     // image per streamed buffer (afterimages)
constexpr uint32_t kStreamIndex = 0x800BE6A4u, kStreamParity = 0x800BE700u;
constexpr uint32_t kGameTexture = 0x8027AEE8u;    // the game's two 0x1000 buffers
constexpr uint32_t kP2Lists = 0x807F1000u;        // 4 display lists, 0x100 each
constexpr uint32_t kP2Texture = 0x807F2000u;      // her two 0x1000 buffers
constexpr uint32_t kP2Packed = 0x807F4000u;       // compressed frame (DMA target)
constexpr uint32_t kP2Header = 0x807F0420u;       // 8-byte frame header
constexpr uint32_t kP2Loaded = 0x807F0410u;       // frame unpacked in each of her buffers
constexpr uint32_t kFrameTable = 0x004A0918u;     // ROM: frame offsets (func_800821B0)
constexpr uint32_t kFrameData = 0x0040AC80u - 0x05000000u;

bool g_draw_swapped = false;
std::array<uint8_t, 0x198> g_draw_p1;
std::array<uint8_t, 8> g_draw_slot_images;
uint32_t g_draw_lists[2];
int g_draw_k = -1;

void unpack_frame(uint8_t* rdram, recomp_context* ctx, uint16_t image, uint32_t dst) {
    const uint32_t idx = static_cast<uint32_t>(image & 0x7FF) % 0x322u;
    const uint32_t header = idx == 0 ? kFrameTable : kFrameTable + idx * 4u - 4u;
    recomp::do_rom_read(rdram, static_cast<gpr>(static_cast<int32_t>(kP2Header)), 0x10000000u + header, 8);
    const uint32_t start = static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kP2Header))));
    const uint32_t end = static_cast<uint32_t>(MEM_W(4, static_cast<gpr>(static_cast<int32_t>(kP2Header))));
    const uint32_t size = end - start;
    if (size == 0 || size > 0x2000u || (start & 1) != 0) return;
    recomp::do_rom_read(rdram, static_cast<gpr>(static_cast<int32_t>(kP2Packed)), 0x10000000u + start + kFrameData,
                        (size + 7u) & ~7u);
    const recomp_context saved = *ctx;
    ctx->r4 = static_cast<gpr>(static_cast<int32_t>(kP2Packed));
    ctx->r5 = static_cast<gpr>(static_cast<int32_t>(dst));
    ctx->r6 = 0x1000;
    ctx->r29 -= 0x40;
    Trouble_RLE_Type2(rdram, ctx);
    *ctx = saved;
}

// A copy of a game display list with its texture images moved to hers.
uint32_t copy_list(uint8_t* rdram, uint32_t src, uint32_t dst) {
    for (uint32_t off = 0; off + 8 <= 0x100u; off += 8) {
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(off, static_cast<gpr>(static_cast<int32_t>(src))));
        uint32_t w1 = static_cast<uint32_t>(MEM_W(off + 4, static_cast<gpr>(static_cast<int32_t>(src))));
        if ((w0 >> 24) == 0xFD && w1 >= kGameTexture && w1 < kGameTexture + 0x2000u) {
            w1 = kP2Texture + (w1 - kGameTexture);
        }
        MEM_W(off, static_cast<gpr>(static_cast<int32_t>(dst))) = static_cast<int32_t>(w0);
        MEM_W(off + 4, static_cast<gpr>(static_cast<int32_t>(dst))) = static_cast<int32_t>(w1);
        if ((w0 >> 24) == 0xB8) break;   // end of list
    }
    return dst;
}
} // namespace

extern "C" void mm_p2_draw_begin(uint8_t* rdram, recomp_context* ctx) {
    using namespace mm::player2;
    if (g_draw_swapped) return;
    const int p2 = slot(rdram);
    if (p2 < 0) return;
    const uint32_t rec = 0x800EF510u + static_cast<uint32_t>(p2) * 0x198u;
    if (static_cast<uint32_t>(ctx->r20) != rec) return;   // $s4: the actor being drawn
    uint8_t* const p1 = rdram + (0x800EF510u - 0x80000000u);
    uint8_t* const me = rdram + (rec - 0x80000000u);
    std::memcpy(g_draw_p1.data(), p1, 0x198);
    std::memcpy(g_draw_slot_images.data(), rdram + (kSlotImages - 0x80000000u), 8);
    std::memcpy(p1, me, 0x198);
    MEM_H(0xD2, static_cast<gpr>(static_cast<int32_t>(0x800EF510u))) = 0x0016;
    g_draw_swapped = true;
    g_draw_k = -1;

    const int k = MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kStreamIndex))) +
                  MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kStreamParity)));
    const uint16_t image = MEM_HU(0x84, static_cast<gpr>(static_cast<int32_t>(rec)));
    if (k < 0 || k > 1 || (image & 0xF800) != 0x5800) return;
    const gpr loaded = static_cast<gpr>(static_cast<int32_t>(kP2Loaded + static_cast<uint32_t>(k) * 2u));
    if (MEM_HU(0, loaded) != image) {
        unpack_frame(rdram, ctx, image, kP2Texture + static_cast<uint32_t>(k) * 0x1000u);
        MEM_H(0, loaded) = static_cast<int16_t>(image);
    }
    const gpr table = static_cast<gpr>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kDlTable))));
    for (int f = 0; f < 2; ++f) {
        const int id = 2 + 2 * k + f;
        const uint32_t game_list = static_cast<uint32_t>(MEM_W(id * 4, table));
        g_draw_lists[f] = game_list;
        MEM_W(id * 4, table) = static_cast<int32_t>(
            copy_list(rdram, game_list, kP2Lists + static_cast<uint32_t>(k * 2 + f) * 0x100u));
    }
    g_draw_k = k;
}

extern "C" void mm_p2_draw_end(uint8_t* rdram, recomp_context*) {
    if (!g_draw_swapped) return;
    g_draw_swapped = false;
    std::memcpy(rdram + (0x800EF510u - 0x80000000u), g_draw_p1.data(), 0x198);
    std::memcpy(rdram + (kSlotImages - 0x80000000u), g_draw_slot_images.data(), 8);
    if (g_draw_k >= 0) {
        const gpr table = static_cast<gpr>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kDlTable))));
        for (int f = 0; f < 2; ++f) MEM_W((2 + 2 * g_draw_k + f) * 4, table) = static_cast<int32_t>(g_draw_lists[f]);
        g_draw_k = -1;
    }
}

// Grabbing each other has two limits, applied while func_8001EC1C (once a
// frame, from func_80020024) resolves contacts: there a grab box (+0x80 &
// 0x80, a player reaching out) takes an actor flagged 0x1000, as both
// players are. (1) Not from the air: Marina grabbing something while
// airborne hangs on it, and the other player, carried, holds on to her:
// both froze in mid-air. (2) Not during the cooldown after a carry (tick).
// Then the other player's 0x1000 is off for the routine.
namespace {
uint32_t g_contacts_cleared[2]{};   // records whose 0x1000 was taken off
}

extern "C" void mm_p2_contacts_begin(uint8_t* rdram) {
    using namespace mm::player2;
    g_contacts_cleared[0] = g_contacts_cleared[1] = 0;
    static const bool off = std::getenv("MM_TEST_NO_GRAB_RULES") != nullptr;
    if (off) return;
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    const uint32_t rec[2] = {kActors, kActors + static_cast<uint32_t>(p2) * kStride};
    const bool cooling = MEM_W(0, addr(kP2GrabCooldown)) > 0;
    for (int i = 0; i < 2; ++i) {
        const uint32_t grabber = static_cast<uint32_t>(MEM_W(0x80, addr(rec[i])));
        const uint32_t target = static_cast<uint32_t>(MEM_W(0x80, addr(rec[1 - i])));
        const bool reaching = (grabber & 0x80u) != 0;
        const bool airborne = (grabber & 0x00020000u) != 0;
        if (reaching && (airborne || cooling) && (target & 0x1000u) != 0) {
            MEM_W(0x80, addr(rec[1 - i])) = static_cast<int32_t>(target & ~0x1000u);
            g_contacts_cleared[1 - i] = rec[1 - i];
        }
    }
}

extern "C" void mm_p2_contacts_end(uint8_t* rdram) {
    using namespace mm::player2;
    for (uint32_t r : g_contacts_cleared) {
        if (r != 0 && MEM_W(0x80, addr(r)) != 0) MEM_W(0x80, addr(r)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, addr(r))) | 0x1000u);
    }
}

// func_8004ED10's entry (troublemakers.us1.toml): nonzero = skip this call.
// Player 1's update moves to the end of player 2's while player 2 carries
// her (update_core).
extern "C" int mm_marina_update_hook(uint8_t* rdram) {
    using namespace mm::player2;
    if (g_in_own_call) return 0;
    const int p2 = slot(rdram);
    if (p2 <= 0) return 0;
    const gpr b = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    if ((static_cast<uint32_t>(MEM_W(0x80, b)) & kHoldingFlag) == 0 || MEM_HU(0xD6, b) != 0) return 0;
    g_p1_deferred = true;
    return 1;
}

// Each player's own view (netplay). The game has one camera, shared by both
// players and by the simulation (actor positions are camera-relative and the
// camera decides what is active), so it stays as it is: the players'
// midpoint. Only the picture moves. LookAt_Update builds the view matrix
// (eye (0, 0, 448) looking at the origin) into 0x804600C0 and 0x804700C0;
// afterwards its translation is moved by half the distance from the other
// player to this machine's player, so the view centres on this player the
// way the camera centres on the midpoint (same dead zone and easing). The
// players stay within 144 px of the midpoint across (Marina's screen clamp),
// well inside what the game draws in widescreen. Only those two matrices
// differ between the machines (the netplay memory check leaves them out);
// the game's own computation, stack leftovers included, is untouched.
namespace {
std::atomic<float> g_view_x{0.0f}, g_view_y{0.0f};   // render thread reads
float g_smooth_x = 0.0f, g_smooth_y = 0.0f;

// Adds `delta` (px) to element [3][col] (translation) of an N64 Mtx.
void shift_translation(uint8_t* rdram, uint32_t mtx, int col, float delta) {
    const gpr m = static_cast<gpr>(static_cast<int32_t>(mtx));
    const int idx = 3 * 4 + col;
    const int32_t whole = MEM_H(idx * 2, m);
    const uint32_t frac = MEM_HU(32 + idx * 2, m);
    int32_t v = static_cast<int32_t>((static_cast<uint32_t>(whole) << 16) | frac);
    v += static_cast<int32_t>(std::lround(delta * 65536.0f));
    MEM_H(idx * 2, m) = static_cast<int16_t>(v >> 16);
    MEM_H(32 + idx * 2, m) = static_cast<int16_t>(v & 0xFFFF);
}
} // namespace

namespace mm::player2 {
void view_offset(float& x, float& y) {
    x = g_view_x.load(std::memory_order_relaxed);
    y = g_view_y.load(std::memory_order_relaxed);
}
} // namespace mm::player2

extern "C" void mm_view_begin(uint8_t*) {}

// The rest of the picture: tile layers and stage blocks are not 3D. The
// draw builder (func_80009BE8) emits them as texture rectangles in screen
// coordinates (10.2 fixed point, 12 bits; the widescreen wings use the
// negative half), the blocks inline and the three tile layers as calls to
// their own display lists (0x80400000..0x80418000). After the builder they
// move by the same offset as the view; the HUD comes later, after the
// orthographic projection, and stays. The view matrices moved the 3D
// objects; without this the blocks and terrain stayed put on screen and
// seemed to slide along with the player.
namespace {
uint32_t g_layer_mark[6]{};                    // first texrect word of each layer list, as last moved

void shift_texrect(uint8_t* rdram, uint32_t at, int dx4) {
    const gpr a = static_cast<gpr>(static_cast<int32_t>(at));
    uint32_t w0 = static_cast<uint32_t>(MEM_W(0, a)), w1 = static_cast<uint32_t>(MEM_W(4, a));
    const uint32_t lrx = (((w0 >> 12) & 0xFFFu) + static_cast<uint32_t>(dx4)) & 0xFFFu;
    const uint32_t ulx = (((w1 >> 12) & 0xFFFu) + static_cast<uint32_t>(dx4)) & 0xFFFu;
    w0 = (w0 & ~(0xFFFu << 12)) | (lrx << 12);
    w1 = (w1 & ~(0xFFFu << 12)) | (ulx << 12);
    MEM_W(0, a) = static_cast<int32_t>(w0);
    MEM_W(4, a) = static_cast<int32_t>(w1);
}

// A tile layer's list, from its start to G_ENDDL; once per rebuild.
void shift_layer(uint8_t* rdram, uint32_t list, int dx4) {
    const int index = static_cast<int>((list - 0x80400000u) / 0x4000u);
    if (index < 0 || index >= 6) return;
    uint32_t first = 0;
    for (uint32_t pc = list, n = 0; n < 2000; pc += 8, ++n) {
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(pc))));
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        if (op == 0xB8) break;
        if (op == 0xE4 || op == 0xE5) {
            if (first == 0) {
                first = static_cast<uint32_t>(MEM_W(4, static_cast<gpr>(static_cast<int32_t>(pc))));
                if (first == g_layer_mark[index]) return;   // already moved: not rebuilt since
            }
            shift_texrect(rdram, pc, dx4);
        }
    }
    for (uint32_t pc = list, n = 0; n < 2000; pc += 8, ++n) {
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(pc))));
        if ((w0 >> 24) == 0xE4 || (w0 >> 24) == 0xE5) {
            g_layer_mark[index] = static_cast<uint32_t>(MEM_W(4, static_cast<gpr>(static_cast<int32_t>(pc))));
            break;
        }
        if ((w0 >> 24) == 0xB8) break;
    }
}
} // namespace

extern "C" void mm_view_end(uint8_t* rdram) {
    using namespace mm::player2;
    const int own = mm::netplay::active() ? mm::netplay::player() : 0;
    const int p2 = own != 0 && !mm::far::active(rdram) ? slot(rdram) : -1;   // far apart: each machine's camera is its player's
    float tx = 0.0f, ty = 0.0f;
    if (p2 > 0) {
        const gpr a1 = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
        const gpr a2 = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
        const float x1 = MEM_W(0x88, a1) / 65536.0f, y1 = MEM_W(0x8C, a1) / 65536.0f;
        const float x2 = MEM_W(0x88, a2) / 65536.0f, y2 = MEM_W(0x8C, a2) / 65536.0f;
        tx = std::clamp((own == 1 ? x1 - x2 : x2 - x1) * 0.5f, -160.0f, 160.0f);
        (void)y1;
        (void)y2;   // across only: the tile layers draw 7 rows, nothing above or below
    }
    if (const char* t = std::getenv("MM_VIEW_TEST")) {   // test: a fixed offset "x,y"
        tx = static_cast<float>(std::atof(t));
        if (const char* c = std::strchr(t, ',')) ty = static_cast<float>(std::atof(c + 1));
    }
    if (const char* t = std::getenv("MM_VIEW_TEST")) {   // test: offset "x" every other second, no easing
        static long frames = 0;
        tx = (++frames / 60) % 2 ? static_cast<float>(std::atof(t)) : 0.0f;
        g_smooth_x = tx;
    }
    // Eased, so a rejoin (a jump of up to 250 px) glides instead of cutting.
    g_smooth_x += (tx - g_smooth_x) * 0.12f;
    g_smooth_y += (ty - g_smooth_y) * 0.12f;
    if (tx == 0.0f && ty == 0.0f && std::abs(g_smooth_x) < 0.05f && std::abs(g_smooth_y) < 0.05f) {
        g_smooth_x = g_smooth_y = 0.0f;
    }
    g_view_x.store(g_smooth_x, std::memory_order_relaxed);
    g_view_y.store(g_smooth_y, std::memory_order_relaxed);
    if (g_smooth_x == 0.0f && g_smooth_y == 0.0f) return;
    for (uint32_t mtx : {0x804600C0u, 0x804700C0u}) {
        shift_translation(rdram, mtx, 0, -g_smooth_x);
        shift_translation(rdram, mtx, 1, -g_smooth_y);
    }
}

// Thread_MainProc, just before the frame's graphics task goes to the RSP:
// the whole frame list (task data, from the arena base + 0x180) is final.
extern "C" void mm_far_shadow(uint8_t* rdram, recomp_context* ctx, uint32_t task);

extern "C" void mm_view_frame_dl(uint8_t* rdram, recomp_context* ctx, uint32_t task) {
    mm_far_shadow(rdram, ctx, task);
    const float off = g_view_x.load(std::memory_order_relaxed);
    const int dx4 = -static_cast<int>(std::lround(off * 4.0f));
    if (dx4 == 0 || task < 0x80000000u || task >= 0x80800000u) return;
    const uint32_t start = static_cast<uint32_t>(MEM_W(0x30, static_cast<gpr>(static_cast<int32_t>(task)))) | 0x80000000u;
    const uint32_t size = static_cast<uint32_t>(MEM_W(0x34, static_cast<gpr>(static_cast<int32_t>(task))));
    if (start < 0x80000000u || start >= 0x80800000u || size > 0x10000u) return;
    static const bool dbg = std::getenv("MM_VIEW_DEBUG") != nullptr;
    int moved = 0;
    for (uint32_t pc = start; pc < start + size; pc += 8) {
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(pc))));
        const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, static_cast<gpr>(static_cast<int32_t>(pc))));
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        if (op == 0xB8) break;
        if (op == 0x01 && (w0 & 0x00010000u) != 0 && (w1 & 0xFFFFu) == 0x0080u) break;   // the HUD's projection
        if (op == 0xE4 || op == 0xE5) {
            shift_texrect(rdram, pc, dx4);
            ++moved;
        }
        if (op == 0x06) {
            const uint32_t target = w1 | 0x80000000u;
            if (target >= 0x80400000u && target < 0x80418000u) shift_layer(rdram, target, dx4);
        }
    }
    if (dbg) std::fprintf(stderr, "[view] frame list %08X+%X: %d rectangles moved by %d\n", start, size, moved, dx4);
}

extern "C" void mm_camera_begin(uint8_t* rdram) {
    using namespace mm::player2;
    g_camera_swapped = false;
    if (mm::far::secondary_pass()) return;   // the second camera: its target is set up already
    if (!mm::cheats::coop_enabled()) return;
    const int p2 = slot(rdram);
    if (p2 < 0) return;
    // Far apart (far_coop.h): the game's camera follows player 1, or player
    // 2 in her machine's shadow frame; no midpoint.
    const bool far = mm::far::active(rdram);
    if (far && !mm::far::swapped()) return;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    const gpr b = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
    g_saved_x = MEM_W(0x88, m);
    g_saved_y = MEM_W(0x8C, m);
    g_saved_wx = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u)));
    g_saved_wy = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu)));
    g_mid_x = far ? MEM_W(0x88, b) : static_cast<int32_t>((static_cast<int64_t>(g_saved_x) + MEM_W(0x88, b)) / 2);
    g_mid_y = far ? MEM_W(0x8C, b) : static_cast<int32_t>((static_cast<int64_t>(g_saved_y) + MEM_W(0x8C, b)) / 2);
    MEM_W(0x88, m) = g_mid_x;
    MEM_W(0x8C, m) = g_mid_y;
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u))) = g_saved_wx + (g_mid_x - g_saved_x);
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu))) = g_saved_wy + (g_mid_y - g_saved_y);
    g_camera_swapped = true;
}

extern "C" void mm_camera_end(uint8_t* rdram) {
    if (!g_camera_swapped) return;
    g_camera_swapped = false;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    MEM_W(0x88, m) = g_saved_x + (MEM_W(0x88, m) - g_mid_x);
    MEM_W(0x8C, m) = g_saved_y + (MEM_W(0x8C, m) - g_mid_y);
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u))) = g_saved_wx;
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu))) = g_saved_wy;
}
