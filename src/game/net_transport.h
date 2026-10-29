// The connection netplay runs over (netplay.cpp): one partner, two reliable
// ordered channels. Implementations: ENet over UDP with an address and port
// (net_enet.cpp) and Steam lobbies with Steam's relays (steam_net.cpp). The
// netplay logic only sees these events and sends.
//
// Game thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mm::netplay {

struct NetEvent {
    enum Type {
        None,          // nothing happened
        Connected,     // the partner is there (host: a guest arrived; guest: reached the host)
        Received,      // a message from the partner
        Disconnected,  // the partner went away
        Failed,        // the connection cannot go on (text says why)
    } type = None;
    int channel = 0;
    std::vector<uint8_t> data;
    std::string text;
};

class Transport {
public:
    virtual ~Transport() = default;
    // The next event, waiting up to timeout_ms for one.
    virtual NetEvent poll(uint32_t timeout_ms) = 0;
    // Reliable, ordered, any size; dropped when there is no partner.
    virtual void send(int channel, const uint8_t* data, size_t size) = 0;
    // Push queued sends out now.
    virtual void flush() {}
    // Turn the partner away (a refused player); the host keeps waiting.
    virtual void drop_partner() = 0;
    // A line for the menu while no partner is there yet.
    virtual std::string waiting_text() const = 0;
};

// Each returns nullptr with `error` set when it cannot start.
std::unique_ptr<Transport> enet_host(int port, std::string& error);
std::unique_ptr<Transport> enet_join(const std::string& address, std::string& error);
std::unique_ptr<Transport> steam_host(std::string& error);
std::unique_ptr<Transport> steam_join(uint64_t lobby, std::string& error);   // 0: find a friend hosting

// Steam, outside a session: a join the player asked for through Steam (an
// accepted invite, Join Game). The lobby id once, or 0.
uint64_t steam_join_request();
// Game thread, every frame: the menu's friends list and its requests.
void steam_tick();

// Tests: MM_NET_LAG="<delay ms>[,<jitter ms>[,<loss %>]]" wraps a transport
// in a deliberately bad connection (net_lag.cpp); otherwise returns it as is.
std::unique_ptr<Transport> with_test_lag(std::unique_ptr<Transport> net);

} // namespace mm::netplay
