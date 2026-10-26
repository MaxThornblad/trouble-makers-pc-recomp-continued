// Headless test harness for actor work (environment variables only; inert
// otherwise). Runs once per frame from mm_debug_menu_tick, counting frames
// from the last save-state load:
//   MM_TEST_SCRIPT="30:,2:B,60:,2:RB"  frames:buttons runs (R/L/U/D/A/B), fed
//                                      to Marina's and the global pad words
//   MM_TEST_DUMP=40,120                list every active actor on those frames
//   MM_TEST_TRACE=121[,frames]         one line per frame for a slot (and Marina)
//   MM_TEST_DIFF=121:100-150           print changed words of a slot in a range
//   MM_TEST_SPAWN=18@20@40@0[@flags[@param]]  spawn a type on a frame at dx,dy from Marina
//                                      (flags 0 = the type's own; param: the item parameter, see actors.h)
//   MM_TEST_POKE="DF=40"               bytes to set on slots MM_TEST_SPAWN makes
//   MM_TEST_WORDS=121@60               print all of a slot's words on a frame
//   MM_TEST_PEEK=800C8040[:n]          print 8 (or n) words on frame 1
//   MM_TEST_SETW=121:D0=2003F@30         set a slot's word on a frame (';' for more)
//   MM_TEST_MACHINE=frame              request the clanball machine (Blockman) as the Spawn page does
//   MM_TEST_WATCH=800F43C0@40          SIGTRAP with mm_watch_ptr at that word (gdb: watch -l *mm_watch_ptr)
//   MM_TEST_PKG=1                      with MM_TEST_WARP: print whether the machine's code is loaded
//   MM_TEST_FIND=800E0048              list RDRAM words equal to a value (frame 2)
//   MM_TEST_CRASH=frame                null write on that frame (crash report test)
//   MM_TEST_FFWD=from-to[:speed]       fast-forward between frames
//   MM_TEST_INSPECT=121                collision view on, that slot highlighted
//   MM_TEST_SLOPE=dx,dy,41,45          spawn a slope block object (cell codes) on frame 5
//   MM_TEST_ITEMS=frame                item records near the camera
//   MM_TEST_TERRAIN="x,y;x,y"          terrain bytes around world points (frame 3)
// With MM_TEST_SCRIPT, "[clock] t<frame> <unix time>" lines every 10 frames
// line screenshots up with frames (Wine runs far slower than 60 fps).
#include "dev_harness.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

#include "recomp.h"
#include "actors.h"
#include "custom_actors.h"
#include "inspector.h"
#include "terrain.h"
#include "ultramodern/ultramodern.hpp"
#include "savestate.h"

extern "C" void func_8006CFDC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8004ED10(uint8_t* rdram, recomp_context* ctx);   // Marina's update(slot)

extern "C" { volatile uint32_t* mm_watch_ptr = nullptr; }

namespace mm::dev_harness {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorStride = 0x198u;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

bool in_list(const char* list, long value) {
    for (const char* p = list; p && *p;) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) break;
        if (v == value) return true;
        p = (*end == ',') ? end + 1 : end;
        if (*end != ',') break;
    }
    return false;
}

uint16_t script_buttons(uint8_t* rdram, const char* script, long t) {
    long acc = 0;
    const char* p = script;
    while (*p) {
        char* end = nullptr;
        const long n = std::strtol(p, &end, 10);
        p = end;
        if (*p == ':') ++p;
        uint16_t b = 0;
        while (*p && *p != ',') {
            switch (*p) {
            case 'R': b |= MEM_HU(0, addr(0x800BE510u)); break;
            case 'L': b |= MEM_HU(0, addr(0x800BE50Cu)); break;
            case 'U': b |= 0x0800; break;
            case 'D': b |= 0x0400; break;
            case 'A': b |= 0x8000; break;
            case 'B': b |= 0x4000; break;
            }
            ++p;
        }
        if (*p == ',') ++p;
        if (t < acc + n) return b;
        acc += n;
    }
    return 0;
}

void dump_actor(uint8_t* rdram, long t, int slot) {
    const gpr a = slot_addr(slot);
    std::fprintf(stderr,
                 "[dump] t%ld slot %3d type %04X st %04X fl %08X pos %d,%d hp %d img %04X draw %04X "
                 "98 %08X d6 %d spr %08X dl %08X box %d,%d,%d,%d\n",
                 t, slot, MEM_HU(0xD2, a), MEM_HU(0xD0, a), MEM_W(0x80, a), MEM_H(0x88, a), MEM_H(0x8C, a),
                 MEM_H(0xE0, a), MEM_HU(0x84, a), MEM_HU(0x94, a), MEM_W(0x98, a), MEM_HU(0xD6, a),
                 MEM_W(0x18C, a), MEM_W(0x17C, a), MEM_H(0xAA, a), MEM_H(0xAC, a), MEM_H(0xAE, a),
                 MEM_H(0xB0, a));
}

} // namespace

void tick(uint8_t* rdram) {
    static const char* script = std::getenv("MM_TEST_SCRIPT");
    static const char* dump = std::getenv("MM_TEST_DUMP");
    static const char* trace = std::getenv("MM_TEST_TRACE");
    static const char* diff = std::getenv("MM_TEST_DIFF");
    static const char* spawn = std::getenv("MM_TEST_SPAWN");
    static const char* peek = std::getenv("MM_TEST_PEEK");
    static const char* words = std::getenv("MM_TEST_WORDS");   // slot@frame: all words
    static const char* inspect = std::getenv("MM_TEST_INSPECT");   // =slot: collision view on, slot selected
    if (!std::getenv("MM_TEST_KILL") && !std::getenv("MM_TEST_SETHP") && !std::getenv("MM_TEST_FFWD") && !std::getenv("MM_TEST_CRASH") && !script && !dump && !trace && !diff && !spawn && !peek && !words && !inspect && !std::getenv("MM_TEST_FIND") && !std::getenv("MM_TEST_SETW") && !std::getenv("MM_TEST_MACHINE") && !std::getenv("MM_TEST_WATCH") && !std::getenv("MM_TEST_PKG") && !std::getenv("MM_TEST_P2PROBE")) return;
    if (std::getenv("MM_TEST_PKG")) {             // with MM_TEST_WARP: is the clanball machine's code loaded?
        static long frames = 0;
        if (++frames % 120 == 0 && MEM_HU(0, addr(0x800BE4F0u)) == 6) {
            std::fprintf(stderr, "[pkg] machine %d\n", mm::actors::clanball_machine_loaded(rdram) ? 1 : 0);
        }
    }
    static uint32_t generation = 0;
    static long t = 0;
    static uint16_t prev = 0;
    const uint32_t g = mm::savestate::load_generation();
    if (g != generation) {
        generation = g;
        t = 0;
    }
    if (generation == 0) return;
    ++t;
    if (script && t % 10 == 0) {                 // wall clock, to time screenshots
        const double secs = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "[clock] t%ld %.1f\n", t, secs);
    }
    if (script) {
        const uint16_t in = script_buttons(rdram, script, t);
        const uint16_t press = in & ~prev;
        MEM_H(0, addr(0x801370CCu)) = in;
        MEM_H(0, addr(0x801370CEu)) = press;
        MEM_H(0, addr(0x800BE4F8u)) = in;
        MEM_H(0, addr(0x800BE4FCu)) = press;
        MEM_H(0, addr(0x801225F0u)) = in;
        MEM_H(0, addr(0x8011DD70u)) = press;
        prev = in;
    }
    if (const char* items = std::getenv("MM_TEST_ITEMS"); items && t == std::atol(items)) {
        // Item records within 200 px of the camera: kind, handler, position.
        const uint32_t list = mm::inspector::item_list();
        const int cx = MEM_H(0, addr(0x800BE558u)), cy = MEM_H(0, addr(0x800BE55Cu));
        std::fprintf(stderr, "[items] list %08X cam %d,%d marina %d,%d\n", list, cx, cy,
                     cx + MEM_H(0x88, slot_addr(0)), cy + MEM_H(0x8C, slot_addr(0)));
        for (uint32_t l = list; list != 0 && l < list + 6 * 1024; l += 6) {
            const uint16_t kind = MEM_HU(0, addr(l));
            if (kind == 0) break;
            const int x = MEM_HU(2, addr(l)), y = MEM_HU(4, addr(l));
            if (std::abs(x - cx) > 200 || std::abs(y - cy) > 150) continue;
            std::fprintf(stderr, "[items] %08X kind %04X handler %08X at %d,%d\n", l, kind,
                         static_cast<uint32_t>(MEM_W(((kind & 0x3FF0u) >> 4) * 4, addr(0x800D37BCu))), x, y);
        }
    }
    static const long terrain_at = std::getenv("MM_TEST_TERRAIN_AT") ? std::atol(std::getenv("MM_TEST_TERRAIN_AT")) : 3;
    if (const char* tm = std::getenv("MM_TEST_TERRAIN"); tm && t == terrain_at) {
        // "x,y;x,y": terrain bytes every 8 px within 40 px of each world point.
        for (const char* p = tm; p && *p;) {
            int wx = 0, wy = 0;
            if (std::sscanf(p, "%d,%d", &wx, &wy) == 2) {
                const int h = MEM_HU(0, addr(0x800BE648u));
                std::fprintf(stderr, "[terrain] around %d,%d\n", wx, wy);
                for (int dy = 40; dy >= -40; dy -= 8) {
                    std::fprintf(stderr, "[terrain] y%4d:", wy + dy);
                    for (int dx = -40; dx <= 40; dx += 8) {
                        const uint32_t col = (static_cast<uint32_t>(wx + dx) & 0xFFFF) >> 4;
                        const uint32_t row = static_cast<uint32_t>(h - (wy + dy) - 1) & 0xFFFF;
                        const uint32_t cell = 0x8010CDF0u + (col & MEM_HU(0, addr(0x800BE658u))) +
                                              ((row << MEM_HU(0, addr(0x800BE660u))) & MEM_HU(0, addr(0x800BE65Cu)));
                        std::fprintf(stderr, " %02X", MEM_BU(0, addr(cell)));
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            p = std::strchr(p, ';');
            if (p) ++p;
        }
    }
    if (const char* sl = std::getenv("MM_TEST_SLOPE"); sl && t == 5) {
        // "dx,dy,left,right": a slope block object with those cell codes.
        int dx = 40, dy = 30;
        unsigned left = 0x41, right = 0x45;
        std::sscanf(sl, "%d,%d,%x,%x", &dx, &dy, &left, &right);
        const int slot = mm::actors::spawn_now(rdram, mm::custom_actors::type_id(mm::custom_actors::kBlock), dx, dy);
        if (slot >= 0) {
            const gpr a = slot_addr(slot);
            MEM_H(0x84, a) = 0x44 + 2 * 0x19;
            MEM_H(0xAA, a) = -16; MEM_H(0xAC, a) = 16; MEM_H(0xAE, a) = 8; MEM_H(0xB0, a) = -8;
            mm::custom_actors::make_slope(rdram, slot, static_cast<uint8_t>(left), static_cast<uint8_t>(right));
            std::fprintf(stderr, "[harness] slope block in slot %d\n", slot);
        }
    }
    if (const char* ff = std::getenv("MM_TEST_FFWD")) {
        // "from-to[:speed]": fast-forward between those frames (default 3x).
        long from = 0, to = 0;
        int speed = 3;
        std::sscanf(ff, "%ld-%ld:%d", &from, &to, &speed);
        if (t == from) ultramodern::set_speed_multiplier(speed);
        if (t == to) ultramodern::set_speed_multiplier(1);
    }
    if (const char* cr = std::getenv("MM_TEST_CRASH"); cr && t == std::atol(cr)) {
        std::fprintf(stderr, "[harness] crashing on purpose (crash report test)\n");
        *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;
    }
    if (const char* sh = std::getenv("MM_TEST_SETHP")) {
        int slot = 0, hp = 0;                    // "slot:hp[@frame]" (frame 10 by default)
        long at = 10;
        if (std::sscanf(sh, "%d:%d@%ld", &slot, &hp, &at) >= 2 && t == at) {
            MEM_H(0xE0, slot_addr(slot)) = static_cast<int16_t>(hp);
        }
    }
    if (const char* kl = std::getenv("MM_TEST_KILL")) {
        int slot = 0;                            // "slot@frame": the game's Clancer death (func_8006CFDC)
        long at = 0;
        if (std::sscanf(kl, "%d@%ld", &slot, &at) == 2 && t == at) {
            recomp_context ctx{};
            ctx.r29 = static_cast<gpr>(static_cast<int32_t>(0x8045FF00u));
            ctx.r4 = static_cast<gpr>(slot);
            func_8006CFDC(rdram, &ctx);
            std::fprintf(stderr, "[harness] killed slot %d\n", slot);
        }
    }
    if (inspect && t == 1) {
        if (!mm::inspector::overlay_enabled()) mm::inspector::toggle_overlay();
        mm::inspector::set_selected_slot(std::atoi(inspect));
    }
    if (spawn) {                                 // several specs separated by ';', each once per run
        static std::vector<const char*> done;
        for (const char* p = spawn; p && *p;) {
            unsigned type = 0;
            long at = 0;
            int dx = 40, dy = 0;
            unsigned flags = 0, param = 0;
            if (std::sscanf(p, "%x@%ld@%d@%d@%x@%x", &type, &at, &dx, &dy, &flags, &param) >= 2 && t == at &&
                std::find(done.begin(), done.end(), p) == done.end()) {
                done.push_back(p);
                const int slot = mm::actors::spawn_now(rdram, static_cast<uint16_t>(type), dx, dy, param);
                if (slot >= 0 && flags != 0) MEM_W(0x80, slot_addr(slot)) = static_cast<int32_t>(flags);
                // MM_TEST_POKE="DF=40,A0=E0": bytes to set on spawned slots
                if (const char* poke = std::getenv("MM_TEST_POKE"); poke && slot >= 0) {
                    for (const char* q = poke; q && *q;) {
                        unsigned off = 0, val = 0;
                        if (std::sscanf(q, "%x=%x", &off, &val) == 2) MEM_B(off, slot_addr(slot)) = static_cast<int8_t>(val);
                        q = std::strchr(q, ',');
                        if (q) ++q;
                    }
                }
                std::fprintf(stderr, "[harness] spawned %04X in slot %d\n", type, slot);
            }
            p = std::strchr(p, ';');
            if (p) ++p;
        }
    }
    if (const char* sw = std::getenv("MM_TEST_SETW")) {   // slot:off=value@frame[;...] (words)
        for (const char* p = sw; p && *p;) {
            int slot = 0;
            unsigned off = 0, val = 0;
            long at = 0;
            if (std::sscanf(p, "%d:%x=%x@%ld", &slot, &off, &val, &at) == 4 && t == at) {
                MEM_W(off, slot_addr(slot)) = static_cast<int32_t>(val);
            }
            p = std::strchr(p, ';');
            if (p) ++p;
        }
    }
    if (const char* kit = std::getenv("MM_TEST_MACHINE"); kit && t == std::atol(kit)) {   // =frame[@dx@dy]
        mm::actors::request_spawn(mm::actors::kClanballMachine);   // as the Spawn page does
        std::fprintf(stderr, "[harness] clanball machine requested\n");
    }
    if (const char* w = std::getenv("MM_TEST_WATCH")) {   // vaddr@frame: SIGTRAP with mm_watch_ptr set (for gdb)
        unsigned va = 0;
        long at = 0;
        if (std::sscanf(w, "%x@%ld", &va, &at) == 2 && t == at) {
            mm_watch_ptr = reinterpret_cast<volatile uint32_t*>(rdram + (va - 0x80000000u));
            std::fprintf(stderr, "[harness] watch %08X at %p\n", va, static_cast<const void*>(const_cast<uint32_t*>(mm_watch_ptr)));
#ifdef SIGTRAP
            std::raise(SIGTRAP);
#endif
        }
    }
    if (const char* probe = std::getenv("MM_TEST_P2PROBE")) {   // from,to: words Marina's update writes
        // Each frame in range: snapshot game memory, run Marina's update once
        // more, note every word it changed outside her own slot (and the
        // scratch stack), then put memory back exactly. At the end, print the
        // written words grouped into ranges with how many frames wrote them.
        long from = 0, to = 0;
        std::sscanf(probe, "%ld,%ld", &from, &to);
        static std::vector<uint8_t> before;
        static std::vector<uint32_t> hits;
        constexpr uint32_t kSize = 0x810000;
        if (t >= from && t <= to) {
            if (hits.empty()) hits.assign(kSize / 4, 0);
            before.assign(rdram, rdram + kSize);
            recomp_context c{};
            c.r29 = static_cast<gpr>(static_cast<int32_t>(0x8045FF00u));
            c.r4 = 0;
            c.f_odd = &c.f0.u32h;          // float register pairing, as run_thread_function sets it
            func_8004ED10(rdram, &c);
            for (uint32_t w = 0; w < kSize / 4; ++w) {
                if (reinterpret_cast<const uint32_t*>(rdram)[w] != reinterpret_cast<const uint32_t*>(before.data())[w]) ++hits[w];
            }
            std::memcpy(rdram, before.data(), kSize);
        }
        if (t == to + 1 && !hits.empty()) {
            const uint32_t slot0 = 0xEF510, slot0_end = slot0 + 0x198;
            for (uint32_t w = 0; w < kSize / 4;) {
                const uint32_t a = w * 4;
                if (hits[w] == 0 || (a >= slot0 && a < slot0_end) || (a >= 0x45F000 && a < 0x460000)) { ++w; continue; }
                uint32_t e = w, most = 0;
                while (e < kSize / 4 && hits[e] != 0) { most = std::max(most, hits[e]); ++e; }
                std::fprintf(stderr, "[p2probe] %08X-%08X (%u bytes) written in up to %u frames\n",
                             0x80000000u + a, 0x80000000u + e * 4, (e - w) * 4, most);
                w = e;
            }
        }
    }
    if (words) {
        int slot = 0;
        long at = 0;
        if (std::sscanf(words, "%d@%ld", &slot, &at) == 2 && t == at) {
            const gpr a = slot_addr(slot);
            for (uint32_t o = 0x80; o < kActorStride; o += 0x20) {
                std::fprintf(stderr, "[words] +%03X:", o);
                for (uint32_t k = o; k < o + 0x20 && k < kActorStride; k += 4) {
                    std::fprintf(stderr, " %08X", static_cast<uint32_t>(MEM_W(k, a)));
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    if (const char* find = std::getenv("MM_TEST_FIND"); find && t == 2) {
        // Every aligned word in RDRAM equal to the value (after the load).
        const uint32_t want = static_cast<uint32_t>(std::strtoul(find, nullptr, 16));
        for (uint32_t va = 0x80000000u; va < 0x80800000u; va += 4) {
            if (static_cast<uint32_t>(MEM_W(0, addr(va))) == want) std::fprintf(stderr, "[find] %08X\n", va);
        }
    }
    if (peek && t == 1) {
        char* end = nullptr;
        const uint32_t va = static_cast<uint32_t>(std::strtoul(peek, &end, 16));
        const int count = (end && *end == ':') ? std::atoi(end + 1) : 8;
        for (int i = 0; i < count; ++i) {
            std::fprintf(stderr, "[peek] %08X: %08X\n", va + 4 * i, MEM_W(4 * i, addr(va)));
        }
    }
    if (dump && in_list(dump, t)) {
        for (int s = 0; s < 208; ++s) {
            if (MEM_W(0x80, slot_addr(s)) != 0) dump_actor(rdram, t, s);
        }
    }
    if (trace) {
        const int slot = std::atoi(trace);
        const char* comma = std::strchr(trace, ',');
        const long frames = comma ? std::atol(comma + 1) : 300;
        if (t <= frames) {
            dump_actor(rdram, t, slot);
            const gpr m = slot_addr(0);
            std::fprintf(stderr, "[marina] t%ld st %04X fl %08X pos %d,%d d6 %d cam %d,%d\n", t,
                         MEM_HU(0xD0, m), MEM_W(0x80, m), MEM_H(0x88, m), MEM_H(0x8C, m), MEM_HU(0xD6, m),
                         MEM_H(0, addr(0x800BE558u)), MEM_H(0, addr(0x800BE55Cu)));
        }
    }
    if (diff) {
        int slot = 0;
        long from = 0, to = 0;
        if (std::sscanf(diff, "%d:%ld-%ld", &slot, &from, &to) == 3) {
            static uint32_t before[0x198 / 4];
            const gpr a = slot_addr(slot);
            for (uint32_t o = 0x80; o < kActorStride; o += 4) {
                const uint32_t w = static_cast<uint32_t>(MEM_W(o, a));
                if (t > from && t <= to && w != before[o / 4]) {
                    std::fprintf(stderr, "[diff] t%ld +%03X %08X -> %08X\n", t, o, before[o / 4], w);
                }
                before[o / 4] = w;
            }
        }
    }
}

} // namespace mm::dev_harness

// MM_TEST_P1DIFF=from,to (harness frames): every word Marina's real update
// (func_80020024's call, hooked in troublemakers.us1.toml) changes outside
// her slot, per frame. Finds her globals the P2 probe can't (a flag she sets
// and clears again a frame later).
namespace {
std::vector<uint8_t> g_p1diff_before;
long g_p1diff_frame = 0;
}
extern "C" void mm_test_p1diff_begin(uint8_t* rdram) {
    static const char* env = std::getenv("MM_TEST_P1DIFF");
    if (!env) return;
    static uint32_t gen = 0;
    static long t = 0;
    if (mm::savestate::load_generation() != gen) { gen = mm::savestate::load_generation(); t = 0; }
    ++t;
    long from = 0, to = 0;
    std::sscanf(env, "%ld,%ld", &from, &to);
    g_p1diff_before.clear();
    if (t < from || t > to) return;
    g_p1diff_frame = t;
    g_p1diff_before.assign(rdram, rdram + 0x800000);
}
extern "C" void mm_test_p1diff_end(uint8_t* rdram) {
    if (g_p1diff_before.empty()) return;
    const uint32_t* a = reinterpret_cast<const uint32_t*>(g_p1diff_before.data());
    const uint32_t* b = reinterpret_cast<const uint32_t*>(rdram);
    for (uint32_t w = 0; w < 0x800000 / 4; ++w) {
        const uint32_t va = 0x80000000u + w * 4;
        if (a[w] == b[w] || (va >= 0x800EF510u && va < 0x800EF510u + 0x198u) || (va >= 0x80120000u && va < 0x80129000u)) continue;
        std::fprintf(stderr, "[p1diff] t%ld %08X %08X -> %08X\n", g_p1diff_frame, va, a[w], b[w]);
    }
    g_p1diff_before.clear();
}
