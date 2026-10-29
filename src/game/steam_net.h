// Steam transport for netplay: no IP addresses, no port forwarding.
//
// Uses Steamworks through its flat C API, loaded at run time (steam_api64.dll
// on Windows, libsteam_api.so elsewhere, next to the executable), as Valve's
// free sample app 480 "Spacewar": both players need Steam running and signed
// in, nobody needs to own anything (both show as "playing Spacewar"). The
// host creates a friends-only lobby; a friend joins by Steam invite, by Join
// Game on the host in the friends list, by "Join a friend on Steam" in the
// Session page (it finds a friend hosting), or by the lobby id. Game traffic
// goes over ISteamNetworkingMessages, direct when possible and through
// Valve's relays otherwise. See docs/NETPLAY.md and coop-anything's
// playbook/networking-steam-spacewar.md, which this follows.
//
// Game thread only. Netplay uses it as a transport (net_transport.h,
// steam_host / steam_join at the end of steam_net.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mm::steam {

// Loads the library and starts Steam (once). False with a reason when Steam
// is not available (library missing, Steam not running, ...).
bool start(std::string& why);

// Host: create a lobby. Joining: a lobby id, or 0 to find a friend hosting
// (and otherwise wait for an invite).
void host();
void join(uint64_t lobby);
// Leave the lobby and close the session (Steam stays started).
void leave();

// Drop the current partner (host: a refused player).
void drop_peer();

struct Event {
    enum Type { None, Connected, Received, Disconnected, Failed } type = None;
    int channel = 0;
    std::vector<uint8_t> data;
    std::string text;   // Failed: why
};
// Runs Steam callbacks and receives; one event per call (None when idle).
Event poll();

// Reliable, ordered send to the partner on channel 0 or 1 (any size).
bool send(int channel, const uint8_t* data, size_t size);

// For the menu: the lobby (host) and a line of status.
uint64_t lobby();
std::string status();

// Steam's overlay (Shift+Tab, its invite window) is there: only in games
// Steam itself started.
bool overlay_enabled();
// Friends online (for inviting without the overlay): in this game first.
struct Friend {
    uint64_t id = 0;
    std::string name;
    bool playing = false;   // in Spacewar (this game's Steam stand-in)
    bool hosting = false;   // ... with a lobby: probably hosting
};
std::vector<Friend> online_friends();
// Host: send a friend a Steam invite to the lobby (it appears in their Steam
// chat; accepting it while the game runs joins). No overlay needed.
bool invite(uint64_t friend_id);
// The lobby a friend is in (in this game), or 0: to join them from the menu.
uint64_t friend_lobby(uint64_t friend_id);
// Started (the library loaded, Steam running); hosting with a lobby.
bool started();
bool hosting();

// A join the player asked for through Steam while not in a session (an
// accepted invite, Join Game): the lobby id, taken once. 0 when none.
uint64_t take_join_request();

} // namespace mm::steam
