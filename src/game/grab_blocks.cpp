#include "grab_blocks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "actors.h"
#include "cheats.h"
#include "custom_actors.h"
#include "player2.h"
#include "recomp.h"
#include "terrain.h"

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

// Stage blocks are solid because their cells in the terrain collision map
// are (terrain.h); the item record only draws the block.

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

gpr slot_addr(int slot) {
    return addr(kActors + static_cast<uint32_t>(slot) * kActorStride);
}

// Stage block kinds that can be picked up. Every block-like item handler
// (table 0x800D37BC, indexed by (kind & 0x3FF0) >> 4) stores a shape number
// in its draw slot (+0x80), taken from a 4-frame table at 0x800D3740 + 8 * n
// (frame counter 0x800D37A4, or 0x800D37A8 for the background kinds), and the
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
    static const Shape kBig{0x3760, 32, 32, true, "big cylinder block"};
    static const Shape kSlope0{0x3780, 32, 16, false, "slope block"};
    static const Shape kSlope1{0x3788, 32, 16, false, "slope block"};
    static const Shape kSlope2{0x3790, 32, 16, false, "slope block"};
    static const Shape kSlope3{0x3798, 32, 16, false, "slope block"};
    switch ((kind & 0x3FF0u) >> 4) {
        case 0x01: return &kBlock;
        case 0x02: return &kWide;
        case 0x03: return &kTall;
        case 0x04: return &kSmall;
        case 0x06: return &kBig;               // 1-3: stacks of these, 32 px apart, 32 px wide in the terrain
        case 0x2B: case 0x2F: return &kSlope0;
        case 0x2C: case 0x30: return &kSlope1;
        case 0x2D: case 0x31: return &kSlope2;
        case 0x2E: case 0x32: return &kSlope3;
        // 0x11-0x16 draw the same shapes darker with no terrain cells: background
        // bricks behind the playfield (3-7, 1-3), not blocks. Left alone.
        default: return nullptr;
    }
    (void)rdram;
}

bool is_slope(const Shape* shape) {
    return shape->table >= 0x3780;
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
            out[n++] = mm::terrain::cell(rdram, px, y + dy);
            if (w < 32) break;
        }
    }
    return n;
}

// Is a block object right at Marina's hand? Then her grab takes that. (Only
// close to its box: a block object she stands on is near her hand too, but
// her grab can't reach it, and must not stop her taking the stage block.)
bool object_in_reach(uint8_t* rdram, int fx, int fy) {
    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const uint16_t type = mm::custom_actors::type_id(mm::custom_actors::kBlock);
    constexpr int kSlack = 6;
    for (int s = 1; s < 0x90; ++s) {
        const gpr a = slot_addr(s);
        if (MEM_W(0x80, a) == 0 || MEM_HU(0xD2, a) != type) continue;
        const int ox = cx + MEM_H(0x88, a), oy = cy + MEM_H(0x8C, a);
        if (fx >= ox + MEM_H(0xAA, a) - kSlack && fx <= ox + MEM_H(0xAC, a) + kSlack &&
            fy >= oy + MEM_H(0xB0, a) - kSlack && fy <= oy + MEM_H(0xAE, a) + kSlack) {
            return true;
        }
    }
    return false;
}

} // namespace

// One player's grab at the stage blocks: `record` is her actor record, `hold`
// and `press` her input words (Marina's at 0x801370CC/CE; player 2's own copy).
static void try_grab(uint8_t* rdram, uint32_t list, uint32_t record, uint16_t hold, uint16_t press);

void on_item_list(uint8_t* rdram, uint32_t list) {
    if (!mm::cheats::grab_blocks_live()) return;
    const uint16_t game_state = MEM_HU(0, addr(kGameState));
    if (game_state != 6 && game_state != 10) return;
    static const bool dbg = std::getenv("MM_GB_DEBUG") != nullptr;
    static int dbg_n = 0;
    if (dbg && (++dbg_n % 120) == 1) {
        const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
        std::fprintf(stderr, "[gb-debug] p1 %d,%d blocks:", cx + MEM_H(0x88, slot_addr(0)), cy + MEM_H(0x8C, slot_addr(0)));
        uint32_t l = list;
        int shown = 0;
        for (int i = 0; i < kMaxItems && shown < 40; ++i, l += 6) {
            const uint16_t kind = MEM_HU(0, addr(l));
            if (kind == 0) break;
            if ((kind & 0xC000u) != 0 || shape_of(rdram, kind) == nullptr) continue;
            const int bx = MEM_HU(2, addr(l)), by = MEM_HU(4, addr(l));
            static const bool red_only = std::getenv("MM_GB_DEBUG_RED") != nullptr;
            if (red_only ? (kind & 0xF) != 1 : std::abs(bx - cx) > 400) continue;
            std::fprintf(stderr, " %03X@%d,%d", kind, bx, by);
            ++shown;
        }
        std::fprintf(stderr, "\n");
    }
    try_grab(rdram, list, kActors, MEM_HU(0, addr(kMarinaHold)), MEM_HU(0, addr(kMarinaPress)));
    uint32_t record = 0;
    uint16_t hold = 0, press = 0;
    if (mm::player2::controls(rdram, record, hold, press)) try_grab(rdram, list, record, hold, press);
}

static void try_grab(uint8_t* rdram, uint32_t list, uint32_t record, uint16_t hold, uint16_t press) {
    if ((press & kGrabButton) == 0) return;
    const gpr m = addr(record);
    const uint32_t flags = static_cast<uint32_t>(MEM_W(0x80, m));
    if ((flags & kHoldingFlag) != 0) return;

    const int cx = MEM_H(0, addr(kCameraX)), cy = MEM_H(0, addr(kCameraY));
    const int mx = cx + MEM_H(0x88, m);
    const int base_y = cy + MEM_H(0x8C, m);
    // Where her hand reaches: in front at chest height, or straight up / down
    // while Up / Down is held (the game's own grab works in all three).
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
    (void)marina_y;
    // spawn_now places relative to slot 0 (player 1), whoever grabbed.
    const int slot = mm::actors::spawn_now(rdram, mm::custom_actors::type_id(mm::custom_actors::kBlock),
                                           bx - (cx + MEM_H(0x88, slot_addr(0))), by - (cy + MEM_H(0x8C, slot_addr(0))));
    if (slot < 0) return;
    const Shape* shape = shape_of(rdram, kind);
    const gpr a = slot_addr(slot);
    // Look like the stage block: its shape's image (the table's lowest frame,
    // as the game's break pieces use), its colour's sprite.
    uint16_t first_frame = 0xFFFF;
    for (int f = 0; f < 4; ++f) {
        first_frame = std::min<uint16_t>(first_frame, MEM_HU(2 * f, addr(0x800D0000u | shape->table)));
    }
    MEM_H(0x84, a) = static_cast<int16_t>(0x44 + 2 * first_frame);
    MEM_W(0x18C, a) = MEM_W((kind & 0xF) * 4, addr(kBlockSprites));
    // Same size for collision, standing on it and landing.
    MEM_H(0xAA, a) = static_cast<int16_t>(-shape->w / 2);
    MEM_H(0xAC, a) = static_cast<int16_t>(shape->w / 2);
    MEM_H(0xAE, a) = static_cast<int16_t>(shape->h / 2);
    MEM_H(0xB0, a) = static_cast<int16_t>(-shape->h / 2);
    // Take the stage block out for good: no drawing, no collision. A slope
    // keeps its two cells' slope codes so it can put them back where it lands.
    uint32_t cells[4];
    const int count = block_cells(rdram, bx, by, shape->w, shape->h, cells);
    if (is_slope(shape) && count == 2) {
        mm::custom_actors::make_slope(rdram, slot, MEM_BU(0, addr(cells[0])), MEM_BU(0, addr(cells[1])));
    }
    for (int i = 0; i < count; ++i) MEM_B(0, addr(cells[i])) = 0;
    MEM_H(0, addr(best)) = static_cast<int16_t>(kind | kGone);
    std::fprintf(stderr, "[grab-blocks] %s %03X at %d,%d is now an object (slot %d, image %02X)\n",
                 shape->what, kind & 0x3FFF, bx, by, slot, 0x44 + 2 * first_frame);
}

} // namespace mm::grab_blocks
