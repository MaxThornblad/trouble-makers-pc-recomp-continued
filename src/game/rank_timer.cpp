#include "rank_timer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "cheats.h"
#include "recomp.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

namespace mm::rank_timer {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 = gameplay
constexpr uint32_t kCurrentStage = 0x80178162u;  // progression row
constexpr uint32_t kStageTimer = 0x801781E0u;    // frames
constexpr uint32_t kParTable = 0x800C8478u;      // u16 per row, frames
constexpr int kCampaignRows = 58;                // rows 58+ are not campaign stages

std::atomic<bool> g_active{false};
std::atomic<int> g_timer{0};
std::atomic<int> g_par{0};

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

struct Rank {
    char letter;
    int limit;   // frames; the rank holds while timer < limit (0 = no limit)
};

// Mirrors func_8001A758.
Rank rank_for(int timer, int par) {
    if (timer < par) return {'S', par};
    if (timer < par + 1800) return {'A', par + 1800};
    if (timer < par + 7200) return {'B', par + 7200};
    const int c_limit = std::min(par + 18000, 36000);
    if (timer < c_limit) return {'C', c_limit};
    return {'D', 0};
}

} // namespace

void publish(uint8_t* rdram) {
    const int row = MEM_HU(0, addr(kCurrentStage));
    const bool gameplay = MEM_HU(0, addr(kGameState)) == 6;
    const int par = (row < kCampaignRows) ? MEM_HU(row * 2, addr(kParTable)) : 0;
    g_timer.store(MEM_HU(0, addr(kStageTimer)), std::memory_order_relaxed);
    g_par.store(par, std::memory_order_relaxed);
    g_active.store(gameplay && par > 0, std::memory_order_release);
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    if (!mm::cheats::rank_timer_enabled() || !g_active.load(std::memory_order_acquire)) {
        return;
    }
    const int timer = g_timer.load(std::memory_order_relaxed);
    const int par = g_par.load(std::memory_order_relaxed);
    const Rank rank = rank_for(timer, par);

    char text[48];
    if (rank.limit > 0) {
        const int left = rank.limit - timer;          // frames, > 0
        const int minutes = left / 3600;
        const int seconds = (left / 60) % 60;
        const int hundredths = (left % 60) * 100 / 60;
        std::snprintf(text, sizeof text, "%c  %d:%02d.%02d", rank.letter, minutes, seconds, hundredths);
    } else {
        std::snprintf(text, sizeof text, "D");
    }

    ImVec4 color;
    switch (rank.letter) {
        case 'S': color = ImVec4(1.00f, 0.85f, 0.25f, 1.0f); break;   // gold
        case 'A': color = ImVec4(0.45f, 0.95f, 0.55f, 1.0f); break;   // green
        case 'B': color = ImVec4(0.45f, 0.75f, 1.00f, 1.0f); break;   // blue
        case 'C': color = ImVec4(0.85f, 0.85f, 0.85f, 1.0f); break;   // grey
        default:  color = ImVec4(1.00f, 0.45f, 0.40f, 1.0f); break;   // red
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##rank_timer", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale * 1.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}
#endif

} // namespace mm::rank_timer

