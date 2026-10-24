// What the debug menu's Spawn page offers (src/game/debug_menu.cpp).
//
// Shared actor types (0x00-0x7C, table 0x800C7FE0) work in every stage, but
// many expect a spawner to fill in extra fields first, so a bare spawn of
// them crashes or does nothing. Each type was spawned bare in the 1-9 save
// state headless (MM_TEST_SPAWN_TYPE) and in groups under Wine with
// screenshots; the results decide the two lists below. See docs/GAME_MAP.md.
#pragma once

#include <cstdint>

namespace mm::spawn_catalog {

struct Entry {
    uint16_t type;
    const char* name;
    const char* note;    // shown under the list for the selected entry
    uint32_t param = 0;  // stage item parameter (colour / variant); see actors.h
};

// Curated list: types identified on screen, plus custom types (bank 0x7F).
extern const Entry kCatalog[];
extern const int kCatalogCount;

// Every shared type that spawned bare without crashing ("show all").
extern const uint16_t* const kSafeTypes;
extern const int kSafeTypeCount;

// The curated name of a type, or nullptr.
const char* name_of(uint16_t type);

} // namespace mm::spawn_catalog
