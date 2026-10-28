// How many frames ahead this side sends its input (netplay.cpp), chosen from
// the measured round trip. The input for a frame must reach the partner
// before the partner plays that frame: delay x 16.7 ms has to cover the
// one-way trip plus the jitter, or both games stop and wait.
//
// The choice only shapes when inputs arrive, never what the game does with
// them: every input is tied to a frame number and both machines wait for
// both inputs of a frame, so each side can pick its own delay at any time
// and the games stay identical.
#pragma once

#include <cstdint>

namespace mm::netplay {

class DelayController {
public:
    static constexpr int kMin = 2, kMax = 10;

    // A measured round trip, in milliseconds.
    void add_rtt(double ms);
    // Once per frame: the delay to use now. Up at once when the trip needs
    // more; down one frame after five seconds of needing less.
    int update(int current);

    double rtt_ms() const { return smoothed_; }   // for the overlay, 0 before the first sample
    double rtt_high_ms() const;                   // the worst of the recent samples
    int wanted() const;                           // the delay the recent samples ask for

private:
    static constexpr int kSamples = 16;   // about 4 s at four pings a second
    double samples_[kSamples]{};
    int count_ = 0, next_ = 0;
    double smoothed_ = 0;
    int calm_frames_ = 0;
};

} // namespace mm::netplay
