#include "replay.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "mm_audio_input.hpp"
#include "savestate.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

extern "C" volatile uint32_t* mm_watch_ptr;
extern uint8_t dmem[];                // RSP DMEM (librecomp/src/rsp.cpp)
extern "C" void mm_virtual_audio_clock(bool on);
extern "C" void mm_virtual_audio_frame();

namespace mm::replay {
namespace {

// Game memory that is fingerprinted: the game's 8 MiB, the runtime data just
// above it and the audio ucode's state at 0x8083FF20 (what a save state
// holds, see savestate.cpp), in 64 KiB blocks.
constexpr uint32_t kHashBase = 0x80000000u;
constexpr uint32_t kHashSize = 0x00840000u;
constexpr uint32_t kBlock = 0x10000u;
constexpr int kBlocks = static_cast<int>(kHashSize / kBlock);

// Words the game fills from the host clock (osGetTime) for its own profiling
// and never reads back: left out of the fingerprint.
//   0x801374DC  time taken by func_800457C8 (GameState_Gameplay)
//   0x80182018  timer start (func_80083CCC, debug timing print)
constexpr std::array<uint32_t, 2> kClockWords{0x801374DCu, 0x80182018u};

// The two 320x240x16 framebuffers the main loop alternates between
// (Thread_MainProc: 0x801DA800 / 0x803DA800). What is in them depends on the
// renderer (RT64 writes pixels back, a headless build draws nothing), not on
// the game, so they are left out of the fingerprint.
struct Range { uint32_t start, end; };
constexpr std::array<Range, 2> kRendererRanges{{
    {0x801DA800u, 0x801DA800u + 320u * 240u * 2u},
    {0x803DA800u, 0x803DA800u + 320u * 240u * 2u},
}};

struct Poll {
    uint32_t frame;
    uint8_t controller;
    uint8_t connected;
    uint16_t buttons;
    float x;
    float y;
};
static_assert(sizeof(Poll) == 16);

std::filesystem::path g_folder;
std::atomic<Mode> g_mode{Mode::Idle};
std::atomic<long> g_frame{0};
std::atomic<long> g_first_mismatch{-1};

enum class Request { None, Record, PlayLatest, Stop };
std::atomic<Request> g_request{Request::None};

// Game thread only from here on.
std::filesystem::path g_dir;
std::vector<Poll> g_polls;           // recorded (Recording) or to answer (Playing)
size_t g_cursor = 0;
bool g_poll_mismatch_reported = false;
std::ofstream g_hash_out;
std::ofstream g_input_out;           // recording: polls as they happen
size_t g_recorded = 0;
std::vector<uint64_t> g_expected;    // recorded hashes while playing, kBlocks per frame
long g_length = -1;
std::vector<long> g_dumps;
long g_global_frame = 0;

// Synthetic pad script (recording only).
struct PadStep { long frames; uint16_t buttons; float x, y; };
std::vector<PadStep> g_pad;

constexpr uint16_t kA = 0x8000, kB = 0x4000, kZ = 0x2000, kStart = 0x1000;
constexpr uint16_t kDUp = 0x0800, kDDown = 0x0400, kDLeft = 0x0200, kDRight = 0x0100;

void parse_pad(const char* script) {
    g_pad.clear();
    for (const char* p = script; p && *p;) {
        PadStep step{0, 0, 0.0f, 0.0f};
        step.frames = std::strtol(p, const_cast<char**>(&p), 10);
        if (*p == ':') ++p;
        for (; *p && *p != ','; ++p) {
            switch (*p) {
                case 'A': step.buttons |= kA; break;
                case 'B': step.buttons |= kB; break;
                case 'Z': step.buttons |= kZ; break;
                case 'S': step.buttons |= kStart; break;
                case 'U': step.y = 1.0f; break;
                case 'D': step.y = -1.0f; break;
                case 'L': step.x = -1.0f; break;
                case 'R': step.x = 1.0f; break;
                case 'u': step.buttons |= kDUp; break;
                case 'd': step.buttons |= kDDown; break;
                case 'l': step.buttons |= kDLeft; break;
                case 'r': step.buttons |= kDRight; break;
                default: break;
            }
        }
        if (*p == ',') ++p;
        g_pad.push_back(step);
    }
}

bool pad_input(long frame, uint16_t* buttons, float* x, float* y) {
    long at = 0;
    for (const PadStep& s : g_pad) {
        if (frame < at + s.frames) {
            *buttons = s.buttons;
            *x = s.x;
            *y = s.y;
            return true;
        }
        at += s.frames;
    }
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
    return true;
}

bool filter(int controller, uint16_t* buttons, float* x, float* y) {
    const long frame = g_frame.load(std::memory_order_relaxed);
    const Mode mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Recording) {
        bool connected = controller == 0;
        if (controller == 0 && !g_pad.empty()) connected = pad_input(frame, buttons, x, y);
        if (!connected) {
            *buttons = 0;
            *x = *y = 0.0f;
        }
        const Poll poll{static_cast<uint32_t>(frame), static_cast<uint8_t>(controller),
                        static_cast<uint8_t>(connected ? 1 : 0), *buttons, *x, *y};
        g_input_out.write(reinterpret_cast<const char*>(&poll), sizeof poll);
        ++g_recorded;
        return connected;
    }
    if (mode == Mode::Playing) {
        if (g_cursor < g_polls.size()) {
            const Poll& p = g_polls[g_cursor++];
            if ((p.frame != static_cast<uint32_t>(frame) || p.controller != controller) &&
                !g_poll_mismatch_reported) {
                g_poll_mismatch_reported = true;
                std::fprintf(stderr,
                    "[replay] poll %zu: recorded at frame %u controller %u, now frame %ld controller %d\n",
                    g_cursor - 1, p.frame, p.controller, frame, controller);
            }
            *buttons = p.buttons;
            *x = p.x;
            *y = p.y;
            return p.connected != 0;
        }
        *buttons = 0;
        *x = *y = 0.0f;
        return controller == 0;
    }
    return controller == 0;
}

uint64_t hash_block(const uint8_t* p) {
    // 64-bit multiply-xor over 8-byte words; fast and plenty for spotting
    // differences.
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < kBlock; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

void fingerprint(uint8_t* rdram, std::array<uint64_t, kBlocks>& out) {
    // Masked while hashing: the clock words, and each game thread's host
    // context pointer (OSThread::context, 8 bytes; differs per process).
    std::vector<uint32_t> masked(kClockWords.begin(), kClockWords.end());
    for (uint32_t t : mm::savestate::game_thread_addresses()) {
        const uint32_t context = t + static_cast<uint32_t>(offsetof(OSThread, context));
        masked.push_back(context);
        masked.push_back(context + 4);
    }
    std::vector<uint32_t> saved(masked.size());
    for (size_t i = 0; i < masked.size(); ++i) {
        uint32_t* w = reinterpret_cast<uint32_t*>(rdram + (masked[i] - 0x80000000u));
        saved[i] = *w;
        *w = 0;
    }
    const uint8_t* base = rdram + (kHashBase - 0x80000000u);
    static std::vector<uint8_t> scratch(kBlock);
    for (int b = 0; b < kBlocks; ++b) {
        const uint32_t start = kHashBase + static_cast<uint32_t>(b) * kBlock, end = start + kBlock;
        const uint8_t* block = base + static_cast<size_t>(b) * kBlock;
        bool copied = false;
        for (const Range& r : kRendererRanges) {
            if (r.end <= start || r.start >= end) continue;
            if (!copied) {
                std::memcpy(scratch.data(), block, kBlock);
                copied = true;
            }
            const uint32_t from = std::max(r.start, start), to = std::min(r.end, end);
            std::memset(scratch.data() + (from - start), 0, to - from);
        }
        out[b] = hash_block(copied ? scratch.data() : block);
    }
    for (size_t i = 0; i < masked.size(); ++i) {
        *reinterpret_cast<uint32_t*>(rdram + (masked[i] - 0x80000000u)) = saved[i];
    }
}

void write_dump(uint8_t* rdram, const std::filesystem::path& dir, long frame) {
    std::ofstream f(dir / ("dump_" + std::to_string(frame) + ".bin"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(rdram + (kHashBase - 0x80000000u)), kHashSize);
    std::ofstream d(dir / ("dmem_" + std::to_string(frame) + ".bin"), std::ios::binary);
    d.write(reinterpret_cast<const char*>(dmem), 0x1000);   // RSP DMEM, for comparing too
}

std::filesystem::path out_dir() {
    return g_mode.load() == Mode::Playing ? g_dir / "play" : g_dir;
}

void stop(const char* why) {
    const Mode mode = g_mode.load();
    if (mode == Mode::Idle) return;
    g_input_out.close();
    g_hash_out.close();
    mm_virtual_audio_clock(false);
    ultramodern::set_deterministic_events(false);
    if (mode == Mode::Playing) recomp::end_save_sandbox();
    mm_audio_input::set_input_filter(nullptr);
    g_mode.store(Mode::Idle);
    std::fprintf(stderr, "[replay] %s stopped (%s) after %ld frames, %zu polls%s\n",
                 mode == Mode::Recording ? "recording" : "playback", why, g_frame.load(),
                 mode == Mode::Recording ? g_recorded : g_cursor,
                 mode == Mode::Playing
                     ? (g_first_mismatch.load() < 0 ? "; memory matched every frame"
                                                    : ("; memory first differed at frame " +
                                                       std::to_string(g_first_mismatch.load())).c_str())
                     : "");
    g_polls.clear();
    g_expected.clear();
}

bool start_recording(uint8_t* rdram, recomp_context* ctx, const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string error;
    // Host events from here on arrive at fixed points (see ultramodern.hpp);
    // switched on before the state is taken so none slips in between.
    ultramodern::set_deterministic_events(true);
    // Take the start state, then continue from it loaded back, exactly as a
    // playback (or a netplay peer) will: a running game carries a little
    // host-side state a load does not recreate (some leftover in the audio
    // RSP), and starting both sides from a load leaves no such difference.
    if (!mm::savestate::save_now(rdram, ctx, dir / "start.tmstate", error) ||
        !mm::savestate::load_now(rdram, ctx, dir / "start.tmstate", error)) {
        ultramodern::set_deterministic_events(false);
        std::fprintf(stderr, "[replay] cannot start recording: %s\n", error.c_str());
        return false;
    }
    {
        // The cartridge save as it is now: the game may read it during the
        // replay, so playback must see the same data.
        const std::vector<uint8_t> save = recomp::get_save_data();
        std::ofstream f(dir / "eeprom.bin", std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(save.data()), static_cast<std::streamsize>(save.size()));
    }
    g_dir = dir;
    g_polls.clear();
    g_cursor = 0;
    g_frame.store(0);
    g_first_mismatch.store(-1);
    g_hash_out.open(dir / "hashes.bin", std::ios::binary | std::ios::trunc);
    g_input_out.open(dir / "input.bin", std::ios::binary | std::ios::trunc);
    g_recorded = 0;
    if (const char* pad = std::getenv("MM_REPLAY_PAD")) parse_pad(pad);
    mm_virtual_audio_clock(true);
    g_mode.store(Mode::Recording);
    mm_audio_input::set_input_filter(filter);
    std::fprintf(stderr, "[replay] recording into %s\n", dir.string().c_str());
    return true;
}

bool start_playing(uint8_t* rdram, recomp_context* ctx, const std::filesystem::path& dir) {
    std::vector<Poll> polls;
    {
        std::ifstream f(dir / "input.bin", std::ios::binary | std::ios::ate);
        if (!f) {
            std::fprintf(stderr, "[replay] no input.bin in %s\n", dir.string().c_str());
            return false;
        }
        const auto size = static_cast<size_t>(f.tellg());
        polls.resize(size / sizeof(Poll));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(polls.data()), static_cast<std::streamsize>(polls.size() * sizeof(Poll)));
    }
    std::vector<uint64_t> expected;
    {
        std::ifstream f(dir / "hashes.bin", std::ios::binary | std::ios::ate);
        if (f) {
            const auto size = static_cast<size_t>(f.tellg());
            const size_t record = sizeof(uint32_t) + kBlocks * sizeof(uint64_t);
            f.seekg(0);
            for (size_t r = 0; r < size / record; ++r) {
                uint32_t frame;
                f.read(reinterpret_cast<char*>(&frame), sizeof frame);
                const size_t at = expected.size();
                expected.resize(at + kBlocks);
                f.read(reinterpret_cast<char*>(expected.data() + at), kBlocks * sizeof(uint64_t));
            }
        }
    }
    std::vector<uint8_t> save;
    {
        std::ifstream f(dir / "eeprom.bin", std::ios::binary);
        save.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::string error;
    ultramodern::set_deterministic_events(true);
    // The recording's cartridge save, kept in memory: the player's own save
    // file is not read or touched while the replay runs.
    recomp::begin_save_sandbox(save);
    if (!mm::savestate::load_now(rdram, ctx, dir / "start.tmstate", error)) {
        recomp::end_save_sandbox();
        ultramodern::set_deterministic_events(false);
        std::fprintf(stderr, "[replay] cannot play %s: %s\n", dir.string().c_str(), error.c_str());
        return false;
    }
    if (const char* w = std::getenv("MM_REPLAY_WATCH")) {   // debugging: gdb watch on a word after the load
        mm_watch_ptr = reinterpret_cast<volatile uint32_t*>(rdram + (std::strtoul(w, nullptr, 16) - 0x80000000u));
#ifdef SIGTRAP
        std::raise(SIGTRAP);
#endif
    }
    g_dir = dir;
    g_polls = std::move(polls);
    g_expected = std::move(expected);
    g_cursor = 0;
    g_poll_mismatch_reported = false;
    g_frame.store(0);
    g_first_mismatch.store(-1);
    std::error_code ec;
    std::filesystem::create_directories(dir / "play", ec);
    g_hash_out.open(dir / "play" / "hashes.bin", std::ios::binary | std::ios::trunc);
    mm_virtual_audio_clock(true);
    g_mode.store(Mode::Playing);
    mm_audio_input::set_input_filter(filter);
    std::fprintf(stderr, "[replay] playing %s (%zu polls)\n", dir.string().c_str(), g_polls.size());
    return true;
}

std::filesystem::path new_replay_dir() {
    const std::time_t now = std::time(nullptr);
    char name[32];
    std::strftime(name, sizeof name, "%Y%m%d-%H%M%S", std::localtime(&now));
    return g_folder / name;
}

std::filesystem::path latest_replay_dir() {
    std::filesystem::path best;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(g_folder, ec)) {
        if (e.is_directory() && std::filesystem::exists(e.path() / "input.bin") &&
            (best.empty() || e.path().filename() > best.filename())) {
            best = e.path();
        }
    }
    return best;
}

// "<dir>@<frame>" from the environment.
bool env_at(const char* name, std::filesystem::path& dir, long& at) {
    const char* v = std::getenv(name);
    if (v == nullptr) return false;
    std::string s(v);
    const size_t sep = s.rfind('@');
    if (sep == std::string::npos) return false;
    dir = s.substr(0, sep);
    at = std::strtol(s.c_str() + sep + 1, nullptr, 10);
    return true;
}

} // namespace

void configure(const std::filesystem::path& folder) {
    g_folder = folder;
}

void request_record() { g_request.store(Request::Record); }
void request_play_latest() { g_request.store(Request::PlayLatest); }
void request_stop() { g_request.store(Request::Stop); }

Mode mode() { return g_mode.load(std::memory_order_relaxed); }
long frame() { return g_frame.load(std::memory_order_relaxed); }
long first_mismatch() { return g_first_mismatch.load(std::memory_order_relaxed); }

void on_frame(uint8_t* rdram, recomp_context* ctx) {
    ++g_global_frame;
    static bool env_read = false;
    static std::filesystem::path env_record, env_play;
    static long record_at = -1, play_at = -1;
    if (!env_read) {
        env_read = true;
        env_at("MM_REPLAY_RECORD", env_record, record_at);
        env_at("MM_REPLAY_PLAY", env_play, play_at);
        if (const char* len = std::getenv("MM_REPLAY_LENGTH")) g_length = std::strtol(len, nullptr, 10);
        if (const char* speed = std::getenv("MM_REPLAY_SPEED")) {
            ultramodern::set_speed_multiplier(static_cast<uint32_t>(std::strtoul(speed, nullptr, 10)));
        }
        for (const char* p = std::getenv("MM_REPLAY_DUMP"); p && *p;) {
            g_dumps.push_back(std::strtol(p, const_cast<char**>(&p), 10));
            if (*p == ',') ++p;
            else break;
        }
    }

    // Advance the running replay past the frame that just ended.
    Mode mode = g_mode.load();
    if (mode != Mode::Idle) {
        const long next = g_frame.load() + 1;
        if (g_length >= 0 && next > g_length) {
            stop("length reached");
            mode = Mode::Idle;
        } else if (mode == Mode::Playing && g_cursor >= g_polls.size() &&
                   static_cast<size_t>(next) * kBlocks > g_expected.size() && !g_expected.empty()) {
            stop("end of recording");
            mode = Mode::Idle;
        } else {
            g_frame.store(next);
        }
    }

    // The virtual audio clock (replays, fast-forward) drains one frame here.
    mm_virtual_audio_frame();

    // Start / stop requests.
    switch (g_request.exchange(Request::None)) {
        case Request::Record:
            if (mode == Mode::Idle && !g_folder.empty()) start_recording(rdram, ctx, new_replay_dir());
            break;
        case Request::PlayLatest:
            if (mode == Mode::Idle) {
                const auto dir = latest_replay_dir();
                if (!dir.empty()) start_playing(rdram, ctx, dir);
            }
            break;
        case Request::Stop: stop("requested"); break;
        case Request::None: break;
    }
    if (g_mode.load() == Mode::Idle) {
        if (record_at >= 0 && g_global_frame == record_at) start_recording(rdram, ctx, env_record);
        if (play_at >= 0 && g_global_frame == play_at) start_playing(rdram, ctx, env_play);
    }
    mode = g_mode.load();
    // Fast-forward runs in deterministic event mode too: one VI per game
    // frame and no catch-up. Normally, when the game falls behind the VI
    // thread it runs extra audio updates per frame, and at several times
    // normal speed that exhausts its audio DMA buffers (the game's own
    // "dmaPtr" assertion in music.c) or corrupts the audio command list.
    const bool want_deterministic = mode != Mode::Idle || ultramodern::get_speed_multiplier() > 1;
    if (want_deterministic != ultramodern::deterministic_events()) {
        ultramodern::set_deterministic_events(want_deterministic);
    }
    if (mode == Mode::Idle) return;

    // Fingerprint the frame point the replay is now at.
    const long f = g_frame.load();
    std::array<uint64_t, kBlocks> hashes;
    fingerprint(rdram, hashes);
    const uint32_t f32 = static_cast<uint32_t>(f);
    g_hash_out.write(reinterpret_cast<const char*>(&f32), sizeof f32);
    g_hash_out.write(reinterpret_cast<const char*>(hashes.data()), sizeof hashes);
    if (mode == Mode::Playing && g_first_mismatch.load() < 0 &&
        static_cast<size_t>(f + 1) * kBlocks <= g_expected.size()) {
        const uint64_t* want = g_expected.data() + static_cast<size_t>(f) * kBlocks;
        for (int b = 0; b < kBlocks; ++b) {
            if (want[b] != hashes[b]) {
                g_first_mismatch.store(f);
                std::fprintf(stderr, "[replay] memory differs from the recording at frame %ld (first block %08X)\n",
                             f, kHashBase + static_cast<uint32_t>(b) * kBlock);
                break;
            }
        }
    }
    if (f % 60 == 0) {
        g_hash_out.flush();
        g_input_out.flush();
    }
    for (long d : g_dumps) {
        if (d == f) write_dump(rdram, out_dir(), f);
    }
}

} // namespace mm::replay

// Translated Thread_MainProc, right before it blocks on its VI queue
// (troublemakers.us1.toml): in deterministic event mode, this frame's VI
// message, at its real time.
extern "C" void mm_deterministic_vi(uint8_t* rdram) {
    ultramodern::deterministic_frame(rdram);
}
