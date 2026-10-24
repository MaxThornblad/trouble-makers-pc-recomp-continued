#include "inspector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "custom_actors.h"
#include "recomp.h"
#include "spawn_catalog.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" int mm_widescreen_gameplay_active(uint8_t* rdram);

namespace mm::inspector {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 and 10 = stage gameplay
constexpr uint32_t kCameraX = 0x800BE558u;       // Q16.16 camera centre
constexpr uint32_t kCameraY = 0x800BE55Cu;
constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorStride = 0x198u;
constexpr int kSlots = 208;

// Terrain collision map (see grab_blocks.cpp / docs/GAME_MAP.md).
constexpr uint32_t kTerrainMap = 0x8010CDF0u;
constexpr uint32_t kTerrainHeight = 0x800BE648u;
constexpr uint32_t kTerrainColMask = 0x800BE658u;
constexpr uint32_t kTerrainRowMask = 0x800BE65Cu;
constexpr uint32_t kTerrainShift = 0x800BE660u;

// Item records and their handlers.
constexpr uint32_t kItemHandlers = 0x800D37BCu;
constexpr uint32_t kBlockHandler = 0x80044390u;
constexpr uint32_t kGemHandler = 0x800446F0u;
constexpr int kMaxItems = 512;

// Area scanned around the camera centre (px): wide enough for widescreen.
constexpr int kScanHalfW = 272, kScanUp = 112, kScanDown = 104;

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

struct Cell {
    int x, y;                    // world px of the cell's left / bottom edge
    uint8_t value;
};

struct Item {
    int x, y;                    // world px (record position)
    int half;                    // half size of the box drawn
    uint16_t kind;
};

struct Snapshot {
    bool valid = false;
    bool widescreen = false;
    int cam_x = 0, cam_y = 0;
    std::vector<ActorInfo> actors;
    std::vector<Cell> cells;
    std::vector<Item> items;
};

std::mutex g_mutex;
Snapshot g_shared;
std::atomic<bool> g_overlay{false};
std::atomic<bool> g_labels{true};
std::atomic<int> g_selected{-1};
std::atomic<uint32_t> g_item_list{0};

uint32_t terrain_cell(uint8_t* rdram, int x, int y) {
    const uint32_t col = (static_cast<uint32_t>(x) & 0xFFFF) >> 4;
    const uint32_t row = static_cast<uint32_t>(MEM_HU(0, addr(kTerrainHeight)) - y - 1) & 0xFFFF;
    return kTerrainMap + (col & MEM_HU(0, addr(kTerrainColMask))) +
           ((row << MEM_HU(0, addr(kTerrainShift))) & MEM_HU(0, addr(kTerrainRowMask)));
}

ActorInfo read_actor(uint8_t* rdram, int slot, int mx, int my) {
    const gpr a = slot_addr(slot);
    ActorInfo i{};
    i.slot = slot;
    i.type = MEM_HU(0xD2, a);
    i.state = MEM_HU(0xD0, a);
    i.flags = static_cast<uint32_t>(MEM_W(0x80, a));
    i.hp = MEM_H(0xE0, a);
    i.x = MEM_H(0x88, a);
    i.y = MEM_H(0x8C, a);
    i.hit[0] = MEM_H(0xAA, a);
    i.hit[1] = MEM_H(0xAC, a);
    i.hit[2] = MEM_H(0xAE, a);
    i.hit[3] = MEM_H(0xB0, a);
    i.attack[0] = MEM_H(0xA2, a);
    i.attack[1] = MEM_H(0xA4, a);
    i.attack[2] = MEM_H(0xA6, a);
    i.attack[3] = MEM_H(0xA8, a);
    i.vx = MEM_W(0xEC, a) >> 12;
    i.vy = MEM_W(0xF0, a) >> 12;
    i.signals = static_cast<uint32_t>(MEM_W(0x98, a));
    i.held = MEM_HU(0xD6, a);
    i.image = MEM_HU(0x84, a);
    i.draw = MEM_HU(0x94, a);
    i.stand_on = MEM_HU(0xCC, a);
    i.distance = static_cast<int>(std::hypot(i.x - mx, i.y - my));
    return i;
}

} // namespace

void publish(uint8_t* rdram) {
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    Snapshot s;
    s.valid = game_state == 6 || game_state == 10;
    if (s.valid) {
        s.cam_x = MEM_H(0, addr(kCameraX));
        s.cam_y = MEM_H(0, addr(kCameraY));
        s.widescreen = mm_widescreen_gameplay_active(rdram) != 0;
        const int mx = MEM_H(0x88, slot_addr(0)), my = MEM_H(0x8C, slot_addr(0));
        for (int slot = 0; slot < kSlots; ++slot) {
            if (MEM_W(0x80, slot_addr(slot)) != 0) s.actors.push_back(read_actor(rdram, slot, mx, my));
        }
        std::sort(s.actors.begin(), s.actors.end(),
                  [](const ActorInfo& a, const ActorInfo& b) { return a.distance < b.distance; });

        if (g_overlay.load(std::memory_order_relaxed)) {
            // Terrain cells in view. Rows are counted down from the map height.
            const int height = MEM_HU(0, addr(kTerrainHeight));
            const int left = (s.cam_x - kScanHalfW) & ~15;
            const int row_top = (height - 1 - (s.cam_y + kScanUp)) >> 4;
            const int row_bottom = (height - 1 - (s.cam_y - kScanDown)) >> 4;
            for (int row = std::max(row_top, 0); row <= row_bottom; ++row) {
                const int cell_top = height - 1 - row * 16;          // highest y in the cell
                for (int x = left; x <= s.cam_x + kScanHalfW; x += 16) {
                    if (x < 0) continue;
                    const uint8_t v = MEM_BU(0, addr(terrain_cell(rdram, x, cell_top)));
                    if (v != 0) s.cells.push_back({x, cell_top - 15, v});
                }
            }
            // Item records in view.
            const uint32_t list = g_item_list.load(std::memory_order_relaxed);
            if (list >= 0x80000000u && list < 0x80800000u) {
                uint32_t l = list;
                for (int i = 0; i < kMaxItems; ++i, l += 6) {
                    const uint16_t kind = MEM_HU(0, addr(l));
                    if (kind == 0) break;
                    if ((kind & 0xC000u) != 0) continue;
                    const int x = MEM_HU(2, addr(l)), y = MEM_HU(4, addr(l));
                    if (std::abs(x - s.cam_x) > kScanHalfW || std::abs(y - s.cam_y) > kScanUp) continue;
                    const uint32_t handler = static_cast<uint32_t>(
                        MEM_W(((kind & 0x3FF0u) >> 4) * 4, addr(kItemHandlers)));
                    s.items.push_back({x, y, handler == kBlockHandler ? 16 : (handler == kGemHandler ? 6 : 8), kind});
                }
            }
        }
    }
    std::lock_guard lock{g_mutex};
    g_shared = std::move(s);
}

void note_item_list(uint32_t list) {
    g_item_list.store(list, std::memory_order_relaxed);
}

bool overlay_enabled() { return g_overlay.load(std::memory_order_relaxed); }
void toggle_overlay() {
    g_overlay.store(!g_overlay.load());
    std::fprintf(stderr, "[inspector] collision overlay %s\n", g_overlay.load() ? "on" : "off");
}
bool labels_enabled() { return g_labels.load(std::memory_order_relaxed); }
void toggle_labels() { g_labels.store(!g_labels.load()); }
void set_selected_slot(int slot) { g_selected.store(slot, std::memory_order_relaxed); }

std::vector<ActorInfo> actors() {
    std::lock_guard lock{g_mutex};
    return g_shared.valid ? g_shared.actors : std::vector<ActorInfo>{};
}

std::string type_name(uint16_t type) {
    if (const auto* t = mm::custom_actors::info(type)) return t->name;
    if (const char* n = mm::spawn_catalog::name_of(type)) return n;
    switch (type) {
        case 0x0009: return "Particle";
        case 0x000D: return "(scenery)";
        case 0x0016: return "Marina";
        case 0x0034: return "(shadow / effect)";
        case 0x2604: return "(stage helper)";
        case 0x2605: return "Brown carry block";
        default: return "";
    }
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    if (!g_overlay.load(std::memory_order_relaxed)) return;
    Snapshot s;
    {
        std::lock_guard lock{g_mutex};
        if (!g_shared.valid) return;
        s = g_shared;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Same canvas mapping as the enemy bars: 320x240 fills the window height,
    // camera-relative px from the window centre, +y up.
    const float scale = io.DisplaySize.y / 240.0f;
    const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    const float half_w = s.widescreen ? io.DisplaySize.x * 0.5f : 144.0f * scale;
    dl->PushClipRect(ImVec2(centre.x - half_w, centre.y - 100.0f * scale),
                     ImVec2(centre.x + half_w, centre.y + 90.0f * scale), true);
    // Camera-relative box (left, right, top, bottom in px, +y up) to screen.
    const auto rect = [&](float l, float r, float t, float b, ImVec2& p0, ImVec2& p1) {
        p0 = ImVec2(centre.x + l * scale, centre.y - t * scale);
        p1 = ImVec2(centre.x + r * scale, centre.y - b * scale);
    };
    const float thin = std::max(1.0f, 0.35f * scale);

    for (const Cell& c : s.cells) {
        ImVec2 p0, p1;
        rect(static_cast<float>(c.x - s.cam_x), static_cast<float>(c.x + 16 - s.cam_x),
             static_cast<float>(c.y + 16 - s.cam_y), static_cast<float>(c.y - s.cam_y), p0, p1);
        if ((c.value & 0xC0) == 0xC0) {
            dl->AddRectFilled(p0, p1, IM_COL32(255, 60, 60, 60));
            dl->AddRect(p0, p1, IM_COL32(255, 80, 80, 150), 0.0f, 0, thin);
        } else if ((c.value & 0xC0) == 0x40) {
            dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(80, 230, 255, 230), 2.0f * thin);
        } else {
            dl->AddRect(p0, p1, IM_COL32(200, 200, 200, 110), 0.0f, 0, thin);
        }
        if ((c.value & 0x3F) != 0 && scale >= 3.0f) {
            char t[4];
            std::snprintf(t, sizeof t, "%02X", c.value);
            dl->AddText(ImVec2(p0.x + 2, p0.y + 1), IM_COL32(255, 255, 255, 160), t);
        }
    }

    for (const Item& it : s.items) {
        ImVec2 p0, p1;
        const float x = static_cast<float>(it.x - s.cam_x), y = static_cast<float>(it.y - s.cam_y);
        rect(x - it.half, x + it.half, y + it.half, y - it.half, p0, p1);
        dl->AddRect(p0, p1, IM_COL32(200, 120, 255, 200), 0.0f, 0, thin);
    }

    const int selected = g_selected.load(std::memory_order_relaxed);
    const bool labels = g_labels.load(std::memory_order_relaxed);
    for (const ActorInfo& a : s.actors) {
        ImVec2 p0, p1;
        const float x = static_cast<float>(a.x), y = static_cast<float>(a.y);
        rect(x + a.hit[0], x + a.hit[1], y + a.hit[2], y + a.hit[3], p0, p1);
        const bool marina = a.slot == 0;
        if ((a.flags & 0x00040000u) != 0) dl->AddRectFilled(p0, p1, IM_COL32(255, 150, 40, 70));
        const ImU32 hit_col = marina ? IM_COL32(255, 255, 255, 230)
                                     : ((a.flags & 0x400u) != 0 ? IM_COL32(80, 255, 120, 220)
                                                                : IM_COL32(80, 255, 120, 90));
        dl->AddRect(p0, p1, hit_col, 0.0f, 0, thin);
        if ((a.flags & 0x2000u) != 0) {
            dl->AddLine(p0, ImVec2(p1.x, p0.y), IM_COL32(255, 230, 60, 255), 3.0f * thin);
        }
        if ((a.flags & 0x80u) != 0) {
            ImVec2 q0, q1;
            rect(x + a.attack[0], x + a.attack[1], y + a.attack[2], y + a.attack[3], q0, q1);
            dl->AddRect(q0, q1, IM_COL32(255, 60, 60, 230), 0.0f, 0, thin);
        }
        // Origin cross.
        const ImVec2 o(centre.x + x * scale, centre.y - y * scale);
        dl->AddLine(ImVec2(o.x - 3 * thin, o.y), ImVec2(o.x + 3 * thin, o.y), hit_col, thin);
        dl->AddLine(ImVec2(o.x, o.y - 3 * thin), ImVec2(o.x, o.y + 3 * thin), hit_col, thin);
        if (a.slot == selected) {
            dl->AddRect(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3),
                        IM_COL32(255, 60, 255, 255), 0.0f, 0, 2.5f * thin);
        }
        if (labels || a.slot == selected) {
            char t[24];
            std::snprintf(t, sizeof t, "%d:%04X", a.slot, a.type);
            const ImVec2 tp(p0.x, p0.y - ImGui::GetFontSize() - 1);
            dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), t);
            dl->AddText(tp, a.slot == selected ? IM_COL32(255, 120, 255, 255) : IM_COL32(230, 255, 230, 220), t);
        }
    }
    dl->PopClipRect();
}
#endif

} // namespace mm::inspector
