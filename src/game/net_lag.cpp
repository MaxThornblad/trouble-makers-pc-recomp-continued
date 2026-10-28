// A bad connection on purpose (tests): wraps a transport and holds each
// outgoing message back by a delay, a random jitter, and now and then a lost
// packet's resend. Each side delays what it sends, so the round trip is
// about twice the delay.
//
// MM_NET_LAG="<delay ms>[,<jitter ms>[,<loss %>]]", e.g. "60,15,1".
//
// The channels stay reliable and ordered, as ENet and Steam keep them: a
// message never overtakes an earlier one on its channel, so a "lost" one
// holds up those behind it (head-of-line blocking), as a real resend does.
#include "net_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>

namespace mm::netplay {
namespace {

using Clock = std::chrono::steady_clock;

class Lagged final : public Transport {
public:
    Lagged(std::unique_ptr<Transport> inner, double delay_ms, double jitter_ms, double loss_pct)
        : inner_(std::move(inner)), delay_(delay_ms), jitter_(jitter_ms), loss_(loss_pct / 100.0) {}

    NetEvent poll(uint32_t timeout_ms) override {
        const auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            release();
            // Wait no longer than the next held message is due.
            auto wait = until - Clock::now();
            if (!held_.empty()) wait = std::min(wait, held_.front().due - Clock::now());
            const auto ms = std::max<long>(0, static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(wait).count()));
            NetEvent ev = inner_->poll(static_cast<uint32_t>(ms));
            if (ev.type != NetEvent::None || Clock::now() >= until) {
                release();
                return ev;
            }
        }
    }

    void send(int channel, const uint8_t* data, size_t size) override {
        const auto now = Clock::now();
        double ms = delay_ + (jitter_ > 0 ? std::uniform_real_distribution<double>(-jitter_, jitter_)(rng_) : 0.0);
        if (loss_ > 0 && std::uniform_real_distribution<double>(0, 1)(rng_) < loss_)
            ms += 2 * delay_ + 20;   // lost once: resent after about a round trip
        auto due = now + std::chrono::microseconds(static_cast<long>(std::max(0.0, ms) * 1000));
        if (channel >= 0 && channel < 2) {
            due = std::max(due, last_due_[channel]);   // ordered within the channel
            last_due_[channel] = due;
        }
        Held h{due, channel, std::vector<uint8_t>(data, data + size)};
        // Keep the queue sorted by due time (ties keep their order).
        auto at = std::upper_bound(held_.begin(), held_.end(), h,
                                   [](const Held& a, const Held& b) { return a.due < b.due; });
        held_.insert(at, std::move(h));
        release();
    }

    void flush() override {
        release();
        inner_->flush();
    }

    void drop_partner() override {
        held_.clear();
        inner_->drop_partner();
    }

    std::string waiting_text() const override { return inner_->waiting_text(); }

private:
    struct Held {
        Clock::time_point due;
        int channel;
        std::vector<uint8_t> data;
    };

    void release() {
        const auto now = Clock::now();
        bool sent = false;
        while (!held_.empty() && held_.front().due <= now) {
            inner_->send(held_.front().channel, held_.front().data.data(), held_.front().data.size());
            held_.pop_front();
            sent = true;
        }
        if (sent) inner_->flush();
    }

    std::unique_ptr<Transport> inner_;
    double delay_, jitter_, loss_;
    std::deque<Held> held_;
    Clock::time_point last_due_[2]{};
    std::mt19937 rng_{12345};
};

} // namespace

std::unique_ptr<Transport> with_test_lag(std::unique_ptr<Transport> net) {
    const char* spec = std::getenv("MM_NET_LAG");
    if (!net || !spec || !*spec) return net;
    double v[3] = {0, 0, 0};
    std::sscanf(spec, "%lf,%lf,%lf", &v[0], &v[1], &v[2]);
    std::fprintf(stderr, "[netplay] test lag: %.0f ms +-%.0f ms, %.1f%% lost\n", v[0], v[1], v[2]);
    return std::make_unique<Lagged>(std::move(net), v[0], v[1], v[2]);
}

} // namespace mm::netplay
