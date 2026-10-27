// ENet transport for netplay (net_transport.h): reliable, ordered UDP with
// an address and port, one guest at a time.
#include "net_transport.h"

#include <cstdlib>

#include <enet/enet.h>

namespace mm::netplay {
namespace {

constexpr int kChannels = 2;

bool ensure_enet(std::string& error) {
    static bool ready = false;
    if (!ready) {
        if (enet_initialize() != 0) {
            error = "network start-up failed";
            return false;
        }
        ready = true;
    }
    return true;
}

class Enet final : public Transport {
public:
    Enet(ENetHost* host, ENetPeer* peer, bool is_host, std::string waiting)
        : host_(host), peer_(peer), is_host_(is_host), waiting_(std::move(waiting)) {}

    ~Enet() override {
        if (peer_) enet_peer_disconnect_now(peer_, 0);
        enet_host_flush(host_);
        enet_host_destroy(host_);
    }

    NetEvent poll(uint32_t timeout_ms) override {
        ENetEvent ev;
        for (;;) {
            const int got = enet_host_service(host_, &ev, timeout_ms);
            if (got < 0) {
                NetEvent e;
                e.type = NetEvent::Failed;
                e.text = "network error";
                return e;
            }
            if (got == 0) return {};
            NetEvent e;
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    if (is_host_ && peer_ && peer_ != ev.peer) {
                        enet_peer_disconnect_now(ev.peer, 0);   // one guest at a time
                        continue;
                    }
                    peer_ = ev.peer;
                    enet_peer_timeout(ev.peer, 32, 10000, 30000);
                    e.type = NetEvent::Connected;
                    return e;
                case ENET_EVENT_TYPE_RECEIVE:
                    e.type = NetEvent::Received;
                    e.channel = ev.channelID;
                    e.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                    enet_packet_destroy(ev.packet);
                    return e;
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (ev.peer != peer_) continue;
                    peer_ = nullptr;
                    e.type = NetEvent::Disconnected;
                    return e;
                default:
                    continue;
            }
        }
    }

    void send(int channel, const uint8_t* data, size_t size) override {
        if (!peer_) return;
        ENetPacket* p = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer_, static_cast<enet_uint8>(channel), p);
    }

    void flush() override { enet_host_flush(host_); }

    void drop_partner() override {
        if (peer_) enet_peer_disconnect_later(peer_, 0);
    }

    std::string waiting_text() const override { return waiting_; }

private:
    ENetHost* host_;
    ENetPeer* peer_;
    bool is_host_;
    std::string waiting_;
};

} // namespace

std::unique_ptr<Transport> enet_host(int port, std::string& error) {
    if (!ensure_enet(error)) return nullptr;
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = static_cast<enet_uint16>(port);
    ENetHost* host = enet_host_create(&addr, 1, kChannels, 0, 0);
    if (!host) {
        error = "cannot listen on port " + std::to_string(port) + " (in use?)";
        return nullptr;
    }
    return std::make_unique<Enet>(host, nullptr, true,
                                  "Hosting on port " + std::to_string(port) + ": waiting for a player");
}

std::unique_ptr<Transport> enet_join(const std::string& address, std::string& error) {
    if (!ensure_enet(error)) return nullptr;
    std::string name = address;
    int port = 7845;
    if (const size_t colon = address.rfind(':'); colon != std::string::npos && address.find(':') == colon) {
        name = address.substr(0, colon);
        port = std::atoi(address.c_str() + colon + 1);
    }
    ENetAddress addr{};
    if (enet_address_set_host(&addr, name.c_str()) != 0) {
        error = "cannot find " + name;
        return nullptr;
    }
    addr.port = static_cast<enet_uint16>(port);
    ENetHost* host = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (!host) {
        error = "network start-up failed";
        return nullptr;
    }
    ENetPeer* peer = enet_host_connect(host, &addr, kChannels, 0);
    if (!peer) {
        enet_host_destroy(host);
        error = "cannot connect";
        return nullptr;
    }
    // The peer only counts once connected (the Connected event).
    auto t = std::make_unique<Enet>(host, nullptr, false, "Joining " + name + ":" + std::to_string(port) + "...");
    (void)peer;
    return t;
}

} // namespace mm::netplay
