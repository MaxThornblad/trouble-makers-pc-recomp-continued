#include "steam_net.h"

#include "net_transport.h"
#include "netplay.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mm::steam {
namespace {

using u64 = uint64_t;

// Steamworks flat C API (steam_api_flat.h): plain C functions taking the
// interface pointer first. Loaded by name; no SDK headers or import library.
struct Api {
    int (*InitFlat)(char* err) = nullptr;
    bool (*IsSteamRunning)() = nullptr;
    int (*GetHSteamPipe)() = nullptr;
    void (*DispatchInit)() = nullptr;
    void (*DispatchRunFrame)(int) = nullptr;
    bool (*DispatchNext)(int, void*) = nullptr;
    void (*DispatchFree)(int) = nullptr;
    bool (*DispatchCallResult)(int, u64, void*, int, int, bool*) = nullptr;
    void* (*User)() = nullptr;
    void* (*Matchmaking)() = nullptr;
    void* (*Friends)() = nullptr;
    void* (*Messages)() = nullptr;
    void* (*Utils)() = nullptr;
    u64 (*GetSteamID)(void*) = nullptr;
    u64 (*CreateLobby)(void*, int, int) = nullptr;
    u64 (*JoinLobby)(void*, u64) = nullptr;
    void (*LeaveLobby)(void*, u64) = nullptr;
    bool (*SetLobbyData)(void*, u64, const char*, const char*) = nullptr;
    const char* (*GetLobbyData)(void*, u64, const char*) = nullptr;
    u64 (*GetLobbyOwner)(void*, u64) = nullptr;
    int (*GetNumLobbyMembers)(void*, u64) = nullptr;
    u64 (*GetLobbyMemberByIndex)(void*, u64, int) = nullptr;
    bool (*SetRichPresence)(void*, const char*, const char*) = nullptr;
    void (*ClearRichPresence)(void*) = nullptr;
    void (*InviteDialog)(void*, u64) = nullptr;
    int (*GetFriendCount)(void*, int) = nullptr;
    u64 (*GetFriendByIndex)(void*, int, int) = nullptr;
    bool (*GetFriendGamePlayed)(void*, u64, void*) = nullptr;
    void (*IdentitySet)(void*, u64) = nullptr;
    u64 (*IdentityGet)(void*) = nullptr;
    int (*SendToUser)(void*, const void*, const void*, uint32_t, int, int) = nullptr;
    int (*Receive)(void*, int, void**, int) = nullptr;
    bool (*AcceptSession)(void*, const void*) = nullptr;
    bool (*CloseSession)(void*, const void*) = nullptr;
    void (*Release)(void*) = nullptr;
    bool (*SetConfigInt)(void*, int, int32_t) = nullptr;
    // Optional (an older library or the test stand-in may lack them).
    void* (*SteamUtils)() = nullptr;
    bool (*IsOverlayEnabled)(void*) = nullptr;
    bool (*InviteUserToLobby)(void*, u64, u64) = nullptr;
    const char* (*GetFriendPersonaName)(void*, u64) = nullptr;
    int (*GetFriendPersonaState)(void*, u64) = nullptr;
};
Api a;
void* g_user = nullptr;
void* g_mm = nullptr;
void* g_friends = nullptr;
void* g_msgs = nullptr;
void* g_utils = nullptr;
void* g_steam_utils = nullptr;   // ISteamUtils (overlay), when the library has it
int g_pipe = 0;
bool g_started = false;
std::string g_fail;

// Callback ids and layouts (Windows packs callback structs to 8, other
// platforms to 4: only LobbyCreated_t's lobby moves).
constexpr int kGameLobbyJoinRequested = 333;
constexpr int kRichPresenceJoinRequested = 337;
constexpr int kLobbyEnter = 504;
constexpr int kLobbyChatUpdate = 506;
constexpr int kLobbyCreated = 513;
constexpr int kCallCompleted = 703;
constexpr int kSessionRequest = 1251;
constexpr int kSessionFailed = 1252;
#ifdef _WIN32
constexpr size_t kLobbyCreatedLobby = 8;
#else
constexpr size_t kLobbyCreatedLobby = 4;
#endif
// SteamNetworkingMessage_t on 64-bit: data pointer 0, size 8, sender 16.
constexpr size_t kMsgData = 0, kMsgSize = 8, kMsgSender = 16;
constexpr int kIdentityBytes = 160;   // SteamNetworkingIdentity is 136; room to spare
constexpr int kSendReliableNoNagle = 8 | 1;
constexpr uint32_t kPiece = 60000;    // per message; bigger ones are split
constexpr int kResultOK = 1, kResultLimitExceeded = 25;

enum class Mode { Idle, Hosting, Joining, Finding };
Mode g_mode = Mode::Idle;
u64 g_me = 0;
u64 g_lobby = 0;
u64 g_peer = 0;
bool g_connected = false;
std::string g_status;
std::deque<Event> g_events;
std::vector<uint8_t> g_partial[2];
u64 g_join_request = 0;
std::chrono::steady_clock::time_point g_next_scan{};

template <typename T>
T get(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

template <typename F>
bool load(void* lib, F& fn, const char* name) {
#ifdef _WIN32
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name)));
#else
    fn = reinterpret_cast<F>(dlsym(lib, name));
#endif
    if (!fn) g_fail = std::string("this Steam library is too old (no ") + name + ")";
    return fn != nullptr;
}

std::filesystem::path exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::string(buf, n)).parent_path();
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

void* open_library() {
    const char* override_path = std::getenv("MM_STEAM_LIB");   // tests: a stand-in library
#ifdef _WIN32
    const char* name = "steam_api64.dll";
    if (override_path) return LoadLibraryA(override_path);
    if (void* l = LoadLibraryA((exe_dir() / name).string().c_str())) return l;
    return LoadLibraryA(name);
#else
    const char* name = "libsteam_api.so";
    if (override_path) return dlopen(override_path, RTLD_NOW);
    if (void* l = dlopen((exe_dir() / name).string().c_str(), RTLD_NOW)) return l;
    return dlopen(name, RTLD_NOW);
#endif
}

// Steam reads the app id from steam_appid.txt in the working directory.
void write_app_id() {
    for (const auto& dir : {std::filesystem::current_path(), exe_dir()}) {
        std::error_code ec;
        const auto path = dir / "steam_appid.txt";
        if (std::filesystem::exists(path, ec)) continue;
        std::ofstream f(path);
        f << "480\n";
    }
}

void set_status(std::string s) {
    if (s != g_status) std::fprintf(stderr, "[steam] %s\n", s.c_str());
    g_status = std::move(s);
}

void identity(uint8_t (&buf)[kIdentityBytes], u64 id) {
    std::memset(buf, 0, sizeof buf);
    a.IdentitySet(buf, id);
}

bool is_member(u64 who) {
    if (g_lobby == 0) return false;
    const int n = a.GetNumLobbyMembers(g_mm, g_lobby);
    for (int i = 0; i < n; ++i) {
        if (a.GetLobbyMemberByIndex(g_mm, g_lobby, i) == who) return true;
    }
    return false;
}

void disconnected(const char* why) {
    if (!g_connected && g_peer == 0) return;
    std::fprintf(stderr, "[steam] partner gone (%s)\n", why);
    if (g_peer != 0) {
        uint8_t id[kIdentityBytes];
        identity(id, g_peer);
        a.CloseSession(g_msgs, id);
    }
    g_peer = 0;
    g_connected = false;
    g_partial[0].clear();
    g_partial[1].clear();
    Event e;
    e.type = Event::Disconnected;
    g_events.push_back(std::move(e));
}

void failed(std::string why) {
    set_status(why);
    Event e;
    e.type = Event::Failed;
    e.text = std::move(why);
    g_events.push_back(std::move(e));
}

void on_lobby_created(int result, u64 lobby) {
    if (g_mode != Mode::Hosting || g_lobby != 0) return;   // arrives twice on real Steam
    if (result != kResultOK || lobby == 0) {
        failed("Steam could not create a lobby (result " + std::to_string(result) + ")");
        return;
    }
    g_lobby = lobby;
    a.SetLobbyData(g_mm, lobby, "troublemakers", "1");
    a.SetLobbyData(g_mm, lobby, "build", MM_GIT_COMMIT);
    const std::string connect = "+connect_lobby " + std::to_string(lobby);
    a.SetRichPresence(g_friends, "connect", connect.c_str());
    a.SetRichPresence(g_friends, "status", "Hosting Trouble Makers co-op");
    std::fprintf(stderr, "[steam] lobby %llu\n", static_cast<unsigned long long>(lobby));
    // Steam's invite window needs its overlay, which only games Steam started
    // have. Without it: invite from the friends list on the Session page.
    if (overlay_enabled()) {
        a.InviteDialog(g_friends, lobby);
        set_status("Hosting on Steam: invite a friend (Shift+Tab or the list below), or they pick "
                   "\"Join a friend on Steam\".");
    } else {
        set_status("Hosting on Steam: invite a friend from the list below, or they pick "
                   "\"Join a friend on Steam\". (No Steam overlay: start the game from Steam for Shift+Tab.)");
    }
}

void on_lobby_enter(u64 lobby, uint32_t response) {
    if (g_mode != Mode::Joining || g_connected) return;
    if (response != 1) {
        failed("could not join that Steam lobby (it is full, gone or private)");
        return;
    }
    g_lobby = lobby;
    const char* tag = a.GetLobbyData(g_mm, lobby, "troublemakers");
    if (!tag || std::strcmp(tag, "1") != 0) {
        a.LeaveLobby(g_mm, lobby);
        g_lobby = 0;
        failed("that Steam lobby is not a Trouble Makers game");
        return;
    }
    g_peer = a.GetLobbyOwner(g_mm, lobby);
    g_connected = true;
    set_status("In the host's Steam lobby");
    Event e;
    e.type = Event::Connected;
    g_events.push_back(std::move(e));
}

void handle(int callback, const uint8_t* p) {
    switch (callback) {
        case kLobbyCreated:
            on_lobby_created(get<int32_t>(p, 0), get<u64>(p, kLobbyCreatedLobby));
            break;
        case kLobbyEnter:
            on_lobby_enter(get<u64>(p, 0), get<uint32_t>(p, 16));
            break;
        case kLobbyChatUpdate: {
            const u64 lobby = get<u64>(p, 0), who = get<u64>(p, 8);
            const uint32_t change = get<uint32_t>(p, 24);
            if (lobby == g_lobby && who == g_peer && (change & (0x2 | 0x4 | 0x8 | 0x10)) != 0) {
                disconnected("left the lobby");
            }
            break;
        }
        case kGameLobbyJoinRequested:
            g_join_request = get<u64>(p, 0);
            break;
        case kRichPresenceJoinRequested: {
            char connect[257];
            std::memcpy(connect, p + 8, 256);
            connect[256] = 0;
            if (const char* at = std::strstr(connect, "+connect_lobby ")) {
                g_join_request = std::strtoull(at + 15, nullptr, 10);
            }
            break;
        }
        case kSessionRequest: {
            const u64 who = a.IdentityGet(const_cast<uint8_t*>(p));
            const bool ok = g_mode == Mode::Hosting ? (g_peer == 0 || g_peer == who) && is_member(who) : who == g_peer;
            if (ok) a.AcceptSession(g_msgs, p);
            std::fprintf(stderr, "[steam] session request from %llu: %s\n", static_cast<unsigned long long>(who),
                         ok ? "accepted" : "ignored");
            break;
        }
        case kSessionFailed: {
            const u64 who = a.IdentityGet(const_cast<uint8_t*>(p));
            if (who == g_peer) disconnected("connection failed");
            break;
        }
        default:
            break;
    }
}

void run_callbacks() {
    a.DispatchRunFrame(g_pipe);
    struct {
        int32_t user;
        int32_t callback;
        uint8_t* param;
        int32_t size;
    } msg{};
    while (a.DispatchNext(g_pipe, &msg)) {
        if (msg.callback == kCallCompleted) {
            const u64 call = get<u64>(msg.param, 0);
            const int expected = get<int32_t>(msg.param, 8);
            const uint32_t size = get<uint32_t>(msg.param, 12);
            std::vector<uint8_t> buf(size + 8);
            bool io_failed = false;
            if (a.DispatchCallResult(g_pipe, call, buf.data(), static_cast<int>(size), expected, &io_failed) && !io_failed) {
                handle(expected, buf.data());
            }
        } else {
            handle(msg.callback, msg.param);
        }
        a.DispatchFree(g_pipe);
    }
}

// A friend running Spacewar (app 480) with a lobby: maybe hosting.
void find_friend() {
    struct {
        uint8_t bytes[64];
    } info{};
    const int n = a.GetFriendCount(g_friends, 4);   // immediate friends
    for (int i = 0; i < n; ++i) {
        const u64 f = a.GetFriendByIndex(g_friends, i, 4);
        std::memset(&info, 0, sizeof info);
        if (!a.GetFriendGamePlayed(g_friends, f, &info)) continue;
        const u64 game = get<u64>(info.bytes, 0);
        const u64 lobby = get<u64>(info.bytes, 16);
        if ((game & 0xFFFFFF) != 480 || lobby == 0) continue;
        std::fprintf(stderr, "[steam] friend %llu is in lobby %llu: joining\n", static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(lobby));
        g_mode = Mode::Joining;
        a.JoinLobby(g_mm, lobby);
        set_status("Joining a friend's game on Steam...");
        return;
    }
    set_status("Looking for a friend hosting on Steam (or accept their invite)...");
}

void receive() {
    for (int ch = 0; ch < 2; ++ch) {
        void* msgs[32];
        for (;;) {
            const int n = a.Receive(g_msgs, ch, msgs, 32);
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) {
                const uint8_t* m = static_cast<const uint8_t*>(msgs[i]);
                const uint8_t* data = get<const uint8_t*>(m, kMsgData);
                const int32_t size = get<int32_t>(m, kMsgSize);
                const u64 from = a.IdentityGet(const_cast<uint8_t*>(m + kMsgSender));
                bool take = false;
                if (from != 0 && size >= 1) {
                    if (g_mode == Mode::Hosting && g_peer == 0 && is_member(from)) {
                        g_peer = from;   // the first lobby member to talk is the partner
                        g_connected = true;
                        Event e;
                        e.type = Event::Connected;
                        g_events.push_back(std::move(e));
                    }
                    take = from == g_peer;
                }
                if (take) {
                    // 1-byte frame: 0 whole, 1 a piece, 2 the last piece.
                    auto& part = g_partial[ch];
                    const uint8_t kind = data[0];
                    part.insert(part.end(), data + 1, data + size);
                    if (kind != 1) {
                        Event e;
                        e.type = Event::Received;
                        e.channel = ch;
                        e.data.swap(part);
                        g_events.push_back(std::move(e));
                        part.clear();
                    }
                }
                a.Release(msgs[i]);
            }
        }
    }
}

} // namespace

bool start(std::string& why) {
    if (g_started) return true;
    void* lib = open_library();
    if (!lib) {
        why = "Steam is not set up (steam_api64.dll is missing next to the game)";
        return false;
    }
    g_fail.clear();
    bool ok = load(lib, a.InitFlat, "SteamAPI_InitFlat") && load(lib, a.IsSteamRunning, "SteamAPI_IsSteamRunning") &&
              load(lib, a.GetHSteamPipe, "SteamAPI_GetHSteamPipe") &&
              load(lib, a.DispatchInit, "SteamAPI_ManualDispatch_Init") &&
              load(lib, a.DispatchRunFrame, "SteamAPI_ManualDispatch_RunFrame") &&
              load(lib, a.DispatchNext, "SteamAPI_ManualDispatch_GetNextCallback") &&
              load(lib, a.DispatchFree, "SteamAPI_ManualDispatch_FreeLastCallback") &&
              load(lib, a.DispatchCallResult, "SteamAPI_ManualDispatch_GetAPICallResult") &&
              load(lib, a.User, "SteamAPI_SteamUser_v023") && load(lib, a.Matchmaking, "SteamAPI_SteamMatchmaking_v009") &&
              load(lib, a.Friends, "SteamAPI_SteamFriends_v018") &&
              load(lib, a.Messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002") &&
              load(lib, a.Utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004") &&
              load(lib, a.GetSteamID, "SteamAPI_ISteamUser_GetSteamID") &&
              load(lib, a.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby") &&
              load(lib, a.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby") &&
              load(lib, a.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby") &&
              load(lib, a.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData") &&
              load(lib, a.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData") &&
              load(lib, a.GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner") &&
              load(lib, a.GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers") &&
              load(lib, a.GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex") &&
              load(lib, a.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence") &&
              load(lib, a.ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence") &&
              load(lib, a.InviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog") &&
              load(lib, a.GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount") &&
              load(lib, a.GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex") &&
              load(lib, a.GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed") &&
              load(lib, a.IdentitySet, "SteamAPI_SteamNetworkingIdentity_SetSteamID64") &&
              load(lib, a.IdentityGet, "SteamAPI_SteamNetworkingIdentity_GetSteamID64") &&
              load(lib, a.SendToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser") &&
              load(lib, a.Receive, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel") &&
              load(lib, a.AcceptSession, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser") &&
              load(lib, a.CloseSession, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser") &&
              load(lib, a.Release, "SteamAPI_SteamNetworkingMessage_t_Release") &&
              load(lib, a.SetConfigInt, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
    if (!ok) {
        why = g_fail;
        return false;
    }
    {
        const std::string fail = g_fail;   // optional: missing ones leave g_fail as it was
        load(lib, a.SteamUtils, "SteamAPI_SteamUtils_v010");
        load(lib, a.IsOverlayEnabled, "SteamAPI_ISteamUtils_IsOverlayEnabled");
        load(lib, a.InviteUserToLobby, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
        load(lib, a.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
        load(lib, a.GetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState");
        g_fail = fail;
    }
    write_app_id();
    if (!a.IsSteamRunning()) {
        why = "Steam is not running: start Steam and sign in first";
        return false;
    }
    char err[1024] = {};
    const int r = a.InitFlat(err);
    if (r != 0) {
        why = r == 2 ? "Steam is not running: start Steam and sign in first"
                     : std::string("Steam did not start (") + (err[0] ? err : std::to_string(r).c_str()) + ")";
        return false;
    }
    a.DispatchInit();
    g_pipe = a.GetHSteamPipe();
    g_user = a.User();
    g_mm = a.Matchmaking();
    g_friends = a.Friends();
    g_msgs = a.Messages();
    g_utils = a.Utils();
    g_steam_utils = a.SteamUtils ? a.SteamUtils() : nullptr;
    g_me = a.GetSteamID(g_user);
    // Room for a whole resync state in flight (k_ESteamNetworkingConfig_
    // SendBufferSize 9, RecvBufferSize 47, RecvMaxMessageSize 49).
    a.SetConfigInt(g_utils, 9, 32 << 20);
    a.SetConfigInt(g_utils, 47, 32 << 20);
    a.SetConfigInt(g_utils, 49, 1 << 20);
    g_started = true;
    std::fprintf(stderr, "[steam] started (you are %llu)\n", static_cast<unsigned long long>(g_me));
    return true;
}

void host() {
    leave();
    g_mode = Mode::Hosting;
    a.CreateLobby(g_mm, 1 /* friends only */, 2);
    set_status("Creating a Steam lobby...");
}

void join(uint64_t lobby) {
    leave();
    if (lobby == 0) {
        g_mode = Mode::Finding;
        g_next_scan = std::chrono::steady_clock::now();
        set_status("Looking for a friend hosting on Steam (or accept their invite)...");
        return;
    }
    g_mode = Mode::Joining;
    a.JoinLobby(g_mm, lobby);
    set_status("Joining Steam lobby " + std::to_string(lobby) + "...");
}

void leave() {
    if (!g_started) return;
    if (g_peer != 0) {
        uint8_t id[kIdentityBytes];
        identity(id, g_peer);
        a.CloseSession(g_msgs, id);
    }
    if (g_lobby != 0) a.LeaveLobby(g_mm, g_lobby);
    if (g_mode == Mode::Hosting) a.ClearRichPresence(g_friends);
    g_lobby = 0;
    g_peer = 0;
    g_connected = false;
    g_mode = Mode::Idle;
    g_events.clear();
    g_partial[0].clear();
    g_partial[1].clear();
}

void drop_peer() {
    if (g_peer == 0) return;
    uint8_t id[kIdentityBytes];
    identity(id, g_peer);
    a.CloseSession(g_msgs, id);
    g_peer = 0;
    g_connected = false;
}

Event poll() {
    if (!g_started) return {};
    if (g_events.empty()) {
        run_callbacks();
        if (g_mode == Mode::Finding && std::chrono::steady_clock::now() >= g_next_scan) {
            g_next_scan = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            if (g_join_request != 0) {
                const u64 l = g_join_request;
                g_join_request = 0;
                g_mode = Mode::Joining;
                a.JoinLobby(g_mm, l);
            } else {
                find_friend();
            }
        }
        if (g_mode != Mode::Idle) receive();
    }
    if (g_events.empty()) return {};
    Event e = std::move(g_events.front());
    g_events.pop_front();
    return e;
}

bool send(int channel, const uint8_t* data, size_t size) {
    if (!g_started || g_peer == 0) return false;
    uint8_t id[kIdentityBytes];
    identity(id, g_peer);
    std::vector<uint8_t> piece;
    size_t off = 0;
    do {
        const size_t n = std::min<size_t>(kPiece, size - off);
        const bool last = off + n >= size;
        piece.assign(1, static_cast<uint8_t>(off == 0 && last ? 0 : (last ? 2 : 1)));
        piece.insert(piece.end(), data + off, data + off + n);
        int r = 0;
        for (int attempt = 0; attempt < 2000; ++attempt) {   // send buffer full: wait a moment
            r = a.SendToUser(g_msgs, id, piece.data(), static_cast<uint32_t>(piece.size()), kSendReliableNoNagle, channel);
            if (r != kResultLimitExceeded) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (r != kResultOK) {
            std::fprintf(stderr, "[steam] send failed (result %d)\n", r);
            return false;
        }
        off += n;
    } while (off < size);
    return true;
}

uint64_t lobby() { return g_lobby; }
bool started() { return g_started; }
bool hosting() { return g_started && g_mode == Mode::Hosting && g_lobby != 0; }

bool overlay_enabled() {
    return g_started && g_steam_utils && a.IsOverlayEnabled && a.IsOverlayEnabled(g_steam_utils);
}

std::vector<Friend> online_friends() {
    std::vector<Friend> out;
    if (!g_started || !a.GetFriendPersonaState) return out;
    struct {
        uint8_t bytes[64];
    } info{};
    const int n = a.GetFriendCount(g_friends, 4);   // immediate friends
    for (int i = 0; i < n && out.size() < 64; ++i) {
        const u64 f = a.GetFriendByIndex(g_friends, i, 4);
        if (a.GetFriendPersonaState(g_friends, f) == 0) continue;   // offline
        Friend fr;
        fr.id = f;
        const char* name = a.GetFriendPersonaName ? a.GetFriendPersonaName(g_friends, f) : nullptr;
        fr.name = name ? name : std::to_string(f);
        std::memset(&info, 0, sizeof info);
        if (a.GetFriendGamePlayed(g_friends, f, &info)) {
            const u64 game = get<u64>(info.bytes, 0);
            fr.playing = (game & 0xFFFFFF) == 480;
            fr.hosting = fr.playing && get<u64>(info.bytes, 16) != 0;
        }
        out.push_back(std::move(fr));
    }
    // Those in the game first, then by name.
    std::stable_sort(out.begin(), out.end(), [](const Friend& x, const Friend& y) {
        return x.playing != y.playing ? x.playing : x.name < y.name;
    });
    return out;
}

bool invite(uint64_t friend_id) {
    if (!g_started || g_mode != Mode::Hosting || g_lobby == 0 || !a.InviteUserToLobby) return false;
    const bool ok = a.InviteUserToLobby(g_mm, g_lobby, friend_id);
    std::fprintf(stderr, "[steam] invited %llu: %s\n", static_cast<unsigned long long>(friend_id), ok ? "sent" : "failed");
    return ok;
}

uint64_t friend_lobby(uint64_t friend_id) {
    if (!g_started) return 0;
    struct {
        uint8_t bytes[64];
    } info{};
    if (!a.GetFriendGamePlayed(g_friends, friend_id, &info)) return 0;
    return (get<u64>(info.bytes, 0) & 0xFFFFFF) == 480 ? get<u64>(info.bytes, 16) : 0;
}
std::string status() { return g_status; }

uint64_t take_join_request() {
    if (!g_started) return 0;
    if (g_mode == Mode::Idle) run_callbacks();
    const u64 l = g_join_request;
    g_join_request = 0;
    return l;
}


} // namespace mm::steam

// ---------------------------------------------------------------------------
// As a netplay transport (net_transport.h).

namespace mm::netplay {
namespace {

class SteamTransport final : public Transport {
public:
    ~SteamTransport() override { mm::steam::leave(); }

    NetEvent poll(uint32_t timeout_ms) override {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            mm::steam::Event ev = mm::steam::poll();
            NetEvent e;
            switch (ev.type) {
                case mm::steam::Event::None:
                    if (std::chrono::steady_clock::now() >= until) return e;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                case mm::steam::Event::Connected: e.type = NetEvent::Connected; break;
                case mm::steam::Event::Received: e.type = NetEvent::Received; break;
                case mm::steam::Event::Disconnected: e.type = NetEvent::Disconnected; break;
                case mm::steam::Event::Failed: e.type = NetEvent::Failed; break;
            }
            e.channel = ev.channel;
            e.data = std::move(ev.data);
            e.text = std::move(ev.text);
            return e;
        }
    }

    void send(int channel, const uint8_t* data, size_t size) override { mm::steam::send(channel, data, size); }
    void drop_partner() override { mm::steam::drop_peer(); }
    std::string waiting_text() const override { return mm::steam::status(); }
};

} // namespace

std::unique_ptr<Transport> steam_host(std::string& error) {
    if (!mm::steam::start(error)) return nullptr;
    mm::steam::host();
    return std::make_unique<SteamTransport>();
}

std::unique_ptr<Transport> steam_join(uint64_t lobby, std::string& error) {
    if (!mm::steam::start(error)) return nullptr;
    mm::steam::join(lobby);
    return std::make_unique<SteamTransport>();
}

uint64_t steam_join_request() {
    return mm::steam::take_join_request();
}

namespace {
std::mutex g_friends_mutex;
std::vector<SteamFriend> g_friend_list;
std::atomic<uint64_t> g_friend_request{0};
long g_friend_frames = 0;
} // namespace

void steam_tick() {
    if (!mm::steam::started()) {
        std::lock_guard lock{g_friends_mutex};
        g_friend_list.clear();
        return;
    }
    // The friends list for the menu, every two seconds.
    if ((g_friend_frames++ % 120) == 0) {
        std::vector<SteamFriend> list;
        for (const auto& f : mm::steam::online_friends()) list.push_back({f.id, f.name, f.playing, f.hosting});
        std::lock_guard lock{g_friends_mutex};
        g_friend_list = std::move(list);
    }
    if (const uint64_t id = g_friend_request.exchange(0)) {
        if (mm::steam::hosting()) {
            mm::steam::invite(id);
        } else if (const uint64_t lobby = mm::steam::friend_lobby(id)) {
            request_join_steam(lobby);
        }
    }
}

std::vector<SteamFriend> steam_friends() {
    std::lock_guard lock{g_friends_mutex};
    return g_friend_list;
}

bool steam_hosting() {
    return mm::steam::hosting();
}

void request_steam_friend(uint64_t friend_id) {
    g_friend_request.store(friend_id);
}

} // namespace mm::netplay
