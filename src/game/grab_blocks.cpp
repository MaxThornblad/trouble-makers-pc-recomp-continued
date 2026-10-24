#include "grab_blocks.h"

#include <cstdio>
#include <cstdlib>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "recomp.h"

namespace mm::grab_blocks {
namespace {

constexpr uint32_t kGameState = 0x800BE4F0u;     // 6 and 10 = stage gameplay
constexpr uint32_t kCameraX = 0x800BE558u;       // Q16.16 camera centre
constexpr uint32_t kCameraY = 0x800BE55Cu;
constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorStride = 0x198u;
constexpr uint32_t kMarinaPress = 0x801370CEu;
constexpr uint16_t kGrabButton = 0x4000;         // N64 B
constexpr uint32_t kHoldingFlag = 0x02000000u;   // Marina +0x80 while holding
constexpr uint32_t kFacingLeftFlag = 0x20u;      // Marina +0x80 when facing left
constexpr uint32_t kItemHandlers = 0x800D37BCu;
constexpr uint32_t kBlockHandler = 0x80044390u;  // item handler type 1: blocks
constexpr uint32_t kBlockSprites = 0x800D1968u;  // sprite per colour
constexpr uint16_t kGone = 0x4000;               // item "gone" bit (game uses 0x4000/0x8000)
constexpr int kMaxItems = 512;
constexpr int kGrid = 32;
constexpr int kReachX = 22, kReachTolerance = 26;

// Loose block fields (unused by the generic actor code for this type).
constexpr uint32_t kFieldRecord = 0x110;         // address of the block's item record
constexpr uint32_t kFieldKind = 0x114;           // the record's kind before pickup
constexpr uint32_t kFieldOrigin = 0x118;         // original x << 16 | y (world px)

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

uint32_t g_list = 0;                             // last item list seen

bool is_block(uint8_t* rdram, uint16_t kind) {
    if (kind == 0) return false;
    const uint32_t handler = static_cast<uint32_t>(
        MEM_W(((kind & 0xFFF0u) >> 4) * 4, addr(kItemHandlers)));
    return handler == kBlockHandler;
}

// Is a solid block record (other than `self`) occupying the cell at (x, y)?
bool cell_taken(uint8_t* rdram, int x, int y, uint32_t self) {
    if (g_list == 0) return false;
    uint32_t l = g_list;
    for (int i = 0; i < kMaxItems; ++i, l += 6) {
        const uint16_t kind = MEM_HU(0, addr(l));
        if (kind == 0) break;
        if (l == self || (kind & 0xC000u) != 0 || !is_block(rdram, kind)) continue;
        if (std::abs(MEM_HU(2, addr(l)) - x) < kGrid && std::abs(MEM_HU(4, addr(l)) - y) < kGrid) {
            return true;
        }
    }
    return false;
}

// Would a block at world (x, y) overlap Marina's hit box?
bool overlaps_marina(uint8_t* rdram, int x, int y) {
    const gpr m = slot_addr(0);
    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const int mx = cx + MEM_H(0x88, m), my = cy + MEM_H(0x8C, m);
    const int left = mx + MEM_H(0xAA, m), right = mx + MEM_H(0xAC, m);
    const int top = my + MEM_H(0xAE, m), bottom = my + MEM_H(0xB0, m);
    return x + 16 > left && x - 16 < right && y + 16 > bottom && y - 16 < top;
}

} // namespace

void on_item_list(uint8_t* rdram, uint32_t list) {
    g_list = list;
    if (!mm::cheats::grab_blocks_live()) return;
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    if (game_state != 6 && game_state != 10) return;
    if ((MEM_HU(0, addr(kMarinaPress)) & kGrabButton) == 0) return;
    const gpr m = slot_addr(0);
    const uint32_t flags = static_cast<uint32_t>(MEM_W(0x80, m));
    if ((flags & kHoldingFlag) != 0) return;

    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const int mx = cx + MEM_H(0x88, m);
    const int my = cy + MEM_H(0x8C, m) + (MEM_H(0xAE, m) + MEM_H(0xB0, m)) / 2;
    const int fx = mx + ((flags & kFacingLeftFlag) != 0 ? -kReachX : kReachX);

    uint32_t best = 0;
    int best_d = 1 << 30;
    uint32_t l = list;
    for (int i = 0; i < kMaxItems; ++i, l += 6) {
        const uint16_t kind = MEM_HU(0, addr(l));
        if (kind == 0) break;
        if ((kind & 0xC000u) != 0 || !is_block(rdram, kind)) continue;
        if ((kind & 0xF) == 1) continue;                // colour 1 breaks: leave it to the game
        const int dx = MEM_HU(2, addr(l)) - fx, dy = MEM_HU(4, addr(l)) - my;
        if (std::abs(dx) > kReachTolerance || std::abs(dy) > kReachTolerance) continue;
        const int d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = l;
        }
    }
    if (best == 0) return;

    const uint16_t kind = MEM_HU(0, addr(best));
    const int bx = MEM_HU(2, addr(best)), by = MEM_HU(4, addr(best));
    const int marina_y = cy + MEM_H(0x8C, m);
    const int slot = mm::actors::spawn_now(rdram, mm::custom_actors::type_id(mm::custom_actors::kLooseBlock),
                                           bx - mx, by - marina_y);
    if (slot < 0) return;
    const gpr a = slot_addr(slot);
    MEM_W(0x18C, a) = MEM_W((kind & 0xF) * 4, addr(kBlockSprites));
    MEM_W(kFieldRecord, a) = static_cast<int32_t>(best);
    MEM_W(kFieldKind, a) = kind;
    MEM_W(kFieldOrigin, a) = static_cast<int32_t>((static_cast<uint32_t>(bx) << 16) | static_cast<uint32_t>(by));
    MEM_H(0, addr(best)) = static_cast<int16_t>(kind | kGone);
    std::fprintf(stderr, "[grab-blocks] block at %d,%d is now loose (slot %d)\n", bx, by, slot);
}

bool settle(uint8_t* rdram, int slot, bool home) {
    const gpr a = slot_addr(slot);
    const uint32_t record = static_cast<uint32_t>(MEM_W(kFieldRecord, a));
    if (record < 0x80000000u || record >= 0x80800000u) return false;   // spawned, no record
    const uint16_t kind = static_cast<uint16_t>(MEM_W(kFieldKind, a));
    const uint32_t origin = static_cast<uint32_t>(MEM_W(kFieldOrigin, a));
    const int ox = static_cast<int>(origin >> 16), oy = static_cast<int>(origin & 0xFFFF);

    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const int wx = cx + MEM_H(0x88, a), wy = cy + MEM_H(0x8C, a);
    // Snap to the grid the block came from (its original offset within 32).
    const auto snap = [](int v, int o) {
        const int rel = v - o;
        const int cells = (rel >= 0 ? rel + kGrid / 2 : rel - kGrid / 2) / kGrid;
        return o + cells * kGrid;
    };
    int gx = snap(wx, ox), gy = snap(wy, oy);
    bool placed = false;
    if (home) {
        gx = ox;
        gy = oy;
        placed = true;
    }
    for (int tries = 0; tries < 3 && !placed; ++tries, gy += kGrid) {
        if (cell_taken(rdram, gx, gy, record)) continue;
        if (overlaps_marina(rdram, gx, gy)) return false;  // wait until she steps away
        placed = true;
    }
    if (!placed) {
        gx = ox;                                    // nowhere sensible: back where it was
        gy = oy;
    }
    MEM_H(2, addr(record)) = static_cast<int16_t>(gx);
    MEM_H(4, addr(record)) = static_cast<int16_t>(gy);
    MEM_H(0, addr(record)) = static_cast<int16_t>(kind & ~0xC000u);
    std::fprintf(stderr, "[grab-blocks] block at rest at %d,%d settled at %d,%d\n", wx, wy, gx, gy);
    return true;
}

} // namespace mm::grab_blocks
