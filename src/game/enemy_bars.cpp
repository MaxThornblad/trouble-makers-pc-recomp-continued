#include "enemy_bars.h"
#include "player2.h"

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
constexpr uint32_t kCameraX = 0x800BE558u;       // Q16.16 camera centre
constexpr uint32_t kCameraY = 0x800BE55Cu;
constexpr uint32_t kActorStride = 0x198u;
constexpr int kSlots = 208;

constexpr int kShowFrames = 4 * 60;              // a bar lingers this long after a hit
constexpr int kFadeFrames = 40;
constexpr int kBossHealth = 100;                 // at least this much: keep the bar up
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
    int pending = 0;             // damage in the current run of drops, not yet shown
    float wx = 0, wy = 0;        // last world position of the number spot
    long ones[3] = {-1000, -1000, -1000};   // frames of the last three drops of exactly 1
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
// Floating damage numbers, in world pixels so they stay put while the camera
// scrolls. Spawned when a run of health drops ends (a timer never ends its
// run, so it never shows a number), rise for a second and fade out.
struct Number {
    float x, y;                  // world pixels, +y up
    int amount;
    bool marina;                 // Marina's own damage is shown in red
    long born;
};
constexpr int kNumberFrames = 60;
std::vector<Number> g_numbers;   // game thread

std::mutex g_bars_mutex;
std::vector<Bar> g_bars;
std::vector<Number> g_numbers_shared;
long g_frame_shared = 0;
int g_cam_x = 0, g_cam_y = 0;
bool g_widescreen = false;

} // namespace

void publish(uint8_t* rdram) {
    ++g_frame;
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    const bool gameplay = game_state == 6 || game_state == 10;
    const bool paused = MEM_HU(0, addr(kGamePaused)) != 0;
    const uint32_t generation = mm::savestate::load_generation();
    const bool show_bars = mm::cheats::enemy_bars_enabled();
    const bool show_numbers = mm::cheats::damage_numbers_enabled();
    if (!gameplay || (!show_bars && !show_numbers) || generation != g_generation) {
        g_valid = false;
        g_generation = generation;
        g_numbers.clear();
        std::lock_guard lock{g_bars_mutex};
        g_bars.clear();
        g_numbers_shared.clear();
        return;
    }
    if (paused) return;                              // keep the last bars while paused

    // First pass: read every slot and count simultaneous drops.
    struct Reading { uint16_t type; int16_t hp; };
    std::array<Reading, kSlots> now{};
    int drops = 0;
    for (int i = 0; i < kSlots; ++i) {
        const gpr a = addr(kActors + i * kActorStride);
        now[i].type = MEM_W(0x80, a) != 0 ? MEM_HU(0xD2, a) : 0;
        now[i].hp = MEM_H(0xE0, a);
        const Slot& s = g_slots[i];
        if (g_valid && now[i].type != 0 && now[i].type == s.type && now[i].hp < s.hp) ++drops;
    }
    const bool reset = drops >= kResetBurst;

    const int cam_x = MEM_H(0, addr(kCameraX));
    const int cam_y = MEM_H(0, addr(kCameraY));
    std::vector<Bar> bars;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        const Reading r = now[i];
        if (!g_valid || r.type == 0 || r.type != s.type) {
            // A finishing hit can remove the actor at once: show what it took.
            if (g_valid && s.pending > 0 && show_numbers) {
                g_numbers.push_back({s.wx, s.wy, s.pending, i == 0, g_frame});
            }
            s = Slot{};
            s.type = r.type;
            s.hp = r.hp;
            s.max_hp = std::max<int16_t>(r.hp, 0);
            continue;
        }
        const gpr a = addr(kActors + i * kActorStride);
        s.wx = static_cast<float>(cam_x + MEM_H(0x88, a));
        s.wy = static_cast<float>(cam_y + MEM_H(0x8C, a)) +
               static_cast<float>(std::max(MEM_H(0xAE, a), MEM_H(0xB0, a))) + 14.0f;
        if (r.hp < s.hp) {
            ++s.run;
            // A slow countdown (one point every few frames) is not a run but
            // still not a hit: three single-point drops within a second.
            if (s.hp - r.hp == 1) {
                s.ones[0] = s.ones[1];
                s.ones[1] = s.ones[2];
                s.ones[2] = g_frame;
                if (g_frame - s.ones[0] <= 60) s.run = kTimerRun;
            }
            if (s.run >= kTimerRun) {
                s.ignore = true;
                s.tracked = false;
                s.pending = 0;
            }
            if (!reset && !s.ignore && s.hp > 0) {
                s.pending += s.hp - std::max<int>(r.hp, 0);
            }
            if (!reset && !s.ignore && s.hp > 0 && r.hp > 0 && s.max_hp >= 2) {
                s.tracked = true;
                s.last_hit = g_frame;
            }
        } else {
            if (s.pending > 0 && show_numbers) {
                g_numbers.push_back({s.wx, s.wy, s.pending, i == 0, g_frame});
            }
            s.pending = 0;
            s.run = 0;
        }
        s.max_hp = std::max(s.max_hp, r.hp);
        s.hp = r.hp;

        if (i == 0 || !show_bars || !s.tracked || r.hp <= 0) continue;
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
        const float x = static_cast<float>(MEM_H(0x88, a));
        const float top = static_cast<float>(MEM_H(0x8C, a)) +
                          static_cast<float>(std::max(MEM_H(0xAE, a), MEM_H(0xB0, a)));
        bars.push_back({x, top + 6.0f,
                        std::clamp(static_cast<float>(r.hp) / s.max_hp, 0.0f, 1.0f), alpha});
    }
    g_valid = true;
    g_numbers.erase(std::remove_if(g_numbers.begin(), g_numbers.end(),
                                   [](const Number& n) { return g_frame - n.born > kNumberFrames; }),
                    g_numbers.end());

    std::lock_guard lock{g_bars_mutex};
    g_bars = std::move(bars);
    g_numbers_shared = g_numbers;
    g_frame_shared = g_frame;
    g_cam_x = cam_x;
    g_cam_y = cam_y;
    g_widescreen = mm_widescreen_gameplay_active(rdram) != 0;
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    std::vector<Bar> bars;
    std::vector<Number> numbers;
    bool widescreen;
    long frame;
    int cam_x, cam_y;
    {
        std::lock_guard lock{g_bars_mutex};
        if (g_bars.empty() && g_numbers_shared.empty()) return;
        bars = g_bars;
        numbers = g_numbers_shared;
        widescreen = g_widescreen;
        frame = g_frame_shared;
        cam_x = g_cam_x;
        cam_y = g_cam_y;
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
    float vx, vy;   // netplay's own-view offset (player2.h)
    mm::player2::view_offset(vx, vy);
    for (const Bar& b : bars) {
        const float cx = centre.x + (b.x - vx) * scale;
        const float cy = centre.y - (b.y - vy) * scale;
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

    // Damage numbers: rise about 20 px over a second, fading in the last third.
    ImFont* font = ImGui::GetFont();
    const float size = 11.0f * scale;
    for (const Number& n : numbers) {
        const float age = static_cast<float>(frame - n.born) / kNumberFrames;
        if (age < 0.0f || age > 1.0f) continue;
        const float rx = n.x - cam_x;
        const float ry = n.y - cam_y + 20.0f * (1.0f - (1.0f - age) * (1.0f - age));
        const float cx = centre.x + (rx - vx) * scale;
        const float cy = centre.y - (ry - vy) * scale;
        if (std::abs(cx - centre.x) > half_w || cy < centre.y - 100.0f * scale ||
            cy > centre.y + 90.0f * scale) {
            continue;
        }
        const float alpha = age < 0.66f ? 1.0f : (1.0f - age) / 0.34f;
        char text[16];
        std::snprintf(text, sizeof text, "-%d", n.amount);
        const ImVec2 ext = font->CalcTextSizeA(size, 1e9f, 0.0f, text);
        const ImVec2 pos(cx - ext.x * 0.5f, cy - ext.y * 0.5f);
        const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(220 * alpha));
        const float o = std::max(1.0f, 0.4f * scale);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx != 0 || dy != 0) {
                    dl->AddText(font, size, ImVec2(pos.x + dx * o, pos.y + dy * o), shadow, text);
                }
            }
        }
        const ImU32 colour = n.marina ? IM_COL32(255, 90, 80, static_cast<int>(255 * alpha))
                                      : IM_COL32(255, 235, 120, static_cast<int>(255 * alpha));
        dl->AddText(font, size, pos, colour, text);
    }
}
#endif

} // namespace mm::enemy_bars
