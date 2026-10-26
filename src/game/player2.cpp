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
constexpr uint32_t kGrabbableFlag = 0x1000u;     // +0x80: Marina's grab can take this actor   // func_80020024 skips Marina's update while nonzero

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
constexpr uint32_t kP2Globals = 0x807F0000u;
constexpr uint32_t kP2PrevButtons = kP2Globals + 0x400u;
constexpr uint32_t kP2JoinTimer = kP2Globals + 0x404u;     // frames P1 has been in play without P2
constexpr uint32_t kP2AwayTimer = kP2Globals + 0x408u;     // frames P2 has been too far from P1

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
};
std::mutex g_marker_mutex;
Marker g_marker;

uint8_t* at(uint8_t* rdram, uint32_t vaddr) {
    return rdram + (vaddr - 0x80000000u);
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
    x += ((MEM_W(0x80, addr(kActors)) & 0x20) != 0 ? 24 : -24) << 16;
    std::memcpy(p + 0x88, &x, 4);
    MEM_W(0x80, addr(kActors + static_cast<uint32_t>(slot) * kStride)) &= ~(0x02000000 | kGrabbableFlag);   // not holding anything; not grabbable
    MEM_H(0xD6, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = 0;
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
    std::fprintf(stderr, "[player2] joined in slot %d\n", slot);
}

// Put player 2 back next to player 1: a fresh copy of her (as she joins),
// used after a defeat and when she has been left far behind.
void rejoin(uint8_t* rdram, int slot) {
    setup(rdram, slot);
}

void update_core(uint8_t* rdram, recomp_context* ctx, int slot) {
    g.slot = slot;
    if (MEM_HU(0, addr(kMarinaPaused)) != 0) return;   // the game is not running Marina either
    // Defeated: she comes straight back by player 1 instead of running
    // Marina's own defeat (which would end the game for both).
    if (MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) <= 0) {
        std::fprintf(stderr, "[player2] defeated, rejoining\n");
        rejoin(rdram, slot);
    }

    // Player 1 aside.
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, globals_size()> p1_globals;
    std::memcpy(p1_record.data(), slot_ptr(rdram, 0), kStride);
    uint32_t p1_flags;
    std::memcpy(&p1_flags, p1_record.data() + 0x80, 4);   // host-endian word, as MEM_W stores it
    copy_globals_out(rdram, p1_globals.data());

    // Player 2 in. (With Marina's "can be grabbed" flag, kept off her own
    // slot, see below.)
    std::memcpy(slot_ptr(rdram, 0), slot_ptr(rdram, slot), kStride);
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    MEM_W(0x80, addr(kActors)) = static_cast<int32_t>(
        static_cast<uint32_t>(MEM_W(0x80, addr(kActors))) | (p1_flags & kGrabbableFlag));
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
    if (std::getenv("MM_P2_TRACE") && buttons != 0) {
        std::fprintf(stderr, "[p2trace] p2 input %04X (pads %04X/%02X %04X/%02X, p1 index %d)\n", buttons,
                     MEM_HU(0, addr(kPads)), MEM_BU(4, addr(kPads)), MEM_HU(0, addr(kPads + 6)), MEM_BU(4, addr(kPads + 6)),
                     MEM_W(0, addr(0x801370C0u)));
    }

    if (std::getenv("MM_P2_TRACE")) {
        std::fprintf(stderr, "[p2trace] p2 in: st %04X 98 %08X 94 %04X D4 %04X DA %08X\n", MEM_HU(0xD0, addr(kActors)),
                     static_cast<uint32_t>(MEM_W(0x98, addr(kActors))), MEM_HU(0x94, addr(kActors)), MEM_HU(0xD4, addr(kActors)),
                     static_cast<uint32_t>(MEM_W(0xD8, addr(kActors))));
    }
    // Marina's update, as func_80020024 calls it.
    const gpr saved_a0 = ctx->r4;
    const uint16_t state_before = MEM_HU(0xD0, addr(kActors));
    ctx->r4 = 0;
    func_8004ED10(rdram, ctx);
    ctx->r4 = saved_a0;
    if (std::getenv("MM_P2_TRACE") && MEM_HU(0xD0, addr(kActors)) != state_before) {
        std::fprintf(stderr, "[p2trace] p2 state %04X -> %04X (98 %08X, input %04X/%04X, 70CC %08X %08X)\n", state_before,
                     MEM_HU(0xD0, addr(kActors)), static_cast<uint32_t>(MEM_W(0x98, addr(kActors))), MEM_HU(0, addr(kHold)), MEM_HU(0, addr(kPress)),
                     static_cast<uint32_t>(MEM_W(0, addr(0x801370CCu))), static_cast<uint32_t>(MEM_W(0, addr(0x801370D0u))));
    }

    for (int i = 0; i < 6; ++i) MEM_B(i, addr(kPads)) = static_cast<int8_t>(pad0[i]);

    // Player 2 out, player 1 back.
    std::memcpy(slot_ptr(rdram, slot), slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    // Players don't pick each other up: Marina's grab takes any actor whose
    // +0x80 has 0x1000, and player 2, a copy of Marina's record, had it. The
    // grab put her in the held state while her own Marina code kept running;
    // she slid off, held "herself" (slot 0 in her update) and after the throw
    // jumped on the spot for good. Her slot never carries the flag.
    MEM_W(0x80, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = static_cast<int32_t>(
        static_cast<uint32_t>(MEM_W(0x80, addr(kActors + static_cast<uint32_t>(slot) * kStride))) & ~kGrabbableFlag);
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
    copy_globals_in(rdram, p1_globals.data());
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
        m.health = MEM_H(0xE0, a);
        // Full health is what she joined with (a copy of player 1's); grows if she heals past it.
        static int full = 0;
        if (m.health > full || g.joined) full = m.health;
        g.joined = false;
        m.max_health = full;
    }
    std::lock_guard lock{g_marker_mutex};
    g_marker = m;
}

void tick(uint8_t* rdram) {
    const int p2 = slot(rdram);
    if (std::getenv("MM_P2_TRACE")) {   // test: both players each frame
        static long n = 0;
        const gpr m = addr(kActors);
        const gpr b = p2 >= 0 ? addr(kActors + static_cast<uint32_t>(p2) * kStride) : m;
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
    const int px = MEM_H(0x88, a), py = MEM_H(0x8C, a);
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

// Player 2's grab must not take player 1 either. func_8001EC1C (once a
// frame, from func_80020024) resolves contacts between actors: a grab box
// (+0x80 & 0x80, here player 2 reaching out) takes an actor flagged 0x1000,
// and Marina has that flag. Player 2 would hold "slot 0", which in her own
// update is herself: she slid away, shaking. While she reaches out, player
// 1's 0x1000 is off for the routine.
namespace {
bool g_contacts_cleared = false;
}

extern "C" void mm_p2_contacts_begin(uint8_t* rdram) {
    using namespace mm::player2;
    g_contacts_cleared = false;
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    const gpr b = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    if ((static_cast<uint32_t>(MEM_W(0x80, b)) & 0x80u) == 0) return;
    const uint32_t flags = static_cast<uint32_t>(MEM_W(0x80, m));
    if ((flags & 0x1000u) == 0) return;
    MEM_W(0x80, m) = static_cast<int32_t>(flags & ~0x1000u);
    g_contacts_cleared = true;
}

extern "C" void mm_p2_contacts_end(uint8_t* rdram) {
    if (!g_contacts_cleared) return;
    g_contacts_cleared = false;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    MEM_W(0x80, m) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, m)) | 0x1000u);
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

extern "C" void mm_view_end(uint8_t* rdram) {
    using namespace mm::player2;
    const int own = mm::netplay::active() ? mm::netplay::player() : 0;
    const int p2 = own != 0 ? slot(rdram) : -1;
    float tx = 0.0f, ty = 0.0f;
    if (p2 > 0) {
        const gpr a1 = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
        const gpr a2 = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
        const float x1 = MEM_W(0x88, a1) / 65536.0f, y1 = MEM_W(0x8C, a1) / 65536.0f;
        const float x2 = MEM_W(0x88, a2) / 65536.0f, y2 = MEM_W(0x8C, a2) / 65536.0f;
        tx = std::clamp((own == 1 ? x1 - x2 : x2 - x1) * 0.5f, -160.0f, 160.0f);
        ty = std::clamp((own == 1 ? y1 - y2 : y2 - y1) * 0.5f, -100.0f, 100.0f);
    }
    if (const char* t = std::getenv("MM_VIEW_TEST")) {   // test: a fixed offset "x,y"
        tx = static_cast<float>(std::atof(t));
        if (const char* c = std::strchr(t, ',')) ty = static_cast<float>(std::atof(c + 1));
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

extern "C" void mm_camera_begin(uint8_t* rdram) {
    using namespace mm::player2;
    g_camera_swapped = false;
    if (!mm::cheats::coop_enabled()) return;
    const int p2 = slot(rdram);
    if (p2 < 0) return;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    const gpr b = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
    g_saved_x = MEM_W(0x88, m);
    g_saved_y = MEM_W(0x8C, m);
    g_saved_wx = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u)));
    g_saved_wy = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu)));
    g_mid_x = static_cast<int32_t>((static_cast<int64_t>(g_saved_x) + MEM_W(0x88, b)) / 2);
    g_mid_y = static_cast<int32_t>((static_cast<int64_t>(g_saved_y) + MEM_W(0x8C, b)) / 2);
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
