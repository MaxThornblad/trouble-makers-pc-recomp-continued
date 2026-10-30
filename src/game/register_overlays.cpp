// Register the translated program's sections and host function entries.
//
// Registers the recompiled game's overlay/section table with librecomp so the
// runtime can resolve N64 overlay loads (load_overlays/unload_overlays) to the
// correct host-side recompiled functions.
//
// The section/overlay data is emitted by N64Recomp into
// RecompiledFuncs/recomp_overlays.inl (section_table[], num_sections=177,
// overlay_sections_by_index[]). That .inl is NOT compiled directly by the
// mm_recompiled library — it is #included here, exactly once, by the TU that
// feeds it to recomp::overlays::register_overlays(). Modeled on
// reference/Zelda64Recomp/src/main/register_overlays.cpp.

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "../../RecompiledFuncs/recomp_overlays.inl"

#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include "mm_audio_input.hpp"

// Set by ultramodern::quit() (librecomp/src/recomp.cpp) and polled by the
// runtime's own event threads. We poll it from the AI_LEN mirror thread for the
// same reason: rdram is munmap'd only after the event threads join — i.e. only
// after `exited` has been observed — so stopping the mirror on `exited`
// guarantees it never writes to freed rdram during teardown.
extern std::atomic_bool exited;

namespace {
// AI_LEN normally mirrors the host's real audio queue (below). That makes how
// much audio the game mixes each frame depend on host timing, and with it the
// game's audio state, which replays and netplay must reproduce exactly. In
// virtual mode the mirror thread stays out and the game thread sets AI_LEN at
// each frame point from a virtual DAC instead: it drains exactly one 60 Hz
// frame of samples per game frame and fills by what the game submits.
// Fast-forward uses it too: at several times normal speed the real queue
// overflows and is cleared over and over, AI_LEN swings wildly and the game
// can mix a corrupt audio command list (seen as an aspMain "unhandled jump
// target" crash at 8x).
std::atomic<bool> g_virtual_audio{false};
volatile uint32_t* g_ai_len = nullptr;
uint64_t g_virtual_drained = 0;      // bytes, game thread only
uint32_t g_virtual_remainder = 0;
bool g_virtual_requested = false;    // replays / netplay (game thread)

// `restart`: begin again from the canonical two queued frames even if the
// clock is already running (a replay or session start must not inherit
// whatever fast-forward left).
void set_virtual_audio(bool on, bool restart = false) {
    if (on == g_virtual_audio.load(std::memory_order_relaxed) && !(on && restart)) return;
    if (on) {
        // Keep the mirror thread out first, and give a write it already began
        // time to land before ours.
        g_virtual_audio.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        // Start with two frames of audio queued, the same on every machine.
        const uint64_t two_frames = static_cast<uint64_t>(mm_audio_input::game_sample_rate()) * 4u * 2u / 60u;
        const uint64_t submitted = mm_audio_input::submitted_audio_bytes();
        g_virtual_drained = submitted - two_frames;
        g_virtual_remainder = 0;
        if (g_ai_len) *g_ai_len = static_cast<uint32_t>(two_frames);
    }
    g_virtual_audio.store(on, std::memory_order_release);
}
} // namespace

#include "audio_clock.h"

extern "C" void mm_virtual_audio_clock(bool on) {
    g_virtual_requested = on;
    set_virtual_audio(on || ultramodern::get_speed_multiplier() > 1, on);
}

// The clock's state, for save states: a state put back must bring the
// audio level the game sees along, or the frames after it read a stale one.
extern "C" void mm_virtual_audio_get(mm_audio_clock_state* out) {
    out->submitted = mm_audio_input::submitted_audio_bytes();
    out->drained = g_virtual_drained;
    out->remainder = g_virtual_remainder;
    out->ai_len = g_ai_len ? *g_ai_len : 0;
}

extern "C" void mm_virtual_audio_set(const mm_audio_clock_state* in) {
    mm_audio_input::set_submitted_audio_bytes(in->submitted);
    g_virtual_drained = in->drained;
    g_virtual_remainder = in->remainder;
    // A state taken while the clock did not run (normal speed, the host
    // queue drives AI_LEN) carries a stale "drained": put back under the
    // clock (fast-forward, a replay) the level read submitted - drained, any
    // number of seconds of audio, and the game mixed a corrupt command list
    // (its sample DMAs read past the ROM, then an invalid RSP task quit the
    // game). Such a state starts the clock again from two queued frames.
    const uint64_t two_frames = static_cast<uint64_t>(mm_audio_input::game_sample_rate()) * 4u * 2u / 60u;
    const bool stale = in->drained > in->submitted || in->submitted - in->drained > two_frames * 8u;
    if (stale) {
        g_virtual_drained = in->submitted - std::min<uint64_t>(two_frames, in->submitted);
        g_virtual_remainder = 0;
    }
    if (g_ai_len && g_virtual_audio.load(std::memory_order_relaxed)) {
        *g_ai_len = stale ? static_cast<uint32_t>(in->submitted - g_virtual_drained) : in->ai_len;
    }
}

// Game thread, once per frame point.
extern "C" void mm_virtual_audio_frame() {
    set_virtual_audio(g_virtual_requested || ultramodern::get_speed_multiplier() > 1);
    if (!g_virtual_audio.load(std::memory_order_relaxed) || g_ai_len == nullptr) return;
    const uint64_t step = static_cast<uint64_t>(mm_audio_input::game_sample_rate()) * 4u + g_virtual_remainder;
    g_virtual_drained += (step / 60u) & ~3ull;
    g_virtual_remainder = static_cast<uint32_t>(step - ((step / 60u) & ~3ull) * 60u);
    const uint64_t submitted = mm_audio_input::submitted_audio_bytes();
    if (g_virtual_drained > submitted) g_virtual_drained = submitted;   // underrun: the DAC ran dry
    *g_ai_len = static_cast<uint32_t>(submitted - g_virtual_drained);
    static const bool trace = std::getenv("MM_TRACE_AUDIO_CLOCK") != nullptr;
    if (trace) {
        static long n = 0;
        std::fprintf(stderr, "[audio-clock] %ld submitted %llu drained %llu ai_len %u\n", n++,
                     static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(g_virtual_drained),
                     *g_ai_len);
    }
}

namespace {

// Host-side replacement for rmonMain, the N64 debug-monitor thread entry.
// rmonMain is in N64Recomp's ignored_funcs (rmon is never recompiled), but
// this game's boot code (decomp src/boot.c) passes it to osCreateThread, so
// the runtime resolves it BY ADDRESS when the rmon thread first runs — with
// no entry registered, get_function() aborts. On retail hardware the rmon
// thread parks forever waiting for debugger serial traffic that never
// arrives; returning immediately is equivalent (the host thread exits
// cleanly). Address from the decomp: symbol_addrs_libultra.txt,
// rmonMain = 0x8009A2B8. This is the only ignored function the game's own
// code uses as a thread entry (pimgr/vimgr spawn theirs inside natively
// wrapped osCreatePiManager/osCreateViManager, which never execute).
//
// Must be registered from GameEntry::on_init_callback, NOT from main():
// librecomp's init() calls init_overlays() when the game starts, which
// func_map.clear()s anything registered earlier.
constexpr int32_t kRmonMainVram = 0x8009A2B8;

void rmonMain_host(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    std::fprintf(stderr, "[rmon] rmon thread started; idling (host no-op).\n");
}

} // namespace

namespace troublemakers {

void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}

void on_game_init(uint8_t* rdram, recomp_context* /*ctx*/) {
    recomp::overlays::add_loaded_function(kRmonMainVram, rmonMain_host);

    // Sound_Update (decomp music.c, asm at 0x800023F0) reads the AI hardware
    // registers directly (`lui $t9,0xA450; lw $t1,4($t9)` = AI_LEN_REG at
    // 0xA4500004) instead of calling osAiGetLength, then `srl $t2,$t1,2` ->
    // gAudioSamplesLeft: it divides the byte count by 4 to get remaining stereo
    // frames. This is the game's ONLY raw MMIO touch — every other D_A4*
    // access in the decomp lives in libultra functions librecomp replaces
    // wholesale.
    //
    // librecomp maps N64 addresses flat (addr - 0x80000000), so that `lw` hits
    // rdram offset 0x24500004 — beyond the committed mem_size but inside
    // librecomp's PROT_NONE reservation. Commit the AI register page and mirror
    // a *real* AI_LEN into it. On hardware AI_LEN = bytes remaining in the
    // in-flight AI DMA; the game uses it to pace audio (it backs off until the
    // DAC drains). With the page merely zero-filled, AI_LEN always reads 0
    // ("drained") and the game's pacing logic free-runs.
    //
    // MEM_W (recomp.h) is a plain aligned host word load — no byte swizzle for
    // word accesses (only MEM_B/MEM_H carry the ^3/^2 XOR), so writing a plain
    // little-endian u32 to rdram+0x24500004 is read back verbatim by the
    // recompiled `lw`. We mirror ultramodern's view of the output buffer:
    // get_frames_remaining() is in stereo frames at the game's sample rate, and
    // *4 = bytes (2 channels * sizeof(s16)), which is exactly AI_LEN's unit and
    // what the game's `srl 2` turns back into frames — so gAudioSamplesLeft
    // tracks the real DAC queue depth instead of always reading zero.
    constexpr uint32_t kAiRegsVaddr = 0xA4500000u;
    void* ai_page = rdram + (kAiRegsVaddr - 0x80000000u);
#if defined(_WIN32)
    // librecomp reserves the whole rdram range with VirtualAlloc
    // (MEM_COMMIT|MEM_RESERVE, PAGE_NOACCESS) and flips the low mem_size to
    // PAGE_READWRITE (librecomp/src/recomp.cpp) — the POSIX branch's
    // mmap(PROT_NONE)+mprotect twin. Committing our extra page is the same
    // one-call protection flip.
    DWORD old_protect = 0;
    if (VirtualProtect(ai_page, 0x1000, PAGE_READWRITE, &old_protect) == 0) {
        std::fprintf(stderr, "[recomp] VirtualProtect(AI register page) failed: %lu\n",
                     GetLastError());
        return;
    }
#else
    if (mprotect(ai_page, 0x1000, PROT_READ | PROT_WRITE) != 0) {
        std::perror("[recomp] mprotect(AI register page)");
        return;
    }
#endif

    // 0xA4500004 - 0x80000000 = 0x24500004, which is 4-byte aligned, so a plain
    // aligned word store is valid (no swizzle crossing). volatile: this is a
    // memory-mapped register mirror — the store must reach rdram every tick
    // even under -O2 (the value read from get_frames_remaining is opaque to the
    // compiler, but volatile documents and guarantees the MMIO store semantic).
    constexpr uintptr_t kAiLenOffset = 0xA4500004u - 0x80000000u;
    volatile uint32_t* ai_len = reinterpret_cast<volatile uint32_t*>(rdram + kAiLenOffset);
    g_ai_len = ai_len;

    // ~1ms cadence: the DAC drains continuously and the game samples AI_LEN
    // once per Sound_Update, so 1ms keeps the value fresh without busy-burning
    // a core. Self-terminates on `exited` (see the extern above) so it never
    // outlives rdram. Detached: the process either runs until ultramodern::quit
    // (thread observes `exited` and returns) or is killed/aborts (thread dies
    // with the process).
    std::thread([ai_len]() {
        while (!exited.load(std::memory_order_relaxed)) {
            if (!g_virtual_audio.load(std::memory_order_acquire)) {
                *ai_len = static_cast<uint32_t>(mm_audio_input::get_frames_remaining()) * 4u;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();

    // Widescreen view-cull widening is applied by same-thread translated
    // return hooks (see troublemakers.us1.toml [patches]): expanded gameplay
    // uses camX +/- 0x180 while 4:3/cinematics retain +/- 0x90. A former host
    // timer raced the game's per-frame rewrite; the return-boundary hook is
    // deterministic. See docs/README.md, "Camera rectangle".
}

} // namespace troublemakers
