#include "crash_handler.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>

#include "session_log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace mm::crash_handler {
namespace {

std::atomic<int> g_state{-1}, g_stage{-1}, g_scene{-1};
std::atomic<bool> g_reported{false};

void emit(const char* text) {
    std::fputs(text, stderr);
    mm::session_log::write_emergency(text);
}

#ifdef _WIN32
// "module+0xOFFSET" for an address.
void describe(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module != nullptr) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
    }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    if (g_reported.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    char line[512];
    char where[256];
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    describe(reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof where);
    std::snprintf(line, sizeof line,
                  "\n[crash] exception 0x%08lx at %s thread %lu (game-state=%d stage=%d scene=%d)\n",
                  static_cast<unsigned long>(rec->ExceptionCode), where, GetCurrentThreadId(),
                  g_state.load(), g_stage.load(), g_scene.load());
    emit(line);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::snprintf(line, sizeof line, "[crash] %s address 0x%llx\n",
                      rec->ExceptionInformation[0] == 0 ? "read of" : rec->ExceptionInformation[0] == 1 ? "write to" : "execute at",
                      static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        emit(line);
    }
#ifdef _M_X64
    CONTEXT ctx = *info->ContextRecord;
    std::snprintf(line, sizeof line, "[crash] rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx\n",
                  ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rsp);
    emit(line);
    // Walk the stack with the x64 unwind tables.
    for (int frame = 0; frame < 40 && ctx.Rip != 0; ++frame) {
        describe(ctx.Rip, where, sizeof where);
        std::snprintf(line, sizeof line, "[crash]   #%02d %s\n", frame, where);
        emit(line);
        DWORD64 image_base = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (fn == nullptr) {
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);   // leaf function
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    }
#endif
    emit("[crash] end of report\n");
    mm::session_log::flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void on_signal(int sig) {
    if (!g_reported.exchange(true)) {
        char line[160];
        std::snprintf(line, sizeof line, "\n[crash] signal %d (game-state=%d stage=%d scene=%d)\n", sig,
                      g_state.load(), g_stage.load(), g_scene.load());
        emit(line);
        mm::session_log::flush();
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

void install() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_exception);
#endif
    std::signal(SIGABRT, on_signal);
#ifndef _WIN32
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGBUS, on_signal);
#endif
}

void note_scene(int game_state, int stage, int scene) {
    g_state.store(game_state, std::memory_order_relaxed);
    g_stage.store(stage, std::memory_order_relaxed);
    g_scene.store(scene, std::memory_order_relaxed);
}

} // namespace mm::crash_handler
