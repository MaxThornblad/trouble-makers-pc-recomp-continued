// Netplay (docs/NETPLAY.md, phase 2): two machines play co-op over UDP.
//
// Both run the whole game. Each frame only controller input crosses the
// network: the host plays Marina (controller 1 in the game), the guest plays
// player 2 (controller 2). A frame runs only once both players' input for it
// is known (lockstep), and each side sends its input a few frames ahead
// (the input delay), so moderate latency costs nothing visible.
//
// A session starts from the host's game: the host takes a save state, sends
// it with its cartridge save and its game-changing settings (cheats.h,
// SessionSettings, plus the widescreen mode), and both sides load it at the
// same frame point. From there they run identically (phase 0). Once a second
// both compare a fingerprint of game memory; if they ever differ (or the host
// loads a save state), the host sends a fresh state and both carry on from
// it (a resync), so a divergence heals in about a second.
//
// Transport: ENet (reliable, ordered UDP), one connection, default port 7845;
// or Steam (steam_net.h): lobbies and Steam's relays, no address or port.
// Everything except the request functions runs on the game thread, from the
// frame hook.
//
// Environment (headless tests and scripting):
//   MM_NET_HOST=<port>[@<frame>]     host from that frame on (default port 7845; "steam": on Steam)
//   MM_NET_JOIN=<address>[:<port>][@<frame>]   ("steam[:<lobby>]": on Steam)
//   MM_NET_DELAY=<frames>            input delay (host decides; default 4)
//   MM_NET_PAD="20:,30:r"            synthetic local input (replay.h syntax)
//   MM_NET_LENGTH=<frames>           leave after that many session frames
//   MM_NET_LOG=<file>                append per-second fingerprints (tests)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "recomp.h"

namespace mm::netplay {

constexpr int kDefaultPort = 7845;

// <config>/netplay: temporary state files and netplay.cfg (join address).
void configure(const std::filesystem::path& folder);

// Any thread. Acted on at the next frame point.
void request_host(int port = kDefaultPort);
void request_join(const std::string& address);   // "host[:port]", or "steam[:<lobby id>]"
// Over Steam (steam_net.h): host a lobby; join one (0: a friend hosting).
void request_host_steam();
void request_join_steam(uint64_t lobby);
void request_leave();

// The address last joined (netplay.cfg), for the menu.
std::string last_address();

enum class Status { Off, Hosting, Joining, Connected };
Status status();
bool active();         // a session is running (deterministic, inputs locked)
bool is_guest();       // connected as the guest: local save states / spawns are off
int player();          // 1 host, 2 guest, 0 none
std::string status_text();   // one line for the menu / overlay

// Game thread, from mm_savestate_frame_hook (after replay::on_frame).
void on_frame(uint8_t* rdram, recomp_context* ctx);

#ifdef MM_HAS_GRAPHICS
// Render thread: a small status line while connecting / waiting.
void draw_overlay();
#endif

} // namespace mm::netplay
