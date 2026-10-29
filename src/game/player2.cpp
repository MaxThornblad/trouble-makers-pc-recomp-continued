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
#include "frame_input.h"
#include "netplay.h"
#include "player2_internal.h"
#include "player2_mount.h"
#include "throw_cheats.h"


extern "C" void func_8004ED10(uint8_t* rdram, recomp_context* ctx);   // Marina's update(slot)

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
constexpr uint32_t kPadsRead = 0x8012AD88u;       // the same, Input_Update's first read
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
constexpr uint32_t kRequest = 0x800BE5F4u;
constexpr uint32_t kHistoryLen = 64;
constexpr std::array<Range, 12> kGlobals{{
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
    // Requests to Marina, taken up by her next update: what a star teleporter
    // asks of whoever holds it, and the flight her own update then asks of
    // herself. Shared, player 1 took up player 2's flight (and flew).
    // (Last, so the copy's earlier offsets stay as they were.)
    {kRequest, 4},
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
// 0x500 the second camera (far_coop.cpp), 0x600 the lead swap
// (player2_lead.cpp), 0x610 moon jump rises left per player (cheats.cpp),
// 0x620 throw charge and 0x628 pending throw strength per character
// and 0x630 aim per character (throw_cheats.cpp), 0x634 what she shook, 0x418 player 2's mount (player2_mount.cpp),
// 0x1000 display lists, 0x2000 textures, 0x4000 packed frame.
constexpr uint32_t kP2Globals = 0x807F0000u;
constexpr uint32_t kP2PrevButtons = kP2Globals + 0x400u;
constexpr uint32_t kP2JoinTimer = kP2Globals + 0x404u;     // frames P1 has been in play without P2
constexpr uint32_t kP2AwayTimer = kP2Globals + 0x408u;     // frames P2 has been too far from P1
constexpr uint32_t kP2FullHealth = kP2Globals + 0x40Cu;    // the most health either player has had (s16)
constexpr uint32_t kP2GrabCooldown = kP2Globals + 0x414u;  // frames until the players may grab each other again
constexpr uint32_t kP2Shaken = kP2Globals + 0x634u;   // slot + 1 of what she shook last frame (0: none)
constexpr uint32_t kShakenBit = 0x00020000u;          // +0x98: being shaken this frame

constexpr uint32_t kGameState = 0x800BE4F0u;   // 6 and 10 = stage gameplay
constexpr uint32_t kCameraX = 0x800BE558u, kCameraY = 0x800BE55Cu;
constexpr uint32_t kCameraPrevX = 0x800BE560u, kCameraPrevY = 0x800BE564u;   // the centre last frame
constexpr uint32_t kWorldX = 0x800BE5D8u, kWorldY = 0x800BE5DCu;   // Marina's world position
constexpr int kJoinDelay = 45;                 // frames into a scene
constexpr int kLeashX = 200, kLeashY = 150;    // px from the camera centre: out of view
constexpr int kLeashFrames = 150;
constexpr int kScreenEdge = 180;              // px either side of the shared camera (keep_on_screen)

struct State {
    int slot = -1;        // host-side cache only; slot() checks the slot itself
    bool joined = false;  // a (re)join since the last marker update
};
State g;

std::mutex g_marker_mutex;
Marker g_marker;

uint8_t* at(uint8_t* rdram, uint32_t vaddr) {
    return rdram + (vaddr - 0x80000000u);
}

// MM_P2_TRACE test lines (not from the shadow frame, mm::far).
bool trace_on() {
    static const bool on = std::getenv("MM_P2_TRACE") != nullptr;
    return on && !mm::faraway::in_shadow();
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

// Not a player's: the frame's matrix arena pointer (0x800EF4F4, inside the
// 0x800EF4F0 range), which flips between the two frame arenas each frame. A
// copy of it put back later points into the other frame's arena.
constexpr uint32_t kSharedWord = 0x800EF4F4u;

void copy_globals_in(uint8_t* rdram, const uint8_t* src) {
    for (const Range& r : kGlobals) {
        const int32_t keep = MEM_W(0, addr(kSharedWord));
        std::memcpy(at(rdram, r.start), src, r.size);
        MEM_W(0, addr(kSharedWord)) = keep;
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
    // One health bar for both: hers is player 1's (the HUD's).
    MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        std::max<int16_t>(1, MEM_H(0xE0, addr(kActors)));
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
    mm::faraway::reset(rdram);   // her own camera starts again from the game's
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] joined in slot %d\n", slot);
}

// Put player 2 back next to player 1: a fresh copy of her (as she joins),
// used after a defeat and when she has been left far behind.
void rejoin(uint8_t* rdram, int slot) {
    setup(rdram, slot);
}

void bring_next_to_lead(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (p2 > 0) rejoin(rdram, p2);
}

void bring_back(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    const int32_t dx = MEM_W(0x88, addr(kActors + static_cast<uint32_t>(p2) * kStride)) - MEM_W(0x88, addr(kActors));
    const int32_t dy = MEM_W(0x8C, addr(kActors + static_cast<uint32_t>(p2) * kStride)) - MEM_W(0x8C, addr(kActors));
    if (std::abs(dx) > (kScreenEdge << 16) || std::abs(dy) > (kLeashY << 16)) {
        std::fprintf(stderr, "[player2] far apart rules over: back next to player 1\n");
        rejoin(rdram, p2);
    }
}

// One shared screen. The game has one camera (the players' midpoint, see
// mm_camera_begin), and only what is near it is active and drawn, so the
// players can't leave it: across, each stays within kScreenEdge px of the
// camera centre (360 px apart at most), pushed back with the outward speed
// taken away, as in other shared-screen co-op games. Without it they ran off
// in opposite directions until the leash pulled player 2 back (a teleport).
// Each machine's own view then shows its player at most 180 px off the
// shared centre, well inside the widescreen picture and active area.

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
    if (mm::faraway::active(rdram)) return;   // far apart allowed: each has her own camera (far_coop.h)
    // On a mount (3-12's cats) the mount places its rider and the stage
    // keeps it in view; the clamp held player 1 at the edge while her cat
    // ran on (she looked gone: off her cat, behind player 2).
    if (mm::player2_mount::riding(rdram)) return;
    clamp_player(rdram, kActors, kWorldX);
    clamp_player(rdram, kActors + static_cast<uint32_t>(slot) * kStride, kP2Globals + global_offset(kWorldX));
}

// Player 2's colour: a cool blue added over Marina's own (the draw
// builder's prim-colour tint at +0x9C..+0x9F, func_80009BE8). Marina's update
// clears those bytes each frame and uses them for her hit flash, so the tint
// goes on only while she is not flashing.
void tint(uint8_t* rdram, int slot) {
    const gpr pa = addr(kActors + static_cast<uint32_t>(slot) * kStride);
    if (MEM_BU(0x9C, pa) == 0 && MEM_BU(0x9D, pa) == 0 && MEM_BU(0x9E, pa) == 0) {
        MEM_B(0x9C, pa) = 0x00;
        MEM_B(0x9D, pa) = 0x30;
        MEM_B(0x9E, pa) = 0x5A;
        MEM_B(0x9F, pa) = static_cast<int8_t>(0xFF);
    }
}

bool lead_swapped(uint8_t* rdram) {
    return MEM_W(0, addr(kLeadBlock)) != 0;
}

void after_actors(uint8_t* rdram) {
    if (lead_swapped(rdram) && slot(rdram) > 0) tint(rdram, 0);
}

// Player 2 as Marina for a moment (the second camera's pass, far_coop.cpp):
// her record in slot 0 and her globals in Marina's, player 1's kept aside.
// The camera reads more of Marina than her position (which way she faces and
// moves, for its look-ahead); with only her position it framed player 2 with
// player 1's, lagging behind her and settling to her one side.
namespace {
struct StandIn {
    bool on = false;
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, globals_size()> p1_globals;
};
StandIn g_stand_in;
}

void as_marina(uint8_t* rdram, const std::function<void(uint8_t*)>& fn) {
    const int p2 = slot(rdram);
    if (p2 <= 0 || g_stand_in.on || !mm::cheats::coop_enabled()) return;
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, globals_size()> p1_globals;
    std::memcpy(p1_record.data(), slot_ptr(rdram, 0), kStride);
    copy_globals_out(rdram, p1_globals.data());
    std::memcpy(slot_ptr(rdram, 0) + 0x80, slot_ptr(rdram, p2) + 0x80, kStride - 0x80);   // (not the render matrices)
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    copy_globals_in(rdram, at(rdram, kP2Globals));
    fn(rdram);
    // What it did is hers.
    std::memcpy(slot_ptr(rdram, p2) + 0x80, slot_ptr(rdram, 0) + 0x80, kStride - 0x80);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(p2) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    copy_globals_out(rdram, at(rdram, kP2Globals));
    std::memcpy(slot_ptr(rdram, 0), p1_record.data(), kStride);
    copy_globals_in(rdram, p1_globals.data());
}

void stand_in_begin(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (p2 <= 0 || g_stand_in.on) return;
    g_stand_in.on = true;
    std::memcpy(g_stand_in.p1_record.data(), slot_ptr(rdram, 0), kStride);
    copy_globals_out(rdram, g_stand_in.p1_globals.data());
    std::memcpy(slot_ptr(rdram, 0) + 0x80, slot_ptr(rdram, p2) + 0x80, kStride - 0x80);   // (not the render matrices)
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    // Not what she carries: something in the camera's pass moves Marina's
    // held object to her hands, and her hands here are in the other camera's
    // terms. The block player 2 carried jumped ahead each frame and her carry
    // code ran after it (super speed).
    MEM_W(0x80, addr(kActors)) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, addr(kActors))) & ~kHoldingFlag);
    MEM_H(0xD6, addr(kActors)) = 0;
    copy_globals_in(rdram, at(rdram, kP2Globals));
}

void stand_in_end(uint8_t* rdram) {
    if (!g_stand_in.on) return;
    g_stand_in.on = false;
    std::memcpy(slot_ptr(rdram, 0), g_stand_in.p1_record.data(), kStride);
    copy_globals_in(rdram, g_stand_in.p1_globals.data());
}

// Player 1's and player 2's controller entries trade places (Input_Update,
// while player 2 leads): the game reads player 2's as player 1's.
void route_pads(uint8_t* rdram) {
    const int p1pad = MEM_W(0, addr(0x801370C0u)), p2pad = p2_pad_index(rdram);
    if (p1pad < 0 || p1pad >= 4 || p2pad < 0) return;
    for (uint32_t base : {kPads, kPadsRead}) {
        for (uint32_t i = 0; i < 6; ++i) {
            const gpr x = addr(base + static_cast<uint32_t>(p1pad) * 6u + i), y = addr(base + static_cast<uint32_t>(p2pad) * 6u + i);
            const int8_t t = MEM_B(0, x);
            MEM_B(0, x) = MEM_B(0, y);
            MEM_B(0, y) = t;
        }
    }
}

bool second_controller_acting(uint8_t* rdram) {
    const int p2pad = p2_pad_index(rdram);
    if (p2pad < 0) return false;
    const gpr e = addr(kPads + static_cast<uint32_t>(p2pad) * 6u);
    const uint16_t pressed = static_cast<uint16_t>(MEM_HU(0, e) & ~MEM_HU(0, addr(kP2PrevButtons)));
    return pressed != 0 || MEM_B(3, e) > 40;   // a new button, or the stick up
}

void mute_second_controller(uint8_t* rdram) {
    const int p2pad = p2_pad_index(rdram);
    if (p2pad < 0) return;
    for (uint32_t base : {kPads, kPadsRead}) {
        const gpr e = addr(base + static_cast<uint32_t>(p2pad) * 6u);
        MEM_H(0, e) = 0;
        MEM_B(2, e) = 0;
        MEM_B(3, e) = 0;
    }
}

void exchange_characters(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    const gpr m = addr(kActors), b = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    // Records (camera-relative positions both, against the same camera).
    std::array<uint8_t, kStride> tmp;
    std::memcpy(tmp.data(), slot_ptr(rdram, 0), kStride);
    std::memcpy(slot_ptr(rdram, 0), slot_ptr(rdram, p2), kStride);
    std::memcpy(slot_ptr(rdram, p2), tmp.data(), kStride);
    MEM_H(0xD2, m) = static_cast<int16_t>(kMarinaType);
    MEM_H(0xD2, b) = static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    // One carrying the other: the held slot follows.
    for (gpr r : {m, b}) {
        if ((static_cast<uint32_t>(MEM_W(0x80, r)) & kHoldingFlag) == 0) continue;
        const uint16_t h = MEM_HU(0xD6, r);
        if (h == 0) MEM_H(0xD6, r) = static_cast<int16_t>(p2);
        else if (h == p2) MEM_H(0xD6, r) = 0;
    }
    // Marina's globals and player 2's copy.
    std::array<uint8_t, globals_size()> mine;
    copy_globals_out(rdram, mine.data());
    copy_globals_in(rdram, at(rdram, kP2Globals));
    std::memcpy(at(rdram, kP2Globals), mine.data(), mine.size());
    // This frame's controls, read with the old pairing: Input_Update left
    // player 1's buttons in 0x800BE538 (next frame's "previous") and her
    // hold / press words; player 2's previous buttons are her own. Each
    // keeps this frame's presses.
    const uint16_t raw1 = MEM_HU(0, addr(0x800BE538u));
    const uint16_t press1 = MEM_HU(0, addr(kP2Globals + global_offset(kPress)));   // (globals already exchanged)
    const uint16_t prev2 = MEM_HU(0, addr(kP2PrevButtons));
    const int p1pad = MEM_W(0, addr(0x801370C0u)), p2pad = p2_pad_index(rdram);
    if (p1pad >= 0 && p1pad < 4 && p2pad >= 0) {
        route_pads(rdram);
        const gpr pad = addr(kPads + static_cast<uint32_t>(p1pad) * 6u);
        const uint16_t raw2 = MEM_HU(0, pad);
        MEM_H(0, addr(0x801370C4u)) = static_cast<int16_t>(raw2);
        MEM_B(0, addr(0x800BE53Cu)) = MEM_B(2, pad);
        MEM_B(0, addr(0x800BE540u)) = MEM_B(3, pad);
        MEM_H(0, addr(kHold)) = static_cast<int16_t>(raw2);
        MEM_H(0, addr(kPress)) = static_cast<int16_t>(raw2 & ~prev2);
        MEM_H(0, addr(0x800BE538u)) = static_cast<int16_t>(raw2);
        // Player 1's next update in player 2's slot finds this frame's press.
        MEM_H(0, addr(kP2PrevButtons)) = static_cast<int16_t>(raw1 & ~press1);
    }
    mm::faraway::exchange_cameras(rdram);
}


// What player 2 carries talks to "Marina": slot 0's record, Marina's
// globals (her position where enemies aim, her player-state block, her input
// words) and controller 1. While it updates, player 2 is all of that: a
// carried handle (type 0x5F) went to player 1's hands when she threw
// something, and the star teleporter of 1-1 (type 0x32: grab it and shake)
// only flew whoever was in slot 0. Player 1 waits in player 2's slot.
namespace {
struct HeldSwap {
    bool on = false;
    int p2 = -1;
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, kStride> p1_waiting;
    std::array<uint8_t, globals_size()> p1_globals;
    uint8_t pad0[6];
};
HeldSwap g_held;
}

void held_swap_end(uint8_t* rdram) {
    if (!g_held.on) return;
    g_held.on = false;
    const int p2 = g_held.p2;
    std::memcpy(g_held.p1_waiting.data(), slot_ptr(rdram, p2), kStride);
    // Player 2 (slot 0) back to her slot, her globals back to her copy.
    std::memcpy(slot_ptr(rdram, p2), slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(p2) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    copy_globals_out(rdram, at(rdram, kP2Globals));
    // Player 1 back, from where she waited: her record there was rebased
    // with every change of camera since (far_coop.cpp), the copy taken at
    // the start was not (she landed a camera's difference away, 156 px).
    std::memcpy(slot_ptr(rdram, 0), g_held.p1_waiting.data(), kStride);
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    copy_globals_in(rdram, g_held.p1_globals.data());
    for (int i = 0; i < 6; ++i) MEM_B(i, addr(kPads)) = static_cast<int8_t>(g_held.pad0[i]);
}

void held_swap_begin(uint8_t* rdram, int actor) {
    held_swap_end(rdram);
    if (!mm::cheats::coop_enabled()) return;
    // Shaken by player 2 last frame (update_core): the object sees it now.
    if (MEM_W(0, addr(kP2Shaken)) == actor + 1) {
        const gpr o = addr(kActors + static_cast<uint32_t>(actor) * kStride);
        MEM_W(0x98, o) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x98, o)) | kShakenBit);
        MEM_W(0, addr(kP2Shaken)) = 0;
    }
    const int p2 = slot(rdram);
    if (p2 <= 0 || actor == p2 || actor == 0) return;
    const gpr b = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    if ((static_cast<uint32_t>(MEM_W(0x80, b)) & kHoldingFlag) == 0 || MEM_HU(0xD6, b) != actor) return;
    g_held.on = true;
    g_held.p2 = p2;
    std::memcpy(g_held.p1_record.data(), slot_ptr(rdram, 0), kStride);
    copy_globals_out(rdram, g_held.p1_globals.data());
    for (int i = 0; i < 6; ++i) g_held.pad0[i] = MEM_BU(i, addr(kPads));
    std::memcpy(slot_ptr(rdram, 0), slot_ptr(rdram, p2), kStride);
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    copy_globals_in(rdram, at(rdram, kP2Globals));
    const int p2pad = p2_pad_index(rdram);
    for (int i = 0; i < 6; ++i) {
        MEM_B(i, addr(kPads)) = p2pad >= 0 ? MEM_BU(i, addr(kPads + static_cast<uint32_t>(p2pad) * 6u)) : 0;
    }
    std::memcpy(slot_ptr(rdram, p2), g_held.p1_record.data(), kStride);
    MEM_H(0xD2, b) = static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
}

// Gems: stage gems are item records the game collects against Marina's
// hitbox only (func_800446F0 from func_800451E4, see cheats.cpp's magnet),
// gems knocked out of enemies are type-8 actors caught the same way. One
// touched by player 2 goes to player 1's hitbox, where the game collects it
// for the team (the HUD's count, the shared health).
void collect_pickups(uint8_t* rdram, uint32_t list) {
    constexpr uint32_t kItemHandlers = 0x800D37BCu, kGemHandler = 0x800446F0u;
    constexpr uint16_t kGemActor = 8;
    constexpr int kSlack = 6;                  // the gem box, +-6 px
    if (!mm::cheats::coop_enabled()) return;
    const uint16_t state = MEM_HU(0, addr(kGameState));
    if (state != 6 && state != 10) return;
    const int p2 = slot(rdram);
    if (p2 <= 0 || MEM_W(0x80, addr(kActors)) == 0) return;
    const gpr b = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    const gpr m = addr(kActors);
    // Boxes in camera-relative px (+y up).
    const int bx = MEM_H(0x88, b), by = MEM_H(0x8C, b);
    const int left = bx + MEM_H(0xAA, b) - kSlack, right = bx + MEM_H(0xAC, b) + kSlack;
    const int bottom = by + MEM_H(0xB0, b) - kSlack, top = by + MEM_H(0xAE, b) + kSlack;
    const int tx = MEM_H(0x88, m) + (MEM_H(0xAA, m) + MEM_H(0xAC, m)) / 2;
    const int ty = MEM_H(0x8C, m) + (MEM_H(0xAE, m) + MEM_H(0xB0, m)) / 2;
    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    static const bool dbg = std::getenv("MM_GEM_DEBUG") != nullptr;
    if (dbg) {
        static int n = 0;
        if ((n++ % 120) == 0) {
            int gems = 0, best = 1 << 30, bxw = 0, byw = 0;
            for (int i = 0; i < 512; ++i) {
                const gpr rec = addr(list + static_cast<uint32_t>(i) * 6u);
                const uint16_t kind = MEM_HU(0, rec);
                if (kind == 0) break;
                if ((kind & 0xC000u) != 0) continue;
                if (static_cast<uint32_t>(MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers))) != kGemHandler) continue;
                ++gems;
                const int d = std::abs(MEM_HU(2, rec) - (bx + cx)) + std::abs(MEM_HU(4, rec) - (by + cy));
                if (d < best) { best = d; bxw = MEM_HU(2, rec); byw = MEM_HU(4, rec); }
            }
            std::fprintf(stderr, "[gems] %d in the list; nearest to p2 (%d,%d) at %d,%d\n", gems, bx + cx, by + cy, bxw, byw);
        }
    }
    for (int i = 0; i < 512 && list != 0; ++i) {
        const gpr rec = addr(list + static_cast<uint32_t>(i) * 6u);
        const uint16_t kind = MEM_HU(0, rec);
        if (kind == 0) break;
        if ((kind & 0xC000u) != 0) continue;
        if (static_cast<uint32_t>(MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers))) != kGemHandler) continue;
        const int x = MEM_HU(2, rec) - cx, y = MEM_HU(4, rec) - cy;
        if (x < left || x > right || y < bottom || y > top) continue;
        MEM_H(2, rec) = static_cast<int16_t>(tx + cx);
        MEM_H(4, rec) = static_cast<int16_t>(ty + cy);
        if (trace_on()) std::fprintf(stderr, "[p2trace] gem %03X at %d,%d goes to player 1\n", kind, x + cx, y + cy);
    }
    for (int s = 1; s < 0xD0; ++s) {
        const gpr a = addr(kActors + static_cast<uint32_t>(s) * kStride);
        if (MEM_W(0x80, a) == 0 || MEM_HU(0xD2, a) != kGemActor) continue;
        const int x = MEM_H(0x88, a), y = MEM_H(0x8C, a);
        if (x < left || x > right || y < bottom || y > top) continue;
        MEM_W(0x88, a) = tx << 16;
        MEM_W(0x8C, a) = ty << 16;
        MEM_W(0xEC, a) = 0;
        MEM_W(0xF0, a) = 0;
        if (trace_on()) std::fprintf(stderr, "[p2trace] gem actor %d goes to player 1\n", s);
    }
}

void request(uint8_t* rdram, uint32_t word) {
    if (slot(rdram) <= 0) return;
    MEM_W(0, addr(kP2Globals + global_offset(kRequest))) = static_cast<int32_t>(word);
}

void place_test(uint8_t* rdram, int32_t x, int32_t y) {
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    const gpr b = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    MEM_W(0x88, b) = x - MEM_W(0, addr(kCameraX));
    MEM_W(0x8C, b) = y - MEM_W(0, addr(kCameraY));
    MEM_W(0, addr(kP2Globals + global_offset(kWorldX))) = x;
    MEM_W(0, addr(kP2Globals + global_offset(kWorldY))) = y;
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
int g_marina_character = 0;   // whose update func_8004ED10 runs: 0 slot 0's character, 1 player 2's slot's
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
        if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] defeated, rejoining\n");
        rejoin(rdram, slot);
    }

    // Player 1 aside.
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, globals_size()> p1_globals;
    std::memcpy(p1_record.data(), slot_ptr(rdram, 0), kStride);
    uint32_t p1_flags;
    std::memcpy(&p1_flags, p1_record.data() + 0x80, 4);   // host-endian word, as MEM_W stores it
    copy_globals_out(rdram, p1_globals.data());

    // Player 2 in, with the shared health (player 1's: the HUD's bar).
    std::memcpy(slot_ptr(rdram, 0), slot_ptr(rdram, slot), kStride);
    int16_t shared_health;
    std::memcpy(&shared_health, p1_record.data() + 0xE0, 2);
    if (shared_health > 0) MEM_H(0xE0, addr(kActors)) = shared_health;
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
    // The camera's step this frame (centre minus previous centre,
    // 0x800BE558 - 0x800BE560, and y) is zero to her. Marina's carry code
    // adds it to her position, making up for a scroll the game spares slot 0
    // there; player 2 is scrolled as every other actor (func_80016D94) and
    // rebased between cameras (far_coop.cpp), so she got it twice: carrying
    // something, she ran ahead of her camera, it followed, she ran further
    // (super speed, super jumps).
    // That is her camera's step (far apart). On the shared camera, carrying
    // player 1, the step is the one the carry makes (the camera follows
    // player 1, who is in her hands), and the carry code needs it as it
    // does for Marina: zeroed, she walked at half speed once the camera
    // moved (normal at a stage's left edge, where it does not).
    const int32_t prev_x = MEM_W(0, addr(kCameraPrevX)), prev_y = MEM_W(0, addr(kCameraPrevY));
    const bool own_step = holding_p1 && !mm::faraway::active(rdram);
    if (!own_step) {
        MEM_W(0, addr(kCameraPrevX)) = MEM_W(0, addr(kCameraX));
        MEM_W(0, addr(kCameraPrevY)) = MEM_W(0, addr(kCameraY));
    }
    g_in_own_call = true;
    g_marina_character = 1;
    func_8004ED10(rdram, ctx);
    g_marina_character = 0;
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
            if (!own_step) {
                MEM_W(0, addr(kCameraPrevX)) = MEM_W(0, addr(kCameraX));
                MEM_W(0, addr(kCameraPrevY)) = MEM_W(0, addr(kCameraY));
            }
            g_in_own_call = true;
            g_marina_character = 1;
            func_8004ED10(rdram, ctx);
            g_marina_character = 0;
            g_in_own_call = false;
        }
    }
    MEM_W(0, addr(kCameraPrevX)) = prev_x;
    MEM_W(0, addr(kCameraPrevY)) = prev_y;
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
        // Still carried: she is where player 2's hands are (the hold point
        // Marina's grab code leaves at +0x104 / +0x108). On the pickup frame
        // the code above can leave her a long way off for a frame (picked
        // up while carrying something herself: 95 px, a visible flicker).
        if ((static_cast<uint32_t>(MEM_W(0x80, addr(kActors))) & kHoldingFlag) != 0 && MEM_HU(0xD6, addr(kActors)) == 0) {
            int32_t hx, hy, x, y;
            std::memcpy(&hx, p1_record.data() + 0x104, 4);
            std::memcpy(&hy, p1_record.data() + 0x108, 4);
            std::memcpy(&x, p1_record.data() + 0x88, 4);
            std::memcpy(&y, p1_record.data() + 0x8C, 4);
            const int32_t p2x = MEM_W(0x88, addr(kActors));
            const bool hold_sane = std::abs(hx - p2x) < (48 << 16) && std::abs(hy - MEM_W(0x8C, addr(kActors))) < (64 << 16);
            if (hold_sane && (std::abs(x - hx) > (12 << 16) || std::abs(y - hy) > (16 << 16))) {
                std::memcpy(p1_record.data() + 0x88, &hx, 4);
                std::memcpy(p1_record.data() + 0x8C, &hy, 4);
            }
        }
    }

    // Shaking what she holds (func_8004FDF8) sets its +0x98 bit 0x20000,
    // which its own update reads (a Clancer drops a gem, func_8006E4EC) and
    // the end of the frame clears (func_8001FF50). Marina shakes before the
    // actor loop; player 2 shakes in it, after anything in a lower slot has
    // updated, so for that the bit came and went unseen. It is kept for the
    // object's update next frame (held_swap_begin).
    {
        const int held = MEM_HU(0xD6, addr(kActors));
        const bool holding = (static_cast<uint32_t>(MEM_W(0x80, addr(kActors))) & kHoldingFlag) != 0;
        const bool shaken = holding && held > 0 && held < slot && held < 0xD0 &&
                            (static_cast<uint32_t>(MEM_W(0x98, addr(kActors + static_cast<uint32_t>(held) * kStride))) & kShakenBit) != 0;
        MEM_W(0, addr(kP2Shaken)) = shaken ? held + 1 : 0;
    }

    // Player 2 out, player 1 back.
    std::memcpy(slot_ptr(rdram, slot), slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    if (!lead_swapped(rdram)) tint(rdram, slot);
    copy_globals_out(rdram, at(rdram, kP2Globals));
    // What her update did to the shared health (a hit, a gem) is player 1's too.
    if (shared_health > 0) {
        const int16_t after = MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride));
        const int16_t shared = std::max<int16_t>(0, after);
        std::memcpy(p1_record.data() + 0xE0, &shared, 2);
    }
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
    // One health bar: hers is player 1's again. (Player 1's update just
    // above, thrown by her, wrote a value of its own into her health field:
    // 30, kept from then on, so her tag showed an all but empty bar.)
    if (MEM_H(0xE0, addr(kActors)) > 0) {
        MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = MEM_H(0xE0, addr(kActors));
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
        // The tag goes over player 2's character: her slot, or slot 0 while
        // she leads.
        const bool lead = lead_swapped(rdram);
        const gpr slot0 = addr(kActors), other = addr(kActors + static_cast<uint32_t>(p2) * kStride);
        const gpr hers = lead ? slot0 : other;
        // Positions in this machine's picture: the second camera's (player 2's
        // slot) when it shows that one (far_coop.h).
        int32_t dx = 0, dy = 0;
        const bool far_apart = mm::faraway::p2_camera_offset(rdram, dx, dy);
        // (Split screen: both views, positions in the game camera's; the
        // overlay shifts them for the second view, split_view.h.)
        const bool split = mm::faraway::split_local();
        const bool second_view = far_apart && mm::faraway::shows_second_view(rdram) && !split;
        const auto view_x = [&](gpr a) { return (MEM_W(0x88, a) - (second_view ? dx : 0)) >> 16; };
        const auto view_y = [&](gpr a) { return (MEM_W(0x8C, a) - (second_view ? dy : 0)) >> 16; };
        m.visible = true;
        m.x = view_x(hers);
        m.top = view_y(hers) + MEM_H(0xAE, hers);
        m.health = MEM_H(0xE0, hers);
        m.max_health = MEM_H(0, addr(kP2FullHealth));
        g.joined = false;
        if (far_apart && !split) {
            // The other character than the one this machine's camera follows.
            const gpr partner = second_view ? slot0 : other;
            m.partner = true;
            m.partner_person = partner == hers ? 2 : 1;
            m.px = view_x(partner);
            m.py = view_y(partner);
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
    if (std::getenv("MM_P2_TRACE")) {   // test: both players each frame, by frame number (a rollback goes back)
        const long n = mm::frame_input::number();
        const gpr m = addr(kActors);
        const gpr b = p2 >= 0 ? addr(kActors + static_cast<uint32_t>(p2) * kStride) : m;
        const auto held_ok = [&](gpr who) {
            if ((static_cast<uint32_t>(MEM_W(0x80, who)) & kHoldingFlag) == 0) return 1;
            const uint16_t h = MEM_HU(0xD6, who);
            return h < 0xD0 && MEM_W(0x80, addr(kActors + static_cast<uint32_t>(h) * kStride)) != 0 ? 1 : 0;
        };
        for (int who = 0; who < 2; ++who) {
            const gpr c = who == 0 ? m : b;
            if ((who == 1 && p2 < 0) || (static_cast<uint32_t>(MEM_W(0x80, c)) & kHoldingFlag) == 0) continue;
            const uint16_t h = MEM_HU(0xD6, c);
            if (h >= 0xD0) continue;
            const gpr ha = addr(kActors + static_cast<uint32_t>(h) * kStride);
            std::fprintf(stderr, "[p2trace] p%d holds %02X type %04X st %04X at %d,%d fl %08X\n", who + 1, h, MEM_HU(0xD2, ha),
                         MEM_HU(0xD0, ha), MEM_W(0x88, ha) >> 16, MEM_W(0x8C, ha) >> 16, static_cast<uint32_t>(MEM_W(0x80, ha)));
        }
        std::fprintf(stderr, "[p2trace] held_ok %d %d hp %d %d full %d\n", held_ok(m), p2 >= 0 ? held_ok(b) : 1,
                     MEM_H(0xE0, m), p2 >= 0 ? MEM_H(0xE0, b) : 0, MEM_H(0, addr(kP2FullHealth)));
        std::fprintf(stderr, "[p2trace] %ld cam %d,%d p1 %d,%d st %04X fl %08X hold %04X | p2(%d) %d,%d st %04X fl %08X hold %04X\n",
                     n, MEM_W(0, addr(kCameraX)) >> 16, MEM_W(0, addr(kCameraY)) >> 16,
                     MEM_W(0x88, m) >> 16, MEM_W(0x8C, m) >> 16, MEM_HU(0xD0, m), static_cast<uint32_t>(MEM_W(0x80, m)), MEM_HU(0xD6, m),
                     p2, MEM_W(0x88, b) >> 16, MEM_W(0x8C, b) >> 16, MEM_HU(0xD0, b), static_cast<uint32_t>(MEM_W(0x80, b)), MEM_HU(0xD6, b));
    }
    publish_marker(rdram, p2);
    mm::player2_mount::tick(rdram);
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
    if (int32_t dx, dy; mm::faraway::p2_camera_offset(rdram, dx, dy)) {
        px = (MEM_W(0x88, a) - dx) >> 16;
        py = (MEM_W(0x8C, a) - dy) >> 16;
    }
    // Not while player 1 is out of the game's view as well: a scripted
    // camera (a boss's introduction, as Migen's) looks away from both.
    const gpr m = addr(kActors);
    const int mx = MEM_H(0x88, m), my = MEM_H(0x8C, m);
    const bool p1_away = mx > kLeashX || mx < -kLeashX || my > kLeashY || my < -kLeashY;
    // (On a mount the stage keeps both riders in view itself, up to ~230 px out.)
    const bool away = !p1_away && !mm::player2_mount::riding(rdram) && (px > kLeashX || px < -kLeashX || py > kLeashY || py < -kLeashY);
    int32_t away_frames = away ? MEM_W(0, addr(kP2AwayTimer)) + 1 : 0;
    if (away_frames >= kLeashFrames) {
        std::fprintf(stderr, "[player2] too far from player 1, rejoining\n");
        rejoin(rdram, p2);
        away_frames = 0;
    }
    MEM_W(0, addr(kP2AwayTimer)) = away_frames;
}


Marker current_marker() {
    std::lock_guard lock{g_marker_mutex};
    return g_marker;
}

} // namespace mm::player2


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
    if (!g_in_own_call) {
        const int p2 = slot(rdram);
        const gpr b = addr(kActors + static_cast<uint32_t>(p2 > 0 ? p2 : 0) * kStride);
        if (p2 > 0 && (static_cast<uint32_t>(MEM_W(0x80, b)) & kHoldingFlag) != 0 && MEM_HU(0xD6, b) == 0) {
            g_p1_deferred = true;
            return 1;
        }
    }
    // The update runs: its character's input is in Marina's words now.
    // (In player 2's picture pass too: all it changes is game memory, which
    // that pass puts back, and the picture then shows the same throw.)
    mm::throw_cheats::before_marina_update(rdram, g_marina_character);
    return 0;
}
