#include "savestate.h"
#include "audio_clock.h"
#include "netplay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "miniz.h"

#include "mm_audio_input.hpp"
#include "cheats.h"
#include "replay.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/overlays.hpp"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#include "mm_graphics.h"
#endif

// Widescreen host state that must travel with a state (widescreen.cpp).
extern "C" uint32_t mm_ws_get_host_bank_end();
extern "C" void mm_ws_set_host_bank_end(uint32_t value);
extern "C" void mm_ws_after_state_load();

extern uint8_t dmem[];               // RSP DMEM (librecomp/src/rsp.cpp)

namespace mm::savestate {
namespace {

constexpr uint32_t kDmemSize = 0x1000;

// ---------------------------------------------------------------------------
// What a state contains
// ---------------------------------------------------------------------------

// Every RDRAM range the game or this port can write. Anything outside is
// never touched after boot (the runtime commits 512 MiB, but only these
// pages are used).
struct RegionSpec {
    uint32_t vaddr;
    uint32_t size;
};
constexpr std::array<RegionSpec, 3> kRegions{{
    // 8 MiB (game + expansion-RAM host arenas, see docs/README.md 14.1) plus
    // the runtime's PI handles and patch data just above it.
    { 0x80000000u, 0x00810000u },
    // Widescreen scratch lists and grids (0x9FFFA000..0x9FFFFFFF).
    { 0x9FFF0000u, 0x00010000u },
    // (version 3) The audio ucode keeps 16 bytes of state at 0x8083FF20 (the
    // game hands it an address past the 8 MiB) and reads them back next task.
    { 0x8083F000u, 0x00001000u },
}};
// Regions in states of version 1 and 2.
constexpr size_t kRegionsV2 = 2;

constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kCurrentScene = 0x800BE5D0u;
constexpr uint32_t kCurrentStage = 0x80178162u;

constexpr char kMagic[8] = {'T', 'M', 'S', 'T', 'A', 'T', 'E', '\0'};
// 1: original. 2: adds the renderer's persistent RDP state (version 1 states
// still load; they simply keep whatever RDP state the session has).
// 3: adds RSP DMEM: each task reloads only 0x000-0xF7F, so what is left in
// 0xF80-0xFFF carries into the next audio task, and replays must restore it.
// 4: adds the virtual audio clock (audio_clock.h): put back, a state's frames
// run again read the audio level they read the first time (rollback).
constexpr uint32_t kFormatVersion = 4;
constexpr const char* kExtension = ".tmstate";

struct Snapshot {
    std::vector<std::vector<uint8_t>> regions; // parallel to kRegions
    std::array<uint64_t, 32> gpr{};
    std::array<uint64_t, 32> fpr{};
    uint64_t hi = 0;
    uint64_t lo = 0;
    uint32_t status_reg = 0;
    uint8_t mips3_float_mode = 0;
    int32_t thread = 0;          // OSThread that took the state
    std::vector<recomp::overlays::LoadedSectionRecord> overlays;
    std::vector<ultramodern::PendingExternalMessage> deferred;
    uint32_t ws_bank_end = 0;
    uint16_t game_state = 0;
    int16_t scene = 0;
    uint16_t stage = 0;
    int64_t saved_at = 0;        // unix time, informational
    std::vector<uint8_t> rdp;    // mm::graphics RDP state; empty if none
    std::vector<uint8_t> dmem;   // RSP DMEM (version 3); empty if none
    mm_audio_clock_state audio_clock{};   // (version 4) the virtual audio clock, when it ran
    uint8_t has_audio_clock = 0;
};

// ---------------------------------------------------------------------------
// Serialization (native little-endian; states are not meant to be portable
// between CPU architectures)
// ---------------------------------------------------------------------------

class Writer {
public:
    template <typename T> void put(const T& v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void bytes(const std::vector<uint8_t>& b) {
        put<uint32_t>(static_cast<uint32_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
    }
    std::vector<uint8_t> out;
};

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& in) : in_(in) {}
    template <typename T> bool get(T& v) {
        if (pos_ + sizeof(T) > in_.size()) return false;
        std::memcpy(&v, in_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool bytes(std::vector<uint8_t>& b, uint32_t expected) {
        uint32_t n = 0;
        if (!get(n) || n != expected || pos_ + n > in_.size()) return false;
        b.assign(in_.begin() + pos_, in_.begin() + pos_ + n);
        pos_ += n;
        return true;
    }
    bool bytes_raw(std::vector<uint8_t>& b, uint32_t n) {
        if (pos_ + n > in_.size()) return false;
        b.assign(in_.begin() + pos_, in_.begin() + pos_ + n);
        pos_ += n;
        return true;
    }
    bool done() const { return pos_ == in_.size(); }
private:
    const std::vector<uint8_t>& in_;
    size_t pos_ = 0;
};

uint64_t fnv1a(const std::vector<uint8_t>& data) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : data) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    return h;
}

std::vector<uint8_t> serialize(const Snapshot& s) {
    Writer w;
    w.put<uint32_t>(static_cast<uint32_t>(kRegions.size()));
    for (size_t i = 0; i < kRegions.size(); ++i) {
        w.put(kRegions[i].vaddr);
        w.bytes(s.regions[i]);
    }
    for (uint64_t r : s.gpr) w.put(r);
    for (uint64_t f : s.fpr) w.put(f);
    w.put(s.hi);
    w.put(s.lo);
    w.put(s.status_reg);
    w.put(s.mips3_float_mode);
    w.put(s.thread);
    w.put<uint32_t>(static_cast<uint32_t>(s.overlays.size()));
    for (const auto& o : s.overlays) {
        w.put(o.rom_addr);
        w.put(o.ram_addr);
    }
    w.put<uint32_t>(static_cast<uint32_t>(s.deferred.size()));
    for (const auto& m : s.deferred) {
        w.put<int32_t>(m.mq);
        w.put<int32_t>(m.mesg);
        w.put<uint8_t>(m.jam ? 1 : 0);
        w.put<uint8_t>(m.requeue_if_blocked ? 1 : 0);
    }
    w.put(s.ws_bank_end);
    w.put(s.game_state);
    w.put(s.scene);
    w.put(s.stage);
    w.put(s.saved_at);
    w.put<uint32_t>(static_cast<uint32_t>(s.rdp.size()));
    w.out.insert(w.out.end(), s.rdp.begin(), s.rdp.end());
    w.put<uint32_t>(static_cast<uint32_t>(s.dmem.size()));
    w.out.insert(w.out.end(), s.dmem.begin(), s.dmem.end());
    w.put(s.has_audio_clock);
    w.put(s.audio_clock.submitted);
    w.put(s.audio_clock.drained);
    w.put(s.audio_clock.remainder);
    w.put(s.audio_clock.ai_len);
    return std::move(w.out);
}

bool deserialize(const std::vector<uint8_t>& payload, Snapshot& s, uint32_t version) {
    Reader r(payload);
    uint32_t region_count = 0;
    if (!r.get(region_count) || region_count != (version >= 3 ? kRegions.size() : kRegionsV2)) return false;
    s.regions.resize(region_count);
    for (size_t i = 0; i < region_count; ++i) {
        uint32_t vaddr = 0;
        if (!r.get(vaddr) || vaddr != kRegions[i].vaddr) return false;
        if (!r.bytes(s.regions[i], kRegions[i].size)) return false;
    }
    for (uint64_t& v : s.gpr) if (!r.get(v)) return false;
    for (uint64_t& v : s.fpr) if (!r.get(v)) return false;
    uint32_t count = 0;
    if (!r.get(s.hi) || !r.get(s.lo) || !r.get(s.status_reg) ||
        !r.get(s.mips3_float_mode) || !r.get(s.thread) || !r.get(count) ||
        count > 4096) {
        return false;
    }
    s.overlays.resize(count);
    for (auto& o : s.overlays) {
        if (!r.get(o.rom_addr) || !r.get(o.ram_addr)) return false;
    }
    if (!r.get(count) || count > 4096) return false;
    s.deferred.resize(count);
    for (auto& m : s.deferred) {
        int32_t mq = 0, mesg = 0;
        uint8_t jam = 0, requeue = 0;
        if (!r.get(mq) || !r.get(mesg) || !r.get(jam) || !r.get(requeue)) return false;
        m = {mq, mesg, jam != 0, requeue != 0};
    }
    if (!(r.get(s.ws_bank_end) && r.get(s.game_state) && r.get(s.scene) &&
          r.get(s.stage) && r.get(s.saved_at))) {
        return false;
    }
    if (version >= 2) {
        uint32_t n = 0;
        if (!r.get(n) || n > (1u << 20) || !r.bytes_raw(s.rdp, n)) return false;
    }
    if (version >= 3) {
        uint32_t n = 0;
        if (!r.get(n) || n > kDmemSize || !r.bytes_raw(s.dmem, n)) return false;
    }
    if (version >= 4) {
        if (!(r.get(s.has_audio_clock) && r.get(s.audio_clock.submitted) && r.get(s.audio_clock.drained) &&
              r.get(s.audio_clock.remainder) && r.get(s.audio_clock.ai_len)))
            return false;
    }
    return r.done();
}

// File: magic[8] u32 version u64 payload_size u64 payload_fnv1a
//       u64 compressed_size, then zlib(payload).
bool write_file(const std::filesystem::path& path, const Snapshot& s, std::string& error) {
    const std::vector<uint8_t> payload = serialize(s);
    mz_ulong compressed_size = mz_compressBound(static_cast<mz_ulong>(payload.size()));
    std::vector<uint8_t> compressed(compressed_size);
    if (mz_compress2(compressed.data(), &compressed_size, payload.data(),
                     static_cast<mz_ulong>(payload.size()), MZ_BEST_SPEED) != MZ_OK) {
        error = "compression failed";
        return false;
    }
    compressed.resize(compressed_size);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot open " + temporary.string();
            return false;
        }
        const uint64_t payload_size = payload.size();
        const uint64_t hash = fnv1a(payload);
        const uint64_t csize = compressed.size();
        f.write(kMagic, sizeof kMagic);
        f.write(reinterpret_cast<const char*>(&kFormatVersion), sizeof kFormatVersion);
        f.write(reinterpret_cast<const char*>(&payload_size), sizeof payload_size);
        f.write(reinterpret_cast<const char*>(&hash), sizeof hash);
        f.write(reinterpret_cast<const char*>(&csize), sizeof csize);
        f.write(reinterpret_cast<const char*>(compressed.data()),
                static_cast<std::streamsize>(compressed.size()));
        if (!f.good()) {
            error = "write failed";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        // Windows cannot rename over an existing file on every filesystem.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool read_file(const std::filesystem::path& path, Snapshot& s, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "empty";
        return false;
    }
    char magic[8]{};
    uint32_t version = 0;
    uint64_t payload_size = 0, hash = 0, csize = 0;
    f.read(magic, sizeof magic);
    f.read(reinterpret_cast<char*>(&version), sizeof version);
    f.read(reinterpret_cast<char*>(&payload_size), sizeof payload_size);
    f.read(reinterpret_cast<char*>(&hash), sizeof hash);
    f.read(reinterpret_cast<char*>(&csize), sizeof csize);
    if (!f.good() || std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
        error = "not a Trouble Makers save state";
        return false;
    }
    if (version < 1 || version > kFormatVersion) {
        error = "made by a different version";
        return false;
    }
    if (payload_size > (64u << 20) || csize > (64u << 20)) {
        error = "corrupt";
        return false;
    }
    std::vector<uint8_t> compressed(csize);
    f.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(csize));
    if (!f.good()) {
        error = "truncated";
        return false;
    }
    std::vector<uint8_t> payload(payload_size);
    mz_ulong out_size = static_cast<mz_ulong>(payload_size);
    if (mz_uncompress(payload.data(), &out_size, compressed.data(),
                      static_cast<mz_ulong>(csize)) != MZ_OK ||
        out_size != payload_size || fnv1a(payload) != hash) {
        error = "corrupt";
        return false;
    }
    if (!deserialize(payload, s, version)) {
        error = "corrupt";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Capture / apply (game thread only)
// ---------------------------------------------------------------------------

std::mutex g_threads_mutex;
std::vector<int32_t> g_game_threads;

uint8_t* rdram_at(uint8_t* rdram, uint32_t vaddr) {
    return rdram + (vaddr - 0x80000000u);
}

// Brings the host side to a point where RDRAM is the whole truth: no RSP or
// RDP work is outstanding and every completion/VI/SI event has been put in
// its N64 queue. Save and load both do this, so the two sides of a load see
// the same kind of moment.
bool quiesce(uint8_t* rdram, std::string& error) {
    if (!ultramodern::wait_for_rsp_idle(std::chrono::milliseconds(1000))) {
        error = "the RSP did not go idle";
        return false;
    }
    ultramodern::deliver_pending_external_messages(rdram);
    return true;
}

bool capture(uint8_t* rdram, recomp_context* ctx, Snapshot& s, std::string& error) {
    if (!ultramodern::wait_for_rsp_idle(std::chrono::milliseconds(1000))) {
        error = "the RSP did not go idle";
        return false;
    }
    // Host events still in flight go into the deferred list, which the state
    // keeps, rather than to the game now: a capture must not change when the
    // game sees them (a state is taken every frame for rollback).
    ultramodern::park_pending_external_messages();

    s.regions.resize(kRegions.size());
    for (size_t i = 0; i < kRegions.size(); ++i) {
        const uint8_t* src = rdram_at(rdram, kRegions[i].vaddr);
        s.regions[i].assign(src, src + kRegions[i].size);
    }
    const gpr* regs = &ctx->r0;
    for (int i = 0; i < 32; ++i) s.gpr[i] = static_cast<uint64_t>(regs[i]);
    const fpr* fregs = &ctx->f0;
    for (int i = 0; i < 32; ++i) s.fpr[i] = fregs[i].u64;
    s.hi = ctx->hi;
    s.lo = ctx->lo;
    s.status_reg = ctx->status_reg;
    s.mips3_float_mode = ctx->mips3_float_mode;
    s.thread = ultramodern::this_thread();
    s.overlays = recomp::overlays::get_loaded_sections();
    s.deferred = ultramodern::get_deferred_external_messages();
    s.ws_bank_end = mm_ws_get_host_bank_end();
    s.game_state = MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kGameState)));
    s.scene = static_cast<int16_t>(MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene))));
    s.stage = MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCurrentStage)));
    s.saved_at = static_cast<int64_t>(std::time(nullptr));
#ifdef MM_HAS_GRAPHICS
    s.rdp = mm::graphics::capture_rdp_state();
#endif
    s.dmem.assign(dmem, dmem + kDmemSize);
    mm_virtual_audio_get(&s.audio_clock);
    s.has_audio_clock = 1;
    return true;
}

std::atomic<uint32_t> g_load_generation{0};

bool apply(uint8_t* rdram, recomp_context* ctx, const Snapshot& s, std::string& error) {
    g_load_generation.fetch_add(1, std::memory_order_relaxed);
    // A state is only valid at the exact frame point it was taken from: the
    // same thread, at the same stack depth. Anything else would resume
    // translated code with a host call stack that does not match RDRAM.
    if (s.thread != ultramodern::this_thread() ||
        static_cast<uint64_t>(ctx->r29) != s.gpr[29]) {
        error = "it was taken at a different point in the game loop";
        return false;
    }
    if (!quiesce(rdram, error)) return false;

    // OSThread::context is a host pointer that lives inside RDRAM. Keep the
    // live ones; the saved values belong to a previous run or moment.
    std::vector<std::pair<int32_t, UltraThreadContext*>> contexts;
    {
        std::lock_guard lock{g_threads_mutex};
        for (int32_t t : g_game_threads) {
            contexts.emplace_back(t, TO_PTR(OSThread, t)->context);
        }
    }

    for (size_t i = 0; i < s.regions.size(); ++i) {   // older states: fewer regions
        std::memcpy(rdram_at(rdram, kRegions[i].vaddr), s.regions[i].data(), kRegions[i].size);
    }
    for (const auto& [t, context] : contexts) {
        TO_PTR(OSThread, t)->context = context;
    }

    gpr* regs = &ctx->r0;
    for (int i = 0; i < 32; ++i) regs[i] = static_cast<gpr>(s.gpr[i]);
    fpr* fregs = &ctx->f0;
    for (int i = 0; i < 32; ++i) fregs[i].u64 = s.fpr[i];
    ctx->hi = s.hi;
    ctx->lo = s.lo;
    ctx->status_reg = s.status_reg;
    ctx->mips3_float_mode = s.mips3_float_mode;
    // f_odd is a host pointer into ctx itself and stays as is.

    if (!recomp::overlays::restore_loaded_sections(s.overlays)) {
        // RDRAM is already replaced; the overlay table names code this build
        // does not have. Continuing would jump into unmapped functions.
        std::fprintf(stderr, "[savestate] fatal: state references unknown code sections\n");
        std::abort();
    }
    ultramodern::set_deferred_external_messages(s.deferred);
    if (s.dmem.size() == kDmemSize) std::memcpy(dmem, s.dmem.data(), kDmemSize);
    if (s.has_audio_clock) mm_virtual_audio_set(&s.audio_clock);
    mm_ws_set_host_bank_end(s.ws_bank_end);
    mm_ws_after_state_load();
#ifdef MM_HAS_GRAPHICS
    if (!s.rdp.empty() && !mm::graphics::restore_rdp_state(s.rdp)) {
        std::fprintf(stderr, "[savestate] renderer state not restored (different renderer build)\n");
    }
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Requests, file I/O worker, notifications
// ---------------------------------------------------------------------------

std::filesystem::path g_folder;
std::atomic<int> g_slot{0};
std::atomic<bool> g_busy{false};

enum class Op { None, Save, Load };
std::atomic<int> g_pending_op{static_cast<int>(Op::None)};
std::atomic<int> g_pending_slot{0};
std::mutex g_load_mutex;
std::unique_ptr<Snapshot> g_load_snapshot; // read ahead by the worker

std::mutex g_toast_mutex;
std::string g_toast;
std::chrono::steady_clock::time_point g_toast_until{};

void toast(const std::string& text) {
    std::fprintf(stderr, "[savestate] %s\n", text.c_str());
    std::lock_guard lock{g_toast_mutex};
    g_toast = text;
    g_toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
}

std::filesystem::path slot_path(int slot) {
    return g_folder / ("slot" + std::to_string(slot) + kExtension);
}

std::string describe(const Snapshot& s) {
    char buf[64];
    if (s.game_state == 6) {
        std::snprintf(buf, sizeof buf, " (stage row %u)", s.stage);
        return buf;
    }
    return "";
}

void request_save(int slot) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    g_pending_slot.store(slot);
    g_pending_op.store(static_cast<int>(Op::Save));
}

void request_load(int slot) {
    // Online the host's game is the one both machines play: the guest
    // cannot load (the host can; netplay then resyncs the guest to it).
    if (mm::netplay::is_guest()) {
        toast("Only the host can load a state while playing online");
        return;
    }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    // Read and decompress off the game and event threads; the game thread only
    // copies memory, at its next frame point.
    std::thread([slot] {
        auto snapshot = std::make_unique<Snapshot>();
        std::string error;
        if (!read_file(slot_path(slot), *snapshot, error)) {
            toast("Slot " + std::to_string(slot) +
                  (error == "empty" ? " is empty" : ": cannot load, " + error));
            g_busy.store(false);
            return;
        }
        {
            std::lock_guard lock{g_load_mutex};
            g_load_snapshot = std::move(snapshot);
        }
        g_pending_slot.store(slot);
        g_pending_op.store(static_cast<int>(Op::Load));
    }).detach();
}

// MM_TEST_SAVESTATE="save@600,load@900,load@1500" drives saves/loads by frame
// number for the headless harness. MM_TEST_SAVESTATE_SLOT picks the slot (9).
struct TestStep { bool save; long frame; };
std::vector<TestStep> g_test_steps;
size_t g_test_index = 0;
long g_frame = 0;

void parse_test_script() {
    const char* script = std::getenv("MM_TEST_SAVESTATE");
    if (script == nullptr) return;
    std::string s(script);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        const size_t at = item.find('@');
        if (at != std::string::npos) {
            g_test_steps.push_back({item.substr(0, at) == "save",
                                    std::strtol(item.c_str() + at + 1, nullptr, 10)});
        }
        pos = end + 1;
    }
}

void run_test_step(uint8_t* rdram, recomp_context* ctx) {
    if (g_test_index >= g_test_steps.size() ||
        g_frame != g_test_steps[g_test_index].frame) {
        return;
    }
    const TestStep step = g_test_steps[g_test_index++];
    const char* slot_env = std::getenv("MM_TEST_SAVESTATE_SLOT");
    const int slot = slot_env ? std::atoi(slot_env) : 9;
    Snapshot snapshot;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    bool ok;
    if (step.save) {
        ok = capture(rdram, ctx, snapshot, error) &&
             write_file(slot_path(slot), snapshot, error);
    } else {
        ok = read_file(slot_path(slot), snapshot, error) &&
             apply(rdram, ctx, snapshot, error);
    }
    const long us = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
    std::fprintf(stderr, "[savestate-test] frame %ld %s slot %d: %s (%ld us) state=%u scene=%d stage=%u\n",
        g_frame, step.save ? "save" : "load", slot, ok ? "ok" : error.c_str(), us,
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kGameState))),
        static_cast<int16_t>(MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene)))),
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCurrentStage))));
}

} // namespace

void configure(const std::filesystem::path& folder) {
    g_folder = folder;
    parse_test_script();
}

namespace {

void change_slot(int delta) {
    const int next = (g_slot.load() + delta + kSlotCount) % kSlotCount;
    g_slot.store(next);
    std::error_code ec;
    const bool used = std::filesystem::exists(slot_path(next), ec);
    toast("Slot " + std::to_string(next) + (used ? "" : " (empty)"));
}

void run_action(mm_audio_input::N64Input action) {
    using mm_audio_input::N64Input;
    switch (action) {
        case N64Input::SaveState: request_save(g_slot.load()); break;
        case N64Input::LoadState: request_load(g_slot.load()); break;
        case N64Input::NextStateSlot: change_slot(1); break;
        case N64Input::PreviousStateSlot: change_slot(-1); break;
        default: break;
    }
}

// Finds the save-state action (if any) bound to this exact input.
bool action_for(mm_audio_input::ControlDevice device, mm_audio_input::InputBinding binding,
                mm_audio_input::N64Input& action) {
    using namespace mm_audio_input;
    for (size_t i = static_cast<size_t>(kFirstHostAction); i < kN64InputCount; ++i) {
        for (size_t slot = 0; slot < kBindingsPerInput; ++slot) {
            if (get_binding(device, static_cast<N64Input>(i), slot) == binding) {
                action = static_cast<N64Input>(i);
                return true;
            }
        }
    }
    return false;
}

// Axis bindings (triggers, stick directions) fire once when they cross this
// threshold, and again only after returning below the release point.
constexpr int kAxisPress = 20000;
constexpr int kAxisRelease = 12000;
std::array<int, SDL_CONTROLLER_AXIS_MAX> g_axis_direction{}; // -1, 0, +1 held

} // namespace

// Fast-forward runs while its input is held; remember which one started it.
mm_audio_input::InputBinding g_ff_binding{};

void set_fast_forward(bool on, mm_audio_input::InputBinding binding) {
    g_ff_binding = on ? binding : mm_audio_input::InputBinding{};
    ultramodern::set_speed_multiplier(on ? mm::cheats::fast_forward_speed() : 1);
}

bool handle_release(const mm_audio_input::InputBinding& binding) {
    if (g_ff_binding.type == mm_audio_input::BindingType::None || !(g_ff_binding == binding)) {
        return false;
    }
    set_fast_forward(false, {});
    return true;
}

bool handle_event(const SDL_Event& event) {
    using namespace mm_audio_input;
    InputBinding binding{};
    ControlDevice device = ControlDevice::Keyboard;

    if (event.type == SDL_KEYUP) {
        return handle_release({BindingType::Keyboard, static_cast<int>(event.key.keysym.scancode)});
    }
    if (event.type == SDL_CONTROLLERBUTTONUP) {
        return handle_release({BindingType::ControllerButton, static_cast<int>(event.cbutton.button)});
    }
    if (event.type == SDL_KEYDOWN) {
        if (event.key.repeat != 0) return false;
        binding = {BindingType::Keyboard, static_cast<int>(event.key.keysym.scancode)};
    } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        device = ControlDevice::Controller;
        binding = {BindingType::ControllerButton, static_cast<int>(event.cbutton.button)};
    } else if (event.type == SDL_CONTROLLERAXISMOTION) {
        const int axis = event.caxis.axis;
        if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return false;
        const int value = event.caxis.value;
        int& held = g_axis_direction[axis];
        if (held != 0) {
            if (value * held < kAxisRelease) {
                const InputBinding released{BindingType::ControllerAxis, (axis + 1) * held};
                held = 0;
                return handle_release(released);
            }
            return false;
        }
        if (value >= kAxisPress) held = 1;
        else if (value <= -kAxisPress) held = -1;
        else return false;
        device = ControlDevice::Controller;
        binding = {BindingType::ControllerAxis, (axis + 1) * held};
    } else {
        return false;
    }

    N64Input action{};
    if (!action_for(device, binding, action)) return false;
    if (action == N64Input::FastForward) {
        set_fast_forward(true, binding);
    } else {
        run_action(action);
    }
    return true;
}

bool save_now(unsigned char* rdram, recomp_context* ctx, const std::filesystem::path& path, std::string& error) {
    Snapshot snapshot;
    return capture(rdram, ctx, snapshot, error) && write_file(path, snapshot, error);
}

struct MemorySnapshot {
    Snapshot s;
};

void MemorySnapshotDeleter::operator()(MemorySnapshot* p) const { delete p; }
MemorySnapshotPtr new_memory_snapshot() { return MemorySnapshotPtr(new MemorySnapshot); }

bool capture_memory(unsigned char* rdram, recomp_context* ctx, MemorySnapshot& into, std::string& error) {
    return capture(rdram, ctx, into.s, error);
}

bool apply_memory(unsigned char* rdram, recomp_context* ctx, const MemorySnapshot& from, std::string& error) {
    return apply(rdram, ctx, from.s, error);
}

bool load_now(unsigned char* rdram, recomp_context* ctx, const std::filesystem::path& path, std::string& error) {
    Snapshot snapshot;
    return read_file(path, snapshot, error) && apply(rdram, ctx, snapshot, error);
}

std::vector<uint32_t> game_thread_addresses() {
    std::lock_guard lock{g_threads_mutex};
    std::vector<uint32_t> out;
    for (int32_t t : g_game_threads) out.push_back(static_cast<uint32_t>(t));
    return out;
}

uint32_t load_generation() {
    return g_load_generation.load(std::memory_order_relaxed);
}

void cancel_fast_forward() {
    if (g_ff_binding.type != mm_audio_input::BindingType::None) {
        set_fast_forward(false, {});
    }
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    std::string text;
    float alpha = 1.0f;
    {
        std::lock_guard lock{g_toast_mutex};
        const auto now = std::chrono::steady_clock::now();
        if (g_toast.empty() || now >= g_toast_until) return;
        text = g_toast;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(g_toast_until - now).count();
        if (left < 400) alpha = static_cast<float>(left) / 400.0f;
    }
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##savestate_toast", nullptr, flags)) {
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
#endif

} // namespace mm::savestate

extern "C" void mm_savestate_thread_created(unsigned char* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    std::lock_guard lock{mm::savestate::g_threads_mutex};
    mm::savestate::g_game_threads.push_back(ultramodern::this_thread());
}

extern "C" void mm_savestate_frame_hook(unsigned char* rdram, recomp_context* ctx) {
    using namespace mm::savestate;
    ++g_frame;
    if (!g_test_steps.empty()) {
        run_test_step(rdram, ctx);
        static const char* trace = std::getenv("MM_TEST_SAVESTATE_TRACE");
        static const long every = trace ? std::max(1L, std::strtol(trace, nullptr, 10)) : 0;
        if (every > 0 && g_frame % every == 0) {
            const auto rd16 = [&](uint32_t a) { return MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(a))); };
            const auto rd32 = [&](uint32_t a) { return static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(a)))); };
            std::fprintf(stderr, "[savestate-trace] frame %ld state=%u scene=%d stage=%u timer=%u camx=%08X marinax=%08X\n",
                g_frame, rd16(kGameState), static_cast<int16_t>(rd16(kCurrentScene)), rd16(kCurrentStage),
                rd16(0x801781E0u), rd32(0x800BE558u), rd32(0x800EF598u));
        }
    }

    mm::replay::on_frame(rdram, ctx);
    mm::netplay::on_frame(rdram, ctx);
    const Op op = static_cast<Op>(g_pending_op.exchange(static_cast<int>(Op::None)));
    if (op == Op::None) return;
    const int slot = g_pending_slot.load();
    std::string error;

    if (op == Op::Save) {
        auto snapshot = std::make_shared<Snapshot>();
        if (!capture(rdram, ctx, *snapshot, error)) {
            toast("Save failed: " + error);
            g_busy.store(false);
            return;
        }
        // Compress and write in the background; the game keeps running.
        std::thread([snapshot, slot] {
            std::string write_error;
            if (write_file(slot_path(slot), *snapshot, write_error)) {
                toast("Saved slot " + std::to_string(slot) + describe(*snapshot));
            } else {
                toast("Save failed: " + write_error);
            }
            g_busy.store(false);
        }).detach();
        return;
    }

    std::unique_ptr<Snapshot> snapshot;
    {
        std::lock_guard lock{g_load_mutex};
        snapshot = std::move(g_load_snapshot);
    }
    if (snapshot && apply(rdram, ctx, *snapshot, error)) {
        toast("Loaded slot " + std::to_string(slot) + describe(*snapshot));
    } else {
        toast("Slot " + std::to_string(slot) + ": cannot load, " +
              (error.empty() ? std::string("no data") : error));
    }
    g_busy.store(false);
}
