// The lead swap (player2.h): who plays slot 0, the game's Marina.
//
// Some of the game answers to slot 0 alone: a star teleporter's flight (and
// the camera that follows it) carries slot 0, and stage scripts can check
// slot 0's position. So when player 2 does something only the lead can, the
// two swap characters (exchange_characters, player2.cpp): records, Marina's
// globals, controllers (player 2's controller entry is read as player 1's
// from then on, Input_Update), cameras when far apart. Each person keeps
// playing the same character in the same place; the tint, the tag and each
// machine's picture follow the person.
//
// Two triggers, both decided from game memory only, so both machines decide
// alike and replays repeat it:
//
//  - The star flight: player 2's character goes into the flight state
//    (0x0301). The star asks it of whoever holds it through Marina's request
//    word, which each player has her own copy of.
//  - A probe, every few frames and every frame player 2 presses something:
//    the gameplay frame runs once from a copy of memory with the two
//    characters swapped, and memory goes back exactly (as the shadow frame,
//    far_coop.cpp). If that frame starts a transition (the room-transition
//    words, the scene, the game state) and the frame as it is does not (or
//    does only with player 2's controller), it is hers to start: the two swap
//    before the real frame, which then starts it.
//
// The lead goes back to player 1 with the next scene (player 2 joins it
// afresh) or when co-op ends. After a swap the lead stays a while before it
// may change again, so two players standing on and next to an exit don't
// flap (coop-anything playbook/actor-ownership.md, rule 7).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cheats.h"
#include "far_coop.h"
#include "frame_input.h"
#include "player2.h"
#include "player2_internal.h"

extern "C" void GameState_Gameplay(uint8_t* rdram, recomp_context* ctx);

namespace mm::player2 {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kCannotPause = 0x800BE4ECu;    // cinematics hold it
constexpr uint32_t kMarinaPaused = 0x800CA230u;
constexpr uint32_t kHoldingFlag = 0x02000000u;
constexpr uint16_t kStarFlight = 0x0301;          // Marina's state while a star teleporter flies her
constexpr uint16_t kCarried = 0x2F;               // (low byte) held by the other player

constexpr uint32_t kLead = kLeadBlock;
constexpr uint32_t kHoldFrames = kLeadBlock + 0x4u;
constexpr uint32_t kCount = kLeadBlock + 0x8u;
constexpr uint32_t kFollow = kLeadBlock + 0xCu;    // the other player joins the lead when her star flight ends

constexpr int kProbeEvery = 4;     // frames
constexpr int kMinLead = 90;       // frames between swaps

// What starts a transition: the game state, the scene and the room-transition
// mode and target (func_80020A90's struct at 0x800D28E4).
constexpr uint32_t kWatched[] = {kGameState, 0x800BE5D0u, 0x800D28E8u, 0x800D28F0u};
constexpr int kWatchedCount = static_cast<int>(sizeof kWatched / sizeof kWatched[0]);

// What a gameplay frame can write: the game's memory (expansion memory above
// 0x80480000 is unused by it, docs/README.md 14.1) and player 2's block.
struct Span { uint32_t start, size; };
constexpr Span kSpans[] = {{0x000000u, 0x480000u}, {0x7F0000u, 0x10000u}};
constexpr uint32_t kPage = 0x1000u;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}
gpr record(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kStride);
}

bool g_probing = false;
std::vector<uint8_t> g_backup;

bool carrying_each_other(uint8_t* rdram, int p2) {
    for (int s : {0, p2}) {
        const gpr r = record(s);
        if ((static_cast<uint32_t>(MEM_W(0x80, r)) & kHoldingFlag) != 0 && MEM_HU(0xD6, r) == (s == 0 ? p2 : 0)) return true;
    }
    return false;
}

// Both in play, neither carrying the other, nothing scripted going on.
bool may_swap(uint8_t* rdram, int p2) {
    const uint16_t state = MEM_HU(0, addr(kGameState));
    if (state != 6 && state != 10) return false;
    if (MEM_HU(0, addr(kCannotPause)) != 0 || MEM_HU(0, addr(kMarinaPaused)) != 0) return false;
    for (int s : {0, p2}) {
        const gpr r = record(s);
        if (MEM_H(0xE0, r) <= 0 || (MEM_HU(0xD0, r) & 0xFF) == kCarried) return false;
        const uint32_t fl = static_cast<uint32_t>(MEM_W(0x80, r));
        const int other = s == 0 ? p2 : 0;
        if ((fl & kHoldingFlag) != 0 && MEM_HU(0xD6, r) == other) return false;
    }
    return true;
}

void swap_lead(uint8_t* rdram, const char* why) {
    exchange_characters(rdram);
    MEM_W(0, addr(kLead)) = MEM_W(0, addr(kLead)) ? 0 : 1;
    MEM_W(0, addr(kHoldFrames)) = kMinLead;
    if (!mm::faraway::in_shadow()) {
        std::fprintf(stderr, "[player2] %s: player %d leads now (frame %ld)\n", why, MEM_W(0, addr(kLead)) ? 2 : 1,
                     mm::frame_input::number());
    }
}

// The gameplay frame once more, from a copy: did it start a transition, or a
// star flight for slot 0? With the characters swapped, as things are, or as
// things are without player 2's controller.
enum class Probe { Swapped, AsIs, Without2 };
const char* const kProbeNames[] = {"", " (as is)", " (without 2's input)"};

bool probe(uint8_t* rdram, recomp_context* ctx, Probe how) {
    static const bool timing = std::getenv("MM_LEAD_TIMING") != nullptr;   // test: what probing costs
    static const bool debug = std::getenv("MM_LEAD_DEBUG") != nullptr;     // test: what each probe saw
    static long total_us = 0, count = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool swapped = how == Probe::Swapped;
    g_backup.resize(0x490000u);
    {
        size_t at = 0;
        for (const Span& sp : kSpans) {
            std::memcpy(g_backup.data() + at, rdram + sp.start, sp.size);
            at += sp.size;
        }
    }
    const recomp_context saved = *ctx;
    g_probing = true;
    mm::faraway::set_probing(true);
    if (swapped) exchange_characters(rdram);
    if (how == Probe::Without2) mute_second_controller(rdram);
    GameState_Gameplay(rdram, ctx);
    bool started = false;
    for (int i = 0; i < kWatchedCount; ++i) {
        uint32_t before;
        std::memcpy(&before, g_backup.data() + (kWatched[i] - 0x80000000u), 4);   // (all in the first span)
        if (static_cast<uint32_t>(MEM_W(0, addr(kWatched[i]))) != before) {
            started = true;
            if (debug) {
                std::fprintf(stderr, "[lead] probe%s: %08X %08X -> %08X\n", kProbeNames[static_cast<int>(how)], kWatched[i], before,
                             static_cast<uint32_t>(MEM_W(0, addr(kWatched[i]))));
            }
        }
    }
    if (MEM_HU(0xD0, record(0)) == kStarFlight) {
        started = true;
        if (debug) std::fprintf(stderr, "[lead] probe%s: star flight\n", kProbeNames[static_cast<int>(how)]);
    }
    mm::faraway::set_probing(false);
    g_probing = false;
    {
        size_t at = 0;
        for (const Span& sp : kSpans) {
            for (uint32_t off = 0; off < sp.size; off += kPage) {
                uint8_t* p = rdram + sp.start + off;
                const uint8_t* q = g_backup.data() + at + off;
                if (std::memcmp(p, q, kPage) != 0) std::memcpy(p, q, kPage);
            }
            at += sp.size;
        }
    }
    *ctx = saved;
    if (timing) {
        total_us += static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        if ((++count % 100) == 0) std::fprintf(stderr, "[lead] %ld probes, %ld us each\n", count, total_us / count);
    }
    return started;
}

} // namespace

// Input_Update, after the controllers are read: while player 2 leads, her
// controller entry is read as player 1's (and player 1's as hers). The lead
// goes back to player 1 when player 2 is gone (a new scene) or co-op ends.
void lead_route_pads(uint8_t* rdram) {
    if (MEM_W(0, addr(kLead)) == 0) return;
    if (!mm::cheats::coop_enabled() || slot(rdram) <= 0) {
        MEM_W(0, addr(kLead)) = 0;
        // No press edges from the change of hands.
        const int p1pad = MEM_W(0, addr(0x801370C0u));
        if (p1pad >= 0 && p1pad < 4) MEM_H(0, addr(0x800BE538u)) = MEM_H(0, addr(0x8012AD70u + static_cast<uint32_t>(p1pad) * 6u));
        if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] player 1 leads again (frame %ld)\n", mm::frame_input::number());
        return;
    }
    route_pads(rdram);
}

// GameState_Gameplay's entry: the swaps.
void lead_frame(uint8_t* rdram, recomp_context* ctx) {
    if (g_probing || mm::faraway::in_shadow()) return;
    if (!mm::cheats::coop_enabled()) return;
    const int p2 = slot(rdram);
    if (p2 <= 0) return;
    if (MEM_W(0, addr(kHoldFrames)) > 0) MEM_W(0, addr(kHoldFrames)) = MEM_W(0, addr(kHoldFrames)) - 1;
    const int32_t n = MEM_W(0, addr(kCount)) + 1;
    MEM_W(0, addr(kCount)) = n;
    static const long test_at = std::getenv("MM_TEST_LEAD_SWAP") ? std::atol(std::getenv("MM_TEST_LEAD_SWAP")) : -1;
    if (n == test_at) {   // test: swap now
        swap_lead(rdram, "test");
        return;
    }
    // A star teleporter's flight came to player 2's character (her update
    // took up the request in her own copy of Marina's mailbox): the flight
    // and its camera are slot 0's, so she leads from here, whatever else is
    // going on (the flight's start can hold the game's pause off: waiting
    // for that left her hanging in the flight state in her own slot, unable
    // to move, while the flight's camera dragged player 1). The other player
    // comes along once she lands.
    if (MEM_HU(0xD0, record(p2)) == kStarFlight && MEM_HU(0xD0, record(0)) != kStarFlight && !carrying_each_other(rdram, p2)) {
        swap_lead(rdram, "star teleporter");
        MEM_W(0, addr(kFollow)) = 1;
        return;
    }
    if (MEM_W(0, addr(kFollow)) != 0 && MEM_HU(0xD0, record(0)) != kStarFlight) {
        MEM_W(0, addr(kFollow)) = 0;
        bring_next_to_lead(rdram);
        if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[player2] landed: the other player comes along\n");
    }
    if (MEM_HU(0, addr(0x800BE4E8u)) != 0 || !may_swap(rdram, p2)) return;   // paused, mid-transition, ...
    static const bool off = std::getenv("MM_NO_LEAD_PROBE") != nullptr;   // test
    // Every few frames (exits stay where they are); every frame while she
    // presses something or holds up (a door may answer one press).
    if (off || MEM_W(0, addr(kHoldFrames)) > 0 || ((n % kProbeEvery) != 0 && !second_controller_acting(rdram))) return;
    if (!probe(rdram, ctx, Probe::Swapped)) return;
    // Player 2 as the lead would start something. As things are, does it
    // start too? Then it is not hers (a timed event, player 1's doing),
    // unless it does not without her controller.
    if (!probe(rdram, ctx, Probe::AsIs)) {
        swap_lead(rdram, "exit");
    } else if (!probe(rdram, ctx, Probe::Without2)) {
        swap_lead(rdram, "the other controller");
    }
}

} // namespace mm::player2

extern "C" void mm_lead_route_pads(uint8_t* rdram) {
    mm::player2::lead_route_pads(rdram);
}

extern "C" void mm_lead_frame(uint8_t* rdram, recomp_context* ctx) {
    mm::player2::lead_frame(rdram, ctx);
}

// Test (MM_TRACE_STARS): the star teleporters (type 0x32, func_800418A8) as
// they change state, with where they send Marina (+0xD8: a room).
extern "C" void mm_test_star(uint8_t* rdram, uint32_t slot) {
    static const bool on = std::getenv("MM_TRACE_STARS") != nullptr;
    if (!on || mm::faraway::in_shadow()) return;
    static uint32_t seen[0xD0];
    const int s = static_cast<int>(slot & 0xFFFF);
    if (s >= 0xD0) return;
    const gpr a = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(s) * 0x198u));
    const uint32_t key = (static_cast<uint32_t>(MEM_HU(0xD2, a)) << 16) | MEM_HU(0xD0, a);
    if (seen[s] == key) return;
    seen[s] = key;
    const int32_t cx = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE558u)));
    const int32_t cy = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE55Cu)));
    std::fprintf(stderr, "[star] slot %d type %04X state %04X at world %d,%d target %04X\n", s, MEM_HU(0xD2, a), MEM_HU(0xD0, a),
                 (cx + MEM_W(0x88, a)) >> 16, (cy + MEM_W(0x8C, a)) >> 16, MEM_HU(0xD8, a));
}
