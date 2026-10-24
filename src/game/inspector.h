// Object inspector and collision view (debug menu "Inspect" page, F2).
//
// The game thread publishes a snapshot every frame: every active actor slot
// (type, state, flags, health, position, hit / attack boxes) plus the terrain
// collision cells and item records around the camera. The renderer draws
// them over the game as outlines:
//   - terrain cells: solid (bits 0xC0) filled red, platform-only (0x40) as a
//     cyan top edge, other non-zero values outlined grey;
//   - actors: hit box green (dim when bit 0x400 is off), attack box red (when
//     bit 0x80 is on), solid actors (0x00040000) filled orange, stand-on
//     platforms (0x2000) with a thick yellow top, Marina white, labels with
//     slot and type, the selected actor in magenta;
//   - item records (gems, blocks): purple.
// Field meanings: docs/GAME_MAP.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm::inspector {

struct ActorInfo {
    int slot;
    uint16_t type;
    uint16_t state;
    uint32_t flags;
    int16_t hp;
    int x, y;                    // camera-relative px, +y up
    int hit[4];                  // left, right, top, bottom (relative)
    int attack[4];
    int vx, vy;                  // px per frame * 16 (Q16.16 >> 12)
    uint32_t signals;            // +0x98
    uint16_t held;               // +0xD6
    uint16_t image;              // +0x84
    uint16_t draw;               // +0x94
    uint16_t stand_on;           // +0xCC
    int distance;                // from Marina, px
};

// Game thread, every frame (mm_debug_menu_tick).
void publish(uint8_t* rdram);

// Item-list hook: remember the stage's item list for the overlay.
void note_item_list(uint32_t list);

// Settings (menu thread).
bool overlay_enabled();
void toggle_overlay();
bool labels_enabled();
void toggle_labels();
void set_selected_slot(int slot);    // -1 = none

// Latest snapshot of active actors, nearest to Marina first.
std::vector<ActorInfo> actors();
// Display name for a type ("" when unknown).
std::string type_name(uint16_t type);

// Renderer: the collision overlay (when enabled).
void draw_overlay();

} // namespace mm::inspector
