#include "throw_cheats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "cheats.h"
#include "far_coop.h"
#include "player2.h"
#include "split_view.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" int mm_widescreen_gameplay_active(uint8_t* rdram);

namespace mm::throw_cheats {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kGamePaused = 0x800BE4E8u;
constexpr uint32_t kHoldingFlag = 0x02000000u;   // +0x80: carrying (slot at +0xD6)
// Marina's input this frame (Input_Update), which her update copies in,
// and the newest entries of her input history (func_800122B0).
constexpr uint32_t kHold = 0x800BE4F8u, kPress = 0x800BE4FCu;
constexpr uint32_t kHistoryPress = 0x8011DD70u, kHistoryHold = 0x801225F0u;
constexpr uint32_t kGrabButton = 0x800BE514u;    // the action button's mask (B by default)
// Player 2's block (player2.cpp layout): per character, frames the throw
// button has been held while charging, and the strength of the throw it
// releases (percent; 0 = none pending).
constexpr uint32_t kCharge = 0x807F0620u;
constexpr uint32_t kPending = 0x807F0628u;
constexpr uint32_t kAim = 0x807F0630u;           // the direction last held while charging
constexpr uint16_t kDirections = 0x0F00u;        // d-pad bits (the stick maps onto them)

constexpr int kFullCharge = 240;                 // frames to the top of the bar (4 s)
constexpr int kMaxFactor = 1500;                 // percent at full charge: absurd
constexpr int32_t kMaxSpeed = 0x7F0000;          // px/frame cap (16.16): keeps the numbers sane

gpr addr(uint32_t vaddr) { return static_cast<gpr>(static_cast<int32_t>(vaddr)); }
gpr slot_addr(int slot) { return addr(kActors + static_cast<uint32_t>(slot) * kStride); }

bool gameplay(uint8_t* rdram) {
    const uint16_t s = MEM_HU(0, addr(kGameState));
    return s == 6 || s == 10;
}

bool charging_on() {
    return mm::cheats::g_active_for_hooks() && mm::cheats::enabled(mm::cheats::Cheat::ChargedThrow);
}

// 100% for a tap, rising with the square of the charge to kMaxFactor.
int factor_for(int frames) {
    const float f = std::min(1.0f, static_cast<float>(frames) / kFullCharge);
    return 100 + static_cast<int>((kMaxFactor - 100) * f * f);
}

void strip(uint8_t* rdram, uint16_t button) {
    MEM_H(0, addr(kHold)) = static_cast<int16_t>(MEM_HU(0, addr(kHold)) & ~button);
    MEM_H(0, addr(kPress)) = static_cast<int16_t>(MEM_HU(0, addr(kPress)) & ~button);
    MEM_H(0, addr(kHistoryHold)) = static_cast<int16_t>(MEM_HU(0, addr(kHistoryHold)) & ~button);
    MEM_H(0, addr(kHistoryPress)) = static_cast<int16_t>(MEM_HU(0, addr(kHistoryPress)) & ~button);
}

void give(uint8_t* rdram, uint16_t button) {
    MEM_H(0, addr(kHold)) = static_cast<int16_t>(MEM_HU(0, addr(kHold)) | button);
    MEM_H(0, addr(kPress)) = static_cast<int16_t>(MEM_HU(0, addr(kPress)) | button);
    MEM_H(0, addr(kHistoryHold)) = static_cast<int16_t>(MEM_HU(0, addr(kHistoryHold)) | button);
    MEM_H(0, addr(kHistoryPress)) = static_cast<int16_t>(MEM_HU(0, addr(kHistoryPress)) | button);
}

// The character whose throw this is: the one standing in slot 0 now.
int g_character = 0;

// Bars for the overlay (render thread reads).
struct Bar {
    float x, y;        // camera-relative px, +y up, above the player
    float fraction;
};
std::mutex g_mutex;
std::array<Bar, 2> g_bars{};
int g_bar_count = 0;
bool g_widescreen = false;

} // namespace

void before_marina_update(uint8_t* rdram, int character) {
    g_character = character;
    const gpr charge = addr(kCharge + 4u * static_cast<uint32_t>(character));
    if (!charging_on() || !gameplay(rdram) || MEM_HU(0, addr(kGamePaused)) != 0) {
        MEM_W(0, charge) = 0;
        return;
    }
    const uint16_t button = MEM_HU(0, addr(kGrabButton));
    const bool carrying = (static_cast<uint32_t>(MEM_W(0x80, slot_addr(0))) & kHoldingFlag) != 0;
    const int held = MEM_W(0, charge);
    if (!carrying) {
        MEM_W(0, charge) = 0;
        MEM_W(0, addr(kPending + 4u * static_cast<uint32_t>(character))) = 0;   // (a release that threw nothing)
        return;
    }
    const bool down = (MEM_HU(0, addr(kHold)) & button) != 0;
    const gpr aim = addr(kAim + 4u * static_cast<uint32_t>(character));
    if (down && (held > 0 || (MEM_HU(0, addr(kPress)) & button) != 0)) {
        // Charging: the press (and the hold) stay away from her update.
        MEM_W(0, charge) = std::min(held + 1, kFullCharge * 4);
        if (held == 0) MEM_W(0, aim) = 0;
        if (const uint16_t dir = MEM_HU(0, addr(kHold)) & kDirections) MEM_W(0, aim) = dir;
        strip(rdram, button);
        return;
    }
    if (!down && held > 0) {
        // Let go: the throw, now, with the charge, the way she aimed while
        // charging if the direction was let go of with the button.
        if ((MEM_HU(0, addr(kHold)) & kDirections) == 0 && MEM_W(0, aim) != 0) {
            MEM_H(0, addr(kHold)) = static_cast<int16_t>(MEM_HU(0, addr(kHold)) | MEM_W(0, aim));
            MEM_H(0, addr(kHistoryHold)) = static_cast<int16_t>(MEM_HU(0, addr(kHistoryHold)) | MEM_W(0, aim));
        }
        give(rdram, button);
        MEM_W(0, addr(kPending + 4u * static_cast<uint32_t>(character))) = factor_for(held);
        MEM_W(0, charge) = 0;
    }
}

void publish(uint8_t* rdram) {
    std::array<Bar, 2> bars{};
    int n = 0;
    if (charging_on() && gameplay(rdram)) {
        const int p2 = mm::player2::slot(rdram);
        for (int character = 0; character < 2; ++character) {
            const int frames = MEM_W(0, addr(kCharge + 4u * static_cast<uint32_t>(character)));
            const int slot = character == 0 ? 0 : p2;
            if (frames <= 0 || slot < 0) continue;
            const gpr a = slot_addr(slot);
            bars[n].x = static_cast<float>(MEM_H(0x88, a));
            bars[n].y = static_cast<float>(MEM_H(0x8C, a) + MEM_H(0xAE, a) + 14);
            bars[n].fraction = std::min(1.0f, static_cast<float>(frames) / kFullCharge);
            ++n;
        }
    }
    std::lock_guard lock{g_mutex};
    g_bars = bars;
    g_bar_count = n;
    g_widescreen = mm_widescreen_gameplay_active(rdram) != 0;
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    std::array<Bar, 2> bars;
    int n;
    {
        std::lock_guard lock{g_mutex};
        bars = g_bars;
        n = g_bar_count;
    }
    if (n == 0) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // The canvas mapping the enemy bars use: 320x240 fills the window
    // height; split screen, each view in its half (split_view.h).
    const int views = mm::split_view::view_count();
    for (int view = 0; view < views; ++view) {
    const mm::split_view::Mapping v = mm::split_view::mapping(view, io.DisplaySize.x, io.DisplaySize.y, true);
    const float scale = v.scale;
    const ImVec2 centre(v.cx, v.cy);
    if (views > 1) dl->PushClipRect(ImVec2(v.x0, v.y0), ImVec2(v.x1, v.y1), true);
    const float w = 34.0f * scale, h = std::max(4.0f, 3.5f * scale);
    for (int i = 0; i < n; ++i) {
        const Bar& b = bars[i];
        const float cx = centre.x + (b.x - v.dx) * scale, cy = centre.y - (b.y - v.dy) * scale;
        const ImVec2 p0(cx - w * 0.5f, cy - h * 0.5f), p1(cx + w * 0.5f, cy + h * 0.5f);
        dl->AddRectFilled(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 210));
        dl->AddRectFilled(p0, p1, IM_COL32(40, 30, 60, 220));
        // Yellow to orange to a pulsing white-hot red at the top.
        const float f = b.fraction;
        int r = 255, g = static_cast<int>(230 - 170 * f), bl = 40;
        if (f >= 1.0f) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 18.0f);
            g = static_cast<int>(60 + 195 * pulse);
            bl = static_cast<int>(60 + 195 * pulse);
            r = 255;
        }
        dl->AddRectFilled(p0, ImVec2(p0.x + w * f, p1.y), IM_COL32(r, g, bl, 255));
    }
    if (views > 1) dl->PopClipRect();
    }
}
#else
void draw_overlay() {}
#endif

} // namespace mm::throw_cheats

// func_800554AC's return (troublemakers.us1.toml): the thrown object's
// velocity is in its +0xF8/+0xFC. a0/a1 (thrower, object) are on the stack.
extern "C" void mm_throw_velocity_hook(uint8_t* rdram, recomp_context* ctx) {
    using namespace mm::throw_cheats;
    const bool log = !mm::faraway::in_shadow();   // (it runs in player 2's picture pass too: memory only)
    const int object = MEM_W(4, ctx->r29) & 0xFFFF;
    if (object == 0xFFFF || object >= 208) return;
    const int p2 = mm::player2::slot(rdram);
    const bool player = object == 0 || (p2 > 0 && object == p2);
    int percent = mm::cheats::throw_strength(player ? mm::cheats::ThrowKind::Players : mm::cheats::ThrowKind::Objects);
    const gpr pending = addr(kPending + 4u * static_cast<uint32_t>(g_character));
    if (MEM_W(0, pending) > 0) {
        percent = static_cast<int>(static_cast<int64_t>(percent) * MEM_W(0, pending) / 100);
        MEM_W(0, pending) = 0;
    }
    if (percent == 100) return;
    const gpr a = slot_addr(object);
    const int32_t vx = MEM_W(0xF8, a), vy = MEM_W(0xFC, a);
    for (uint32_t off : {0xF8u, 0xFCu}) {
        const int64_t v = static_cast<int64_t>(MEM_W(off, a)) * percent / 100;
        MEM_W(off, a) = static_cast<int32_t>(std::clamp<int64_t>(v, -kMaxSpeed, kMaxSpeed));
    }
    if (log) std::fprintf(stderr, "[cheats] throw at %d%%: slot %d (%s), velocity %.1f,%.1f -> %.1f,%.1f px/frame\n", percent, object,
                 player ? "a player" : "an object", vx / 65536.0, vy / 65536.0, MEM_W(0xF8, a) / 65536.0, MEM_W(0xFC, a) / 65536.0);
}
