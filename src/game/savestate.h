// Save states: snapshot and restore the whole running game at any moment.
//
// Why this is possible in a static recompilation: the game has exactly one
// thread that does work (Thread_MainProc; the idle thread only parks and the
// rmon thread exits at boot). Once per frame Thread_MainProc passes through
// the same instruction with the same host call stack, and at that point its
// complete CPU state is (a) RDRAM and (b) its recomp_context registers. The
// translated hook in troublemakers.us1.toml calls mm_savestate_frame_hook()
// there, and both saving and loading happen on that call, after the RSP/RDP
// have gone idle and every pending host event has been delivered.
//
// Host-side state that the game would otherwise lose is captured alongside:
// the overlay function map (which translated code is mapped at each overlay
// slot), host events deferred because their queue was full, and the
// widescreen bank-extent tracker. EEPROM is deliberately NOT part of a state:
// loading never touches the cartridge save file.
#pragma once

#include <filesystem>

#include <SDL2/SDL.h>

#include "recomp.h"

namespace mm::savestate {

constexpr int kSlotCount = 10;

// Folder that holds slot0.tmstate .. slot9.tmstate. Called once at startup.
void configure(const std::filesystem::path& folder);

// F5 = save to the current slot, F7 = load it, F6 / Shift+F6 = next/previous
// slot. Returns true when the event was a save-state hotkey.
bool handle_event(const SDL_Event& event);

#ifdef MM_HAS_GRAPHICS
// Draws the short "Saved slot 3" style notification (ImGui, post-present).
void draw_overlay();
#endif

} // namespace mm::savestate

// GameEntry::thread_create_callback: records every game OSThread so loads can
// keep each thread's live host context pointer.
extern "C" void mm_savestate_thread_created(unsigned char* rdram, recomp_context* ctx);

// Called by translated Thread_MainProc once per frame (see header comment).
extern "C" void mm_savestate_frame_hook(unsigned char* rdram, recomp_context* ctx);
