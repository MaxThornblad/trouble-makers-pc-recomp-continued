// Headless test harness for actor work (environment variables only; inert
// otherwise). Runs once per frame from mm_debug_menu_tick, counting frames
// from the last save-state load:
//   MM_TEST_SCRIPT="30:,2:B,60:,2:RB"  frames:buttons runs (R/L/U/D/A/B), fed
//                                      to Marina's and the global pad words
//   MM_TEST_DUMP=40,120                list every active actor on those frames
//   MM_TEST_TRACE=121[,frames]         one line per frame for a slot (and Marina)
//   MM_TEST_DIFF=121:100-150           print changed words of a slot in a range
//   MM_TEST_SPAWN=18@20@40@0[@flags]   spawn a type on a frame at dx,dy from Marina
//   MM_TEST_POKE="DF=40"               bytes to set on slots MM_TEST_SPAWN makes
//   MM_TEST_WORDS=121@60               print all of a slot's words on a frame
//   MM_TEST_PEEK=800C8040              print 8 words on frame 1
//   MM_TEST_INSPECT=121                collision view on, that slot highlighted
// With MM_TEST_SCRIPT, "[clock] t<frame> <unix time>" lines every 10 frames
// line screenshots up with frames (Wine runs far slower than 60 fps).
#include "dev_harness.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "actors.h"
#include "inspector.h"
#include "savestate.h"

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
    if (!script && !dump && !trace && !diff && !spawn && !peek && !words && !inspect) return;
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
    if (inspect && t == 1) {
        if (!mm::inspector::overlay_enabled()) mm::inspector::toggle_overlay();
        mm::inspector::set_selected_slot(std::atoi(inspect));
    }
    if (spawn) {                                 // several specs separated by ';'
        for (const char* p = spawn; p && *p;) {
            unsigned type = 0;
            long at = 0;
            int dx = 40, dy = 0;
            unsigned flags = 0;
            if (std::sscanf(p, "%x@%ld@%d@%d@%x", &type, &at, &dx, &dy, &flags) >= 2 && t == at) {
                const int slot = mm::actors::spawn_now(rdram, static_cast<uint16_t>(type), dx, dy);
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
    if (peek && t == 1) {
        const uint32_t va = static_cast<uint32_t>(std::strtoul(peek, nullptr, 16));
        for (int i = 0; i < 8; ++i) {
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
