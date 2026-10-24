#include "actors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "custom_actors.h"
#include "recomp.h"
#include "savestate.h"

extern "C" void func_8001E2D0(uint8_t* rdram, recomp_context* ctx);

namespace mm::actors {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 and 10 = stage gameplay
constexpr uint32_t kCurrentStage = 0x80178162u;  // progression row
constexpr uint32_t kCurrentScene = 0x800BE5D0u;
constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorStride = 0x198u;
// Free slots are taken from the top of this range down: the game's own
// spawners fill ranges from the bottom, and gems use 0x7A-0x82.
constexpr int kFirstSlot = 0x30;
constexpr int kLastSlot = 0x79;
// func_8001E2D0 is a leaf that only stores its argument at 0($sp); give it a
// scratch stack word in host-only expansion memory (docs/README.md 14.1 gap).
constexpr uint32_t kScratchStack = 0x8045FF00u;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

struct Owned {
    int slot;
    uint16_t type;
};

std::mutex g_mutex;
std::vector<uint16_t> g_spawn_queue;
bool g_clear_requested = false;
std::string g_last_result = "Nothing spawned yet";
std::atomic<int> g_count{0};

// Game thread only.
std::vector<Owned> g_owned;
int g_stage = -1, g_scene = -1;
uint32_t g_generation = 0;

void set_result(const std::string& text) {
    std::lock_guard lock{g_mutex};
    g_last_result = text;
}

bool alive(uint8_t* rdram, const Owned& o) {
    const gpr a = slot_addr(o.slot);
    return MEM_W(0x80, a) != 0 && MEM_HU(0xD2, a) == o.type;
}

} // namespace

void request_spawn(uint16_t type) {
    std::lock_guard lock{g_mutex};
    if (g_spawn_queue.size() < 4) g_spawn_queue.push_back(type);
}

void request_clear() {
    std::lock_guard lock{g_mutex};
    g_clear_requested = true;
    g_spawn_queue.clear();
}

int spawned_count() {
    return g_count.load(std::memory_order_relaxed);
}

const char* last_result() {
    // Copied into a per-thread buffer so the menu can hold it while drawing.
    thread_local std::string copy;
    std::lock_guard lock{g_mutex};
    copy = g_last_result;
    return copy.c_str();
}

int spawn_now(uint8_t* rdram, uint16_t type, int dx, int dy) {
    int slot = -1;
    for (int i = kLastSlot; i >= kFirstSlot; --i) {
        if (MEM_W(0x80, slot_addr(i)) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    const gpr a = slot_addr(slot);
    for (uint32_t off = 0x80; off < kActorStride; off += 4) {
        MEM_W(off, a) = 0;                       // 0x00-0x7F are renderer matrices
    }
    MEM_H(0xD2, a) = static_cast<int16_t>(type);
    recomp_context ctx{};
    ctx.r29 = addr(kScratchStack);
    ctx.r4 = static_cast<gpr>(slot);
    func_8001E2D0(rdram, &ctx);

    const gpr marina = slot_addr(0);
    MEM_W(0x88, a) = MEM_W(0x88, marina) + static_cast<int32_t>(dx * 65536);
    MEM_W(0x8C, a) = MEM_W(0x8C, marina) + static_cast<int32_t>(dy * 65536);
    if (MEM_W(0x80, a) == 0) MEM_W(0x80, a) = 3;  // active + drawn, as most spawners leave it
    if (const auto* custom = mm::custom_actors::info(type); custom != nullptr && custom->setup) {
        custom->setup(rdram, slot);
    }
    return slot;
}

void tick(uint8_t* rdram) {
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    const bool gameplay = game_state == 6 || game_state == 10;
    const int stage = MEM_HU(0, addr(kCurrentStage));
    const int scene = static_cast<int16_t>(MEM_HU(0, addr(kCurrentScene)));
    const uint32_t generation = mm::savestate::load_generation();
    if (stage != g_stage || scene != g_scene || generation != g_generation || !gameplay) {
        g_owned.clear();                         // the game reuses or clears those slots
        g_stage = stage;
        g_scene = scene;
        g_generation = generation;
    }

    std::vector<uint16_t> queue;
    bool clear = false;
    {
        std::lock_guard lock{g_mutex};
        queue.swap(g_spawn_queue);
        clear = g_clear_requested;
        g_clear_requested = false;
    }

    g_owned.erase(std::remove_if(g_owned.begin(), g_owned.end(),
                                 [rdram](const Owned& o) { return !alive(rdram, o); }),
                  g_owned.end());

    if (clear) {
        for (const Owned& o : g_owned) MEM_W(0x80, slot_addr(o.slot)) = 0;
        char text[64];
        std::snprintf(text, sizeof text, "Removed %zu spawned object%s", g_owned.size(),
                      g_owned.size() == 1 ? "" : "s");
        set_result(text);
        g_owned.clear();
    }

    for (uint16_t type : queue) {
        char text[96];
        if (!gameplay) {
            set_result("Spawning works only during a stage");
            break;
        }
        if (static_cast<int>(g_owned.size()) >= kMaxSpawned) {
            std::snprintf(text, sizeof text, "Limit reached (%d spawned); clear some first", kMaxSpawned);
            set_result(text);
            break;
        }
        // In front of Marina (flag 0x20 at +0x80 = facing left), a little above.
        const int facing = (MEM_W(0x80, slot_addr(0)) & 0x20) != 0 ? -1 : 1;
        const int slot = spawn_now(rdram, type, 40 * facing, 24);
        if (slot < 0) {
            set_result("No free actor slot right now");
            break;
        }
        g_owned.push_back({slot, type});
        std::snprintf(text, sizeof text, "Spawned type 0x%02X in slot %d", type, slot);
        set_result(text);
        std::fprintf(stderr, "[actors] spawned type 0x%02X in slot %d\n", type, slot);
    }
    g_count.store(static_cast<int>(g_owned.size()), std::memory_order_relaxed);

}

} // namespace mm::actors
