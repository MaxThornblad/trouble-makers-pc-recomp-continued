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

// Stage block kinds that can be picked up. Every block-like item handler
// (table 0x800D37BC, indexed by (kind & 0x3FF0) >> 4) stores a shape number
// in its draw slot (+0x80), taken from a 4-frame table at 0x800D3740 + 8 * n
// (frame counter 0x800D37A4, or 0x800D37A8 for the cylinder kinds), and the
// sprite 0x800D1968[colour] (+0x8C). The actor image for shape s is
// 0x44 + 2 * s: the game's own break pieces (func_800441F4) use 0x46 / 0x4C /
// 0x52 / 0x58 / 0x5E / 0x64 for shapes 1 / 4 / 7 / 10 / 13 / 16. Size comes
// from the terrain cells each kind fills (16 px cells).
struct Shape {
    uint16_t table;              // low half of the 0x800D37xx shape table entry
    int w, h;                    // px
    bool breaks_red;             // colour 1 shatters in the game's handler: leave it
    const char* what;
};

const Shape* shape_of(uint8_t* rdram, uint16_t kind) {
    static const Shape kBlock{0x3740, 32, 32, true, "block"};
    static const Shape kWide{0x3748, 32, 16, true, "wall block (wide)"};
    static const Shape kTall{0x3750, 16, 32, true, "wall block (tall)"};
    static const Shape kSmall{0x3758, 16, 16, true, "small block"};
    static const Shape kCylBlock{0x3740, 32, 32, false, "cylinder block"};
    static const Shape kCylWide{0x3748, 32, 16, false, "cylinder (wide)"};
    static const Shape kCylTall{0x3750, 16, 32, false, "cylinder (tall)"};
    static const Shape kCylSmall{0x3758, 16, 16, false, "cylinder (small)"};
    static const Shape kSlope0{0x3780, 32, 16, false, "slope block"};
    static const Shape kSlope1{0x3788, 32, 16, false, "slope block"};
    static const Shape kSlope2{0x3790, 32, 16, false, "slope block"};
    static const Shape kSlope3{0x3798, 32, 16, false, "slope block"};
    switch ((kind & 0x3FF0u) >> 4) {
        case 0x01: return &kBlock;
        case 0x02: return &kWide;
        case 0x03: return &kTall;
        case 0x04: return &kSmall;
        case 0x11: return &kCylBlock;
        case 0x12: return &kCylWide;
        case 0x13: return &kCylTall;
        case 0x14: return &kCylSmall;
        case 0x2B: case 0x2F: return &kSlope0;
        case 0x2C: case 0x30: return &kSlope1;
        case 0x2D: case 0x31: return &kSlope2;
        case 0x2E: case 0x32: return &kSlope3;
        default: return nullptr;
    }
    (void)rdram;
}

uint32_t terrain_cell(uint8_t* rdram, int x, int y) {
    const uint32_t col = (static_cast<uint32_t>(x) & 0xFFFF) >> 4;
    const uint32_t row = static_cast<uint32_t>(MEM_HU(0, addr(kTerrainHeight)) - y - 1) & 0xFFFF;
    return kTerrainMap + (col & MEM_HU(0, addr(kTerrainColMask))) +
           ((row << MEM_HU(0, addr(kTerrainShift))) & MEM_HU(0, addr(kTerrainRowMask)));
}

// The terrain cells (16 px) a block of w x h centred on its record (x, y)
// fills: 1-9 block at 3440,334 covers x 3424-3455, y 320-351; a 16-wide wall
// block at 1032 covers 1024-1039; a 16-tall one at y 869 covers 864-879.
int block_cells(uint8_t* rdram, int x, int y, int w, int h, uint32_t out[4]) {
    int n = 0;
    for (int dy : {0, 16}) {
        if (dy != 0 && h < 32) break;
        for (int dx : {-8, 8}) {
            const int px = w >= 32 ? x + dx : x;
            out[n++] = terrain_cell(rdram, px, y + dy);
            if (w < 32) break;
        }
    }
    return n;
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
        if ((kind & 0xC000u) != 0) continue;
        const Shape* shape = shape_of(rdram, kind);
        if (shape == nullptr) continue;
        if (shape->breaks_red && (kind & 0xF) == 1) continue;   // red ones shatter: leave them to the game
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
    const Shape* shape = shape_of(rdram, kind);
    const gpr a = slot_addr(slot);
    // Look like the stage block: its shape's image, its colour's sprite.
    const uint16_t first_frame = MEM_HU(0, addr(0x800D0000u | shape->table));
    MEM_H(0x84, a) = static_cast<int16_t>(0x44 + 2 * first_frame);
    MEM_W(0x18C, a) = MEM_W((kind & 0xF) * 4, addr(kBlockSprites));
    // Same size for collision, standing on it and landing.
    MEM_H(0xAA, a) = static_cast<int16_t>(-shape->w / 2);
    MEM_H(0xAC, a) = static_cast<int16_t>(shape->w / 2);
    MEM_H(0xAE, a) = static_cast<int16_t>(shape->h / 2);
    MEM_H(0xB0, a) = static_cast<int16_t>(-shape->h / 2);
    // Take the stage block out for good: no drawing, no collision.
    uint32_t cells[4];
    const int count = block_cells(rdram, bx, by, shape->w, shape->h, cells);
    for (int i = 0; i < count; ++i) MEM_B(0, addr(cells[i])) = 0;
    MEM_H(0, addr(best)) = static_cast<int16_t>(kind | kGone);
    std::fprintf(stderr, "[grab-blocks] %s %03X at %d,%d is now an object (slot %d, image %02X)\n",
                 shape->what, kind & 0x3FFF, bx, by, slot, 0x44 + 2 * first_frame);
}

} // namespace mm::grab_blocks
