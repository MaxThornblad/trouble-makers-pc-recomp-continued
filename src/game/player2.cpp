#include "player2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>
#include <cfloat>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "netplay.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" void func_8004ED10(uint8_t* rdram, recomp_context* ctx);   // Marina's update(slot)
extern "C" void Trouble_RLE_Type2(uint8_t* rdram, recomp_context* ctx);   // (src, dst, size) unpack

#include "librecomp/game.hpp"

namespace mm::player2 {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint16_t kMarinaType = 0x0016;
constexpr uint32_t kMarinaPaused = 0x800CA230u;   // func_80020024 skips Marina's update while nonzero

// Controller state the game builds in Input_Update (0x80000FE0) and Marina
// copies into her own input words at the start of her update.
constexpr uint32_t kPads = 0x8012AD70u;           // OSContPad[4], 6 bytes each
constexpr uint32_t kHold = 0x800BE4F8u;
constexpr uint32_t kPress = 0x800BE4FCu;

// Marina's own globals: everything her update writes outside her slot that
// is about her (the probe also found sound-effect channels at 0x8016xxxx,
// which are deliberately shared).
struct Range { uint32_t start, size; };
constexpr std::array<Range, 9> kGlobals{{
    {0x800BE5A4u, 4},
    {0x800BE5D8u, 0x18},     // world position pairs read by enemies (0x800BE5D8, 0x800BE5E8)
    {0x800D57E0u, 4},
    {0x800EF4F0u, 0x20},
    {0x80104090u, 4},
    {0x801069D8u, 4},
    {0x801370CCu, 8},        // her hold / press input words and two more
    {0x801373D0u, 0x98},     // player-state block (0x801373D8..0x80137460 seen written)
    {kHold, 8},              // controller words: P2's own while her update runs
}};

constexpr size_t globals_size() {
    size_t n = 0;
    for (const Range& r : kGlobals) n += r.size;
    return n;
}

// Player 2's own copy of those globals and her previous buttons live in
// host-only expansion memory (unused above 0x80480000, docs/README.md 14.1)
// rather than in host variables, so save states, replays and netplay carry
// them with the rest of the game.
constexpr uint32_t kP2Globals = 0x807F0000u;
constexpr uint32_t kP2PrevButtons = kP2Globals + 0x400u;
constexpr uint32_t kP2JoinTimer = kP2Globals + 0x404u;     // frames P1 has been in play without P2
constexpr uint32_t kP2AwayTimer = kP2Globals + 0x408u;     // frames P2 has been too far from P1

constexpr uint32_t kGameState = 0x800BE4F0u;   // 6 and 10 = stage gameplay
constexpr uint32_t kCameraX = 0x800BE558u, kCameraY = 0x800BE55Cu;
constexpr uint32_t kWorldX = 0x800BE5D8u, kWorldY = 0x800BE5DCu;   // Marina's world position
constexpr int kJoinDelay = 45;                 // frames into a scene
constexpr int kLeashX = 250, kLeashY = 170;    // px from player 1
constexpr int kLeashFrames = 90;

struct State {
    int slot = -1;        // host-side cache only; slot() checks the slot itself
    bool joined = false;  // a (re)join since the last marker update
};
State g;

struct Marker {
    bool visible = false;
    int x = 0, top = 0, health = 0, max_health = 0;
};
std::mutex g_marker_mutex;
Marker g_marker;

uint8_t* at(uint8_t* rdram, uint32_t vaddr) {
    return rdram + (vaddr - 0x80000000u);
}

uint8_t* slot_ptr(uint8_t* rdram, int slot) {
    return at(rdram, kActors + static_cast<uint32_t>(slot) * kStride);
}

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

void copy_globals_out(uint8_t* rdram, uint8_t* dst) {
    for (const Range& r : kGlobals) {
        std::memcpy(dst, at(rdram, r.start), r.size);
        dst += r.size;
    }
}

void copy_globals_in(uint8_t* rdram, const uint8_t* src) {
    for (const Range& r : kGlobals) {
        std::memcpy(at(rdram, r.start), src, r.size);
        src += r.size;
    }
}

// Controller 2's buttons: the second connected pad after player 1's.
uint16_t p2_buttons(uint8_t* rdram) {
    const int p1 = MEM_W(0, addr(0x801370C0u));
    for (int c = 0; c < 4; ++c) {
        if (c == p1) continue;
        const uint32_t pad = kPads + static_cast<uint32_t>(c) * 6u;
        if (MEM_BU(4, addr(pad)) == 0) return MEM_HU(0, addr(pad));   // err_no 0: connected
    }
    return 0;
}

} // namespace

int slot(uint8_t* rdram) {
    // The cached slot, else a scan: a loaded state (or a netplay peer's)
    // brings its player 2 along without this process having spawned her.
    auto is_p2 = [rdram](int s) {
        const gpr a = addr(kActors + static_cast<uint32_t>(s) * kStride);
        return MEM_W(0x80, a) != 0 && MEM_HU(0xD2, a) == mm::custom_actors::type_id(kCustomIndex);
    };
    if (g.slot > 0 && is_p2(g.slot)) return g.slot;
    for (int s = 1; s < 0xD0; ++s) {
        if (is_p2(s)) {
            g.slot = s;
            return s;
        }
    }
    g.slot = -1;
    return -1;
}

void setup(uint8_t* rdram, int slot) {
    // Only one player 2 (her globals live in one place): a new one replaces the old.
    if (const int old = mm::player2::slot(rdram); old >= 0 && old != slot) {
        MEM_W(0x80, addr(kActors + static_cast<uint32_t>(old) * kStride)) = 0;
    }
    // A copy of Marina as she is now, a little behind her.
    uint8_t* p = slot_ptr(rdram, slot);
    int32_t x;
    std::memcpy(p, slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    std::memcpy(&x, p + 0x88, 4);
    x += ((MEM_W(0x80, addr(kActors)) & 0x20) != 0 ? 24 : -24) << 16;
    std::memcpy(p + 0x88, &x, 4);
    MEM_W(0x80, addr(kActors + static_cast<uint32_t>(slot) * kStride)) &= ~0x02000000;   // not holding anything
    MEM_H(0xD6, addr(kActors + static_cast<uint32_t>(slot) * kStride)) = 0;
    copy_globals_out(rdram, at(rdram, kP2Globals));
    MEM_H(0, addr(kP2PrevButtons)) = 0;
    g.slot = slot;
    g.joined = true;
    std::fprintf(stderr, "[player2] joined in slot %d\n", slot);
}

// Put player 2 back next to player 1: a fresh copy of her (as she joins),
// used after a defeat and when she has been left far behind.
void rejoin(uint8_t* rdram, int slot) {
    setup(rdram, slot);
}

void update(uint8_t* rdram, recomp_context* ctx, int slot) {
    g.slot = slot;
    if (MEM_HU(0, addr(kMarinaPaused)) != 0) return;   // the game is not running Marina either
    // Defeated: she comes straight back by player 1 instead of running
    // Marina's own defeat (which would end the game for both).
    if (MEM_H(0xE0, addr(kActors + static_cast<uint32_t>(slot) * kStride)) <= 0) {
        std::fprintf(stderr, "[player2] defeated, rejoining\n");
        rejoin(rdram, slot);
    }

    // Player 1 aside.
    std::array<uint8_t, kStride> p1_record;
    std::array<uint8_t, globals_size()> p1_globals;
    std::memcpy(p1_record.data(), slot_ptr(rdram, 0), kStride);
    copy_globals_out(rdram, p1_globals.data());

    // Player 2 in.
    std::memcpy(slot_ptr(rdram, 0), slot_ptr(rdram, slot), kStride);
    MEM_H(0xD2, addr(kActors)) = static_cast<int16_t>(kMarinaType);
    copy_globals_in(rdram, at(rdram, kP2Globals));
    const uint16_t buttons = p2_buttons(rdram);
    MEM_H(0, addr(kHold)) = static_cast<int16_t>(buttons);
    MEM_H(0, addr(kPress)) = static_cast<int16_t>(buttons & ~MEM_HU(0, addr(kP2PrevButtons)));
    MEM_H(0, addr(kP2PrevButtons)) = static_cast<int16_t>(buttons);

    // Marina's update, as func_80020024 calls it.
    const gpr saved_a0 = ctx->r4;
    ctx->r4 = 0;
    func_8004ED10(rdram, ctx);
    ctx->r4 = saved_a0;

    // Player 2 out, player 1 back.
    std::memcpy(slot_ptr(rdram, slot), slot_ptr(rdram, 0), kStride);
    MEM_H(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride)) =
        static_cast<int16_t>(mm::custom_actors::type_id(kCustomIndex));
    // Her colour: a cool blue added over Marina's own (the draw builder's
    // prim-colour tint at +0x9C..+0x9F, func_80009BE8). Marina's update
    // clears those bytes each frame and uses them for her hit flash, so the
    // tint goes on only while she is not flashing.
    {
        const gpr pa = addr(kActors + static_cast<uint32_t>(slot) * kStride);
        if (MEM_BU(0x9C, pa) == 0 && MEM_BU(0x9D, pa) == 0 && MEM_BU(0x9E, pa) == 0) {
            MEM_B(0x9C, pa) = 0x00;
            MEM_B(0x9D, pa) = 0x30;
            MEM_B(0x9E, pa) = 0x5A;
            MEM_B(0x9F, pa) = static_cast<int8_t>(0xFF);
        }
    }
    copy_globals_out(rdram, at(rdram, kP2Globals));
    std::memcpy(slot_ptr(rdram, 0), p1_record.data(), kStride);
    copy_globals_in(rdram, p1_globals.data());
}

void publish_marker(uint8_t* rdram, int p2) {
    Marker m;
    if (p2 >= 0) {
        const gpr a = addr(kActors + static_cast<uint32_t>(p2) * kStride);
        m.visible = true;
        m.x = MEM_H(0x88, a);
        m.top = MEM_H(0x8C, a) + MEM_H(0xAE, a);
        m.health = MEM_H(0xE0, a);
        // Full health is what she joined with (a copy of player 1's); grows if she heals past it.
        static int full = 0;
        if (m.health > full || g.joined) full = m.health;
        g.joined = false;
        m.max_health = full;
    }
    std::lock_guard lock{g_marker_mutex};
    g_marker = m;
}

void tick(uint8_t* rdram) {
    const int p2 = slot(rdram);
    publish_marker(rdram, p2);
    if (!mm::cheats::coop_enabled()) {
        if (p2 >= 0) {
            MEM_W(0x80, addr(kActors + static_cast<uint32_t>(p2) * kStride)) = 0;
            std::fprintf(stderr, "[player2] left (co-op off)\n");
        }
        return;
    }
    const uint16_t state = MEM_HU(0, addr(kGameState));
    const bool p1_playing = (state == 6 || state == 10) && MEM_W(0x80, addr(kActors)) != 0 &&
                            MEM_HU(0xD2, addr(kActors)) == kMarinaType && MEM_HU(0, addr(kMarinaPaused)) == 0;
    if (!p1_playing) {
        MEM_W(0, addr(kP2JoinTimer)) = 0;
        return;
    }
    if (p2 < 0) {
        const int32_t t = MEM_W(0, addr(kP2JoinTimer)) + 1;
        MEM_W(0, addr(kP2JoinTimer)) = t;
        if (t >= kJoinDelay) {
            MEM_W(0, addr(kP2JoinTimer)) = 0;
            mm::actors::spawn_now(rdram, mm::custom_actors::type_id(kCustomIndex), 0, 0);
        }
        return;
    }
    // Leash: too far from player 1 for a while (left behind, stuck, fallen).
    const gpr a = addr(kActors + static_cast<uint32_t>(p2) * kStride);
    const int dx = MEM_H(0x88, a) - MEM_H(0x88, addr(kActors));
    const int dy = MEM_H(0x8C, a) - MEM_H(0x8C, addr(kActors));
    const bool away = dx > kLeashX || dx < -kLeashX || dy > kLeashY || dy < -kLeashY;
    int32_t away_frames = away ? MEM_W(0, addr(kP2AwayTimer)) + 1 : 0;
    if (away_frames >= kLeashFrames) {
        std::fprintf(stderr, "[player2] too far from player 1, rejoining\n");
        rejoin(rdram, p2);
        away_frames = 0;
    }
    MEM_W(0, addr(kP2AwayTimer)) = away_frames;
}

#ifdef MM_HAS_GRAPHICS
// The "2P" marker and her health, over her head (same canvas mapping as the
// enemy bars: 320x240 fills the window height, camera-relative px, +y up).
void draw_overlay() {
    Marker m;
    {
        std::lock_guard lock{g_marker_mutex};
        m = g_marker;
    }
    if (!m.visible) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float scale = io.DisplaySize.y / 240.0f;
    const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    float vx, vy;
    view_offset(vx, vy);
    const ImVec2 head(centre.x + (m.x - vx) * scale, centre.y - (m.top - vy + 6.0f) * scale);
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
#endif

} // namespace mm::player2

// Shared camera. Translated func_80012830 (camera update) calls these at its
// entry and before its return (troublemakers.us1.toml). While co-op is on,
// Marina's position reads as the midpoint between the two players for the
// camera's sake, so it frames both; her real position comes back after,
// carrying any shift the camera code applied meanwhile.
namespace {
bool g_camera_swapped = false;
int32_t g_saved_x, g_saved_y, g_saved_wx, g_saved_wy, g_mid_x, g_mid_y;
}

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

// Each player's own view (netplay). The game has one camera, shared by both
// players and by the simulation (actor positions are camera-relative and the
// camera decides what is active), so it stays as it is: the players'
// midpoint. Only the picture moves. LookAt_Update builds the view matrix
// (eye (0, 0, 448) looking at the origin) into 0x804600C0 and 0x804700C0;
// afterwards its translation is moved by half the distance from the other
// player to this machine's player, so the view centres on this player the
// way the camera centres on the midpoint (same dead zone and easing). The
// players stay within 144 px of the midpoint across (Marina's screen clamp),
// well inside what the game draws in widescreen. Only those two matrices
// differ between the machines (the netplay memory check leaves them out);
// the game's own computation, stack leftovers included, is untouched.
namespace {
std::atomic<float> g_view_x{0.0f}, g_view_y{0.0f};   // render thread reads
float g_smooth_x = 0.0f, g_smooth_y = 0.0f;

// Adds `delta` (px) to element [3][col] (translation) of an N64 Mtx.
void shift_translation(uint8_t* rdram, uint32_t mtx, int col, float delta) {
    const gpr m = static_cast<gpr>(static_cast<int32_t>(mtx));
    const int idx = 3 * 4 + col;
    const int32_t whole = MEM_H(idx * 2, m);
    const uint32_t frac = MEM_HU(32 + idx * 2, m);
    int32_t v = static_cast<int32_t>((static_cast<uint32_t>(whole) << 16) | frac);
    v += static_cast<int32_t>(std::lround(delta * 65536.0f));
    MEM_H(idx * 2, m) = static_cast<int16_t>(v >> 16);
    MEM_H(32 + idx * 2, m) = static_cast<int16_t>(v & 0xFFFF);
}
} // namespace

namespace mm::player2 {
void view_offset(float& x, float& y) {
    x = g_view_x.load(std::memory_order_relaxed);
    y = g_view_y.load(std::memory_order_relaxed);
}
} // namespace mm::player2

extern "C" void mm_view_begin(uint8_t*) {}

extern "C" void mm_view_end(uint8_t* rdram) {
    using namespace mm::player2;
    const int own = mm::netplay::active() ? mm::netplay::player() : 0;
    const int p2 = own != 0 ? slot(rdram) : -1;
    float tx = 0.0f, ty = 0.0f;
    if (p2 > 0) {
        const gpr a1 = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
        const gpr a2 = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
        const float x1 = MEM_W(0x88, a1) / 65536.0f, y1 = MEM_W(0x8C, a1) / 65536.0f;
        const float x2 = MEM_W(0x88, a2) / 65536.0f, y2 = MEM_W(0x8C, a2) / 65536.0f;
        tx = std::clamp((own == 1 ? x1 - x2 : x2 - x1) * 0.5f, -160.0f, 160.0f);
        ty = std::clamp((own == 1 ? y1 - y2 : y2 - y1) * 0.5f, -100.0f, 100.0f);
    }
    if (const char* t = std::getenv("MM_VIEW_TEST")) {   // test: a fixed offset "x,y"
        tx = static_cast<float>(std::atof(t));
        if (const char* c = std::strchr(t, ',')) ty = static_cast<float>(std::atof(c + 1));
    }
    // Eased, so a rejoin (a jump of up to 250 px) glides instead of cutting.
    g_smooth_x += (tx - g_smooth_x) * 0.25f;
    g_smooth_y += (ty - g_smooth_y) * 0.25f;
    if (tx == 0.0f && ty == 0.0f && std::abs(g_smooth_x) < 0.05f && std::abs(g_smooth_y) < 0.05f) {
        g_smooth_x = g_smooth_y = 0.0f;
    }
    g_view_x.store(g_smooth_x, std::memory_order_relaxed);
    g_view_y.store(g_smooth_y, std::memory_order_relaxed);
    if (g_smooth_x == 0.0f && g_smooth_y == 0.0f) return;
    for (uint32_t mtx : {0x804600C0u, 0x804700C0u}) {
        shift_translation(rdram, mtx, 0, -g_smooth_x);
        shift_translation(rdram, mtx, 1, -g_smooth_y);
    }
}

extern "C" void mm_camera_begin(uint8_t* rdram) {
    using namespace mm::player2;
    g_camera_swapped = false;
    if (!mm::cheats::coop_enabled()) return;
    const int p2 = slot(rdram);
    if (p2 < 0) return;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    const gpr b = static_cast<gpr>(static_cast<int32_t>(0x800EF510u + static_cast<uint32_t>(p2) * 0x198u));
    g_saved_x = MEM_W(0x88, m);
    g_saved_y = MEM_W(0x8C, m);
    g_saved_wx = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u)));
    g_saved_wy = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu)));
    g_mid_x = static_cast<int32_t>((static_cast<int64_t>(g_saved_x) + MEM_W(0x88, b)) / 2);
    g_mid_y = static_cast<int32_t>((static_cast<int64_t>(g_saved_y) + MEM_W(0x8C, b)) / 2);
    MEM_W(0x88, m) = g_mid_x;
    MEM_W(0x8C, m) = g_mid_y;
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u))) = g_saved_wx + (g_mid_x - g_saved_x);
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu))) = g_saved_wy + (g_mid_y - g_saved_y);
    g_camera_swapped = true;
}

extern "C" void mm_camera_end(uint8_t* rdram) {
    if (!g_camera_swapped) return;
    g_camera_swapped = false;
    const gpr m = static_cast<gpr>(static_cast<int32_t>(0x800EF510u));
    MEM_W(0x88, m) = g_saved_x + (MEM_W(0x88, m) - g_mid_x);
    MEM_W(0x8C, m) = g_saved_y + (MEM_W(0x8C, m) - g_mid_y);
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5D8u))) = g_saved_wx;
    MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x800BE5DCu))) = g_saved_wy;
}
