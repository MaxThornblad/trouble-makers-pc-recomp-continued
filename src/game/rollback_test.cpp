// netcode-v2 phase 0: can this game roll back? (docs/HANDOVER-netcode-v2.md)
//
// MM_TEST_ROLLBACK="<every>:<depth>" during a replay playback (MM_REPLAY_PLAY):
// a snapshot of the game is taken in memory at every frame point, and every
// <every> frames the one from <depth> frames ago is put back and those frames
// are run again, unpaced, with the recording's inputs. The replay's own
// fingerprint check then says whether the second run matched the recording,
// and this prints what it cost:
//
//   [rollback-test] snapshot avg 1.9 ms   restore 2.3 ms   4 frames again 9.1 ms (2.3/frame)
//
// Rollback netcode would do exactly this when a predicted input turns out
// wrong, so these numbers (on the owner's PC) decide whether it fits in a
// 16.7 ms frame. Test only: nothing here runs without the variable.
#include "rollback_test.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>

#include "replay.h"
#include "savestate.h"
#include "ultramodern/ultramodern.hpp"

namespace mm::rollback_test {
namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Kept {
    long frame;
    mm::savestate::MemorySnapshotPtr snap;
    uint64_t fingerprint;
};

bool g_on = false;
long g_every = 0, g_depth = 0;
std::deque<Kept> g_ring;                 // the last depth+1 frames, oldest first
std::deque<mm::savestate::MemorySnapshotPtr> g_spare;   // buffers to reuse

// Totals.
double g_snapshot_ms = 0;
long g_snapshots = 0;
long g_rollbacks = 0, g_mismatches = 0;
double g_restore_ms = 0, g_again_ms = 0;

// A rollback under way: frames [g_from, g_target] are being run again.
bool g_again = false;
long g_target = -1;
uint64_t g_target_fingerprint = 0;
Clock::time_point g_again_started;
uint32_t g_speed_before = 1;

void configure() {
    static bool read = false;
    if (read) return;
    read = true;
    const char* v = std::getenv("MM_TEST_ROLLBACK");
    if (!v || !*v) return;
    if (std::sscanf(v, "%ld:%ld", &g_every, &g_depth) != 2 || g_every <= 0 || g_depth <= 0) {
        std::fprintf(stderr, "[rollback-test] MM_TEST_ROLLBACK wants \"<every>:<depth>\"\n");
        return;
    }
    g_on = true;
    std::fprintf(stderr, "[rollback-test] every %ld frames, %ld frames back\n", g_every, g_depth);
}

void report() {
    std::fprintf(stderr,
                 "[rollback-test] %ld rollbacks, %ld mismatched: snapshot avg %.2f ms (%ld), restore avg %.2f ms, "
                 "%ld frames again avg %.2f ms (%.2f/frame)\n",
                 g_rollbacks, g_mismatches, g_snapshots ? g_snapshot_ms / g_snapshots : 0.0, g_snapshots,
                 g_rollbacks ? g_restore_ms / g_rollbacks : 0.0, g_depth, g_rollbacks ? g_again_ms / g_rollbacks : 0.0,
                 g_rollbacks ? g_again_ms / g_rollbacks / g_depth : 0.0);
}

} // namespace

void on_frame(uint8_t* rdram, recomp_context* ctx, long frame) {
    configure();
    if (!g_on) return;
    const uint64_t fingerprint = mm::replay::memory_fingerprint(rdram);

    if (g_again) {
        if (frame < g_target) return;   // still catching up
        // Back where the rollback started: the second run of these frames
        // must have produced the same game.
        const double ms = ms_since(g_again_started);
        g_again_ms += ms;
        g_again = false;
        ultramodern::set_speed_multiplier(g_speed_before);
        const bool same = fingerprint == g_target_fingerprint;
        if (!same) ++g_mismatches;
        std::fprintf(stderr, "[rollback-test] frame %ld: %ld frames again in %.2f ms%s\n", frame, g_depth, ms,
                     same ? "" : "  MISMATCH");
        if (g_rollbacks % 10 == 0) report();
        // The ring holds frames before the rollback; keep going from here.
    }

    // Snapshot this frame.
    mm::savestate::MemorySnapshotPtr snap;
    if (!g_spare.empty()) {
        snap = std::move(g_spare.front());
        g_spare.pop_front();
    } else {
        snap = mm::savestate::new_memory_snapshot();
    }
    std::string error;
    const auto t0 = Clock::now();
    if (!mm::savestate::capture_memory(rdram, ctx, *snap, error)) {
        std::fprintf(stderr, "[rollback-test] snapshot failed at frame %ld: %s\n", frame, error.c_str());
        g_on = false;
        return;
    }
    g_snapshot_ms += ms_since(t0);
    ++g_snapshots;
    g_ring.push_back(Kept{frame, std::move(snap), fingerprint});
    while (static_cast<long>(g_ring.size()) > g_depth + 1) {
        g_spare.push_back(std::move(g_ring.front().snap));
        g_ring.pop_front();
    }

    // Time to roll back?
    if (frame % g_every != 0 || static_cast<long>(g_ring.size()) < g_depth + 1) return;
    Kept& from = g_ring.front();
    if (from.frame != frame - g_depth) return;   // a gap (the ring restarted)
    const auto t1 = Clock::now();
    if (!mm::savestate::apply_memory(rdram, ctx, *from.snap, error)) {
        std::fprintf(stderr, "[rollback-test] restore failed at frame %ld: %s\n", frame, error.c_str());
        g_on = false;
        return;
    }
    mm::replay::rewind_to(from.frame);
    g_restore_ms += ms_since(t1);
    ++g_rollbacks;
    g_again = true;
    g_target = frame;
    g_target_fingerprint = fingerprint;
    g_again_started = Clock::now();
    g_speed_before = ultramodern::get_speed_multiplier();
    ultramodern::set_speed_multiplier(1000);   // the frames again as fast as they run
    // The ring now describes frames the game is about to run again: drop
    // them, they are snapshotted afresh on the way.
    for (Kept& k : g_ring) g_spare.push_back(std::move(k.snap));
    g_ring.clear();
}

void on_stop() {
    if (g_on && g_rollbacks) report();
}

} // namespace mm::rollback_test
