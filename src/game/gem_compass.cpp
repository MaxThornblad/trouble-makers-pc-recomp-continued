#include "split_view.h"
#include "gem_compass.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "cheats.h"
#include "recomp.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" int mm_widescreen_gameplay_active(uint8_t* rdram);

namespace mm::gem_compass {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 = gameplay
constexpr uint32_t kGamePaused = 0x800BE4E8u;
constexpr uint32_t kCurrentStage = 0x80178162u;  // progression row
constexpr uint32_t kGoldGemsHi = 0x80171B10u;    // rows 32..63
constexpr uint32_t kGoldGemsLo = 0x80171B14u;    // rows 0..31
constexpr uint32_t kCameraX = 0x800BE558u;       // Q16.16 camera centre
constexpr uint32_t kCameraY = 0x800BE55Cu;
constexpr uint32_t kMarina = 0x800EF510u;
constexpr uint32_t kItemHandlers = 0x800D37BCu;
constexpr uint32_t kGemHandler = 0x800446F0u;
constexpr int kMaxItems = 512;
constexpr int kCampaignRows = 58;

// Game-thread scan results, consumed by publish() in the same thread.
bool g_scan_ran = false;
bool g_scan_found = false;
int g_scan_x = 0, g_scan_y = 0;

// Published for the render thread.
std::atomic<bool> g_active{false};
std::atomic<bool> g_found{false};
std::atomic<int> g_dx{0};       // gem - camera centre, N64 pixels, +y up
std::atomic<int> g_dy{0};
std::atomic<int> g_dist{0};     // gem - Marina, pixels
std::atomic<bool> g_widescreen{false};

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

} // namespace

void scan_items(uint8_t* rdram, uint32_t list) {
    g_scan_ran = true;
    g_scan_found = false;
    for (int i = 0; i < kMaxItems; ++i, list += 6) {
        const gpr rec = addr(list);
        const uint16_t kind = MEM_HU(0, rec);
        if (kind == 0) break;
        if ((kind & 0xC000u) != 0 || (kind & 3u) != 2) continue;   // gone, or not gold
        const uint32_t handler = static_cast<uint32_t>(
            MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers)));
        if (handler != kGemHandler) continue;
        g_scan_found = true;
        g_scan_x = MEM_HU(2, rec);
        g_scan_y = MEM_HU(4, rec);
        break;
    }
}

void publish(uint8_t* rdram) {
    const int row = MEM_HU(0, addr(kCurrentStage));
    const bool gameplay = MEM_HU(0, addr(kGameState)) == 6 && MEM_HU(0, addr(kGamePaused)) == 0;
    bool collected = true;
    if (row >= 0 && row < kCampaignRows) {
        const uint32_t word = static_cast<uint32_t>(MEM_W(0, addr(row < 32 ? kGoldGemsLo : kGoldGemsHi)));
        collected = ((word >> (row & 31)) & 1u) != 0;
    }
    const bool ran = g_scan_ran;
    g_scan_ran = false;
    if (!gameplay || collected || !ran || !mm::cheats::gem_compass_enabled()) {
        g_active.store(false, std::memory_order_release);
        return;
    }
    const int cam_x = MEM_H(0, addr(kCameraX));
    const int cam_y = MEM_H(0, addr(kCameraY));
    const gpr m = addr(kMarina);
    const int marina_x = cam_x + MEM_H(0x88, m);
    const int marina_y = cam_y + MEM_H(0x8C, m);
    if (g_scan_found) {
        g_dx.store(g_scan_x - cam_x, std::memory_order_relaxed);
        g_dy.store(g_scan_y - cam_y, std::memory_order_relaxed);
        const double ddx = g_scan_x - marina_x, ddy = g_scan_y - marina_y;
        g_dist.store(static_cast<int>(std::sqrt(ddx * ddx + ddy * ddy)), std::memory_order_relaxed);
    }
    g_found.store(g_scan_found, std::memory_order_relaxed);
    g_widescreen.store(mm_widescreen_gameplay_active(rdram) != 0, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
}

#ifdef MM_HAS_GRAPHICS
namespace {

const ImU32 kGold = IM_COL32(255, 214, 64, 255);
const ImU32 kGoldDim = IM_COL32(255, 214, 64, 150);
const ImU32 kShadow = IM_COL32(0, 0, 0, 170);

void draw_label(ImDrawList* dl, ImFont* font, float size, ImVec2 centre, const char* text) {
    const ImVec2 ext = font->CalcTextSizeA(size, 1e9f, 0.0f, text);
    const ImVec2 pos(centre.x - ext.x * 0.5f, centre.y - ext.y * 0.5f);
    dl->AddText(font, size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), kShadow, text);
    dl->AddText(font, size, pos, kGold, text);
}

// A small diamond, the shape of a gem.
void draw_gem(ImDrawList* dl, ImVec2 c, float r, ImU32 fill) {
    const ImVec2 p[4] = {{c.x, c.y - r}, {c.x + r * 0.75f, c.y}, {c.x, c.y + r}, {c.x - r * 0.75f, c.y}};
    dl->AddConvexPolyFilled(p, 4, fill);
    dl->AddPolyline(p, 4, kShadow, ImDrawFlags_Closed, std::max(1.0f, r * 0.15f));
}

} // namespace

void draw_overlay() {
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!g_active.load(std::memory_order_acquire)) return;
    ImFont* font = ImGui::GetFont();
    const float ui = std::max(1.0f, io.DisplaySize.y / 720.0f);
    const float text_size = ImGui::GetFontSize() * ui * 1.15f;

    if (!g_found.load(std::memory_order_relaxed)) {
        // A golden gem that appears later (from an enemy, an item or a reward).
        draw_label(dl, font, text_size,
                   ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f - 100.0f * io.DisplaySize.y / 240.0f + 14.0f * ui),
                   "Golden gem: not in this area yet - pause for the hint");
        return;
    }

    // The game's 320x240 canvas fills the window height, centred. Stages draw
    // into a 288x192 area of it (144 px either side of the camera centre,
    // 100 px above it and 90 below, measured against gem positions); in
    // widescreen the stage also fills the extra width.
    // Split screen: in each view, from that view's camera (split_view.h).
    const int views = mm::split_view::view_count();
    for (int view = 0; view < views; ++view) {
    const mm::split_view::Mapping v = mm::split_view::mapping(view, io.DisplaySize.x, io.DisplaySize.y,
                                                              g_widescreen.load(std::memory_order_relaxed));
    const float scale = v.scale;
    const ImVec2 centre(v.cx, v.cy);
    const float half_w = views > 1 ? v.half_w * scale
                                   : (g_widescreen.load(std::memory_order_relaxed) ? io.DisplaySize.x * 0.5f : 144.0f * scale);
    const float top = 100.0f * scale, bottom = 90.0f * scale;
    const float gx = (static_cast<float>(g_dx.load(std::memory_order_relaxed)) - v.dx) * scale;
    const float gy = -(static_cast<float>(g_dy.load(std::memory_order_relaxed)) - v.dy) * scale;   // +y is up
    if (views > 1) dl->PushClipRect(ImVec2(v.x0, v.y0), ImVec2(v.x1, v.y1), true);
    const int dist = g_dist.load(std::memory_order_relaxed);
    char text[32];
    std::snprintf(text, sizeof text, "%d", dist);

    const float margin = 34.0f * ui;
    const float bob = std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f) * 4.0f * ui;
    if (std::fabs(gx) < half_w && gy > -top && gy < bottom) {
        // On screen: a marker bobbing just above the gem (whose sprite is
        // centred about 5 px above its position).
        const ImVec2 tip(centre.x + gx, centre.y + gy - 14.0f * scale + bob);
        const float s = 9.0f * ui;
        dl->AddTriangleFilled(ImVec2(tip.x - s, tip.y - s * 1.4f), ImVec2(tip.x + s, tip.y - s * 1.4f),
                              tip, kGold);
        dl->AddTriangle(ImVec2(tip.x - s, tip.y - s * 1.4f), ImVec2(tip.x + s, tip.y - s * 1.4f),
                        tip, kShadow, 1.5f * ui);
        if (views > 1) dl->PopClipRect();
        continue;
    }

    // Off screen: an arrow on the screen edge, on the line from the centre.
    const float len = std::sqrt(gx * gx + gy * gy);
    const float ux = gx / len, uy = gy / len;
    const float limit_x = half_w - margin;
    const float limit_y = (uy < 0.0f ? top : bottom) - margin;
    const float t = std::min(ux != 0.0f ? limit_x / std::fabs(ux) : 1e9f,
                             uy != 0.0f ? limit_y / std::fabs(uy) : 1e9f);
    const ImVec2 p(centre.x + ux * t, centre.y + uy * t);
    const float s = 22.0f * ui;
    const float px = -uy, py = ux;                                     // perpendicular
    const ImVec2 tip(p.x + ux * (s + bob * 0.5f), p.y + uy * (s + bob * 0.5f));
    const ImVec2 left(p.x + px * s * 0.7f, p.y + py * s * 0.7f);
    const ImVec2 right(p.x - px * s * 0.7f, p.y - py * s * 0.7f);
    dl->AddTriangleFilled(tip, left, right, kGold);
    dl->AddTriangle(tip, left, right, kShadow, 1.5f * ui);
    const ImVec2 gem_at(p.x - ux * s * 0.9f, p.y - uy * s * 0.9f);
    draw_gem(dl, gem_at, 9.0f * ui, kGoldDim);
    draw_label(dl, font, text_size * 0.85f,
               ImVec2(gem_at.x - ux * s * 1.2f, gem_at.y - uy * s * 1.2f), text);
    if (views > 1) dl->PopClipRect();
    }
}
#endif

} // namespace mm::gem_compass

