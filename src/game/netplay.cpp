#include "netplay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <vector>


#include "cheats.h"
#include "frame_input.h"
#include "net_delay.h"
#include "presentation_switch.h"
#include "net_transport.h"
#include "librecomp/game.hpp"
#include "mm_audio_input.hpp"
#include "replay.h"
#include "savestate.h"
#include "ultramodern/ultramodern.hpp"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

#ifdef _WIN32
#include <stdlib.h>   // _putenv_s
#endif

#ifndef MM_GIT_COMMIT
#define MM_GIT_COMMIT "unknown"
#endif

extern "C" void mm_virtual_audio_clock(bool on);

namespace mm::netplay {
namespace {

// ---------------------------------------------------------------- protocol

constexpr uint32_t kMagic = 0x504E4D54u;   // "TMNP"
constexpr uint32_t kProtocol = 4;   // 3: one side's frame_input (pad, settings, commands) per input; 4: kBye
// Channels: 0 control (hello, refuse, desync), 1 start + input + fingerprints + pings.
constexpr int kHashEvery = 60;             // frames between fingerprint checks

enum Msg : uint8_t {
    kHello = 1,    // guest -> host: magic, protocol, build
    kStart = 2,    // host -> guest: epoch, delay, settings, eeprom, state
    kInput = 3,    // both: epoch, frame, pad, settings and commands (the host's count; the guest's are ignored)
    kHash = 4,     // both: epoch, frame, fingerprint
    kDesync = 5,   // guest -> host: epoch, frame (please resync)
    kRefuse = 6,   // host -> guest: reason text
    kPing = 7,     // both: sender's clock (us), answered with kPong
    kPong = 8,     // both: the clock from the ping, back to its sender
    kBye = 9,      // both: this side ends the session (no reconnecting)
};

struct Writer {
    std::vector<uint8_t> b;
    template <class T> void put(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof v);
    }
    void bytes(const void* p, size_t n) {
        put<uint32_t>(static_cast<uint32_t>(n));
        const auto* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }
};

struct Reader {
    const uint8_t* p;
    size_t n, pos = 0;
    template <class T> bool get(T& v) {
        if (pos + sizeof v > n) return false;
        std::memcpy(&v, p + pos, sizeof v);
        pos += sizeof v;
        return true;
    }
    bool bytes(std::vector<uint8_t>& out) {
        uint32_t len;
        if (!get(len) || pos + len > n) return false;
        out.assign(p + pos, p + pos + len);
        pos += len;
        return true;
    }
};

long g_global = 0;                  // frames since launch
long global_frame() { return g_global; }

uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ------------------------------------------------------------------- state

// One side's contribution to a frame: its pad, and (the host) the settings
// and commands in force for that frame, so a menu change on the host takes
// effect on both machines on the same frame.
struct Input {
    mm::frame_input::Pad pad;
    mm::cheats::SessionSettings settings;
    uint8_t command_count = 0;
    mm::frame_input::Command commands[mm::frame_input::kMaxCommands];
};

static_assert(sizeof(mm::cheats::SessionSettings) == 4 + 8, "SessionSettings has padding: memcmp would compare it");

bool same_input(const Input& a, const Input& b) {
    if (a.pad.buttons != b.pad.buttons || a.pad.x != b.pad.x || a.pad.y != b.pad.y || a.pad.connected != b.pad.connected)
        return false;
    // The whole struct (no padding, static_assert below): a field-by-field
    // compare missed the throw sliders, so a prediction wrong about them was
    // taken as right and the frames in between threw at the old strength.
    if (std::memcmp(&a.settings, &b.settings, sizeof a.settings) != 0) return false;
    if (a.command_count != b.command_count) return false;
    for (int i = 0; i < a.command_count; ++i) {
        if (a.commands[i].kind != b.commands[i].kind || a.commands[i].a != b.commands[i].a ||
            a.commands[i].b != b.commands[i].b)
            return false;
    }
    return true;
}

void write_input(Writer& w, const Input& in) {
    w.put(in.pad);
    w.put(in.settings);
    w.put(in.command_count);
    for (int i = 0; i < in.command_count && i < mm::frame_input::kMaxCommands; ++i) w.put(in.commands[i]);
}

bool read_input(Reader& r, Input& in) {
    if (!r.get(in.pad) || !r.get(in.settings) || !r.get(in.command_count)) return false;
    if (in.command_count > mm::frame_input::kMaxCommands) return false;
    for (int i = 0; i < in.command_count; ++i) {
        if (!r.get(in.commands[i])) return false;
    }
    return true;
}

// The host's settings for the session: its own, with the rules online play
// imposes (always widescreen: each player's own view moves the picture
// sideways and the wings keep the moved edges filled; always co-op).
mm::cheats::SessionSettings host_settings() {
    mm::cheats::SessionSettings s = mm::cheats::capture_session_settings();
    s.widescreen = 1;
    s.coop = 1;
    s.split = 0;   // each player has a screen of her own
    return s;
}

enum class Req { None, Host, Join, Leave, HostSteam, JoinSteam };

std::filesystem::path g_folder;
std::atomic<Status> g_status{Status::Off};
std::atomic<bool> g_active{false};
std::atomic<int> g_player{0};
std::atomic<Req> g_request{Req::None};
std::mutex g_mutex;                 // g_req_address, g_text, g_last_address
std::string g_req_address;
uint64_t g_req_lobby = 0;           // JoinSteam: lobby id, 0 = find a friend hosting
int g_req_port = kDefaultPort;
std::string g_text;
std::string g_last_address;
std::atomic<bool> g_waiting{false};  // stalled for the partner's input

// Game thread only from here on.
std::unique_ptr<Transport> g_net;   // the connection (ENet or Steam), while not Off
bool g_partner = false;             // the transport reported the partner there
bool g_is_host = false;
bool g_peer_ok = false;             // host: hello accepted; guest: connected
bool g_start_wanted = false;        // host: start (or resync) at this frame point
long g_start_at = -1;               // host: start once this global frame comes (after the hello)
// Reconnecting: the connection dropped mid-session. The host waits (the game
// stalls for the partner's input, as on a slow link) and restarts both sides
// from its game when the guest is back; the guest connects again to where it
// joined. Given up after kReconnectWindow (the stall's own limit).
constexpr auto kReconnectWindow = std::chrono::seconds(60);
bool g_reconnecting = false;
std::chrono::steady_clock::time_point g_reconnect_until{};
bool g_rejoin_steam = false;        // guest: how it joined, to join again
std::string g_rejoin_address;
uint64_t g_rejoin_lobby = 0;
uint32_t g_epoch = 0;
int g_delay = 4;                    // frames ahead this side sends its input (net_delay.h)
bool g_delay_fixed = false;         // MM_NET_DELAY: no adapting
DelayController g_delay_ctl;
uint32_t g_last_sent = 0;           // the last frame this side's input went out for
std::atomic<int> g_shown_ping{-1}, g_shown_delay{0};   // for the overlay (render thread)
long g_long_waits = 0;              // frames this session that waited over a frame for the partner
double g_longest_wait_ms = 0;

// Rollback (docs/NETCODE-V2.md, phase 4). A frame whose partner input has
// not arrived runs with a predicted one; a snapshot of every frame point is
// kept, and when a late input differs from its prediction the game goes
// back to that frame's snapshot and runs the frames since again, one per
// hook call, unpaced and unseen, until it is back where it was.
bool g_rollback_mode = true;        // MM_NET_LOCKSTEP=1: wait for every input instead
constexpr int kMaxPredict = 10;     // frames run on prediction before the game waits
struct Kept {
    uint32_t frame;
    mm::savestate::MemorySnapshotPtr snap;
    bool hashed = false;   // its fingerprint went out (once the frame was confirmed)
};
std::deque<Kept> g_snaps;                               // the last frame points, oldest first
std::deque<mm::savestate::MemorySnapshotPtr> g_spare;   // buffers to reuse
std::map<uint32_t, Input> g_predicted;                  // frames run on a prediction: the input used
Input g_remote_last;                                    // the partner's latest input (what is predicted)
uint32_t g_remote_last_frame = 0;
uint32_t g_rollback_to = UINT32_MAX;                    // the earliest frame whose prediction was wrong
bool g_catching_up = false;                             // running frames again
uint32_t g_catch_frame = 0, g_catch_target = 0;
uint32_t g_speed_before = 1;
long g_rollbacks = 0, g_rollback_frames = 0, g_predicted_frames = 0;
bool g_sync_test = false;                               // MM_NET_SYNC_TEST=1: roll back every frame
uint32_t g_sync_tested = 0;                             // the frame the sync test last rolled back to
long g_frame = 0;                   // session frame
std::map<uint32_t, Input> g_local, g_remote;
std::map<uint32_t, uint64_t> g_local_hash, g_remote_hash;
uint32_t g_seen_load_generation = 0;
int g_resyncs = 0;
bool g_sandboxed = false;
bool g_restarted = false;                       // a start was applied during this frame point
mm::cheats::SessionSettings g_start_settings;   // the host's, from the last (re)start

// Widescreen environment, as the guest had it before following the host.
struct EnvSave { const char* name; std::string value; bool had; };
std::vector<EnvSave> g_env_saved;
constexpr const char* kWideEnv[] = {"MM_WIDESCREEN_ACTIVE", "MM_BAND_WINGS", "MM_STATIC_WINGS",
                                    "MM_ENV_WINGS", "MM_MID_WINGS"};

// Test scripting (MM_NET_PAD: frame_input's pad script for this side, for
// the session's frames).
const char* g_pad_script = nullptr;
long g_length = -1;
std::FILE* g_log = nullptr;

void set_text(const std::string& t) {
    std::lock_guard lock{g_mutex};
    if (g_text != t) std::fprintf(stderr, "[netplay] %s\n", t.c_str());
    g_text = t;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1';
}

void follow_widescreen(bool wide) {
    if (env_on("MM_WIDESCREEN_ACTIVE") == wide) return;
    if (g_env_saved.empty()) {
        for (const char* n : kWideEnv) {
            const char* v = std::getenv(n);
            g_env_saved.push_back({n, v ? v : "", v != nullptr});
        }
    }
    for (const char* n : kWideEnv) set_env(n, wide ? "1" : "0");
    std::fprintf(stderr, "[netplay] widescreen %s for the session\n", wide ? "on" : "off");
}

void restore_widescreen() {
    for (const EnvSave& e : g_env_saved) set_env(e.name, e.had ? e.value.c_str() : nullptr);
    g_env_saved.clear();
}

// This side's contribution for session frame `frame`: its pad (the host's
// pad is controller 1, the guest's controller 2), and from the host the
// settings and commands.
Input local_input(long frame) {
    const mm::frame_input::Frame f = mm::frame_input::sample_local(frame);
    Input in;
    in.pad = f.pad[0];
    if (g_is_host) {
        in.settings = host_settings();
        in.command_count = f.command_count;
        for (int i = 0; i < f.command_count; ++i) in.commands[i] = f.commands[i];
    }
    return in;
}

// Hands the game both sides' contributions for frame f (frame_input.h).
void latch_frame(uint32_t frame, const Input& host, const Input& guest) {
    mm::frame_input::Frame f;
    f.pad[0] = host.pad;
    f.pad[1] = guest.pad;
    f.pad[0].connected = f.pad[1].connected = 1;
    f.settings = host.settings;
    f.command_count = host.command_count;
    for (int i = 0; i < host.command_count; ++i) f.commands[i] = host.commands[i];
    mm::frame_input::latch(f, !g_is_host, frame);
}

void send(int channel, const Writer& w) {
    if (g_net) g_net->send(channel, w.b.data(), w.b.size());
}

void flush() {
    if (g_net) g_net->flush();
}

// ---------------------------------------------------------------- sessions

void reset_frames() {
    g_frame = 0;
    g_local.clear();
    g_remote.clear();
    g_local_hash.clear();
    g_remote_hash.clear();
    // The first `delay` frames have no input from either side yet: nothing
    // pressed, under the settings the session (re)started with.
    Input start;
    start.settings = g_start_settings;
    for (int f = 0; f < g_delay; ++f) {
        g_local[static_cast<uint32_t>(f)] = g_is_host ? start : Input{};
        g_remote[static_cast<uint32_t>(f)] = g_is_host ? Input{} : start;
    }
    g_last_sent = static_cast<uint32_t>(g_delay - 1);
    for (Kept& k : g_snaps) g_spare.push_back(std::move(k.snap));
    g_snaps.clear();
    g_predicted.clear();
    g_remote_last = Input{};
    g_remote_last_frame = 0;
    g_rollback_to = UINT32_MAX;
    g_sync_tested = 0;
    if (g_catching_up) {
        g_catching_up = false;
        ultramodern::set_speed_multiplier(g_speed_before);
        mm::presentation::set_live(true);
    }
}

void begin_running() {
    if (!g_active.load()) {
        g_long_waits = 0;
        g_longest_wait_ms = 0;
        g_rollbacks = g_rollback_frames = g_predicted_frames = 0;
    }
    reset_frames();
    mm::frame_input::set_pad_script(0, g_pad_script);
    mm_virtual_audio_clock(true);
    g_seen_load_generation = mm::savestate::load_generation();
    g_active.store(true);
    g_status.store(Status::Connected);
    g_restarted = true;
}

void end_session(const char* why) {
    const bool was_active = g_active.exchange(false);
    if (was_active) {
        std::fprintf(stderr, "[netplay] frames that waited over a frame for the partner: %ld, longest %.0f ms\n",
                     g_long_waits, g_longest_wait_ms);
        std::fprintf(stderr, "[netplay] rollbacks: %ld (%ld frames run again), %ld frames run on a prediction\n",
                     g_rollbacks, g_rollback_frames, g_predicted_frames);
        if (g_catching_up) {
            g_catching_up = false;
            ultramodern::set_speed_multiplier(g_speed_before);
            mm::presentation::set_live(true);
        }
        mm::frame_input::set_pad_script(0, nullptr);
        mm_virtual_audio_clock(false);
        mm::cheats::set_session_settings(nullptr);
        restore_widescreen();
    }
    if (g_sandboxed) {
        recomp::end_save_sandbox();
        g_sandboxed = false;
    }
    if (g_net && g_partner) {   // a goodbye, so the other side does not wait for a reconnect
        Writer w;
        w.put<uint8_t>(kBye);
        send(0, w);
        flush();
    }
    g_net.reset();
    g_peer_ok = false;
    g_start_wanted = false;
    g_reconnecting = false;
    g_waiting.store(false);
    g_status.store(Status::Off);
    g_player.store(0);
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    std::fprintf(stderr, "[netplay] session ended (%s)\n", why);
    set_text(std::string("Netplay ended: ") + why);
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Host: take the game as it is now and start both sides from it.
bool host_start(uint8_t* rdram, recomp_context* ctx) {
    std::error_code ec;
    std::filesystem::create_directories(g_folder, ec);
    const auto path = g_folder / "host_start.tmstate";
    std::string error;
    ultramodern::set_deterministic_events(true);
    // As a replay recording does: take the state and continue from it loaded
    // back, so both sides start from a load.
    if (!mm::savestate::save_now(rdram, ctx, path, error) || !mm::savestate::load_now(rdram, ctx, path, error)) {
        std::fprintf(stderr, "[netplay] cannot take the start state: %s\n", error.c_str());
        return false;
    }
    const std::vector<uint8_t> state = read_file(path);
    const std::vector<uint8_t> eeprom = recomp::get_save_data();
    // The host's settings now; from here on each frame's input carries them,
    // so its menu changes reach both machines on the same frame.
    const mm::cheats::SessionSettings s_settings = host_settings();
    mm::cheats::set_session_settings(&s_settings);
    g_start_settings = s_settings;
    follow_widescreen(true);

    ++g_epoch;
    Writer w;
    w.put<uint8_t>(kStart);
    w.put<uint32_t>(g_epoch);
    w.put<uint32_t>(static_cast<uint32_t>(g_delay));
    w.put(s_settings);
    w.bytes(eeprom.data(), eeprom.size());
    w.bytes(state.data(), state.size());
    // On the input channel: the inputs for the new epoch follow it there, so
    // they cannot overtake it (small packets do overtake a large message on
    // another channel) and be dropped by a guest not yet started.
    send(1, w);
    flush();
    begin_running();
    std::fprintf(stderr, "[netplay] %s from the host's game (epoch %u, %zu KiB state)\n",
                 g_epoch == 1 ? "session started" : (g_reconnecting ? "reconnected" : "resync"), g_epoch,
                 state.size() / 1024);
    if (g_reconnecting) set_text("Netplay: the other player is back");
    g_reconnecting = false;
    return true;
}

// Guest: start from the host's game.
void guest_start(uint8_t* rdram, recomp_context* ctx, Reader& r) {
    uint32_t epoch, delay;
    mm::cheats::SessionSettings settings;
    std::vector<uint8_t> eeprom, state;
    if (!r.get(epoch) || !r.get(delay) || !r.get(settings) || !r.bytes(eeprom) || !r.bytes(state)) {
        std::fprintf(stderr, "[netplay] bad start message\n");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_folder, ec);
    const auto path = g_folder / "guest_start.tmstate";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
    }
    // The host's cartridge save, in memory: this player's own save file is
    // neither read nor written during the session.
    if (g_sandboxed) recomp::end_save_sandbox();
    recomp::begin_save_sandbox(eeprom);
    g_sandboxed = true;
    mm::cheats::set_session_settings(&settings, true);
    g_start_settings = settings;
    follow_widescreen(settings.widescreen != 0);
    ultramodern::set_deterministic_events(true);
    std::string error;
    if (!mm::savestate::load_now(rdram, ctx, path, error)) {
        std::fprintf(stderr, "[netplay] cannot load the host's state: %s\n", error.c_str());
        end_session("could not load the host's game");
        return;
    }
    g_epoch = epoch;
    g_delay = static_cast<int>(std::clamp<uint32_t>(delay, 1, 30));
    if ((epoch > 1 || g_active.load()) && !g_reconnecting) ++g_resyncs;
    begin_running();
    std::fprintf(stderr, "[netplay] %s (epoch %u, delay %d)\n",
                 epoch == 1 ? "joined the host's game" : (g_reconnecting ? "reconnected" : "resynced"), epoch, g_delay);
    if (g_reconnecting) set_text("Netplay: reconnected");
    g_reconnecting = false;
}

void resync(uint8_t* rdram, recomp_context* ctx, const char* why) {
    ++g_resyncs;
    std::fprintf(stderr, "[netplay] resync: %s\n", why);
    host_start(rdram, ctx);
}

// ------------------------------------------------------------------ events

void on_packet(uint8_t* rdram, recomp_context* ctx, const uint8_t* data, size_t size) {
    Reader r{data, size};
    uint8_t type;
    if (!r.get(type)) return;
    switch (type) {
        case kHello: {
            if (!g_is_host) return;
            uint32_t magic = 0, protocol = 0;
            std::vector<uint8_t> build;
            r.get(magic);
            r.get(protocol);
            r.bytes(build);
            const std::string theirs(build.begin(), build.end());
            if (magic != kMagic || protocol != kProtocol || theirs != MM_GIT_COMMIT) {
                Writer w;
                w.put<uint8_t>(kRefuse);
                const std::string why = "different game version (host " MM_GIT_COMMIT ", you " + theirs + ")";
                w.bytes(why.data(), why.size());
                send(0, w);
                flush();
                std::fprintf(stderr, "[netplay] refused a player: %s\n", why.c_str());
                g_net->drop_partner();
                return;
            }
            g_peer_ok = true;
            if (g_active.load()) {
                g_start_wanted = true;               // back after a drop: restart both from here
            } else {
                g_start_at = global_frame() + 20;   // a third of a second of round-trip samples first
            }
            return;
        }
        case kStart:
            if (!g_is_host) guest_start(rdram, ctx, r);
            return;
        case kRefuse: {
            std::vector<uint8_t> why;
            r.bytes(why);
            end_session(std::string(why.begin(), why.end()).c_str());
            return;
        }
        case kInput: {
            uint32_t epoch, frame;
            Input in;
            if (!r.get(epoch) || !r.get(frame) || !read_input(r, in)) return;
            if (epoch != g_epoch || !g_active.load()) return;
            if (frame > g_remote_last_frame || g_remote_last_frame == 0) {
                g_remote_last = in;
                g_remote_last_frame = frame;
            }
            g_remote[frame] = in;
            if (const auto p = g_predicted.find(frame); p != g_predicted.end()) {
                // A frame already run on a prediction: right, or to be run again.
                if (same_input(p->second, in)) g_predicted.erase(p);
                else g_rollback_to = std::min(g_rollback_to, frame);
            }
            return;
        }
        case kBye:
            g_partner = false;
            end_session("the other player left");
            return;
        case kPing: {
            uint64_t t;
            if (!r.get(t)) return;
            Writer w;
            w.put<uint8_t>(kPong);
            w.put<uint64_t>(t);
            send(1, w);
            flush();
            return;
        }
        case kPong: {
            uint64_t t;
            if (!r.get(t)) return;
            const double ms = static_cast<double>(now_us() - t) / 1000.0;
            if (g_active.load() && g_frame < 60) return;   // sent before this (re)start
            g_delay_ctl.add_rtt(ms);
            g_shown_ping.store(static_cast<int>(std::lround(g_delay_ctl.rtt_ms())));
            return;
        }
        case kHash: {
            uint32_t epoch, frame;
            uint64_t h;
            if (!r.get(epoch) || !r.get(frame) || !r.get(h)) return;
            if (epoch == g_epoch && g_active.load()) g_remote_hash[frame] = h;
            return;
        }
        case kDesync: {
            uint32_t epoch, frame;
            if (!r.get(epoch) || !r.get(frame)) return;
            if (g_is_host && epoch == g_epoch && g_active.load()) g_start_wanted = true;
            return;
        }
        default: return;
    }
}

void send_hello() {
    Writer w;
    w.put<uint8_t>(kHello);
    w.put<uint32_t>(kMagic);
    w.put<uint32_t>(kProtocol);
    const std::string build = MM_GIT_COMMIT;
    w.bytes(build.data(), build.size());
    send(0, w);
    flush();
    g_peer_ok = true;
    if (g_rejoin_steam && g_rejoin_lobby == 0) g_rejoin_lobby = steam_current_lobby();
    set_text(g_reconnecting ? "Reconnected, waiting for the host's game..." : "Connected, waiting for the host's game...");
}

std::unique_ptr<Transport> connect_again(std::string& error) {
    return g_rejoin_steam ? steam_join(g_rejoin_lobby, error) : enet_join(g_rejoin_address, error);
}

// The connection dropped mid-session (why), or a reconnect attempt failed
// (why null): keep the session and get the partner back.
void begin_reconnect(const char* why) {
    if (why) {
        std::fprintf(stderr, "[netplay] %s: reconnecting\n", why);
        g_reconnecting = true;
        g_reconnect_until = std::chrono::steady_clock::now() + kReconnectWindow;
    }
    g_peer_ok = false;
    g_start_at = -1;
    if (g_is_host) {
        set_text("The other player dropped out: waiting up to a minute for them to come back...");
        return;   // the transport keeps listening (ENet port, Steam lobby)
    }
    set_text("Connection lost: reconnecting...");
    g_net.reset();
    if (!why) std::this_thread::sleep_for(std::chrono::milliseconds(500));   // between attempts
    std::string error;
    auto net = connect_again(error);
    if (!net) {
        end_session(("could not reconnect: " + error).c_str());
        return;
    }
    g_net = with_test_lag(std::move(net));
}

// Handles the connection's events, waiting up to timeout_ms for the first;
// false when the session is gone.
bool service(uint8_t* rdram, recomp_context* ctx, uint32_t timeout_ms) {
    if (!g_net) return false;
    for (NetEvent ev = g_net->poll(timeout_ms); ev.type != NetEvent::None; ev = g_net->poll(0)) {
        switch (ev.type) {
            case NetEvent::Connected:
                g_partner = true;
                if (g_is_host) set_text("A player is connecting...");
                else send_hello();
                break;
            case NetEvent::Received:
                on_packet(rdram, ctx, ev.data.data(), ev.data.size());
                if (!g_net) return false;   // the packet ended the session
                break;
            case NetEvent::Disconnected:
                g_partner = false;
                if (g_is_host && !g_active.load()) {
                    g_peer_ok = false;
                    g_start_at = -1;
                    set_text(g_net->waiting_text());
                } else if (g_active.load() || g_reconnecting) {
                    begin_reconnect("the connection dropped");
                    return g_net != nullptr;
                } else {
                    end_session("the other player left");
                    return false;
                }
                break;
            case NetEvent::Failed:
                if (g_reconnecting && !g_is_host && std::chrono::steady_clock::now() < g_reconnect_until) {
                    begin_reconnect(nullptr);   // not there yet: try again
                    return g_net != nullptr;
                }
                end_session(ev.text.c_str());
                return false;
            case NetEvent::None:
                break;
        }
    }
    if (!g_partner && !g_active.load()) set_text(g_net->waiting_text());   // e.g. Steam: lobby made
    return true;
}

// Starts a session's connection: hosting (player 1) or joining (player 2).
void begin(std::unique_ptr<Transport> net, const std::string& error, bool host) {
    if (!net) {
        set_text("Netplay: " + error);
        std::fprintf(stderr, "[netplay] %s\n", error.c_str());
        return;
    }
    g_net = with_test_lag(std::move(net));
    g_start_at = -1;
    g_delay_ctl = DelayController{};
    if (!g_delay_fixed) g_delay = 4;   // until the first round trips are in
    g_shown_ping.store(-1);
    g_partner = false;
    g_is_host = host;
    g_peer_ok = false;
    g_epoch = 0;
    g_resyncs = 0;
    g_player.store(host ? 1 : 2);
    g_status.store(host ? Status::Hosting : Status::Joining);
    set_text(g_net->waiting_text());
}

void start_hosting(int port) {
    std::string error;
    auto net = enet_host(port, error);
    begin(std::move(net), error, true);
}

void start_joining(const std::string& address) {
    std::string error;
    auto net = enet_join(address, error);
    begin(std::move(net), error, false);
    if (!g_net) return;
    g_rejoin_steam = false;
    g_rejoin_address = address;
    {
        std::lock_guard lock{g_mutex};
        g_last_address = address;
    }
    if (!g_folder.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_folder, ec);
        std::ofstream f(g_folder / "netplay.cfg", std::ios::trunc);
        f << "join=" << address << '\n';
    }
}

void start_hosting_steam() {
    std::string error;
    auto net = steam_host(error);
    begin(std::move(net), error, true);
}

void start_joining_steam(uint64_t lobby) {
    std::string error;
    auto net = steam_join(lobby, error);
    begin(std::move(net), error, false);
    g_rejoin_steam = true;
    g_rejoin_lobby = lobby;   // 0 (find a friend hosting): the lobby once in (send_hello)
}

// "<value>[@<frame>]"
bool env_at(const char* name, std::string& value, long& at) {
    const char* v = std::getenv(name);
    if (!v) return false;
    std::string s(v);
    at = 0;
    if (const size_t sep = s.rfind('@'); sep != std::string::npos) {
        at = std::strtol(s.c_str() + sep + 1, nullptr, 10);
        s = s.substr(0, sep);
    }
    value = s;
    return true;
}

// Snapshot of frame point `frame` (the game before that frame runs).
void keep_snapshot(uint8_t* rdram, recomp_context* ctx, uint32_t frame) {
    while (!g_snaps.empty() && g_snaps.back().frame >= frame) {   // running this frame again
        g_spare.push_back(std::move(g_snaps.back().snap));
        g_snaps.pop_back();
    }
    mm::savestate::MemorySnapshotPtr snap;
    if (!g_spare.empty()) {
        snap = std::move(g_spare.front());
        g_spare.pop_front();
    } else {
        snap = mm::savestate::new_memory_snapshot();
    }
    std::string error;
    if (!mm::savestate::capture_memory(rdram, ctx, *snap, error)) {
        std::fprintf(stderr, "[netplay] snapshot failed at frame %u: %s\n", frame, error.c_str());
        g_spare.push_back(std::move(snap));
        return;
    }
    g_snaps.push_back(Kept{frame, std::move(snap)});
    while (g_snaps.size() > static_cast<size_t>(kMaxPredict + 2)) {
        g_spare.push_back(std::move(g_snaps.front().snap));
        g_snaps.pop_front();
    }
}

// The partner's input for `frame`: the real one, or a prediction (its
// latest known input) noted as such.
Input partner_input(uint32_t frame) {
    if (const auto it = g_remote.find(frame); it != g_remote.end()) {
        if (const auto p = g_predicted.find(frame); p != g_predicted.end()) g_predicted.erase(p);
        return it->second;
    }
    Input guess = g_remote_last;
    guess.command_count = 0;   // a command is never guessed
    g_predicted[frame] = guess;
    ++g_predicted_frames;
    return guess;
}

bool confirmed(uint32_t frame) {   // every input up to and including this frame is real
    return g_remote.find(frame) != g_remote.end() &&
           (g_predicted.empty() || g_predicted.begin()->first > frame);
}

void latch(uint32_t frame, const Input& theirs) {
    const Input& mine = g_local[frame];
    latch_frame(frame, g_is_host ? mine : theirs, g_is_host ? theirs : mine);
}

// Fingerprints: of the kept snapshots whose frames are confirmed (every
// input up to them real), so both sides hash the same thing, and in
// lockstep of the live frame point. A frame run on a prediction is hashed
// later, from its snapshot, once its inputs are in, or not at all if it had
// to be run again (then its new snapshot is).
void send_hash(uint8_t* rdram, uint32_t f, uint64_t h) {
    (void)rdram;
    g_local_hash[f] = h;
    Writer w;
    w.put<uint8_t>(kHash);
    w.put<uint32_t>(g_epoch);
    w.put<uint32_t>(f);
    w.put<uint64_t>(h);
    send(1, w);
    if (g_log) {
        std::fprintf(g_log, "%u %u %016llx\n", g_epoch, f, static_cast<unsigned long long>(h));
        std::fflush(g_log);
    }
}

void fingerprint_confirmed(uint8_t* rdram, uint32_t f) {
    if (!g_rollback_mode) {
        if (f % kHashEvery == 0 && confirmed(f) && !g_local_hash.count(f)) send_hash(rdram, f, mm::replay::memory_fingerprint(rdram));
        return;
    }
    for (Kept& k : g_snaps) {
        if (k.hashed || k.frame % kHashEvery != 0 || !confirmed(k.frame)) continue;
        size_t size = 0;
        const uint8_t* image = mm::savestate::memory_image(*k.snap, &size);
        if (!image) continue;
        k.hashed = true;
        send_hash(rdram, k.frame, mm::replay::image_fingerprint(image, size, mm::savestate::memory_modules(*k.snap)));
    }
}

// A late input differed from its prediction: back to that frame's snapshot;
// the frames since run again from the next hook calls on.
bool begin_rollback(uint8_t* rdram, recomp_context* ctx, uint32_t to, uint32_t target) {
    const Kept* kept = nullptr;
    for (const Kept& k : g_snaps) {
        if (k.frame == to) kept = &k;
    }
    if (!kept) {
        std::fprintf(stderr, "[netplay] no snapshot of frame %u to roll back to\n", to);
        return false;
    }
    std::string error;
    if (!mm::savestate::apply_memory(rdram, ctx, *kept->snap, error)) {
        std::fprintf(stderr, "[netplay] rollback to frame %u failed: %s\n", to, error.c_str());
        return false;
    }
    g_seen_load_generation = mm::savestate::load_generation();   // our own restore, not a player's load
    ++g_rollbacks;
    g_rollback_frames += target - to;
    g_catching_up = true;
    g_catch_frame = to;
    g_catch_target = target;
    g_speed_before = ultramodern::get_speed_multiplier();
    ultramodern::set_speed_multiplier(1000);
    mm::presentation::set_live(false);
    g_frame = static_cast<long>(to);
    // The frames since: predictions again, from what is known now.
    for (auto it = g_predicted.lower_bound(to); it != g_predicted.end();) it = g_predicted.erase(it);
    g_rollback_to = UINT32_MAX;
    latch(to, partner_input(to));
    static const bool dbg = std::getenv("MM_NET_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[netplay] rollback: frame %u back to %u (%u frames again)\n", target, to, target - to);
    return true;
}

// One session frame: this side's input goes out, the partner's comes in.
void run_frame(uint8_t* rdram, recomp_context* ctx);
void run_frame(uint8_t* rdram, recomp_context* ctx) {
    if (g_catching_up) {
        // The game just ran frame g_catch_frame again.
        ++g_catch_frame;
        if (g_catch_frame < g_catch_target) {
            g_frame = static_cast<long>(g_catch_frame);
            keep_snapshot(rdram, ctx, g_catch_frame);
            latch(g_catch_frame, partner_input(g_catch_frame));
            return;
        }
        // Back where the rollback started: this hook call is that frame's.
        g_catching_up = false;
        ultramodern::set_speed_multiplier(g_speed_before);
        mm::presentation::set_live(true);
        g_frame = static_cast<long>(g_catch_target) - 1;
    }
    if (!g_restarted) ++g_frame;
    g_restarted = false;
    const uint32_t f = static_cast<uint32_t>(g_frame);

    // How far ahead to send, from the measured round trip.
    if (!g_delay_fixed) {
        int d = g_delay_ctl.update(g_delay);
        if (g_rollback_mode) d = std::min(d, 3);   // prediction covers the rest of the trip
        if (d != g_delay) {
            std::fprintf(stderr, "[netplay] delay now %d frames (round trip %.0f ms, worst %.0f ms)\n", d,
                         g_delay_ctl.rtt_ms(), g_delay_ctl.rtt_high_ms());
            g_delay = d;
        }
    }
    g_shown_delay.store(g_delay);

    // This side's input, `delay` frames ahead. When the delay grew, the
    // frames in between get the same input; when it shrank, nothing goes out
    // until the frames already sent are passed.
    for (uint32_t ahead = g_last_sent + 1; ahead <= f + static_cast<uint32_t>(g_delay); ++ahead) {
        const Input in = local_input(static_cast<long>(ahead));
        g_local[ahead] = in;
        Writer w;
        w.put<uint8_t>(kInput);
        w.put<uint32_t>(g_epoch);
        w.put<uint32_t>(ahead);
        write_input(w, in);
        send(1, w);
        g_last_sent = ahead;
    }

    // Four round-trip measurements a second, once the session is under way
    // (the first answers would wait behind the guest loading the start state).
    if (f >= 60 && f % 15 == 0) {
        Writer w;
        w.put<uint8_t>(kPing);
        w.put<uint64_t>(now_us());
        send(1, w);
    }

    // Test: MM_NET_TOGGLE=<cheat>@<frame>[,...] toggles a cheat in this side's
    // menu at those session frames (the guest's toggles are refused).
    static const std::vector<std::pair<int, long>> toggles = [] {
        std::vector<std::pair<int, long>> out;
        const char* v = std::getenv("MM_NET_TOGGLE");
        for (const char* p = v; p && *p;) {
            char* end;
            const int cheat = static_cast<int>(std::strtol(p, &end, 10));
            if (*end != '@') break;
            out.emplace_back(cheat, std::strtol(end + 1, &end, 10));
            p = *end == ',' ? end + 1 : end;
            if (*end != ',') break;
        }
        return out;
    }();
    for (const auto& [cheat, at] : toggles) {
        if (g_frame == at && cheat >= 0 && cheat < mm::cheats::kCheatCount)
            mm::cheats::toggle(static_cast<mm::cheats::Cheat>(cheat));
    }

    // Test: MM_NET_POKE=<frame> makes this side differ there (resync check).
    static const long poke_at = std::getenv("MM_NET_POKE") ? std::atol(std::getenv("MM_NET_POKE")) : -1;
    if (poke_at >= 0 && g_frame == poke_at && g_epoch == 1) {
        MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x80400000u))) ^= 0x5A5A5A5A;
        std::fprintf(stderr, "[netplay] test: game memory changed on this side\n");
    }

    // Test: MM_NET_DUMP=<frame> writes this side's game memory there (epoch 1).
    static const long dump_at = std::getenv("MM_NET_DUMP") ? std::atol(std::getenv("MM_NET_DUMP")) : -1;
    if (dump_at >= 0 && g_frame == dump_at && g_epoch == 1) {
        std::ofstream d(g_folder / ("dump_" + std::to_string(g_player.load()) + ".bin"), std::ios::binary);
        d.write(reinterpret_cast<const char*>(rdram), 0x840000);
    }

    flush();

    // A late input differed from its prediction: the frames since run again
    // before this one.
    if (g_rollback_to < f && g_rollback_mode) {
        if (begin_rollback(rdram, ctx, g_rollback_to, f)) return;
        g_rollback_to = UINT32_MAX;
    }
    if (g_sync_test && f > 2 && g_sync_tested != f - 1 && !g_snaps.empty() && g_snaps.back().frame == f - 1) {
        // Test: every frame runs twice; the second run must give the same game.
        g_sync_tested = f - 1;
        if (begin_rollback(rdram, ctx, f - 1, f)) return;
    }

    // The partner's input for this frame: wait for it in lockstep, or when
    // too many frames already rest on predictions.
    const auto started = std::chrono::steady_clock::now();
    const auto can_predict = [&] {
        if (!g_rollback_mode) return false;
        const uint32_t oldest = g_predicted.empty() ? f : g_predicted.begin()->first;
        return static_cast<long>(f) - static_cast<long>(oldest) < kMaxPredict && g_remote_last_frame > 0;
    };
    while (g_remote.find(f) == g_remote.end() && !can_predict()) {
        if (!service(rdram, ctx, 2)) return;   // session ended
        if (g_restarted) {                     // a resync was applied: run its frame 0 instead
            run_frame(rdram, ctx);
            return;
        }
        if (g_is_host && g_start_wanted && (g_frame >= 120 || g_reconnecting)) return;   // handled by the caller at once
        if (g_rollback_to < f && g_rollback_mode) {   // what came in asks for a rollback first
            if (begin_rollback(rdram, ctx, g_rollback_to, f)) return;
            g_rollback_to = UINT32_MAX;
        }
        const auto waited = std::chrono::steady_clock::now() - started;
        if (waited > std::chrono::milliseconds(300)) g_waiting.store(true);
        if (waited > std::chrono::seconds(60)) {
            end_session(g_reconnecting ? "the other player did not come back" : "the other player stopped responding");
            return;
        }
    }
    g_waiting.store(false);
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (ms > 1000.0 / 60.0) ++g_long_waits;
        g_longest_wait_ms = std::max(g_longest_wait_ms, ms);
    }

    if (g_rollback_mode) keep_snapshot(rdram, ctx, f);
    const Input theirs = partner_input(f);
    fingerprint_confirmed(rdram, f);
    latch(f, theirs);
    {
        const Input& host = g_is_host ? g_local[f] : theirs;
        static mm::cheats::SessionSettings s_last;
        if (f > 0 && std::memcmp(&host.settings, &s_last, sizeof s_last) != 0)
            std::fprintf(stderr, "[netplay] settings changed at frame %u (cheats %x, flags %x)\n", f,
                         host.settings.cheats,
                         host.settings.active | host.settings.hold_dash << 1 | host.settings.gem_hints << 2);
        s_last = host.settings;
    }
    {
        const uint32_t keep_from = f > static_cast<uint32_t>(kMaxPredict + 2) ? f - (kMaxPredict + 2) : 0;
        g_local.erase(g_local.begin(), g_local.lower_bound(keep_from));
        g_remote.erase(g_remote.begin(), g_remote.lower_bound(keep_from));
    }

    // Compare fingerprints both sides have.
    for (auto it = g_local_hash.begin(); it != g_local_hash.end();) {
        const auto other = g_remote_hash.find(it->first);
        if (other == g_remote_hash.end()) {
            ++it;
            continue;
        }
        if (other->second != it->second) {
            std::fprintf(stderr, "[netplay] game memory differs at frame %u\n", it->first);
            if (g_is_host) {
                g_start_wanted = true;
            } else {
                Writer w;
                w.put<uint8_t>(kDesync);
                w.put<uint32_t>(g_epoch);
                w.put<uint32_t>(it->first);
                send(0, w);
                flush();
            }
        }
        g_remote_hash.erase(other);
        it = g_local_hash.erase(it);
    }
    // Fingerprints the other side never took (it ran that frame on a
    // prediction) are dropped after a while.
    if (f > 600) {
        g_local_hash.erase(g_local_hash.begin(), g_local_hash.lower_bound(f - 600));
        g_remote_hash.erase(g_remote_hash.begin(), g_remote_hash.lower_bound(f - 600));
    }

    if (g_length >= 0 && g_frame >= g_length) end_session("length reached");
}

} // namespace

// ------------------------------------------------------------------ public

void configure(const std::filesystem::path& folder) {
    g_folder = folder;
    std::ifstream f(folder / "netplay.cfg");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("join=", 0) == 0) {
            std::lock_guard lock{g_mutex};
            g_last_address = line.substr(5);
        }
    }
}

void request_host(int port) {
    {
        std::lock_guard lock{g_mutex};
        g_req_port = port;
    }
    g_request.store(Req::Host);
}

void request_join(const std::string& address) {
    // "steam:<lobby id>" (or "steam") joins over Steam instead.
    if (address.rfind("steam", 0) == 0) {
        const size_t colon = address.find(':');
        request_join_steam(colon == std::string::npos ? 0 : std::strtoull(address.c_str() + colon + 1, nullptr, 10));
        return;
    }
    {
        std::lock_guard lock{g_mutex};
        g_req_address = address;
    }
    g_request.store(Req::Join);
}

void request_leave() { g_request.store(Req::Leave); }

void request_host_steam() { g_request.store(Req::HostSteam); }

void request_join_steam(uint64_t lobby) {
    {
        std::lock_guard lock{g_mutex};
        g_req_lobby = lobby;
    }
    g_request.store(Req::JoinSteam);
}

std::string last_address() {
    std::lock_guard lock{g_mutex};
    return g_last_address;
}

Status status() { return g_status.load(); }
bool active() { return g_active.load(std::memory_order_relaxed); }
bool is_guest() { return g_status.load() != Status::Off && g_player.load() == 2; }
int player() { return g_player.load(); }

std::string status_text() {
    std::lock_guard lock{g_mutex};
    return g_text;
}

void on_frame(uint8_t* rdram, recomp_context* ctx) {
    static bool env_read = false;
    static std::string env_host, env_join;
    static long host_at = -1, join_at = -1;
    long& global = g_global;
    ++global;
    if (g_start_at >= 0 && global >= g_start_at) {
        g_start_at = -1;
        g_start_wanted = true;
    }
    if (!env_read) {
        env_read = true;
        if (env_at("MM_NET_HOST", env_host, host_at) && host_at == 0) host_at = 1;
        if (env_at("MM_NET_JOIN", env_join, join_at) && join_at == 0) join_at = 1;
        if (env_on("MM_NET_LOCKSTEP")) g_rollback_mode = false;
        if (env_on("MM_NET_SYNC_TEST")) g_sync_test = true;
        if (const char* d = std::getenv("MM_NET_DELAY")) {
            g_delay = std::clamp(std::atoi(d), 1, 30);
            g_delay_fixed = true;
        }
        g_pad_script = std::getenv("MM_NET_PAD");   // applied to session frames only (begin_running)
        if (const char* l = std::getenv("MM_NET_LENGTH")) g_length = std::strtol(l, nullptr, 10);
    }
    if (global == host_at) {
        if (env_host == "steam") request_host_steam();
        else request_host(env_host.empty() ? kDefaultPort : std::atoi(env_host.c_str()));
    }
    if (global == join_at) request_join(env_join);
    // Steam: an invite accepted / Join Game picked while not in a game, or
    // +connect_lobby on the command line.
    steam_tick();
    if (g_status.load() == Status::Off && g_request.load() == Req::None && (global % 30) == 0) {
        if (const uint64_t l = steam_join_request()) request_join_steam(l);
    }

    switch (g_request.exchange(Req::None)) {
        case Req::Host:
            if (g_status.load() == Status::Off && mm::replay::mode() == mm::replay::Mode::Idle) {
                int port;
                {
                    std::lock_guard lock{g_mutex};
                    port = g_req_port;
                }
                start_hosting(port);
            }
            break;
        case Req::Join:
            if (g_status.load() == Status::Off && mm::replay::mode() == mm::replay::Mode::Idle) {
                std::string a;
                {
                    std::lock_guard lock{g_mutex};
                    a = g_req_address;
                }
                start_joining(a);
            }
            break;
        case Req::Leave:
            if (g_status.load() != Status::Off) end_session("left");
            break;
        case Req::HostSteam:
            if (g_status.load() == Status::Off && mm::replay::mode() == mm::replay::Mode::Idle) start_hosting_steam();
            break;
        case Req::JoinSteam:
            if (g_status.load() == Status::Off && mm::replay::mode() == mm::replay::Mode::Idle) {
                uint64_t l;
                {
                    std::lock_guard lock{g_mutex};
                    l = g_req_lobby;
                }
                start_joining_steam(l);
            }
            break;
        case Req::None: break;
    }
    if (g_status.load() == Status::Off) return;
    if (!g_log) {
        if (const char* path = std::getenv("MM_NET_LOG")) g_log = std::fopen(path, "a");
    }

    if (!g_active.load()) {
        // Connecting / waiting for a player.
        if (!service(rdram, ctx, 0)) return;
        // The host measures the trip while the guest connects, so the
        // session starts at the right delay.
        if (g_is_host && g_peer_ok && g_partner && (global % 15) == 0) {
            Writer w;
            w.put<uint8_t>(kPing);
            w.put<uint64_t>(now_us());
            send(1, w);
            flush();
        }
        if (g_is_host && g_start_wanted && g_peer_ok) {
            g_start_wanted = false;
            if (!g_delay_fixed && g_delay_ctl.wanted() > 0 && g_epoch == 0) g_delay = g_delay_ctl.wanted();
            if (!host_start(rdram, ctx)) end_session("could not take the start state");
            else {
                set_text("Netplay: you are player 1 (host)");
                run_frame(rdram, ctx);
            }
            return;
        }
        if (g_active.load()) {   // the guest just got the host's game
            set_text("Netplay: you are player 2");
            run_frame(rdram, ctx);
        }
        return;
    }

    // Running. The host resyncs after loading a state itself.
    if (g_is_host && mm::savestate::load_generation() != g_seen_load_generation) g_start_wanted = true;
    service(rdram, ctx, 0);
    if (!g_active.load()) return;
    // Test (MM_NET_TEST_DROP=<frame>, guest): the connection drops there once.
    static const long drop_at = std::getenv("MM_NET_TEST_DROP") ? std::atol(std::getenv("MM_NET_TEST_DROP")) : -1;
    static bool dropped = false;
    if (!g_is_host && !dropped && drop_at >= 0 && static_cast<long>(g_frame) >= drop_at && g_epoch == 1) {
        dropped = true;
        begin_reconnect("test: dropped the connection");
        if (!g_net) return;
    }
    // The guest is back after a drop: both start again from here.
    if (g_is_host && g_start_wanted && g_reconnecting && g_peer_ok) {
        g_start_wanted = false;
        host_start(rdram, ctx);
    }
    // At most one resync every two seconds (a state load is always honoured).
    const bool loaded = g_is_host && mm::savestate::load_generation() != g_seen_load_generation;
    if (g_is_host && g_start_wanted && (loaded || g_frame >= 120)) {
        g_start_wanted = false;
        resync(rdram, ctx, loaded ? "the host loaded a state" : "the games drifted apart");
    }
    run_frame(rdram, ctx);
    if (g_is_host && g_start_wanted && g_active.load() && g_reconnecting && g_peer_ok) {
        // The guest is back after a drop: both start again from here.
        g_start_wanted = false;
        host_start(rdram, ctx);
        run_frame(rdram, ctx);
    } else if (g_is_host && g_start_wanted && g_active.load() && g_frame >= 120) {
        // Requested while waiting: resync now (this frame point) instead.
        g_start_wanted = false;
        resync(rdram, ctx, "the games drifted apart");
        run_frame(rdram, ctx);
    }
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    if (g_status.load() == Status::Off) return;
    std::string text = status_text();
    if (g_waiting.load()) text = "Waiting for the other player...";
    else if (g_active.load()) {
        // Playing: the connection, small and out of the way.
        const int ping = g_shown_ping.load();
        char line[64];
        if (ping < 0) std::snprintf(line, sizeof line, "delay %d", g_shown_delay.load());
        else std::snprintf(line, sizeof line, "%d ms  delay %d", ping, g_shown_delay.load());
        // Bottom right: the rank timer has the top right.
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(size.x - 8.0f, size.y - 8.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.35f);
        constexpr ImGuiWindowFlags small =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##netplay_ping", nullptr, small)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.7f));
            ImGui::TextUnformatted(line);
            ImGui::PopStyleColor();
        }
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##netplay_status", nullptr, flags)) ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}
#endif

} // namespace mm::netplay
