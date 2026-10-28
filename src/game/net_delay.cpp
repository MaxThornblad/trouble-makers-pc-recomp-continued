#include "net_delay.h"

#include <algorithm>
#include <cmath>

namespace mm::netplay {

namespace {
constexpr double kFrameMs = 1000.0 / 60.0;
constexpr double kSlackMs = 4.0;      // processing and the frame point's own timing
constexpr int kCalmFrames = 5 * 60;   // how long a smaller need must last before stepping down
}

void DelayController::add_rtt(double ms) {
    samples_[next_] = ms;
    next_ = (next_ + 1) % kSamples;
    count_ = std::min(count_ + 1, kSamples);
    smoothed_ = smoothed_ == 0 ? ms : smoothed_ * 0.8 + ms * 0.2;
}

double DelayController::rtt_high_ms() const {
    double hi = 0;
    for (int i = 0; i < count_; ++i) hi = std::max(hi, samples_[i]);
    return hi;
}

int DelayController::wanted() const {
    if (count_ == 0) return -1;
    // One way is about half the round trip; the worst recent sample covers
    // the jitter. One frame more: the input leaves at this side's frame
    // point and must be in before the partner's.
    const double one_way = rtt_high_ms() / 2.0 + kSlackMs;
    const int frames = static_cast<int>(std::ceil(one_way / kFrameMs)) + 1;
    return std::clamp(frames, kMin, kMax);
}

int DelayController::update(int current) {
    const int want = wanted();
    if (want < 0) return current;
    if (want > current) {
        calm_frames_ = 0;
        return want;
    }
    if (want < current) {
        if (++calm_frames_ >= kCalmFrames) {
            calm_frames_ = 0;
            return current - 1;
        }
        return current;
    }
    calm_frames_ = 0;
    return current;
}

} // namespace mm::netplay
