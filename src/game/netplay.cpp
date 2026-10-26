#include "netplay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

#include <enet/enet.h>

#include "cheats.h"
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
constexpr uint32_t kProtocol = 1;
constexpr int kChannels = 2;               // 0 control (state), 1 input + fingerprints
constexpr int kHashEvery = 60;             // frames between fingerprint checks

enum Msg : uint8_t {
    kHello = 1,    // guest -> host: magic, protocol, build
    kStart = 2,    // host -> guest: epoch, delay, settings, eeprom, state
    kInput = 3,    // both: epoch, frame, buttons, x, y
    kHash = 4,     // both: epoch, frame, fingerprint
    kDesync = 5,   // guest -> host: epoch, frame (please resync)
    kRefuse = 6,   // host -> guest: reason text
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

// ------------------------------------------------------------------- state

struct Input {
    uint16_t buttons = 0;
    int8_t x = 0, y = 0;
};

enum class Req { None, Host, Join, Leave };

std::filesystem::path g_folder;
std::atomic<Status> g_status{Status::Off};
std::atomic<bool> g_active{false};
std::atomic<int> g_player{0};
std::atomic<Req> g_request{Req::None};
std::mutex g_mutex;                 // g_req_address, g_text, g_last_address
std::string g_req_address;
int g_req_port = kDefaultPort;
std::string g_text;
std::string g_last_address;
std::atomic<bool> g_waiting{false};  // stalled for the partner's input

// Game thread only from here on.
bool g_enet_ready = false;
ENetHost* g_host = nullptr;
ENetPeer* g_peer = nullptr;
bool g_is_host = false;
bool g_peer_ok = false;             // host: hello accepted; guest: connected
bool g_start_wanted = false;        // host: start (or resync) at this frame point
uint32_t g_epoch = 0;
int g_delay = 4;
long g_frame = 0;                   // session frame; inputs for it are in g_cur
std::map<uint32_t, Input> g_local, g_remote;
Input g_cur[2];                     // this frame: controller 0 (host), 1 (guest)
std::map<uint32_t, uint64_t> g_local_hash, g_remote_hash;
uint32_t g_seen_load_generation = 0;
int g_resyncs = 0;
bool g_sandboxed = false;
bool g_restarted = false;           // a start was applied during this frame point

// Widescreen environment, as the guest had it before following the host.
struct EnvSave { const char* name; std::string value; bool had; };
std::vector<EnvSave> g_env_saved;
constexpr const char* kWideEnv[] = {"MM_WIDESCREEN_ACTIVE", "MM_BAND_WINGS", "MM_STATIC_WINGS",
                                    "MM_ENV_WINGS", "MM_MID_WINGS"};

// Test scripting.
struct PadStep { long frames; uint16_t buttons; int8_t x, y; };
std::vector<PadStep> g_pad;
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

void parse_pad(const char* script) {
    g_pad.clear();
    for (const char* p = script; p && *p;) {
        PadStep step{0, 0, 0, 0};
        step.frames = std::strtol(p, const_cast<char**>(&p), 10);
        if (*p == ':') ++p;
        for (; *p && *p != ','; ++p) {
            switch (*p) {
                case 'A': step.buttons |= 0x8000; break;
                case 'B': step.buttons |= 0x4000; break;
                case 'Z': step.buttons |= 0x2000; break;
                case 'S': step.buttons |= 0x1000; break;
                case 'u': step.buttons |= 0x0800; break;
                case 'd': step.buttons |= 0x0400; break;
                case 'l': step.buttons |= 0x0200; break;
                case 'r': step.buttons |= 0x0100; break;
                case 'U': step.y = 127; break;
                case 'D': step.y = -127; break;
                case 'L': step.x = -127; break;
                case 'R': step.x = 127; break;
                default: break;
            }
        }
        if (*p == ',') ++p;
        g_pad.push_back(step);
    }
}

Input local_input(long frame) {
    Input in;
    if (!g_pad.empty()) {
        long at = 0;
        for (const PadStep& s : g_pad) {
            if (frame < at + s.frames) return Input{s.buttons, s.x, s.y};
            at += s.frames;
        }
        return in;
    }
    uint16_t b = 0;
    float x = 0, y = 0;
    if (mm_audio_input::sample_local_input(&b, &x, &y)) {
        in.buttons = b;
        in.x = static_cast<int8_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 127.0f));
        in.y = static_cast<int8_t>(std::lround(std::clamp(y, -1.0f, 1.0f) * 127.0f));
    }
    return in;
}

// The game's controller polls during a session: both players' input for this
// frame, whoever is local.
bool filter(int controller, uint16_t* buttons, float* x, float* y) {
    if (controller == 0 || controller == 1) {
        const Input& in = g_cur[controller];
        *buttons = in.buttons;
        *x = in.x / 127.0f;
        *y = in.y / 127.0f;
        return true;
    }
    *buttons = 0;
    *x = *y = 0.0f;
    return false;
}

void send(int channel, const Writer& w) {
    if (!g_peer) return;
    ENetPacket* p = enet_packet_create(w.b.data(), w.b.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(g_peer, static_cast<enet_uint8>(channel), p);
}

void flush() {
    if (g_host) enet_host_flush(g_host);
}

// ---------------------------------------------------------------- sessions

void reset_frames() {
    g_frame = 0;
    g_local.clear();
    g_remote.clear();
    g_local_hash.clear();
    g_remote_hash.clear();
    // The first `delay` frames have no input from either side yet: nothing pressed.
    for (int f = 0; f < g_delay; ++f) {
        g_local[static_cast<uint32_t>(f)] = Input{};
        g_remote[static_cast<uint32_t>(f)] = Input{};
    }
}

void begin_running() {
    reset_frames();
    mm_virtual_audio_clock(true);
    mm_audio_input::set_input_filter(filter);
    g_seen_load_generation = mm::savestate::load_generation();
    g_active.store(true);
    g_status.store(Status::Connected);
    g_restarted = true;
}

void end_session(const char* why) {
    const bool was_active = g_active.exchange(false);
    if (was_active) {
        mm_audio_input::set_input_filter(nullptr);
        mm_virtual_audio_clock(false);
        mm::cheats::set_session_settings(nullptr);
        restore_widescreen();
    }
    if (g_sandboxed) {
        recomp::end_save_sandbox();
        g_sandboxed = false;
    }
    if (g_peer) {
        enet_peer_disconnect_now(g_peer, 0);
        g_peer = nullptr;
    }
    if (g_host) {
        enet_host_flush(g_host);
        enet_host_destroy(g_host);
        g_host = nullptr;
    }
    g_peer_ok = false;
    g_start_wanted = false;
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
    // The host's settings as they were when the session began, all session
    // long (its menu changes wait until the session ends).
    static mm::cheats::SessionSettings s_settings;
    if (!g_active.load()) {
        s_settings = mm::cheats::capture_session_settings();
        // Online play is always widescreen: each player's own view moves the
        // picture sideways, and the widescreen wings (and wider activity
        // around the camera) keep the moved edges filled.
        s_settings.widescreen = 1;
    }
    mm::cheats::set_session_settings(&s_settings);
    follow_widescreen(true);

    ++g_epoch;
    Writer w;
    w.put<uint8_t>(kStart);
    w.put<uint32_t>(g_epoch);
    w.put<uint32_t>(static_cast<uint32_t>(g_delay));
    w.put(s_settings);
    w.bytes(eeprom.data(), eeprom.size());
    w.bytes(state.data(), state.size());
    send(0, w);
    flush();
    begin_running();
    std::fprintf(stderr, "[netplay] %s from the host's game (epoch %u, %zu KiB state)\n",
                 g_epoch == 1 ? "session started" : "resync", g_epoch, state.size() / 1024);
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
    mm::cheats::set_session_settings(&settings);
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
    if (epoch > 1 || g_active.load()) ++g_resyncs;
    begin_running();
    std::fprintf(stderr, "[netplay] %s (epoch %u, delay %d)\n", epoch == 1 ? "joined the host's game" : "resynced",
                 epoch, g_delay);
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
                enet_peer_disconnect_later(g_peer, 0);
                return;
            }
            g_peer_ok = true;
            g_start_wanted = true;
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
            if (!r.get(epoch) || !r.get(frame) || !r.get(in.buttons) || !r.get(in.x) || !r.get(in.y)) return;
            if (epoch != g_epoch || !g_active.load()) return;
            if (static_cast<long>(frame) >= g_frame) g_remote[frame] = in;
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

// Services the connection once; false when it is gone.
bool service(uint8_t* rdram, recomp_context* ctx, uint32_t timeout_ms) {
    if (!g_host) return false;
    ENetEvent ev;
    int got = enet_host_service(g_host, &ev, timeout_ms);
    while (got > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (g_is_host) {
                    if (g_peer && g_peer != ev.peer) {
                        enet_peer_disconnect_now(ev.peer, 0);   // one guest at a time
                        break;
                    }
                    g_peer = ev.peer;
                    set_text("A player is connecting...");
                } else {
                    g_peer = ev.peer;
                    Writer w;
                    w.put<uint8_t>(kHello);
                    w.put<uint32_t>(kMagic);
                    w.put<uint32_t>(kProtocol);
                    const std::string build = MM_GIT_COMMIT;
                    w.bytes(build.data(), build.size());
                    send(0, w);
                    flush();
                    g_peer_ok = true;
                    set_text("Connected, waiting for the host's game...");
                }
                enet_peer_timeout(ev.peer, 32, 10000, 30000);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                on_packet(rdram, ctx, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                if (!g_host) return false;   // the packet ended the session
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (ev.peer == g_peer) {
                    g_peer = nullptr;
                    if (g_is_host && !g_active.load()) {
                        g_peer_ok = false;
                        set_text("Hosting: waiting for a player");
                    } else {
                        end_session("the other player left");
                        return false;
                    }
                }
                break;
            default: break;
        }
        got = enet_host_service(g_host, &ev, 0);
    }
    return got >= 0;
}

bool ensure_enet() {
    if (!g_enet_ready) {
        if (enet_initialize() != 0) {
            set_text("Netplay: network start-up failed");
            return false;
        }
        g_enet_ready = true;
    }
    return true;
}

void start_hosting(int port) {
    if (!ensure_enet()) return;
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = static_cast<enet_uint16>(port);
    g_host = enet_host_create(&addr, 1, kChannels, 0, 0);
    if (!g_host) {
        set_text("Netplay: cannot listen on port " + std::to_string(port) + " (in use?)");
        return;
    }
    g_is_host = true;
    g_peer = nullptr;
    g_peer_ok = false;
    g_epoch = 0;
    g_resyncs = 0;
    g_player.store(1);
    g_status.store(Status::Hosting);
    set_text("Hosting on port " + std::to_string(port) + ": waiting for a player");
}

void start_joining(const std::string& address) {
    if (!ensure_enet()) return;
    std::string host = address;
    int port = kDefaultPort;
    if (const size_t colon = address.rfind(':'); colon != std::string::npos && address.find(':') == colon) {
        host = address.substr(0, colon);
        port = std::atoi(address.c_str() + colon + 1);
    }
    ENetAddress addr{};
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        set_text("Netplay: cannot find " + host);
        return;
    }
    addr.port = static_cast<enet_uint16>(port);
    g_host = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (!g_host) {
        set_text("Netplay: network start-up failed");
        return;
    }
    g_peer = enet_host_connect(g_host, &addr, kChannels, 0);
    if (!g_peer) {
        enet_host_destroy(g_host);
        g_host = nullptr;
        set_text("Netplay: cannot connect");
        return;
    }
    g_is_host = false;
    g_peer_ok = false;
    g_epoch = 0;
    g_resyncs = 0;
    g_player.store(2);
    g_status.store(Status::Joining);
    set_text("Joining " + host + ":" + std::to_string(port) + "...");
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

// One session frame: this side's input goes out, the partner's comes in.
void run_frame(uint8_t* rdram, recomp_context* ctx);
void run_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!g_restarted) ++g_frame;
    g_restarted = false;
    const uint32_t f = static_cast<uint32_t>(g_frame);

    // This side's input, `delay` frames ahead.
    {
        const uint32_t ahead = f + static_cast<uint32_t>(g_delay);
        const Input in = local_input(static_cast<long>(ahead));
        g_local[ahead] = in;
        Writer w;
        w.put<uint8_t>(kInput);
        w.put<uint32_t>(g_epoch);
        w.put<uint32_t>(ahead);
        w.put(in.buttons);
        w.put(in.x);
        w.put(in.y);
        send(1, w);
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

    // Once a second: do both machines still have the same game?
    if (f % kHashEvery == 0) {
        const uint64_t h = mm::replay::memory_fingerprint(rdram);
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
    flush();

    // The partner's input for this frame.
    const auto started = std::chrono::steady_clock::now();
    while (g_remote.find(f) == g_remote.end()) {
        if (!service(rdram, ctx, 2)) return;   // session ended
        if (g_restarted) {                     // a resync was applied: run its frame 0 instead
            run_frame(rdram, ctx);
            return;
        }
        if (g_is_host && g_start_wanted && g_frame >= 120) return;   // handled by the caller at once
        const auto waited = std::chrono::steady_clock::now() - started;
        if (waited > std::chrono::milliseconds(300)) g_waiting.store(true);
        if (waited > std::chrono::seconds(60)) {
            end_session("the other player stopped responding");
            return;
        }
    }
    g_waiting.store(false);

    const Input mine = g_local[f], theirs = g_remote[f];
    g_cur[g_is_host ? 0 : 1] = mine;
    g_cur[g_is_host ? 1 : 0] = theirs;
    g_local.erase(g_local.begin(), g_local.lower_bound(f));
    g_remote.erase(g_remote.begin(), g_remote.lower_bound(f));

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
    {
        std::lock_guard lock{g_mutex};
        g_req_address = address;
    }
    g_request.store(Req::Join);
}

void request_leave() { g_request.store(Req::Leave); }

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
    static long host_at = -1, join_at = -1, global = 0;
    ++global;
    if (!env_read) {
        env_read = true;
        if (env_at("MM_NET_HOST", env_host, host_at) && host_at == 0) host_at = 1;
        if (env_at("MM_NET_JOIN", env_join, join_at) && join_at == 0) join_at = 1;
        if (const char* d = std::getenv("MM_NET_DELAY")) g_delay = std::clamp(std::atoi(d), 1, 30);
        if (const char* p = std::getenv("MM_NET_PAD")) parse_pad(p);
        if (const char* l = std::getenv("MM_NET_LENGTH")) g_length = std::strtol(l, nullptr, 10);
    }
    if (global == host_at) request_host(env_host.empty() ? kDefaultPort : std::atoi(env_host.c_str()));
    if (global == join_at) request_join(env_join);

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
        case Req::None: break;
    }
    if (g_status.load() == Status::Off) return;
    if (!g_log) {
        if (const char* path = std::getenv("MM_NET_LOG")) g_log = std::fopen(path, "a");
    }

    if (!g_active.load()) {
        // Connecting / waiting for a player.
        if (!service(rdram, ctx, 0)) return;
        if (g_is_host && g_start_wanted && g_peer_ok) {
            g_start_wanted = false;
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
    // At most one resync every two seconds (a state load is always honoured).
    const bool loaded = g_is_host && mm::savestate::load_generation() != g_seen_load_generation;
    if (g_is_host && g_start_wanted && (loaded || g_frame >= 120)) {
        g_start_wanted = false;
        resync(rdram, ctx, loaded ? "the host loaded a state" : "the games drifted apart");
    }
    run_frame(rdram, ctx);
    if (g_is_host && g_start_wanted && g_active.load() && g_frame >= 120) {
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
    else if (g_active.load()) return;   // playing: nothing on screen
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
