/* A stand-in for Steam's libsteam_api.so, for testing netplay over Steam with
 * two copies of the game on one Linux machine (coop-anything's
 * playbook/networking-steam-spacewar.md, "the mock DLL").
 *
 * Exports the flat API functions src/game/steam_net.cpp uses. Each copy is a
 * Steam user given by MOCK_STEAM_ID; lobbies, members, lobby data and "what a
 * friend is playing" are small files in MOCK_STEAM_DIR (default
 * /tmp/mocksteam); messages go over localhost UDP (port 40000 + id % 10000).
 * Like Steam it holds a stranger's messages until AcceptSessionWithUser,
 * sends LobbyCreated twice (call result and broadcast), and tells lobby
 * members when someone leaves. MOCK_STEAM_FRIENDS lists friends' ids;
 * MOCK_STEAM_INVITE=<lobby> fakes an accepted invite. Callback structs use
 * Linux packing (4).
 *
 *   cc -shared -fPIC -O2 -o libsteam_api.so tools/steam_mock/mock_steam.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t u64;
#define EXPORT __attribute__((visibility("default")))

static u64 g_me;
static int g_sock = -1;
static char g_dir[256] = "/tmp/mocksteam";
static u64 g_lobby;
static int g_dummy_iface;
static u64 g_next_call = 1000;
static u64 g_accepted[16];
static int g_naccepted;
static u64 g_requested[16];
static int g_nrequested;
static double g_invite_at;
static u64 g_invite_lobby;

/* callback queue */
typedef struct { int id; int size; uint8_t data[300]; } Cb;
static Cb g_cbs[64];
static int g_ncb;
static Cb g_current;
typedef struct { u64 call; int id; int size; uint8_t data[64]; } CallResult;
static CallResult g_results[16];
static int g_nresults;

/* held / ready messages */
typedef struct { u64 from; int channel; int size; uint8_t* data; } Msg;
static Msg g_ready[4096];
static int g_nready;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void push_cb(int id, const void* data, int size) {
    if (g_ncb >= 64) return;
    g_cbs[g_ncb].id = id;
    g_cbs[g_ncb].size = size;
    memcpy(g_cbs[g_ncb].data, data, size);
    ++g_ncb;
}

static u64 push_result(int id, const void* data, int size) {
    u64 call = g_next_call++;
    if (g_nresults < 16) {
        g_results[g_nresults].call = call;
        g_results[g_nresults].id = id;
        g_results[g_nresults].size = size;
        memcpy(g_results[g_nresults].data, data, size);
        ++g_nresults;
    }
    uint8_t done[16] = {0};
    memcpy(done, &call, 8);
    memcpy(done + 8, &id, 4);
    memcpy(done + 12, &size, 4);
    push_cb(703, done, 16);
    return call;
}

static int port_of(u64 id) { return 40000 + (int)(id % 10000); }

static void path(char* out, size_t n, const char* what, u64 id) {
    snprintf(out, n, "%s/%s_%llu", g_dir, what, (unsigned long long)id);
}

static void udp_send(u64 to, int channel, const void* data, int size) {
    uint8_t buf[70000];
    if (size > 65000) return;
    memcpy(buf, &g_me, 8);
    memcpy(buf + 8, &channel, 4);
    memcpy(buf + 12, data, size);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_of(to));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int tries = 0; tries < 1000; ++tries) {
        if (sendto(g_sock, buf, size + 12, 0, (struct sockaddr*)&a, sizeof a) >= 0) return;
        if (errno != EAGAIN && errno != ENOBUFS) return;
        usleep(1000);
    }
}

static bool accepted(u64 id) {
    for (int i = 0; i < g_naccepted; ++i) if (g_accepted[i] == id) return true;
    return false;
}

static void lobby_members(u64 lobby, u64* out, int* n) {
    char p[300];
    path(p, sizeof p, "members", lobby);
    *n = 0;
    FILE* f = fopen(p, "r");
    if (!f) return;
    unsigned long long v;
    while (*n < 8 && fscanf(f, "%llu", &v) == 1) out[(*n)++] = v;
    fclose(f);
}

static void pump(void) {
    uint8_t buf[70000];
    for (;;) {
        ssize_t n = recv(g_sock, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 12) break;
        u64 from;
        int channel;
        memcpy(&from, buf, 8);
        memcpy(&channel, buf + 8, 4);
        if (channel == -1) {   /* a member left the lobby */
            uint8_t up[28] = {0};
            memcpy(up, &g_lobby, 8);
            memcpy(up + 8, &from, 8);
            memcpy(up + 16, &from, 8);
            uint32_t st = 2;
            memcpy(up + 24, &st, 4);
            push_cb(506, up, 28);
            continue;
        }
        if (!accepted(from)) {
            bool asked = false;
            for (int i = 0; i < g_nrequested; ++i) if (g_requested[i] == from) asked = true;
            if (!asked && g_nrequested < 16) {
                g_requested[g_nrequested++] = from;
                uint8_t ident[160] = {0};
                int type = 16;
                memcpy(ident, &type, 4);
                memcpy(ident + 8, &from, 8);
                push_cb(1251, ident, 160);
            }
        }
        if (g_nready < 4096) {
            Msg* m = &g_ready[g_nready++];
            m->from = from;
            m->channel = channel;
            m->size = (int)(n - 12);
            m->data = malloc(m->size);
            memcpy(m->data, buf + 12, m->size);
        }
    }
}

EXPORT bool SteamAPI_IsSteamRunning(void) { return true; }

EXPORT int SteamAPI_InitFlat(char* err) {
    (void)err;
    const char* id = getenv("MOCK_STEAM_ID");
    g_me = id ? strtoull(id, NULL, 10) : 76561190000000001ull;
    const char* d = getenv("MOCK_STEAM_DIR");
    if (d) snprintf(g_dir, sizeof g_dir, "%s", d);
    mkdir(g_dir, 0777);
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int big = 8 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_of(g_me));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_sock, (struct sockaddr*)&a, sizeof a) != 0) return 1;
    const char* inv = getenv("MOCK_STEAM_INVITE");
    if (inv) {
        g_invite_lobby = strtoull(inv, NULL, 10);
        g_invite_at = now() + 1.0;
    }
    fprintf(stderr, "[mock-steam] user %llu on port %d\n", (unsigned long long)g_me, port_of(g_me));
    return 0;
}

EXPORT int SteamAPI_GetHSteamPipe(void) { return 1; }
EXPORT void SteamAPI_ManualDispatch_Init(void) {}
EXPORT void SteamAPI_ManualDispatch_RunFrame(int pipe) {
    (void)pipe;
    pump();
    if (g_invite_lobby && now() >= g_invite_at) {
        uint8_t req[16] = {0};
        memcpy(req, &g_invite_lobby, 8);
        push_cb(333, req, 16);
        g_invite_lobby = 0;
    }
}
EXPORT bool SteamAPI_ManualDispatch_GetNextCallback(int pipe, void* out) {
    (void)pipe;
    if (g_ncb == 0) return false;
    g_current = g_cbs[0];
    memmove(g_cbs, g_cbs + 1, sizeof(Cb) * (size_t)(--g_ncb));
    struct { int32_t user; int32_t cb; uint8_t* param; int32_t size; } *m = out;
    m->user = 1;
    m->cb = g_current.id;
    m->param = g_current.data;
    m->size = g_current.size;
    return true;
}
EXPORT void SteamAPI_ManualDispatch_FreeLastCallback(int pipe) { (void)pipe; }
EXPORT bool SteamAPI_ManualDispatch_GetAPICallResult(int pipe, u64 call, void* buf, int size, int expected, bool* failed) {
    (void)pipe;
    for (int i = 0; i < g_nresults; ++i) {
        if (g_results[i].call != call) continue;
        if (g_results[i].id != expected || size < g_results[i].size) return false;
        memcpy(buf, g_results[i].data, g_results[i].size);
        *failed = false;
        g_results[i] = g_results[--g_nresults];
        return true;
    }
    return false;
}

EXPORT void* SteamAPI_SteamUser_v023(void) { return &g_dummy_iface; }
EXPORT void* SteamAPI_SteamMatchmaking_v009(void) { return &g_dummy_iface; }
EXPORT void* SteamAPI_SteamFriends_v018(void) { return &g_dummy_iface; }
EXPORT void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void) { return &g_dummy_iface; }
EXPORT void* SteamAPI_SteamNetworkingUtils_SteamAPI_v004(void) { return &g_dummy_iface; }
EXPORT u64 SteamAPI_ISteamUser_GetSteamID(void* s) { (void)s; return g_me; }

EXPORT u64 SteamAPI_ISteamMatchmaking_CreateLobby(void* s, int type, int max) {
    (void)s; (void)type; (void)max;
    g_lobby = 109775240000000000ull + (g_me % 100000);
    char p[300];
    path(p, sizeof p, "members", g_lobby);
    FILE* f = fopen(p, "w");
    if (f) { fprintf(f, "%llu\n", (unsigned long long)g_me); fclose(f); }
    path(p, sizeof p, "data", g_lobby);
    unlink(p);
    path(p, sizeof p, "playing", g_me);
    f = fopen(p, "w");
    if (f) { fprintf(f, "%llu\n", (unsigned long long)g_lobby); fclose(f); }
    uint8_t res[12] = {0};
    int ok = 1;
    memcpy(res, &ok, 4);
    memcpy(res + 4, &g_lobby, 8);   /* Linux packing: lobby at 4 */
    u64 call = push_result(513, res, 12);
    push_cb(513, res, 12);
    return call;
}

EXPORT u64 SteamAPI_ISteamMatchmaking_JoinLobby(void* s, u64 lobby) {
    (void)s;
    u64 members[8];
    int n;
    lobby_members(lobby, members, &n);
    uint8_t res[20] = {0};
    memcpy(res, &lobby, 8);
    uint32_t response = n > 0 && n < 2 ? 1 : 2;
    if (response == 1) {
        char p[300];
        path(p, sizeof p, "members", lobby);
        FILE* f = fopen(p, "a");
        if (f) { fprintf(f, "%llu\n", (unsigned long long)g_me); fclose(f); }
        g_lobby = lobby;
    }
    memcpy(res + 16, &response, 4);
    return push_result(504, res, 20);
}

EXPORT void SteamAPI_ISteamMatchmaking_LeaveLobby(void* s, u64 lobby) {
    (void)s;
    u64 members[8];
    int n;
    lobby_members(lobby, members, &n);
    char p[300];
    path(p, sizeof p, "members", lobby);
    FILE* f = fopen(p, "w");
    for (int i = 0; i < n; ++i) {
        if (members[i] == g_me) continue;
        if (f) fprintf(f, "%llu\n", (unsigned long long)members[i]);
        udp_send(members[i], -1, "", 0);
    }
    if (f) fclose(f);
    path(p, sizeof p, "playing", g_me);
    unlink(p);
    if (lobby == g_lobby) g_lobby = 0;
}

EXPORT bool SteamAPI_ISteamMatchmaking_SetLobbyData(void* s, u64 lobby, const char* k, const char* v) {
    (void)s;
    char p[300];
    path(p, sizeof p, "data", lobby);
    FILE* f = fopen(p, "a");
    if (!f) return false;
    fprintf(f, "%s=%s\n", k, v);
    fclose(f);
    return true;
}

EXPORT const char* SteamAPI_ISteamMatchmaking_GetLobbyData(void* s, u64 lobby, const char* k) {
    (void)s;
    static char value[256];
    value[0] = 0;
    char p[300], line[512];
    path(p, sizeof p, "data", lobby);
    FILE* f = fopen(p, "r");
    if (!f) return value;
    size_t kl = strlen(k);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, k, kl) == 0 && line[kl] == '=') {
            snprintf(value, sizeof value, "%s", line + kl + 1);
            value[strcspn(value, "\n")] = 0;
        }
    }
    fclose(f);
    return value;
}

EXPORT u64 SteamAPI_ISteamMatchmaking_GetLobbyOwner(void* s, u64 lobby) {
    (void)s;
    u64 m[8];
    int n;
    lobby_members(lobby, m, &n);
    return n > 0 ? m[0] : 0;
}
EXPORT int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void* s, u64 lobby) {
    (void)s;
    u64 m[8];
    int n;
    lobby_members(lobby, m, &n);
    return n;
}
EXPORT u64 SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void* s, u64 lobby, int i) {
    (void)s;
    u64 m[8];
    int n;
    lobby_members(lobby, m, &n);
    return i >= 0 && i < n ? m[i] : 0;
}

EXPORT bool SteamAPI_ISteamFriends_SetRichPresence(void* s, const char* k, const char* v) {
    (void)s;
    fprintf(stderr, "[mock-steam] rich presence %s = %s\n", k, v);
    return true;
}
EXPORT void SteamAPI_ISteamFriends_ClearRichPresence(void* s) { (void)s; }
EXPORT void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void* s, u64 lobby) {
    (void)s;
    fprintf(stderr, "[mock-steam] invite dialog for lobby %llu\n", (unsigned long long)lobby);
}
static int friends(u64* out) {
    const char* f = getenv("MOCK_STEAM_FRIENDS");
    int n = 0;
    while (f && *f && n < 16) {
        out[n++] = strtoull(f, (char**)&f, 10);
        if (*f == ',') ++f;
    }
    return n;
}
EXPORT int SteamAPI_ISteamFriends_GetFriendCount(void* s, int flags) {
    (void)s; (void)flags;
    u64 f[16];
    return friends(f);
}
EXPORT u64 SteamAPI_ISteamFriends_GetFriendByIndex(void* s, int i, int flags) {
    (void)s; (void)flags;
    u64 f[16];
    int n = friends(f);
    return i >= 0 && i < n ? f[i] : 0;
}
EXPORT bool SteamAPI_ISteamFriends_GetFriendGamePlayed(void* s, u64 fr, void* info) {
    (void)s;
    char p[300];
    path(p, sizeof p, "playing", fr);
    FILE* f = fopen(p, "r");
    if (!f) return false;
    unsigned long long lobby = 0;
    if (fscanf(f, "%llu", &lobby) != 1) lobby = 0;
    fclose(f);
    u64 game = 480;
    memcpy(info, &game, 8);
    memcpy((uint8_t*)info + 16, &lobby, 8);
    return true;
}

EXPORT void SteamAPI_SteamNetworkingIdentity_SetSteamID64(void* ident, u64 id) {
    int type = 16;
    memcpy(ident, &type, 4);
    memcpy((uint8_t*)ident + 8, &id, 8);
}
EXPORT u64 SteamAPI_SteamNetworkingIdentity_GetSteamID64(void* ident) {
    u64 id;
    memcpy(&id, (uint8_t*)ident + 8, 8);
    return id;
}

EXPORT int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void* s, const void* ident, const void* data, uint32_t size,
                                                               int flags, int channel) {
    (void)s; (void)flags;
    u64 to = SteamAPI_SteamNetworkingIdentity_GetSteamID64((void*)ident);
    if (size > 65000) return 25;
    if (!accepted(to) && g_naccepted < 16) g_accepted[g_naccepted++] = to;   /* talking first opens the session */
    udp_send(to, channel, data, (int)size);
    return 1;
}

EXPORT int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void* s, int channel, void** out, int max) {
    (void)s;
    pump();
    int got = 0;
    for (int i = 0; i < g_nready && got < max;) {
        Msg* m = &g_ready[i];
        if (m->channel != channel || !accepted(m->from)) { ++i; continue; }
        uint8_t* msg = calloc(1, 16 + 160);
        memcpy(msg, &m->data, 8);
        memcpy(msg + 8, &m->size, 4);
        SteamAPI_SteamNetworkingIdentity_SetSteamID64(msg + 16, m->from);
        out[got++] = msg;
        memmove(g_ready + i, g_ready + i + 1, sizeof(Msg) * (size_t)(g_nready - i - 1));
        --g_nready;
    }
    return got;
}

EXPORT bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void* s, const void* ident) {
    (void)s;
    u64 id = SteamAPI_SteamNetworkingIdentity_GetSteamID64((void*)ident);
    if (!accepted(id) && g_naccepted < 16) g_accepted[g_naccepted++] = id;
    return true;
}
EXPORT bool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void* s, const void* ident) {
    (void)s;
    u64 id = SteamAPI_SteamNetworkingIdentity_GetSteamID64((void*)ident);
    for (int i = 0; i < g_naccepted; ++i) {
        if (g_accepted[i] == id) g_accepted[i] = g_accepted[--g_naccepted];
    }
    for (int i = 0; i < g_nrequested; ++i) {
        if (g_requested[i] == id) g_requested[i] = g_requested[--g_nrequested];
    }
    return true;
}
EXPORT void SteamAPI_SteamNetworkingMessage_t_Release(void* msg) {
    uint8_t* m = msg;
    void* data;
    memcpy(&data, m, 8);
    free(data);
    free(m);
}
EXPORT bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(void* s, int k, int32_t v) {
    (void)s; (void)k; (void)v;
    return true;
}
