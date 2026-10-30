#include "far_coop.h"
#include "split_view.h"
#include "state_registry.h"
#include "replay.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "cheats.h"
#include "netplay.h"
#include "player2.h"
#include "savestate.h"
#ifdef MM_HAS_GRAPHICS
#include "mm_graphics.h"
#endif

extern "C" void YellowGem_GetFlag(uint8_t* rdram, recomp_context* ctx);     // a0: stage; nonzero: the save has its golden gem
extern "C" void func_80012830(uint8_t* rdram, recomp_context* ctx);          // the camera
extern "C" void func_8001DE30(uint8_t* rdram, recomp_context* ctx);          // map actor prefetch
extern "C" void Graphics_Begin_Frame(uint8_t* rdram, recomp_context* ctx);   // a whole frame (a0: buffer)
extern "C" void func_80022470(uint8_t* rdram, recomp_context* ctx);          // the HUD health bar's frame

namespace mm::faraway {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr int kSlots = 0xD0;
constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kCurrentStage = 0x80178162u;
constexpr uint32_t kCamX = 0x800BE558u, kCamY = 0x800BE55Cu;
constexpr uint32_t kWorldX = 0x800BE5D8u, kWorldY = 0x800BE5DCu;   // Marina's world position (Q16.16)

// The camera's globals: what func_80012830 and its parts read and write
// besides Marina (0x800BE544..0x800BE577: modes, targets, smoothing,
// centre, previous centre, view rectangle; 0x800BE61C/620: per-frame step).
// The scroll locks at 0x800BE62C/630 stay shared: a locked arena camera
// locks both.
struct Range {
    uint32_t start, size;
};
// 0x801782B0/B4: how far the camera leads ahead of Marina (func_80046498 and
// func_8004667C ease it towards +-0x40 by her input and speed). Shared, the
// two cameras pulled it each their way and player 2's lagged behind her.
constexpr std::array<Range, 3> kCameraSet{{{0x800BE544u, 0x34u}, {0x800BE61Cu, 8u}, {0x801782B0u, 8u}}};
constexpr uint32_t kSetSize = 0x44u;
constexpr uint32_t kSetCamX = kCamX - 0x800BE544u;   // offsets of the centre inside the set
constexpr uint32_t kSetCamY = kCamY - 0x800BE544u;

constexpr uint32_t kLockX = 0x800BE62Cu, kLockY = 0x800BE630u;   // scroll locks (func_80012830)
constexpr int32_t kNearX = 0x140 << 16, kNearY = 0xC0 << 16;         // "in the same arena"
constexpr uint16_t kSharedCameraStages[] = {35};                      // 3-12 CERBERUS (see active())

// The second camera in game memory (player 2's block, player2.cpp layout).
constexpr uint32_t kStore = 0x807F0500u;          // +0 valid, +4 the set
constexpr uint32_t kStoreSet = kStore + 4u;

// Onscreen-only actors (flag 0x4) die outside camera +-0x1C0 x +-0xA0
// (func_80016D94, as patched for widescreen).
constexpr int32_t kKillX = 0x1C0 << 16, kKillY = 0xA0 << 16;
// Player 2's walls around her camera, as Marina's around hers (func_80014C44).
constexpr int32_t kWallX = 0x90 << 16;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}
uint8_t* at(uint8_t* rdram, uint32_t vaddr) {
    return rdram + (vaddr - 0x80000000u);
}
gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kStride);
}

// Test (MM_TRACE_EXIT): which actor's update changes the exit / scene words.
void trace_exit(uint8_t* rdram, const char* where, int slot) {
    static const bool on = std::getenv("MM_TRACE_EXIT") != nullptr;
    if (!on) return;
    static uint32_t last[0x18];
    static int last_slot = -1;
    static const char* last_where = "";
    uint32_t now[0x18];
    for (uint32_t i = 0; i < 0x14; ++i) now[i] = static_cast<uint32_t>(MEM_W(0, addr(0x800D28E0u + i * 4)));
    now[(0x800D2910u - 0x800D28E0u) / 4] = 0;   // a frame counter
    now[0x14] = static_cast<uint32_t>(MEM_W(0, addr(0x800BE4F0u)));
    now[0x15] = static_cast<uint32_t>(MEM_W(0, addr(0x800BE4ECu)));
    now[0x16] = static_cast<uint32_t>(MEM_W(0, addr(0x800BE5D0u)));
    now[0x17] = static_cast<uint32_t>(MEM_W(0, addr(0x800BE5F4u)));
    if (std::memcmp(now, last, sizeof now) != 0) {
        const int ps = last_slot;
        std::fprintf(stderr, "[exit] changed after %s slot %d (type %04X):", last_where, ps,
                     ps >= 0 ? MEM_HU(0xD2, slot_addr(ps)) : 0);
        for (int i = 0; i < 0x18; ++i) {
            if (now[i] != last[i]) {
                const uint32_t a = i < 0x14 ? 0x800D28E0u + i * 4 : (i == 0x14 ? 0x800BE4F0u : i == 0x15 ? 0x800BE4ECu : i == 0x16 ? 0x800BE5D0u : 0x800BE5F4u);
                std::fprintf(stderr, " %08X %08X->%08X", a, last[i], now[i]);
            }
        }
        std::fprintf(stderr, "\n");
        std::memcpy(last, now, sizeof now);
    }
    last_slot = slot;
    last_where = where;
}

bool g_shadow = false;
bool g_probing = false;           // a lead probe runs (player2_lead.cpp): also thrown away
// Player 2's HUD bar in her picture: its shown health (see swap in the
// shadow frame), anew for a new player 2 or a loaded state.
struct P2Bar { int slot = -1; uint32_t generation = 0; int16_t shown = 0; };
P2Bar g_p2_bar;
constexpr uint32_t kShownHealth = 0x800BE63Cu;   // s16: the HUD bar's shown health (func_80022470)
bool g_swapped = false;
bool g_secondary = false;
bool g_frame_on = false;          // far rules on this frame (set before the camera)
int g_context = 0;                // 0: the game's camera set is loaded; 1: the second one
uint8_t g_parked[kSetSize];       // the set not loaded while context 1 runs
std::vector<int> g_spared;        // onscreen-only actors spared this frame

void read_set(uint8_t* rdram, uint8_t* dst) {
    for (const Range& r : kCameraSet) {
        std::memcpy(dst, at(rdram, r.start), r.size);
        dst += r.size;
    }
}
void write_set(uint8_t* rdram, const uint8_t* src) {
    for (const Range& r : kCameraSet) {
        std::memcpy(at(rdram, r.start), src, r.size);
        src += r.size;
    }
}
int32_t set_word(const uint8_t* set, uint32_t off) {
    int32_t v;
    std::memcpy(&v, set + off, 4);   // host-endian words, as MEM_W keeps them
    return v;
}

bool store_valid(uint8_t* rdram) {
    return MEM_W(0, addr(kStore)) == 1;
}

// The camera not loaded now: the store while the game's set is loaded,
// the parked set while the second one is.
void other_camera(uint8_t* rdram, int32_t& x, int32_t& y) {
    if (g_context == 0) {
        x = MEM_W(0, addr(kStoreSet + kSetCamX));
        y = MEM_W(0, addr(kStoreSet + kSetCamY));
    } else {
        x = set_word(g_parked, kSetCamX);
        y = set_word(g_parked, kSetCamY);
    }
}

// Everything the game keeps relative to the camera (what func_80016D94
// scrolls: actors with flag 0x2 and not 0x8, and Marina in slot 0) moves by
// the change of camera, so world positions stay.
void rebase(uint8_t* rdram, int32_t dx, int32_t dy) {
    if (dx == 0 && dy == 0) return;
    // What the players carry: its hold point (+0x104 / +0x108, written by the
    // carrier, read by the carried actor) is camera-relative too.
    for (int who : {0, mm::player2::slot(rdram)}) {
        if (who < 0) continue;
        const gpr c = slot_addr(who);
        if ((static_cast<uint32_t>(MEM_W(0x80, c)) & 0x02000000u) == 0) continue;
        const uint16_t held = MEM_HU(0xD6, c);
        if (held >= kSlots) continue;
        const gpr h = slot_addr(held);
        MEM_W(0x104, h) = MEM_W(0x104, h) - dx;
        MEM_W(0x108, h) = MEM_W(0x108, h) - dy;
    }
    for (int s = 0; s < kSlots; ++s) {
        const gpr a = slot_addr(s);
        const uint32_t fl = static_cast<uint32_t>(MEM_W(0x80, a));
        if (fl == 0) continue;
        if (s != 0 && ((fl & 0x2u) == 0 || (fl & 0x8u) != 0)) continue;
        MEM_W(0x88, a) = MEM_W(0x88, a) - dx;
        MEM_W(0x8C, a) = MEM_W(0x8C, a) - dy;
    }
}

void switch_to(uint8_t* rdram, int target) {
    if (target == g_context) return;
    const int32_t cx = MEM_W(0, addr(kCamX)), cy = MEM_W(0, addr(kCamY));
    uint8_t loaded[kSetSize];
    read_set(rdram, loaded);
    if (target == 1) {
        std::memcpy(g_parked, loaded, kSetSize);
        write_set(rdram, at(rdram, kStoreSet));
    } else {
        std::memcpy(at(rdram, kStoreSet), loaded, kSetSize);
        write_set(rdram, g_parked);
    }
    g_context = target;
    rebase(rdram, MEM_W(0, addr(kCamX)) - cx, MEM_W(0, addr(kCamY)) - cy);
}

// Swap the game's camera set with the stored one for good (shadow frame).
void swap_roles(uint8_t* rdram) {
    const int32_t cx = MEM_W(0, addr(kCamX)), cy = MEM_W(0, addr(kCamY));
    uint8_t loaded[kSetSize], stored[kSetSize];
    read_set(rdram, loaded);
    std::memcpy(stored, at(rdram, kStoreSet), kSetSize);
    write_set(rdram, stored);
    std::memcpy(at(rdram, kStoreSet), loaded, kSetSize);
    rebase(rdram, MEM_W(0, addr(kCamX)) - cx, MEM_W(0, addr(kCamY)) - cy);
}

int32_t cheb(int32_t dx, int32_t dy) {
    const int64_t ax = std::abs(static_cast<int64_t>(dx));
    const int64_t ay = std::abs(static_cast<int64_t>(dy)) * 4 / 3;
    return static_cast<int32_t>(std::min<int64_t>(std::max(ax, ay), 0x7FFFFFFF));
}

} // namespace

// Local split screen (cheats.h): both players at one machine, a camera and
// half the window each.
bool split_local() {
    return mm::cheats::split_screen_enabled() && !mm::netplay::active();
}

bool active(uint8_t* rdram) {
    static const int force = [] {   // test: MM_FAR=1 on locally too, =0 off online too
        const char* e = std::getenv("MM_FAR");
        return e ? (e[0] == '1' ? 1 : 0) : -1;
    }();
    if (force == 0 || (force < 0 && !mm::netplay::active() && !split_local())) return false;
    if (!mm::cheats::coop_enabled()) return false;
    const uint16_t state = MEM_HU(0, addr(kGameState));
    if (state != 6 && state != 10) return false;
    // Stages where the screen moves on its own (3-12, CERBERUS: a looping
    // chase that scrolls at 10 px a frame and pushes Marina along, wrapping
    // its camera back a lap at the loop's end) keep one shared camera: a
    // second camera would not scroll, and a player left behind the loop's
    // start is in a part of the world the lap wrap moves out from under
    // her. On one screen the edge carries both players, as it carries
    // Marina in the original game.
    const uint16_t stage = MEM_HU(0, addr(kCurrentStage));
    for (const uint16_t shared : kSharedCameraStages) {
        if (stage == shared) return false;
    }
    return mm::player2::slot(rdram) > 0;
}

bool in_shadow() { return g_shadow || g_probing; }

void hide_collected_golden_gems(uint8_t* rdram, uint32_t list) {
    constexpr uint32_t kItemHandlers = 0x800D37BCu, kGemHandler = 0x800446F0u;
    constexpr uint16_t kGolden = 2;   // colour (kind & 3), as the handler tests it
    if (list == 0 || in_shadow()) return;
    int checked = -1;                 // the save's answer, asked once
    for (int i = 0; i < 512; ++i) {
        const gpr rec = addr(list + static_cast<uint32_t>(i) * 6u);
        const uint16_t kind = MEM_HU(0, rec);
        if (kind == 0) break;
        if ((kind & 0xC000u) != 0 || (kind & 3u) != kGolden) continue;
        if (static_cast<uint32_t>(MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers))) != kGemHandler) continue;
        if (checked < 0) {
            recomp_context c{};
            c.r29 = addr(0x8045FF00u);   // scratch stack (host-only expansion memory)
            c.r4 = MEM_HU(0, addr(kCurrentStage));
            YellowGem_GetFlag(rdram, &c);
            checked = c.r2 != 0 ? 1 : 0;
        }
        if (checked == 1) {
            MEM_H(0, rec) = static_cast<int16_t>(kind | 0x4000u);
            std::fprintf(stderr, "[far] golden gem at %d,%d: the save has it, gone\n", MEM_HU(2, rec), MEM_HU(4, rec));
        }
    }
}
void set_probing(bool on) { g_probing = on; }
bool swapped() { return g_swapped; }
bool secondary_pass() { return g_secondary; }

void reset(uint8_t* rdram) {
    MEM_W(0, addr(kStore)) = 0;
}

bool p2_camera_offset(uint8_t* rdram, int32_t& dx, int32_t& dy) {
    if (!active(rdram) || !store_valid(rdram)) return false;
    if (g_swapped) {
        dx = dy = 0;        // the game's camera is hers
        return true;
    }
    // (Asked with the game's camera set loaded: the stored one is hers.)
    dx = MEM_W(0, addr(kStoreSet + kSetCamX)) - MEM_W(0, addr(kCamX));
    dy = MEM_W(0, addr(kStoreSet + kSetCamY)) - MEM_W(0, addr(kCamY));
    return true;
}

bool shows_second_view(uint8_t* rdram) {
    static const int force = [] {   // test: MM_FAR_SHADOW=1 always, =0 never
        const char* e = std::getenv("MM_FAR_SHADOW");
        return e ? (e[0] == '1' ? 1 : 0) : -1;
    }();
    if (force == 0 || !active(rdram) || !store_valid(rdram)) return false;
    if (force == 1) return true;
    if (split_local()) return true;   // the second half of the window
    // This machine's person plays player 2's slot: the guest, or the host
    // while player 2 leads (player2.h).
    return mm::netplay::active() && (mm::netplay::player() == 2) != mm::player2::lead_swapped(rdram);
}

void exchange_cameras(uint8_t* rdram) {
    if (!active(rdram) || !store_valid(rdram)) return;
    swap_roles(rdram);
}

} // namespace mm::faraway

using namespace mm::faraway;

extern "C" int mm_far_in_shadow(void) {
    return in_shadow() ? 1 : 0;
}

// func_80020024, after the camera (func_80012830), before the scroll and
// despawn (func_80016D94): the second camera follows player 2 (player 1 in
// the shadow frame); onscreen-only actors in its view are spared.
extern "C" void mm_far_after_camera(uint8_t* rdram, recomp_context* ctx) {
    trace_exit(rdram, "frame start", -1);
    if (!in_shadow()) {   // test: MM_FAR_DUMP=<dir> writes game memory every 20 frames
        static const char* dump = std::getenv("MM_FAR_DUMP");
        static uint32_t n = 0;
        static const bool tc = std::getenv("MM_FAR_TRACECAM") != nullptr;
        if (tc) {
            std::fprintf(stderr, "[fartc] %u", n + 1);   // (n counts on below)
            for (uint32_t a = 0x800BE540u; a < 0x800BE700u; a += 4) std::fprintf(stderr, " %08X", static_cast<uint32_t>(MEM_W(0, addr(a))));
            std::fprintf(stderr, " |");
            for (uint32_t a = kStore; a < kStore + 0x40u; a += 4) std::fprintf(stderr, " %08X", static_cast<uint32_t>(MEM_W(0, addr(a))));
            std::fprintf(stderr, "\n");
        }
        static const long dump_from = std::getenv("MM_FAR_DUMP_FROM") ? std::atol(std::getenv("MM_FAR_DUMP_FROM")) : 0;
        static const long dump_to = std::getenv("MM_FAR_DUMP_TO") ? std::atol(std::getenv("MM_FAR_DUMP_TO")) : 1L << 30;
        ++n;
        if (dump && static_cast<long>(n) >= dump_from && static_cast<long>(n) <= dump_to && (dump_from > 0 || (n % 20) == 0)) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/%05u.bin", dump, n);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(rdram, 1, 0x840000, f);
                std::fclose(f);
            }
        }
    }
    if (!in_shadow()) {   // test: MM_TEST_PLACE="<frame>:<x>,<y>[,2]" puts player 1 (or 2) at a world spot
        static const char* place = std::getenv("MM_TEST_PLACE");
        static uint32_t counted = 0;
        if (place && MEM_HU(0, addr(kGameState)) == 6) {
            // In a replay, its frame: a rollback rewinds it with the game, so
            // a playback places her on the same frame the recording did.
            ++counted;
            const uint32_t frames = mm::replay::mode() != mm::replay::Mode::Idle
                                        ? static_cast<uint32_t>(mm::replay::frame()) + 1
                                        : counted;
            unsigned long at = 0;
            int x = 0, y = 0, who = 1;
            if (std::sscanf(place, "%lu:%d,%d,%d", &at, &x, &y, &who) >= 3 && frames == at) {
                if (who == 2) {
                    mm::player2::place_test(rdram, x << 16, y << 16);
                    if (store_valid(rdram)) {   // her camera with her
                        MEM_W(0, addr(kStoreSet + kSetCamX)) = x << 16;
                        MEM_W(0, addr(kStoreSet + kSetCamY)) = (y + 18) << 16;
                    }
                } else {
                    MEM_W(0x88, slot_addr(0)) = (x << 16) - MEM_W(0, addr(kCamX));
                    MEM_W(0x8C, slot_addr(0)) = (y << 16) - MEM_W(0, addr(kCamY));
                    MEM_W(0, addr(kWorldX)) = x << 16;
                    MEM_W(0, addr(kWorldY)) = y << 16;
                }
                std::fprintf(stderr, "[test] player %d placed at %d,%d\n", who, x, y);
            }
            if (frames == at + 200) {   // what is around her by then
                for (int s = 1; s < kSlots; ++s) {
                    const gpr a = slot_addr(s);
                    if (MEM_W(0x80, a) == 0) continue;
                    const int dx = (MEM_W(0x88, a) - MEM_W(0x88, slot_addr(0))) >> 16;
                    const int dy = (MEM_W(0x8C, a) - MEM_W(0x8C, slot_addr(0))) >> 16;
                    if (std::abs(dx) > 160 || std::abs(dy) > 120) continue;
                    std::fprintf(stderr, "[test] near: slot %d type %04X state %04X at %+d,%+d flags %08X\n", s,
                                 MEM_HU(0xD2, a), MEM_HU(0xD0, a), dx, dy, static_cast<uint32_t>(MEM_W(0x80, a)));
                }
            }
        }
    }
    g_context = 0;
    g_spared.clear();
    g_frame_on = active(rdram);
    if (!g_frame_on) {
        // The far rules just ended (the online game is over) with player 2
        // still here: on one shared screen again, she comes back next to
        // player 1 at once instead of being dragged across the stage.
        if (store_valid(rdram) && !in_shadow()) mm::player2::bring_back(rdram);
        reset(rdram);
        return;
    }
    uint8_t loaded[kSetSize];
    read_set(rdram, loaded);
    if (!store_valid(rdram)) {
        std::memcpy(at(rdram, kStoreSet), loaded, kSetSize);
        MEM_W(0, addr(kStore)) = 1;
    }
    // (A looping stage wraps the game's camera back by a lap at the loop's
    // end; the one such stage, 3-12, keeps a shared camera, see active().
    // Do not mirror big camera jumps onto the second camera: a lead swap
    // or a star teleporter moves the game's camera just as far.)
    const int p2 = mm::player2::slot(rdram);
    const gpr m = slot_addr(0), b = slot_addr(p2);
    const int32_t cx = MEM_W(0, addr(kCamX)), cy = MEM_W(0, addr(kCamY));
    const int32_t m_x = MEM_W(0x88, m), m_y = MEM_W(0x8C, m);
    const int32_t w_x = MEM_W(0, addr(kWorldX)), w_y = MEM_W(0, addr(kWorldY));
    // Whom the second camera follows: player 2, or player 1 when the roles
    // are swapped (the game's own camera then followed player 2).
    const int32_t tx = g_swapped ? w_x : cx + MEM_W(0x88, b);
    const int32_t ty = g_swapped ? w_y : cy + MEM_W(0x8C, b);
    // A scroll lock (0x800BE62C / 0x800BE630: arenas, set pieces) holds the
    // game's camera. Near it, hers is the same camera (both are in the
    // arena); away from it, hers is not held where she happens to be.
    const uint16_t lock_x = MEM_HU(0, addr(kLockX)), lock_y = MEM_HU(0, addr(kLockY));
    const bool locked = lock_x != 0 || lock_y != 0;
    if (locked && std::abs(tx - cx) < kNearX && std::abs(ty - cy) < kNearY) {
        std::memcpy(at(rdram, kStoreSet), loaded, kSetSize);
    } else {
        // Whom it follows stands in for Marina, all of her: the camera leads
        // by which way she faces and moves, not just where she is.
        if (!g_swapped) mm::player2::stand_in_begin(rdram);
        write_set(rdram, at(rdram, kStoreSet));
        MEM_W(0, addr(kWorldX)) = tx;
        MEM_W(0, addr(kWorldY)) = ty;
        MEM_W(0x88, m) = tx - MEM_W(0, addr(kCamX));
        MEM_W(0x8C, m) = ty - MEM_W(0, addr(kCamY));
        MEM_H(0, addr(kLockX)) = 0;
        MEM_H(0, addr(kLockY)) = 0;
        const recomp_context saved = *ctx;
        g_secondary = true;
        func_80012830(rdram, ctx);
        g_secondary = false;
        *ctx = saved;
        MEM_H(0, addr(kLockX)) = static_cast<int16_t>(lock_x);
        MEM_H(0, addr(kLockY)) = static_cast<int16_t>(lock_y);
        read_set(rdram, at(rdram, kStoreSet));
        mm::player2::stand_in_end(rdram);
    }
    write_set(rdram, loaded);
    MEM_W(0x88, m) = m_x;
    MEM_W(0x8C, m) = m_y;
    MEM_W(0, addr(kWorldX)) = w_x;
    MEM_W(0, addr(kWorldY)) = w_y;

    static const bool dbg = std::getenv("MM_FAR_DEBUG") != nullptr;
    static uint32_t dbg_n = 0;
    if (dbg && !in_shadow() && (dbg_n++ % 60) == 0) {
        std::fprintf(stderr, "[far] camera %d,%d second %d,%d | p1 %d,%d p2 %d,%d%s\n", cx >> 16, cy >> 16,
                     MEM_W(0, addr(kStoreSet + kSetCamX)) >> 16, MEM_W(0, addr(kStoreSet + kSetCamY)) >> 16,
                     w_x >> 16, w_y >> 16, (cx + MEM_W(0x88, b)) >> 16, (cy + MEM_W(0x8C, b)) >> 16,
                     g_swapped ? " (swapped)" : "");
    }

    // Spare onscreen-only actors (flags 0x2 | 0x4, not 0x8) in the second
    // camera's view: func_80016D94 would remove them as off the screen.
    const int32_t ox = MEM_W(0, addr(kStoreSet + kSetCamX)), oy = MEM_W(0, addr(kStoreSet + kSetCamY));
    for (int s = 1; s < kSlots; ++s) {
        const gpr a = slot_addr(s);
        const uint32_t fl = static_cast<uint32_t>(MEM_W(0x80, a));
        if ((fl & 0x6u) != 0x6u || (fl & 0x8u) != 0) continue;
        const int32_t wx = cx + MEM_W(0x88, a), wy = cy + MEM_W(0x8C, a);
        if (std::abs(wx - ox) <= kKillX && std::abs(wy - oy) <= kKillY) {
            MEM_W(0x80, a) = static_cast<int32_t>(fl & ~0x4u);
            g_spared.push_back(s);
        }
    }
}

// After func_80016D94: the spared actors are onscreen-only again.
extern "C" void mm_far_after_scroll(uint8_t* rdram) {
    for (int s : g_spared) {
        const gpr a = slot_addr(s);
        if (MEM_W(0x80, a) != 0) MEM_W(0x80, a) = MEM_W(0x80, a) | 0x4;
    }
    g_spared.clear();
}

// After the map actor prefetch (func_8001DE30): once more around the second
// camera, so what is placed near player 2 comes to life there too.
extern "C" void mm_far_after_prefetch(uint8_t* rdram, recomp_context* ctx) {
    if (!g_frame_on) return;
    const recomp_context saved = *ctx;
    switch_to(rdram, 1);
    func_8001DE30(rdram, ctx);
    switch_to(rdram, 0);
    *ctx = saved;
}

// func_8001751C, before each actor's update: run it with the camera it is
// nearest to.
namespace {
// The camera set (0 the game's, 1 the second) nearest to an actor.
int nearest(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    int32_t ox, oy;
    other_camera(rdram, ox, oy);
    const int32_t cx = MEM_W(0, addr(kCamX)), cy = MEM_W(0, addr(kCamY));
    const int32_t rx = MEM_W(0x88, a), ry = MEM_W(0x8C, a);
    const int32_t here = cheb(rx, ry);
    const int32_t there = cheb(cx + rx - ox, cy + ry - oy);
    return there < here ? 1 - g_context : g_context;
}

constexpr uint32_t kHoldingFlag = 0x02000000u;   // +0x80 while carrying (held slot at +0xD6)

// The camera set a player held `slot` with, or -1. A carrier writes the hold
// point (+0x104 / +0x108, camera-relative) in her update and the carried
// actor reads it in its own: both must see the same camera. Player 1 (slot
// 0) updates with the game's camera, player 2 with her nearest.
int holder_set(uint8_t* rdram, int slot) {
    const gpr m = slot_addr(0);
    if ((static_cast<uint32_t>(MEM_W(0x80, m)) & kHoldingFlag) != 0 && MEM_HU(0xD6, m) == slot) return 0;
    const int p2 = mm::player2::slot(rdram);
    if (p2 > 0 && p2 != slot) {
        const gpr b = slot_addr(p2);
        if ((static_cast<uint32_t>(MEM_W(0x80, b)) & kHoldingFlag) != 0 && MEM_HU(0xD6, b) == slot) {
            return nearest(rdram, p2);
        }
    }
    return -1;
}
} // namespace


namespace {
// Repeating scenery (docs/README.md 12.1): one copy of a tree, a dune panel,
// that puts itself within 256 px of the camera every frame. It belongs to
// whichever picture is being made: the game's camera (player 1's, or in the
// shadow frame player 2's), never to the camera it happens to be nearest.
// Run with the nearest, it stayed by player 1 and player 2's picture had no
// foreground dunes or cacti.
bool wrapping_scenery(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    const uint16_t type = MEM_HU(0xD2, a), state = MEM_HU(0xD0, a);
    return (type == 0x181C && state == 2) || (type == 0x000D && state == 0x50);
}
} // namespace

extern "C" void mm_far_actor(uint8_t* rdram, uint32_t slot) {
    const int s = static_cast<int>(slot & 0xFFFF);
    trace_exit(rdram, "actor", s);
    // Whose camera a carried object runs with: its carrier's, found before
    // the held swap below puts player 1's record in player 2's slot (after
    // it, player 1's position decided, the block player 2 carried ran with
    // player 1's camera and hers with her own: offset by the cameras'
    // distance at her hands, her carry code pulled her after it, faster
    // every frame).
    const int held = g_frame_on ? holder_set(rdram, s) : -1;
    mm::player2::held_swap_begin(rdram, s);
    if (!g_frame_on) return;
    if (held < 0 && wrapping_scenery(rdram, s)) {
        switch_to(rdram, 0);
        return;
    }
    switch_to(rdram, held >= 0 ? held : nearest(rdram, s));
}

// func_80020024 after the actor loop, before Marina's walls (func_80014C44):
// back to the game's camera; player 2 keeps within her own camera's walls.
extern "C" void mm_far_after_actors(uint8_t* rdram) {
    trace_exit(rdram, "loop end", -1);
    mm::player2::held_swap_end(rdram);
    mm::player2::after_actors(rdram);
    if (!g_frame_on) return;
    switch_to(rdram, 0);
    int32_t dx, dy;
    if (p2_camera_offset(rdram, dx, dy)) mm::player2::clamp_x(rdram, dx, kWallX);
}

// Marina's walls are the game camera's: in the shadow frame that camera is
// player 2's, so player 1 is left where she is instead.
namespace {
int32_t g_wall_saved[4];
}
extern "C" void mm_far_walls_begin(uint8_t* rdram) {
    if (!g_swapped) return;
    g_wall_saved[0] = MEM_W(0x88, slot_addr(0));
    g_wall_saved[1] = MEM_W(0x8C, slot_addr(0));
    g_wall_saved[2] = MEM_W(0, addr(kWorldX));
    g_wall_saved[3] = MEM_W(0, addr(kWorldY));
}
extern "C" void mm_far_walls_end(uint8_t* rdram) {
    if (!g_swapped) return;
    MEM_W(0x88, slot_addr(0)) = g_wall_saved[0];
    MEM_W(0x8C, slot_addr(0)) = g_wall_saved[1];
    MEM_W(0, addr(kWorldX)) = g_wall_saved[2];
    MEM_W(0, addr(kWorldY)) = g_wall_saved[3];
}

// ---------------------------------------------------------------------------
// The shadow frame (player 2's picture on her machine).

namespace {

constexpr uint32_t kRam = 0x840000u;          // what save states and fingerprints cover
constexpr uint32_t kPage = 0x1000u;
constexpr uint32_t kArenas = 0x00460000u;        // physical, 2 x 0x10000 (Graphics_Begin_Frame)
constexpr uint32_t kLayers = 0x00400000u;        // tile layer lists, 0x18000 (widescreen.cpp)
constexpr uint32_t kLayersSize = 0x18000u;
constexpr uint32_t kMirror = 0x00840000u;        // above what save states and fingerprints cover (8 MB + 256 KB), below 16 MB (RT64 masks 0xFFFFFF)
constexpr uint32_t kMirrorSize = 0x00380000u;    // one per frame buffer
// Split screen: player 2's view goes to a framebuffer of its own (one per
// game framebuffer, 320x240 16-bit), after the mirrors, below 16 MB.
constexpr uint32_t kSplitFb = kMirror + 2u * kMirrorSize;   // 0x00F40000
constexpr uint32_t kSplitFbStride = 0x30000u;
constexpr float kSplitMaxAspect = 8.0f / 3.0f;   // the widened tile grids reach 2x the 4:3 width (widescreen.cpp)

std::vector<uint8_t> g_backup;
std::vector<uint8_t> g_dirty;

struct Builder {
    uint8_t* rdram;
    uint32_t arena;          // the shadow frame's arena (physical)
    uint32_t base, used, cap;
    uint32_t fb_from, fb_to; // colour image to retarget
    uint32_t seg[16]{};
    std::unordered_map<uint32_t, uint32_t> lists;
    uint32_t timg_at = 0;    // where the last SETTIMG word 1 sits (in the copy)
    uint32_t timg_src = 0, timg_w0 = 0, timg_len = 0;
    bool overflow = false;
    int copied = 0;

    uint8_t* p(uint32_t phys) { return rdram + phys; }
    uint32_t r32(uint32_t phys) { return static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x80000000u | phys)))); }
    void w32(uint32_t phys, uint32_t v) { MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x80000000u | phys))) = static_cast<int32_t>(v); }

    bool dirty(uint32_t phys, uint32_t len) {
        if (phys >= kRam) return false;
        const uint32_t end = std::min(kRam, phys + std::max(len, 1u));
        for (uint32_t pg = phys / kPage; pg <= (end - 1) / kPage; ++pg) {
            if (g_dirty[pg]) return true;
        }
        return false;
    }
    uint32_t alloc(uint32_t len, uint32_t align_like) {
        uint32_t at_ = (used + 15u) & ~15u;
        at_ += align_like & 15u;
        if (at_ + len > cap) {
            overflow = true;
            return 0;
        }
        used = at_ + len;
        return base + at_;
    }
    // Where the renderer should read [phys, phys + len) from.
    uint32_t reloc(uint32_t phys, uint32_t len) {
        if (phys >= arena && phys < arena + 0x10000u) return base + (phys - arena);
        if (phys >= kLayers && phys < kLayers + kLayersSize) return base + 0x10000u + (phys - kLayers);
        if (!dirty(phys, len)) return phys;
        const uint32_t to = alloc(len, phys);
        if (to == 0) return phys;
        std::memcpy(p(to), p(phys), len);
        ++copied;
        return to;
    }
    uint32_t resolve(uint32_t w1) {
        return (seg[(w1 >> 24) & 0xF] + (w1 & 0x00FFFFFFu)) & 0x00FFFFFFu;
    }
    static uint32_t keep_form(uint32_t original, uint32_t phys) {
        return (original & 0x80000000u) ? (phys | 0x80000000u) : phys;
    }
    bool segmented(uint32_t w1) {
        return ((w1 >> 24) & 0xF) != 0;
    }

    // Rewrite the list at src (physical) into the mirror; returns its copy.
    uint32_t list(uint32_t src, int depth) {
        if (depth > 12 || src >= kRam || (src & 7u) != 0) return src;
        if (auto it = lists.find(src); it != lists.end()) return it->second;
        uint32_t dst;
        if (src >= arena && src < arena + 0x10000u) {
            dst = base + (src - arena);
        } else if (src >= kLayers && src < kLayers + kLayersSize) {
            dst = base + 0x10000u + (src - kLayers);
        } else {
            uint32_t n = 0;
            while (n < 0x4000u) {
                const uint32_t w0 = r32(src + n * 8);
                ++n;
                if ((w0 >> 24) == 0xB8) break;
                if ((w0 >> 24) == 0x06 && ((w0 >> 16) & 0xFF) == 1) break;
                if (src + n * 8 >= kRam) break;
            }
            dst = alloc(n * 8, 0);
            if (dst == 0) return src;
            std::memcpy(p(dst), p(src), n * 8);
        }
        lists[src] = dst;
        for (uint32_t pc = dst, n = 0; n < 0x4000u; pc += 8, ++n) {
            const uint32_t w0 = r32(pc), w1 = r32(pc + 4);
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            if (op == 0xB8) break;
            switch (op) {
                case 0x01: case 0x03: case 0x04: {   // matrix, movemem, vertices
                    if (segmented(w1)) break;
                    const uint32_t from = resolve(w1);
                    const uint32_t to = reloc(from, w0 & 0xFFFFu);
                    if (to != from) w32(pc + 4, keep_form(w1, to));
                    break;
                }
                case 0x06: {
                    const uint32_t from = resolve(w1);
                    const uint32_t to = list(from, depth + 1);
                    if (!segmented(w1) && to != from) w32(pc + 4, keep_form(w1, to));
                    if (((w0 >> 16) & 0xFF) == 1) return dst;   // a branch: this list ends
                    break;
                }
                case 0xBC:
                    if ((w0 & 0xFFu) == 0x06) {                 // segment base
                        const uint32_t s = ((w0 >> 8) & 0xFFFFu) / 4u;
                        const uint32_t v = w1 & 0x00FFFFFFu;
                        if (v >= arena && v < arena + 0x10000u) w32(pc + 4, keep_form(w1, base + (v - arena)));
                        if (s < 16) seg[s] = v;   // the source: lists are found where the game put them
                    }
                    break;
                case 0xFD:
                    timg_at = pc + 4;
                    timg_src = resolve(w1);
                    timg_w0 = w0;
                    timg_len = 0;
                    if (segmented(w1)) timg_at = 0;
                    break;
                case 0xF3: case 0xF4: case 0xF0: {
                    if (timg_at == 0) break;
                    const uint32_t siz = (timg_w0 >> 19) & 3u;
                    uint32_t len;
                    if (op == 0xF3) {
                        const uint32_t texels = ((w1 >> 12) & 0xFFFu) + 1u;
                        len = (texels << siz) >> 1;
                    } else if (op == 0xF4) {
                        const uint32_t width = (timg_w0 & 0xFFFu) + 1u;
                        const uint32_t rows = ((w1 & 0xFFFu) >> 2) + 1u;
                        len = ((width << siz) >> 1) * rows;
                    } else {
                        len = (((w1 >> 14) & 0x3FFu) + 1u) * 2u + 64u;
                    }
                    len = std::min<uint32_t>(len + 8u, 0x10000u);
                    if (len <= timg_len) break;
                    timg_len = len;
                    const uint32_t to = reloc(timg_src, len);
                    w32(timg_at, to | 0x80000000u);
                    break;
                }
                case 0xFF: {
                    const uint32_t img = resolve(w1);
                    if (img == fb_from) w32(pc + 4, keep_form(w1, fb_to));
                    break;
                }
                default:
                    break;
            }
        }
        return dst;
    }
};

uint32_t g_shadow_frames = 0;

} // namespace

// Thread_MainProc, just before the frame's graphics task goes out (the list
// was built last time round; this round's frame is next): on player 2's
// machine, build her picture and point the task's list at it.
extern "C" void mm_far_shadow(uint8_t* rdram, recomp_context* ctx, uint32_t task) {
    const bool split = split_local() && shows_second_view(rdram);
#ifdef MM_HAS_GRAPHICS
    static bool split_was = false;
    if (!split && split_was) mm::graphics::set_split_screen(0, false, 0.0f);   // one picture again
    split_was = split;
#endif
    if (!shows_second_view(rdram)) return;
    if (task < 0x80000000u || task >= 0x80800000u) return;
    const uint32_t list_va = static_cast<uint32_t>(MEM_W(0x30, addr(task))) | 0x80000000u;
    const uint32_t list = list_va & 0x00FFFFFFu;
    if (list < kArenas || list >= kArenas + 0x20000u) return;
    const uint32_t i = (list - kArenas) >> 16;
    const uint32_t j = i ^ 1u;
    // The real frame's colour image (its first G_SETCIMG).
    uint32_t fb_real = 0;
    for (uint32_t pc = list, n = 0; n < 16; pc += 8, ++n) {
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, addr(0x80000000u | pc)));
        if ((w0 >> 24) == 0xFF) {
            fb_real = static_cast<uint32_t>(MEM_W(4, addr(0x80000000u | pc))) & 0x00FFFFFFu;
            break;
        }
    }
    if (fb_real == 0) return;

    const auto t0 = std::chrono::steady_clock::now();
    g_backup.assign(rdram, rdram + kRam);
    const recomp_context saved = *ctx;
    // Presentation only: nothing it does may reach host-side game state.
    // Game memory is put back below; the registered state is checked here.
    const uint64_t state_before = mm::state::hash_all(0xCBF29CE484222325ull);
    g_shadow = true;
    g_swapped = true;
    swap_roles(rdram);
    // The HUD's health bar is slot 0's: in this picture, player 2's slot's.
    // Its shown health (0x800BE63C) follows the real one 5 a frame, the
    // part between drawn see-through (func_80022470, in the gameplay frame
    // for slot 0 only): here it was player 1's, so a refill of player 2's
    // stayed see-through. Hers is kept here (the picture's own, not game
    // state) and stepped by the bar's own frame.
    if (const int p2 = mm::player2::slot(rdram); p2 > 0) {
        const int16_t hp = std::max<int16_t>(0, MEM_H(0xE0, addr(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u)));
        MEM_H(0xE0, addr(0x800EF510u)) = hp;
        if (g_p2_bar.slot != p2 || g_p2_bar.generation != mm::savestate::load_generation()) {
            g_p2_bar = {p2, mm::savestate::load_generation(), hp};
        }
        MEM_H(0, addr(kShownHealth)) = g_p2_bar.shown;
        recomp_context bar = *ctx;
        func_80022470(rdram, &bar);
        g_p2_bar.shown = MEM_H(0, addr(kShownHealth));
    }
    ctx->r4 = j;
    Graphics_Begin_Frame(rdram, ctx);
    g_swapped = false;
    g_shadow = false;
    if (mm::state::hash_all(0xCBF29CE484222325ull) != state_before) {
        static int reported = 0;
        if (reported++ < 5)
            std::fprintf(stderr, "[far] BUG: player 2's picture pass changed registered host state (the two "
                                 "machines will drift apart)\n");
    }

    // Which pages the shadow frame changed.
    g_dirty.assign(kRam / kPage, 0);
    for (uint32_t pg = 0; pg < kRam / kPage; ++pg) {
        g_dirty[pg] = std::memcmp(rdram + pg * kPage, g_backup.data() + pg * kPage, kPage) != 0;
    }
    Builder b{};
    b.rdram = rdram;
    b.arena = kArenas + (j << 16);
    b.base = kMirror + i * kMirrorSize;
    b.cap = kMirrorSize;
    b.used = 0x10000u + kLayersSize;
    std::memcpy(rdram + b.base, rdram + b.arena, 0x10000u);
    std::memcpy(rdram + b.base + 0x10000u, rdram + kLayers, kLayersSize);
    uint32_t fb_shadow = 0;
    for (uint32_t pc = b.arena + 0x180u, n = 0; n < 16; pc += 8, ++n) {
        if ((b.r32(pc) >> 24) == 0xFF) {
            fb_shadow = b.r32(pc + 4) & 0x00FFFFFFu;
            break;
        }
    }
    b.fb_from = fb_shadow;
    // Split screen: into its own framebuffer, drawn after the real frame;
    // otherwise into the real one, instead of it.
    const uint32_t fb_split = kSplitFb + i * kSplitFbStride;
    b.fb_to = split ? fb_split : fb_real;
    const uint32_t start = b.list(b.arena + 0x180u, 0);

    // Memory back as it was, exactly.
    for (uint32_t pg = 0; pg < kRam / kPage; ++pg) {
        if (g_dirty[pg]) std::memcpy(rdram + pg * kPage, g_backup.data() + pg * kPage, kPage);
    }
    *ctx = saved;
    if (b.overflow) {
        std::fprintf(stderr, "[far] shadow frame too big for its mirror; showing the game's camera\n");
        return;
    }
    if (split) {
        // The real frame first, then player 2's into the second buffer: the
        // list's first command moves to a little head that goes on with the
        // rest of the list, called from a top list that then branches to the
        // copy.
        const uint32_t head = b.alloc(16, 0), top = b.alloc(16, 0);
        if (head == 0 || top == 0) {
            std::fprintf(stderr, "[far] split screen: no room for the frame head\n");
            return;
        }
        b.w32(head, b.r32(list));
        b.w32(head + 4, b.r32(list + 4));
        b.w32(head + 8, 0x06010000u);
        b.w32(head + 12, 0x80000000u | (list + 8));
        b.w32(top, 0x06000000u);
        b.w32(top + 4, 0x80000000u | head);
        b.w32(top + 8, 0x06010000u);
        b.w32(top + 12, 0x80000000u | start);
        // One full sync per frame: the real frame's turns into a pipe sync,
        // so the renderer finishes (and signals) the frame once, after the
        // second view.
        for (uint32_t pc = list + 8, n = 0; n < 0x4000u; pc += 8, ++n) {
            const uint32_t w0 = b.r32(pc);
            if ((w0 >> 24) == 0xB8) break;
            if ((w0 >> 24) == 0x06 && ((w0 >> 16) & 0xFF) == 1) break;
            if ((w0 >> 24) == 0xE9) {
                b.w32(pc, 0xE7000000u);
                b.w32(pc + 4, 0);
            }
        }
        MEM_W(0, addr(0x80000000u | list)) = 0x06010000;
        MEM_W(4, addr(0x80000000u | list)) = static_cast<int32_t>(0x80000000u | top);
#ifdef MM_HAS_GRAPHICS
        // Person 1 on top: the real frame follows slot 0, which is player 2's
        // person while she leads (player2.h).
        mm::graphics::set_split_screen(fb_split, mm::player2::lead_swapped(rdram), kSplitMaxAspect,
                                       mm::split_view::kShownTop, mm::split_view::kShownBottom, mm::split_view::kPictureRows);
#endif
    } else {
        // The real list (frame arenas are outside the netplay fingerprint)
        // jumps to the copy.
        MEM_W(0, addr(0x80000000u | list)) = 0x06010000;
        MEM_W(4, addr(0x80000000u | list)) = static_cast<int32_t>(0x80000000u | start);
    }
    static const bool dbg = std::getenv("MM_FAR_DEBUG") != nullptr;
    if (dbg && (g_shadow_frames % 60) == 0) {
        size_t dirty = 0;
        for (uint8_t d : g_dirty) dirty += d;
        const long us = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - t0).count());
        std::fprintf(stderr, "[far] shadow %u: %zu pages changed, %d blocks copied, mirror %u KB, fb %06X -> %06X, %ld us\n",
                     g_shadow_frames, dirty, b.copied, b.used / 1024, fb_shadow, fb_real, us);
    }
    ++g_shadow_frames;
}
