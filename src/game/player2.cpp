#include "player2.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" void func_8004ED10(uint8_t* rdram, recomp_context* ctx);   // Marina's update(slot)

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
