// Player 2's look: her own pose (not player 1's) and the "2P" tag over her
// head. See player2.h.
#include "player2.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "player2_internal.h"
#include "librecomp/game.hpp"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" void Trouble_RLE_Type2(uint8_t* rdram, recomp_context* ctx);   // (src, dst, size) unpack

// Her own look. Marina's pose is one of ~800 compressed 64x64 frames that
// the game streams into a single texture: func_800821B0 / func_800822B8
// (main loop) DMA slot 0's frame (+0x84, image class 0x5800) and unpack it
// to 0x8027AEE8 or 0x8027BEE8 (double buffered, index = 0x800BE6A4 +
// 0x800BE700), and the draw builder's class-0x5800 branch (func_80009BE8 at
// 0x8000A474) draws whoever has a Marina image as image 2..5 (the display
// lists in table [0x801376DC] that sample that buffer), with offsets and
// flags read from slot 0, not the actor being drawn. So without help player
// 2 is drawn in player 1's pose. Around her entry in the draw builder
// (0x8000A474 to the loop step at 0x8000DD18) her record stands in for slot
// 0, her own frame is unpacked into her own buffer and images 2..5 point at
// copies of their display lists that sample it. Everything lives in game
// memory at 0x807F1000.. so it is the same on every machine.
namespace {
constexpr uint32_t kDlTable = 0x801376DCu;        // -> display list per class-0 image
constexpr uint32_t kSlotImages = 0x80137480u;     // image per streamed buffer (afterimages)
constexpr uint32_t kStreamIndex = 0x800BE6A4u, kStreamParity = 0x800BE700u;
constexpr uint32_t kGameTexture = 0x8027AEE8u;    // the game's two 0x1000 buffers
constexpr uint32_t kP2Lists = 0x807F1000u;        // 4 display lists, 0x100 each
constexpr uint32_t kP2Texture = 0x807F2000u;      // her two 0x1000 buffers
constexpr uint32_t kP2Packed = 0x807F4000u;       // compressed frame (DMA target)
constexpr uint32_t kP2Header = 0x807F0420u;       // 8-byte frame header
constexpr uint32_t kP2Loaded = 0x807F0410u;       // frame unpacked in each of her buffers
constexpr uint32_t kFrameTable = 0x004A0918u;     // ROM: frame offsets (func_800821B0)
constexpr uint32_t kFrameData = 0x0040AC80u - 0x05000000u;

bool g_draw_swapped = false;
std::array<uint8_t, 0x198> g_draw_p1;
std::array<uint8_t, 8> g_draw_slot_images;
uint32_t g_draw_lists[2];
int g_draw_k = -1;

void unpack_frame(uint8_t* rdram, recomp_context* ctx, uint16_t image, uint32_t dst) {
    const uint32_t idx = static_cast<uint32_t>(image & 0x7FF) % 0x322u;
    const uint32_t header = idx == 0 ? kFrameTable : kFrameTable + idx * 4u - 4u;
    recomp::do_rom_read(rdram, static_cast<gpr>(static_cast<int32_t>(kP2Header)), 0x10000000u + header, 8);
    const uint32_t start = static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kP2Header))));
    const uint32_t end = static_cast<uint32_t>(MEM_W(4, static_cast<gpr>(static_cast<int32_t>(kP2Header))));
    const uint32_t size = end - start;
    if (size == 0 || size > 0x2000u || (start & 1) != 0) return;
    recomp::do_rom_read(rdram, static_cast<gpr>(static_cast<int32_t>(kP2Packed)), 0x10000000u + start + kFrameData,
                        (size + 7u) & ~7u);
    const recomp_context saved = *ctx;
    ctx->r4 = static_cast<gpr>(static_cast<int32_t>(kP2Packed));
    ctx->r5 = static_cast<gpr>(static_cast<int32_t>(dst));
    ctx->r6 = 0x1000;
    ctx->r29 -= 0x40;
    Trouble_RLE_Type2(rdram, ctx);
    *ctx = saved;
}

// A copy of a game display list with its texture images moved to hers.
uint32_t copy_list(uint8_t* rdram, uint32_t src, uint32_t dst) {
    for (uint32_t off = 0; off + 8 <= 0x100u; off += 8) {
        const uint32_t w0 = static_cast<uint32_t>(MEM_W(off, static_cast<gpr>(static_cast<int32_t>(src))));
        uint32_t w1 = static_cast<uint32_t>(MEM_W(off + 4, static_cast<gpr>(static_cast<int32_t>(src))));
        if ((w0 >> 24) == 0xFD && w1 >= kGameTexture && w1 < kGameTexture + 0x2000u) {
            w1 = kP2Texture + (w1 - kGameTexture);
        }
        MEM_W(off, static_cast<gpr>(static_cast<int32_t>(dst))) = static_cast<int32_t>(w0);
        MEM_W(off + 4, static_cast<gpr>(static_cast<int32_t>(dst))) = static_cast<int32_t>(w1);
        if ((w0 >> 24) == 0xB8) break;   // end of list
    }
    return dst;
}
} // namespace

extern "C" void mm_p2_draw_begin(uint8_t* rdram, recomp_context* ctx) {
    using namespace mm::player2;
    if (g_draw_swapped) return;
    const int p2 = slot(rdram);
    if (p2 < 0) return;
    const uint32_t rec = 0x800EF510u + static_cast<uint32_t>(p2) * 0x198u;
    if (static_cast<uint32_t>(ctx->r20) != rec) return;   // $s4: the actor being drawn
    uint8_t* const p1 = rdram + (0x800EF510u - 0x80000000u);
    uint8_t* const me = rdram + (rec - 0x80000000u);
    std::memcpy(g_draw_p1.data(), p1, 0x198);
    std::memcpy(g_draw_slot_images.data(), rdram + (kSlotImages - 0x80000000u), 8);
    std::memcpy(p1, me, 0x198);
    MEM_H(0xD2, static_cast<gpr>(static_cast<int32_t>(0x800EF510u))) = 0x0016;
    g_draw_swapped = true;
    g_draw_k = -1;

    const int k = MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kStreamIndex))) +
                  MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kStreamParity)));
    const uint16_t image = MEM_HU(0x84, static_cast<gpr>(static_cast<int32_t>(rec)));
    if (k < 0 || k > 1 || (image & 0xF800) != 0x5800) return;
    const gpr loaded = static_cast<gpr>(static_cast<int32_t>(kP2Loaded + static_cast<uint32_t>(k) * 2u));
    if (MEM_HU(0, loaded) != image) {
        unpack_frame(rdram, ctx, image, kP2Texture + static_cast<uint32_t>(k) * 0x1000u);
        MEM_H(0, loaded) = static_cast<int16_t>(image);
    }
    const gpr table = static_cast<gpr>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kDlTable))));
    for (int f = 0; f < 2; ++f) {
        const int id = 2 + 2 * k + f;
        const uint32_t game_list = static_cast<uint32_t>(MEM_W(id * 4, table));
        g_draw_lists[f] = game_list;
        MEM_W(id * 4, table) = static_cast<int32_t>(
            copy_list(rdram, game_list, kP2Lists + static_cast<uint32_t>(k * 2 + f) * 0x100u));
    }
    g_draw_k = k;
}

extern "C" void mm_p2_draw_end(uint8_t* rdram, recomp_context*) {
    if (!g_draw_swapped) return;
    g_draw_swapped = false;
    std::memcpy(rdram + (0x800EF510u - 0x80000000u), g_draw_p1.data(), 0x198);
    std::memcpy(rdram + (kSlotImages - 0x80000000u), g_draw_slot_images.data(), 8);
    if (g_draw_k >= 0) {
        const gpr table = static_cast<gpr>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kDlTable))));
        for (int f = 0; f < 2; ++f) MEM_W((2 + 2 * g_draw_k + f) * 4, table) = static_cast<int32_t>(g_draw_lists[f]);
        g_draw_k = -1;
    }
}

#ifdef MM_HAS_GRAPHICS
namespace mm::player2 {
// The "2P" marker and her health, over her head (same canvas mapping as the
// enemy bars: 320x240 fills the window height, camera-relative px, +y up).
void draw_overlay() {
    const Marker m = current_marker();
    if (!m.visible) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float scale = io.DisplaySize.y / 240.0f;
    const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    // The other player off this screen (far apart): an arrow at the edge
    // pointing at her, with her tag and how far she is.
    if (m.partner) {
        const float half_w = centre.x / scale - 12.0f, half_h = 120.0f - 14.0f;
        const float fx = static_cast<float>(m.px), fy = static_cast<float>(m.py) + 16.0f;
        if (std::abs(fx) > half_w + 8.0f || std::abs(fy) > half_h + 8.0f) {
            const float k = std::min(half_w / std::max(std::abs(fx), 1.0f), half_h / std::max(std::abs(fy), 1.0f));
            const ImVec2 tip(centre.x + fx * k * scale, centre.y - fy * k * scale);
            const float len = std::sqrt(fx * fx + fy * fy);
            const ImVec2 dir(fx / len, -fy / len), side(-dir.y, dir.x);
            const float as = std::max(8.0f, 7.0f * scale);
            const ImVec2 back(tip.x - dir.x * as, tip.y - dir.y * as);
            const ImU32 col = m.partner_is_p1 ? IM_COL32(220, 60, 60, 230) : IM_COL32(20, 90, 200, 230);
            dl->AddTriangleFilled(tip, ImVec2(back.x + side.x * as * 0.6f, back.y + side.y * as * 0.6f),
                                  ImVec2(back.x - side.x * as * 0.6f, back.y - side.y * as * 0.6f), col);
            char label[32];
            std::snprintf(label, sizeof label, "%s %dm", m.partner_is_p1 ? "1P" : "2P", static_cast<int>(len / 16.0f));
            ImFont* f = ImGui::GetFont();
            const float size = std::max(10.0f, 5.0f * scale);
            const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
            ImVec2 at(back.x - dir.x * (as * 0.5f + ts.x * 0.6f) - ts.x * 0.5f, back.y - dir.y * (as * 0.5f + ts.y) - ts.y * 0.5f);
            at.x = std::clamp(at.x, 2.0f, io.DisplaySize.x - ts.x - 2.0f);
            at.y = std::clamp(at.y, 2.0f, io.DisplaySize.y - ts.y - 2.0f);
            dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + ts.x + 3, at.y + ts.y + 1), col, 3.0f);
            dl->AddText(f, size, at, IM_COL32(255, 255, 255, 255), label);
        }
    }
    const ImVec2 head(centre.x + m.x * scale, centre.y - (m.top + 6.0f) * scale);
    const char* text = "2P";
    ImFont* font = ImGui::GetFont();
    const float size = std::max(10.0f, 6.0f * scale);
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 p0(head.x - ts.x * 0.5f - size * 0.3f, head.y - ts.y - size * 0.6f);
    const ImVec2 p1(head.x + ts.x * 0.5f + size * 0.3f, head.y - size * 0.2f);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 90, 200, 220), size * 0.3f);
    dl->AddText(font, size, ImVec2(head.x - ts.x * 0.5f, p0.y + size * 0.2f), IM_COL32(255, 255, 255, 255), text);
    // Health under the tag.
    const float w = (p1.x - p0.x) * 1.6f, h = std::max(2.0f, size * 0.22f);
    const ImVec2 b0(head.x - w * 0.5f, p1.y + h * 0.6f), b1(head.x + w * 0.5f, p1.y + h * 1.6f);
    dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 180));
    const float frac = m.max_health > 0 ? std::clamp(static_cast<float>(m.health) / m.max_health, 0.0f, 1.0f) : 0.0f;
    dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * frac, b1.y), IM_COL32(70, 170, 255, 255));
}
} // namespace mm::player2
#endif
