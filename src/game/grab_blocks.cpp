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
constexpr uint16_t kGone = 0x8000;               // item "gone" bit (as a broken block)
constexpr int kMaxItems = 512;
constexpr uint32_t kMarinaHold = 0x801370CCu;
constexpr uint16_t kUp = 0x0800, kDown = 0x0400;
constexpr int kReachX = 22, kReachY = 16, kReachTolerance = 26;

// Terrain collision map, read by func_80012AB4(x, y) (camera-relative x/y,
// which it turns into world pixels): one byte per 16x16 cell at 0x8010CDF0,
// index ((x >> 4) & col_mask) + (((height - y - 1) << shift) & row_mask) for
// world pixel (x, y), +y up. Bits 0xC0 = solid. It covers the whole stage
// (1-9: height 2048, 512 columns) and is not streamed. Stage blocks are solid
// because their cells are solid here; the item record only draws the block.
constexpr uint32_t kTerrainMap = 0x8010CDF0u;
constexpr uint32_t kTerrainHeight = 0x800BE648u;
constexpr uint32_t kTerrainColMask = 0x800BE658u;
constexpr uint32_t kTerrainRowMask = 0x800BE65Cu;
constexpr uint32_t kTerrainShift = 0x800BE660u;


gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

bool is_block(uint8_t* rdram, uint16_t kind) {
    if (kind == 0) return false;
    const uint32_t handler = static_cast<uint32_t>(
        MEM_W(((kind & 0x3FF0u) >> 4) * 4, addr(kItemHandlers)));
    return handler == kBlockHandler;
}

uint32_t terrain_cell(uint8_t* rdram, int x, int y) {
    const uint32_t col = (static_cast<uint32_t>(x) & 0xFFFF) >> 4;
    const uint32_t row = static_cast<uint32_t>(MEM_HU(0, addr(kTerrainHeight)) - y - 1) & 0xFFFF;
    return kTerrainMap + (col & MEM_HU(0, addr(kTerrainColMask))) +
           ((row << MEM_HU(0, addr(kTerrainShift))) & MEM_HU(0, addr(kTerrainRowMask)));
}

// The four cells a 32x32 block centred near (x, y) covers. Block records sit
// on cell borders (1-9: x 3440, y 334 covers x 3424-3455, y 320-351).
void block_cells(uint8_t* rdram, int x, int y, uint32_t out[4]) {
    out[0] = terrain_cell(rdram, x - 8, y - 8);
    out[1] = terrain_cell(rdram, x + 8, y - 8);
    out[2] = terrain_cell(rdram, x - 8, y + 8);
    out[3] = terrain_cell(rdram, x + 8, y + 8);
}

// Is a block object already within Marina's reach? Then her grab takes that.
bool object_in_reach(uint8_t* rdram, int fx, int fy) {
    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const uint16_t type = mm::custom_actors::type_id(mm::custom_actors::kBlock);
    for (int s = 1; s < 0x90; ++s) {
        const gpr a = slot_addr(s);
        if (MEM_W(0x80, a) == 0 || MEM_HU(0xD2, a) != type) continue;
        if (std::abs(cx + MEM_H(0x88, a) - fx) <= kReachTolerance &&
            std::abs(cy + MEM_H(0x8C, a) - fy) <= kReachTolerance) {
            return true;
        }
    }
    return false;
}

} // namespace

void on_item_list(uint8_t* rdram, uint32_t list) {
    if (!mm::cheats::grab_blocks_live()) return;
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    if (game_state != 6 && game_state != 10) return;
    if ((MEM_HU(0, addr(kMarinaPress)) & kGrabButton) == 0) return;
    const gpr m = slot_addr(0);
    const uint32_t flags = static_cast<uint32_t>(MEM_W(0x80, m));
    if ((flags & kHoldingFlag) != 0) return;

    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const int mx = cx + MEM_H(0x88, m);
    const int base_y = cy + MEM_H(0x8C, m);
    // Where her hand reaches: in front at chest height, or straight up / down
    // while Up / Down is held (the game's own grab works in all three).
    const uint16_t hold = MEM_HU(0, addr(kMarinaHold));
    int fx = mx + ((flags & kFacingLeftFlag) != 0 ? -kReachX : kReachX);
    int my = base_y + (MEM_H(0xAE, m) + MEM_H(0xB0, m)) / 2;
    if ((hold & kUp) != 0) {
        fx = mx;
        my = base_y + MEM_H(0xAE, m) + kReachY;
    } else if ((hold & kDown) != 0) {
        fx = mx;
        my = base_y + MEM_H(0xB0, m) - kReachY;
    }

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
    if (best == 0 || object_in_reach(rdram, fx, my)) return;

    const uint16_t kind = MEM_HU(0, addr(best));
    const int bx = MEM_HU(2, addr(best)), by = MEM_HU(4, addr(best));
    const int marina_y = base_y;
    const int slot = mm::actors::spawn_now(rdram, mm::custom_actors::type_id(mm::custom_actors::kBlock),
                                           bx - mx, by - marina_y);
    if (slot < 0) return;
    MEM_W(0x18C, slot_addr(slot)) = MEM_W((kind & 0xF) * 4, addr(kBlockSprites));  // keep its colour
    // Take the stage block out for good: no drawing, no collision.
    uint32_t cells[4];
    block_cells(rdram, bx, by, cells);
    for (uint32_t cell : cells) MEM_B(0, addr(cell)) = 0;
    MEM_H(0, addr(best)) = static_cast<int16_t>(kind | kGone);
    std::fprintf(stderr, "[grab-blocks] block at %d,%d is now an object (slot %d)\n", bx, by, slot);
}

} // namespace mm::grab_blocks
