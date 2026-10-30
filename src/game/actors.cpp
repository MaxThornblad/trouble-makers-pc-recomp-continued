#include "actors.h"
#include "foreign_actors.h"
#include "frame_input.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
struct Request {
    uint16_t type;
    uint32_t param;
};
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

// Requests go through the input stream (frame_input.h), so a spawn is on the
// same frame in a replay and, online, the host's spawns reach both machines.
void request_spawn(uint16_t type, uint32_t param) {
    mm::frame_input::Command c;
    c.kind = mm::frame_input::Cmd::Spawn;
    c.a = type;
    c.b = param;
    mm::frame_input::request(c);
}

void request_clear() {
    mm::frame_input::Command c;
    c.kind = mm::frame_input::Cmd::ClearSpawns;
    mm::frame_input::request(c);
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

int spawn_now(uint8_t* rdram, uint16_t type, int dx, int dy, uint32_t param) {
    int slot = -1;
    if (const int span = mm::foreign_actors::span(type); span > 1) {
        // An object that builds its parts in the slots after its own
        // (whatever is in them): a free run, below 0x90 as its stage's
        // spawner puts it (its parts take part in collisions there), else
        // below the particles.
        slot = free_run(rdram, span, 0x8F);
        if (slot < 0) slot = free_run(rdram, span, 0xBF);
    } else {
        for (int i = kLastSlot; i >= kFirstSlot; --i) {
            if (MEM_W(0x80, slot_addr(i)) == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return -1;
    spawn_in(rdram, slot, type, dx, dy, param);
    return slot;
}

int free_run(uint8_t* rdram, int count, int last) {
    for (int first = last - count + 1; first >= kFirstSlot; --first) {
        bool all_free = true;
        for (int i = first; i < first + count && all_free; ++i) all_free = MEM_W(0x80, slot_addr(i)) == 0;
        if (all_free) return first;
    }
    return -1;
}

void spawn_in(uint8_t* rdram, int slot, uint16_t type, int dx, int dy, uint32_t param) {
    const gpr a = slot_addr(slot);
    for (uint32_t off = 0x80; off < kActorStride; off += 4) {
        MEM_W(off, a) = 0;                       // 0x00-0x7F are renderer matrices
    }
    MEM_H(0xD2, a) = static_cast<int16_t>(type);
    recomp_context ctx{};
    ctx.r29 = addr(kScratchStack);
    ctx.r4 = static_cast<gpr>(slot);
    func_8001E2D0(rdram, &ctx);                  // clears +0x110
    if (param != 0) {
        // Stage spawners hand an item's parameter (colour, variant) to the
        // type's first update as a float at +0x110 (the clanball setup,
        // func_80039724, turns it into +0x150 and picks its colour from it).
        const float f = static_cast<float>(param);
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        MEM_W(0x110, a) = static_cast<int32_t>(bits);
    }

    const gpr marina = slot_addr(0);
    MEM_W(0x88, a) = MEM_W(0x88, marina) + static_cast<int32_t>(dx * 65536);
    MEM_W(0x8C, a) = MEM_W(0x8C, marina) + static_cast<int32_t>(dy * 65536);
    if (MEM_W(0x80, a) == 0) MEM_W(0x80, a) = 3;  // active + drawn, as most spawners leave it
    if (mm::foreign_actors::known(type)) mm::foreign_actors::setup(rdram, slot);
    if (const auto* custom = mm::custom_actors::info(type); custom != nullptr && custom->setup) {
        custom->setup(rdram, slot);
    }
}

bool clanball_machine_loaded(uint8_t* rdram) {
    // Bank 0x18's type table (0x801A6800) points type 0x1D at the
    // controller's update, whose first instruction is addiu sp, sp, -0x58.
    return static_cast<uint32_t>(MEM_W(0, addr(0x801A6874u))) == 0x801AE1B4u &&
           static_cast<uint32_t>(MEM_W(0, addr(0x801AE1B4u))) == 0x27BDFFA8u &&
           static_cast<uint32_t>(MEM_W(0, addr(0x801AE1B8u))) == 0xAFB20030u;
}

// When the controller takes the held clanball as the machine's body (its
// state 0x11) it remakes it as a type 0x1C clanball whose item parameter is
// the float at 0x801AFB6C, 0xE431 in stage 7: bit 0x8000 = pass Marina's grab
// signals on to the slot in the low byte, which is the stage's own
// controller, slot 0x31. A controller elsewhere would get no grab signal and
// fall apart at once, so while a spawned machine's clanball is held the low
// byte names that controller instead; the stage's value is put back after.
constexpr uint32_t kBodyParam = 0x801AFB6Cu;
uint32_t g_saved_body_param = 0;                 // float bits; 0: not overridden

uint32_t float_bits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

void link_machine_bodies(uint8_t* rdram, const std::vector<Owned>& owned) {
    if (!clanball_machine_loaded(rdram)) {
        g_saved_body_param = 0;                  // another stage's package
        return;
    }
    const uint32_t current = static_cast<uint32_t>(MEM_W(0, addr(kBodyParam)));
    const int held = MEM_HU(0xD6, slot_addr(0));   // Marina's held slot
    int controller = -1;
    for (const Owned& o : owned) {
        if (o.type != 0x181D || !alive(rdram, o)) continue;
        const int state = MEM_HU(0xD0, slot_addr(o.slot));
        if ((state == 0x10 || state == 0x11) && held == o.slot - 1) controller = o.slot;
    }
    if (controller >= 0) {
        if (g_saved_body_param == 0) g_saved_body_param = current;
        float stage_value;
        std::memcpy(&stage_value, &g_saved_body_param, sizeof stage_value);
        const uint32_t param = (static_cast<uint32_t>(stage_value) & ~0xFFu) | static_cast<uint32_t>(controller);
        MEM_W(0, addr(kBodyParam)) = static_cast<int32_t>(float_bits(static_cast<float>(param)));
        } else if (g_saved_body_param != 0) {
        MEM_W(0, addr(kBodyParam)) = static_cast<int32_t>(g_saved_body_param);
        g_saved_body_param = 0;
    }
}

const char* spawn_clanball_machine(uint8_t* rdram, int dx, int dy, int* slot) {
    if (!clanball_machine_loaded(rdram)) return "The clanball machine only works in the desert stages";
    // The controller reads the clanball from the slot before its own
    // (func_801AE1B4: state 0x10 waits for it to be held, 0x11 makes it the
    // machine's body) and fills the 12 slots after it with the machine.
    // Slots below 0x90 take part in collisions (a clanball above can't be
    // grabbed), and the machine's parts collide too: all 14 go below 0x90.
    int first = free_run(rdram, 14, 0x8F);
    if (first < 0) return "Not enough free actor slots right now";
    spawn_in(rdram, first, 0x3F, dx, dy, 0x403);
    spawn_in(rdram, first + 1, 0x181D, dx, dy + 46);   // as placed in stage 7: 46 px above the ball
    if (slot) *slot = first;
    return nullptr;
}

void tick(uint8_t* rdram) {
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    const bool gameplay = game_state == 6 || game_state == 10;
    const int stage = MEM_HU(0, addr(kCurrentStage));
    const int scene = static_cast<int16_t>(MEM_HU(0, addr(kCurrentScene)));
    const uint32_t generation = mm::savestate::load_generation();
    if (stage != g_stage || scene != g_scene || generation != g_generation || !gameplay) {
        g_owned.clear();                         // the game reuses or clears those slots
        g_saved_body_param = 0;                  // the stage package was reloaded with the state
        g_stage = stage;
        g_scene = scene;
        g_generation = generation;
    }

    std::vector<Request> queue;
    bool clear = false;
    {
        mm::frame_input::Command c;
        while (mm::frame_input::take_command(mm::frame_input::Cmd::Spawn, &c)) {
            queue.push_back({static_cast<uint16_t>(c.a), c.b});
        }
        if (mm::frame_input::take_command(mm::frame_input::Cmd::ClearSpawns, &c)) {
            clear = true;
            queue.clear();
        }
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

    for (const Request& request : queue) {
        const uint16_t type = request.type;
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
        if (type == kClanballMachine) {
            // At Marina's height, as stage 7 places it: the clanball hovers where it starts.
            int slot = -1;
            if (const char* error = spawn_clanball_machine(rdram, 36 * facing, 0, &slot)) {
                set_result(error);
                break;
            }
            g_owned.push_back({slot, 0x3F});
            g_owned.push_back({slot + 1, 0x181D});
            std::snprintf(text, sizeof text, "Spawned the clanball machine in slots %d-%d", slot, slot + 1);
            set_result(text);
            std::fprintf(stderr, "[actors] clanball machine in slots %d-%d\n", slot, slot + 1);
            continue;
        }
        // Lunar's missile flies right: it starts behind Marina on the left,
        // low enough to pass under a jump onto it.
        const bool missile = type == 0x0F09;
        const int slot = spawn_now(rdram, type, missile ? -150 : 40 * facing, missile ? 8 : 24, request.param);
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
    if (gameplay) link_machine_bodies(rdram, g_owned);

}

} // namespace mm::actors
