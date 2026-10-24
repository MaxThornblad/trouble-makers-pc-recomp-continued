#include "enemy_bars.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <mutex>

#include "cheats.h"
#include "recomp.h"
#include "savestate.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" int mm_widescreen_gameplay_active(uint8_t* rdram);

namespace mm::enemy_bars {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 and 10 = stage gameplay
constexpr uint32_t kGamePaused = 0x800BE4E8u;
constexpr uint32_t kActors = 0x800EF510u;        // slot 0 is Marina
constexpr uint32_t kActorStride = 0x198u;
constexpr int kSlots = 208;

constexpr int kShowFrames = 4 * 60;              // a bar lingers this long after a hit
constexpr int kFadeFrames = 40;
constexpr int kBossHealth = 30;                  // at least this much: keep the bar up
constexpr int kTimerRun = 3;                     // drops in a row = a countdown, not a hit
constexpr int kResetBurst = 4;                   // this many actors at once = a reset

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

struct Slot {
    uint16_t type = 0;
    int16_t hp = 0;
    int16_t max_hp = 0;
    int run = 0;                 // consecutive frames the health went down
    bool ignore = false;         // behaves like a timer
    bool tracked = false;        // has been hit and survived
    long last_hit = -100000;
};
std::array<Slot, kSlots> g_slots;
long g_frame = 0;
bool g_valid = false;
uint32_t g_generation = 0;

struct Bar {
    float x, y;                  // camera-relative pixels, bar centre, +y up
    float fraction;
    float alpha;
};
std::mutex g_bars_mutex;
std::vector<Bar> g_bars;
bool g_widescreen = false;

} // namespace

void publish(uint8_t* rdram) {
    ++g_frame;
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    const bool gameplay = game_state == 6 || game_state == 10;
    const bool paused = MEM_HU(0, addr(kGamePaused)) != 0;
    const uint32_t generation = mm::savestate::load_generation();
    if (!gameplay || !mm::cheats::enemy_bars_enabled() || generation != g_generation) {
        g_valid = false;
        g_generation = generation;
        std::lock_guard lock{g_bars_mutex};
        g_bars.clear();
        return;
    }
    if (paused) return;                              // keep the last bars while paused

    // First pass: read every slot and count simultaneous drops.
    struct Reading { uint16_t type; int16_t hp; };
    std::array<Reading, kSlots> now{};
    int drops = 0;
    for (int i = 1; i < kSlots; ++i) {
        const gpr a = addr(kActors + i * kActorStride);
        now[i].type = MEM_W(0x80, a) != 0 ? MEM_HU(0xD2, a) : 0;
        now[i].hp = MEM_H(0xE0, a);
        const Slot& s = g_slots[i];
        if (g_valid && now[i].type != 0 && now[i].type == s.type && now[i].hp < s.hp) ++drops;
    }
    const bool reset = drops >= kResetBurst;

    std::vector<Bar> bars;
    for (int i = 1; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        const Reading r = now[i];
        if (!g_valid || r.type == 0 || r.type != s.type) {
            s = Slot{};
            s.type = r.type;
            s.hp = r.hp;
            s.max_hp = std::max<int16_t>(r.hp, 0);
            continue;
        }
        if (r.hp < s.hp) {
            ++s.run;
            if (s.run >= kTimerRun) {
                s.ignore = true;
                s.tracked = false;
            }
            if (!reset && !s.ignore && s.hp > 0 && r.hp > 0 && s.max_hp >= 2) {
                s.tracked = true;
                s.last_hit = g_frame;
            }
        } else {
            s.run = 0;
        }
        s.max_hp = std::max(s.max_hp, r.hp);
        s.hp = r.hp;

        if (!s.tracked || r.hp <= 0) continue;
        const bool boss = s.max_hp >= kBossHealth;
        const long since = g_frame - s.last_hit;
        if (!boss && since > kShowFrames) {
            s.tracked = false;
            continue;
        }
        float alpha = 1.0f;
        if (!boss && since > kShowFrames - kFadeFrames) {
            alpha = static_cast<float>(kShowFrames - since) / kFadeFrames;
        }
        const gpr a = addr(kActors + i * kActorStride);
        const float x = static_cast<float>(MEM_H(0x88, a));
        const float top = static_cast<float>(MEM_H(0x8C, a)) +
                          static_cast<float>(std::max(MEM_H(0xAE, a), MEM_H(0xB0, a)));
        bars.push_back({x, top + 6.0f,
                        std::clamp(static_cast<float>(r.hp) / s.max_hp, 0.0f, 1.0f), alpha});
    }
    g_valid = true;

    std::lock_guard lock{g_bars_mutex};
    g_bars = std::move(bars);
    g_widescreen = mm_widescreen_gameplay_active(rdram) != 0;
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    std::vector<Bar> bars;
    bool widescreen;
    {
        std::lock_guard lock{g_bars_mutex};
        if (g_bars.empty()) return;
        bars = g_bars;
        widescreen = g_widescreen;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Same canvas mapping as the gem compass: 320x240 fills the window
    // height; the stage view spans 144 px either side of centre (the whole
    // width in widescreen), 100 px above and 90 below.
    const float scale = io.DisplaySize.y / 240.0f;
    const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    const float half_w = widescreen ? io.DisplaySize.x * 0.5f : 144.0f * scale;
    const float w = 26.0f * scale, h = std::max(3.0f, 2.8f * scale);
    for (const Bar& b : bars) {
        const float cx = centre.x + b.x * scale;
        const float cy = centre.y - b.y * scale;
        if (std::abs(cx - centre.x) > half_w + w || cy < centre.y - 100.0f * scale - h ||
            cy > centre.y + 90.0f * scale) {
            continue;
        }
        const auto a = [&b](int v) { return static_cast<int>(v * b.alpha); };
        const ImVec2 p0(cx - w * 0.5f, cy - h * 0.5f), p1(cx + w * 0.5f, cy + h * 0.5f);
        dl->AddRectFilled(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, a(200)));
        dl->AddRectFilled(p0, p1, IM_COL32(70, 20, 20, a(220)));
        // Green when healthy, through yellow, to red when nearly beaten.
        const float f = b.fraction;
        const int red = static_cast<int>(255 * std::min(1.0f, 2.0f * (1.0f - f)));
        const int green = static_cast<int>(255 * std::min(1.0f, 2.0f * f));
        dl->AddRectFilled(p0, ImVec2(p0.x + w * f, p1.y), IM_COL32(red, green, 40, a(255)));
    }
}
#endif

} // namespace mm::enemy_bars
