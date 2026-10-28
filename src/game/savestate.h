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
#include <memory>
#include <string>
#include <vector>

#include <SDL2/SDL.h>

#include "recomp.h"

namespace mm::savestate {

constexpr int kSlotCount = 10;

// Folder that holds slot0.tmstate .. slot9.tmstate. Called once at startup.
void configure(const std::filesystem::path& folder);

// Acts on the save-state and fast-forward actions bound on the launcher's
// Controls page (keyboard defaults: F5 save, F7 load, F6/F8 next/previous
// slot, hold Tab to fast-forward). Returns true when the event was one of them.
bool handle_event(const SDL_Event& event);

// Stops a held fast-forward (e.g. when a menu takes over input).
void cancel_fast_forward();

// Increments every time a state is loaded, so per-frame watchers (rumble)
// can tell a load's jump in game memory from something that happened in play.
uint32_t load_generation();

// Game thread, inside mm_savestate_frame_hook only (replays, netplay): take a
// state into / restore it from a file synchronously. Return false and set
// `error` on failure.
bool save_now(unsigned char* rdram, recomp_context* ctx, const std::filesystem::path& path, std::string& error);
// RDRAM addresses of the game's OSThreads. Each holds a host pointer
// (OSThread::context, 8 bytes) that differs between processes.
std::vector<uint32_t> game_thread_addresses();

bool load_now(unsigned char* rdram, recomp_context* ctx, const std::filesystem::path& path, std::string& error);

// A state kept in memory, no file (rollback, tests). The buffers inside are
// reused by each capture into the same object. Same rules as save/load_now.
struct MemorySnapshot;
struct MemorySnapshotDeleter { void operator()(MemorySnapshot*) const; };
using MemorySnapshotPtr = std::unique_ptr<MemorySnapshot, MemorySnapshotDeleter>;
MemorySnapshotPtr new_memory_snapshot();
bool capture_memory(unsigned char* rdram, recomp_context* ctx, MemorySnapshot& into, std::string& error);
bool apply_memory(unsigned char* rdram, recomp_context* ctx, const MemorySnapshot& from, std::string& error);

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
