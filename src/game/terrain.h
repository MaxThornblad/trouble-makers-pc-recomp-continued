// Terrain collision map access (see docs/GAME_MAP.md, Collision).
//
// One byte per 16x16 cell at 0x8010CDF0 covering the whole stage, read by the
// game's func_80012AB4(x, y). Index for world pixel (x, y), +y up:
//   ((x >> 4) & [0x800BE658]) + (((H - y - 1) << [0x800BE660]) & [0x800BE65C])
// with H = [0x800BE648]. Bits 0xC0 = solid; 0x40 with low bits = slope /
// stand-on cells (0x41 / 0x45 / 0x49 / 0x4D seen on slope blocks).
#pragma once

#include <cstdint>

#include "recomp.h"

namespace mm::terrain {

constexpr uint32_t kMap = 0x8010CDF0u;
constexpr uint32_t kHeight = 0x800BE648u;
constexpr uint32_t kColMask = 0x800BE658u;
constexpr uint32_t kRowMask = 0x800BE65Cu;
constexpr uint32_t kShift = 0x800BE660u;

inline gpr vaddr(uint32_t a) {
    return static_cast<gpr>(static_cast<int32_t>(a));
}

// Address of the cell holding world pixel (x, y).
inline uint32_t cell(uint8_t* rdram, int x, int y) {
    const uint32_t col = (static_cast<uint32_t>(x) & 0xFFFF) >> 4;
    const uint32_t row = static_cast<uint32_t>(MEM_HU(0, vaddr(kHeight)) - y - 1) & 0xFFFF;
    return kMap + (col & MEM_HU(0, vaddr(kColMask))) +
           ((row << MEM_HU(0, vaddr(kShift))) & MEM_HU(0, vaddr(kRowMask)));
}

inline uint8_t read(uint8_t* rdram, int x, int y) {
    return MEM_BU(0, vaddr(cell(rdram, x, y)));
}

inline void write(uint8_t* rdram, int x, int y, uint8_t value) {
    MEM_B(0, vaddr(cell(rdram, x, y))) = static_cast<int8_t>(value);
}

// Lowest world y of the cell row holding y (cells run down from H - 1).
inline int cell_bottom(uint8_t* rdram, int y) {
    const int h = MEM_HU(0, vaddr(kHeight));
    const int row = (h - 1 - y) >> 4;
    return h - 16 - row * 16;
}

} // namespace mm::terrain
