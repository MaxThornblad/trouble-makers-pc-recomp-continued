#include "spawn_catalog.h"

#include <array>

namespace mm::spawn_catalog {
namespace {

// Bare-spawn results (1-9 save state):
//  - crash headless: 0x11-0x14 (abort), 0x35 and 0x74 (segfault);
//  - page fault under Wine in a group spawn: one of 0x0B 0x0C 0x0E 0x0F 0x10
//    0x15 and one of 0x61 0x62 0x64 0x6D 0x6E 0x70 0x72 0x75 (0x0B's display
//    list pointer is garbage when spawned bare), so all of those are left out;
//  - 0x09 is the particle type (needs an image) and 0x16 is Marina herself.
// Everything else spawned without trouble; many types do nothing visible
// without their spawner's parameters.
constexpr uint16_t kExcluded[] = {
    0x00, 0x09, 0x0B, 0x0C, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x35, 0x61, 0x62, 0x64, 0x6D, 0x6E, 0x70, 0x72, 0x74, 0x75,
};

constexpr bool excluded(uint16_t type) {
    for (uint16_t t : kExcluded) {
        if (t == type) return true;
    }
    return false;
}

constexpr int count_safe() {
    int n = 0;
    for (uint16_t t = 0; t <= 0x7C; ++t) n += excluded(t) ? 0 : 1;
    return n;
}

constexpr auto make_safe() {
    std::array<uint16_t, count_safe()> out{};
    int n = 0;
    for (uint16_t t = 0; t <= 0x7C; ++t) {
        if (!excluded(t)) out[n++] = t;
    }
    return out;
}

constexpr auto kSafe = make_safe();

} // namespace

const Entry kCatalog[] = {
    {0x18, "Pot", "The pot from Clanpot Shake. Grab and throw it."},
    {0x08, "Red gem", "A loose gem; touch it to collect it."},
    {0x1C, "Clanball", "An orange clanball."},
    {0x48, "Clancer", "A small clancer."},
    {0x49, "Bomb clanball", "A purple bomb ball."},
    {0x54, "Blue clancer", "A blue clancer that walks about."},
    {0x7F01, "Loose block", "A block Marina can grab, carry and throw (custom type)."},
    {0x7F00, "Bobbing block", "A block that floats in place: the custom actor table's test type."},
};
const int kCatalogCount = static_cast<int>(sizeof kCatalog / sizeof kCatalog[0]);

const uint16_t* const kSafeTypes = kSafe.data();
const int kSafeTypeCount = static_cast<int>(kSafe.size());

const char* name_of(uint16_t type) {
    for (const Entry& e : kCatalog) {
        if (e.type == type) return e.name;
    }
    return nullptr;
}

} // namespace mm::spawn_catalog
