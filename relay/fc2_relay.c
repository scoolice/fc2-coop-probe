/*
fc2_relay: carries the probe's position packets between two players, next to the game. The C port of fc2_relay.py
(same flags, frames and log lines), so testers do not need Python. 64-bit, as steam_api64.dll is.

The probe (dinput8.dll in the game) keeps its own UDP transport and talks to this relay on 127.0.0.1:
`netstart <game port> 127.0.0.1 <relay port> <id>`. The relay forwards every packet to the other player's relay and
hands what comes back to the probe.

Transports:
  udp    the other relay is reached over UDP (default 127.0.0.1): a stand-in to test the whole chain with two game
         copies on one PC, without Steam.
  steam  Steam (AppID 480 "Spacewar"): friends-only lobby, invite, P2P messages through Valve's relay network by
         SteamID. No IP addresses, no port forwarding. Needs the Steam client running and a 64-bit steam_api64.dll
         with the flat API (Steamworks.NET Standalone 2025.164.1, SHA-256 eb17909a...fb75), loaded at run time.

  fc2_relay --transport udp --game-port 9101 --relay-port 9201 --udp-local 9301 --udp-peer 9302 [--udp-host <addr>]
  fc2_relay --transport steam --host --steam-dll <steam_api64.dll> [--invite "<friend name>"]
  fc2_relay --transport steam --join <lobby id>|friend|wait --steam-dll <steam_api64.dll>
  [--seconds <n>]: stop after n seconds
  fc2_relay --check --steam-dll <steam_api64.dll>: load the DLL, find every function and say whether Steam runs (no login)

Frames between relays: channel 0 = probe packets as they are (unreliable); channel 1 = control (b"ka" keep-alive
every second, b"bye"). A peer silent for 20 s is dropped.

Build (MSYS2 mingw64): gcc -O2 -Wall -o fc2_relay.exe fc2_relay.c -lws2_32 -lwinmm -static
*/
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#define KEEPALIVE_MS 1000
#define TIMEOUT_MS 20000
#define LOBBY_KEY "fc2coop"
#define LOBBY_VAL "1"
#define MAXPKT 2048
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static void logf_(const char *fmt, ...)
{
    char t[16];
    time_t now = time(NULL);
    va_list ap;
    strftime(t, sizeof t, "%H:%M:%S", localtime(&now));
    printf("%s ", t);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static volatile LONG g_stop;
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; g_stop = 1; return TRUE; }

/* a UDP socket bound to addr:port, non-blocking, without the ICMP "port unreachable" resets on later receives */
static SOCKET udp_socket(const char *addr, int port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    u_long nb = 1;
    BOOL off = FALSE;
    DWORD ret;
    if (s == INVALID_SOCKET) die("socket failed (%d)", WSAGetLastError());
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(addr);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) die("cannot bind %s:%d (%d)", addr, port, WSAGetLastError());
    ioctlsocket(s, FIONBIO, &nb);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    return s;
}

/* ---------------------------------------------------------------- transports */
typedef struct { int ch, len; unsigned char d[MAXPKT]; } Frame;

static struct {
    int steam;
    char peer_name[128];
    /* udp */
    SOCKET us;
    struct sockaddr_in upeer;
} T;

/* udp: frames to one fixed peer (1 byte channel + payload) */
static void udp_init(int local, const char *host, int port)
{
    T.us = udp_socket(strncmp(host, "127.", 4) == 0 ? "127.0.0.1" : "0.0.0.0", local);
    memset(&T.upeer, 0, sizeof T.upeer);
    T.upeer.sin_family = AF_INET; T.upeer.sin_port = htons((u_short)port); T.upeer.sin_addr.s_addr = inet_addr(host);
    snprintf(T.peer_name, sizeof T.peer_name, "%s:%d", host, port);
    logf_("udp transport: local %d -> peer %s", local, T.peer_name);
}

static void udp_send(int ch, const unsigned char *d, int n)
{
    unsigned char b[MAXPKT + 1];
    if (n > MAXPKT) return;
    b[0] = (unsigned char)ch; memcpy(b + 1, d, n);
    sendto(T.us, (const char *)b, n + 1, 0, (struct sockaddr *)&T.upeer, sizeof T.upeer);
}

static int udp_poll(Frame *out, int max)
{
    int k = 0;
    while (k < max) {
        unsigned char b[MAXPKT + 1];
        struct sockaddr_in from;
        int fl = sizeof from, r = recvfrom(T.us, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) { if (WSAGetLastError() == WSAECONNRESET) continue; break; }
        if (r < 1 || from.sin_addr.s_addr != T.upeer.sin_addr.s_addr) continue;
        out[k].ch = b[0]; out[k].len = r - 1; memcpy(out[k].d, b + 1, r - 1); k++;
    }
    return k;
}

/* steam: lobby + ISteamNetworkingMessages through the flat C API */
#pragma pack(push, 8)
typedef struct { int32_t user, callback; void *param; int32_t param_size; } CallbackMsg;
typedef struct { uint64_t game_id; uint32_t ip; uint16_t game_port, query_port; uint64_t lobby; } FriendGameInfo;
#pragma pack(pop)
typedef unsigned char sbool;  /* C++ bool in the flat API */

static struct {
    sbool (*IsSteamRunning)(void);
    int32_t (*InitFlat)(char *err);
    void (*Shutdown)(void);
    int32_t (*GetHSteamPipe)(void);
    void (*MD_Init)(void);
    void (*MD_RunFrame)(int32_t);
    sbool (*MD_Next)(int32_t, CallbackMsg *);
    void (*MD_Free)(int32_t);
    sbool (*MD_CallResult)(int32_t, uint64_t, void *, int32_t, int32_t, sbool *);
    void *(*User)(void);
    void *(*MM)(void);
    void *(*Friends)(void);
    void *(*NM)(void);
    uint64_t (*GetSteamID)(void *);
    sbool (*BLoggedOn)(void *);
    uint64_t (*CreateLobby)(void *, int32_t, int32_t);
    uint64_t (*JoinLobby)(void *, uint64_t);
    void (*LeaveLobby)(void *, uint64_t);
    sbool (*SetLobbyData)(void *, uint64_t, const char *, const char *);
    const char *(*GetLobbyData)(void *, uint64_t, const char *);
    int32_t (*NumMembers)(void *, uint64_t);
    uint64_t (*MemberByIndex)(void *, uint64_t, int32_t);
    sbool (*InviteUserToLobby)(void *, uint64_t, uint64_t);
    sbool (*SetRichPresence)(void *, const char *, const char *);
    void (*ClearRichPresence)(void *);
    int32_t (*FriendCount)(void *, int32_t);
    uint64_t (*FriendByIndex)(void *, int32_t, int32_t);
    const char *(*FriendName)(void *, uint64_t);
    sbool (*FriendGame)(void *, uint64_t, FriendGameInfo *);
    void (*IdSet)(void *, uint64_t);
    uint64_t (*IdGet)(const void *);
    int32_t (*Send)(void *, const void *, const void *, uint32_t, int32_t, int32_t);
    int32_t (*Recv)(void *, int32_t, void **, int32_t);
    sbool (*Accept)(void *, const void *);
    void (*MsgRelease)(void *);
} F;

static struct {
    int32_t pipe;
    void *user, *mm, *fr, *nm;
    uint64_t me, lobby, peer;
    int host, logged_first;
    char invite[128];
    unsigned char ident[160];
} S;

static void load(HMODULE h, void **slot, const char *name)
{
    *slot = (void *)GetProcAddress(h, name);
    if (!*slot) die("steam transport: %s not in steam_api64.dll", name);
}

static void steam_bind(const char *dll)
{
    HMODULE h = LoadLibraryA(dll);
    if (!h) die("steam transport: cannot load %s (error %lu)", dll, GetLastError());
#define L(field, name) load(h, (void **)&F.field, name)
    L(IsSteamRunning, "SteamAPI_IsSteamRunning"); L(InitFlat, "SteamAPI_InitFlat"); L(Shutdown, "SteamAPI_Shutdown");
    L(GetHSteamPipe, "SteamAPI_GetHSteamPipe"); L(MD_Init, "SteamAPI_ManualDispatch_Init");
    L(MD_RunFrame, "SteamAPI_ManualDispatch_RunFrame"); L(MD_Next, "SteamAPI_ManualDispatch_GetNextCallback");
    L(MD_Free, "SteamAPI_ManualDispatch_FreeLastCallback"); L(MD_CallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
    L(User, "SteamAPI_SteamUser_v023"); L(MM, "SteamAPI_SteamMatchmaking_v009"); L(Friends, "SteamAPI_SteamFriends_v018");
    L(NM, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"); L(GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    L(BLoggedOn, "SteamAPI_ISteamUser_BLoggedOn"); L(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    L(JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby"); L(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    L(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData"); L(GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    L(NumMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    L(MemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    L(InviteUserToLobby, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    L(SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence"); L(ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence");
    L(FriendCount, "SteamAPI_ISteamFriends_GetFriendCount"); L(FriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    L(FriendName, "SteamAPI_ISteamFriends_GetFriendPersonaName"); L(FriendGame, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    L(IdSet, "SteamAPI_SteamNetworkingIdentity_SetSteamID64"); L(IdGet, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    L(Send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    L(Recv, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    L(Accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"); L(MsgRelease, "SteamAPI_SteamNetworkingMessage_t_Release");
#undef L
}

static uint64_t find_friend_lobby(void)
{
    FriendGameInfo info;
    int i, n = F.FriendCount(S.fr, 4);  /* 4 = immediate friends */
    for (i = 0; i < n; i++) {
        uint64_t fid = F.FriendByIndex(S.fr, i, 4);
        memset(&info, 0, sizeof info);
        if (F.FriendGame(S.fr, fid, &info) && (info.game_id & 0xFFFFFF) == 480 && info.lobby) {
            logf_("steam: friend %s is in a Spacewar lobby", F.FriendName(S.fr, fid));
            return info.lobby;
        }
    }
    return 0;
}

static void *identity(uint64_t id) { memset(S.ident, 0, sizeof S.ident); F.IdSet(S.ident, id); return S.ident; }

static int is_member(uint64_t id)
{
    int i, n;
    if (!S.lobby) return 0;
    n = F.NumMembers(S.mm, S.lobby);
    for (i = 0; i < n; i++) if (F.MemberByIndex(S.mm, S.lobby, i) == id) return 1;
    return 0;
}

static void pick_peer(void)
{
    int i, n = F.NumMembers(S.mm, S.lobby);
    for (i = 0; i < n; i++) {
        uint64_t m = F.MemberByIndex(S.mm, S.lobby, i);
        if (m != S.me) {
            if (m != S.peer) {
                S.peer = m;
                snprintf(T.peer_name, sizeof T.peer_name, "%s", F.FriendName(S.fr, m));
                logf_("steam: peer is %s", T.peer_name);
            }
            return;
        }
    }
    if (S.peer) logf_("steam: peer left the lobby");
    S.peer = 0; strcpy(T.peer_name, "(none yet)");
}

static void invite_friend(const char *name)
{
    int i, n = F.FriendCount(S.fr, 4);
    for (i = 0; i < n; i++) {
        uint64_t fid = F.FriendByIndex(S.fr, i, 4);
        if (_stricmp(F.FriendName(S.fr, fid), name) == 0) {
            logf_("steam: invite to %s sent: %s", name, F.InviteUserToLobby(S.mm, S.lobby, fid) ? "True" : "False");
            return;
        }
    }
    logf_("steam: no friend named '%s'", name);
}

static void join(uint64_t lobby) { F.JoinLobby(S.mm, lobby); }

static void on_callback(int cb, unsigned char *param)
{
    if (cb == 513) {  /* LobbyCreated: arrives twice on real Steam (call result + broadcast) */
        int32_t result = *(int32_t *)param;
        uint64_t lobby = *(uint64_t *)(param + 8);
        if (result == 1 && lobby && lobby != S.lobby) {
            char conn[64];
            S.lobby = lobby;
            F.SetLobbyData(S.mm, lobby, LOBBY_KEY, LOBBY_VAL);
            snprintf(conn, sizeof conn, "+connect_lobby %llu", (unsigned long long)lobby);
            F.SetRichPresence(S.fr, "connect", conn);
            logf_("steam: LOBBY %llu created (friends-only); friend joins with --join %llu or --join friend",
                  (unsigned long long)lobby, (unsigned long long)lobby);
            if (S.invite[0]) invite_friend(S.invite);
        } else if (result != 1) logf_("steam: lobby creation failed, result %d", result);
    } else if (cb == 504) {  /* LobbyEnter: lobby at 0, response at 16 */
        uint64_t lobby = *(uint64_t *)param;
        uint32_t response = *(uint32_t *)(param + 16);
        if (response != 1) { logf_("steam: could not enter lobby %llu (response %u)", (unsigned long long)lobby, response); return; }
        if (!S.host) {
            const char *tag = F.GetLobbyData(S.mm, lobby, LOBBY_KEY);
            if (!tag || strcmp(tag, LOBBY_VAL) != 0) {
                logf_("steam: lobby %llu is not an fc2coop lobby; leaving", (unsigned long long)lobby);
                F.LeaveLobby(S.mm, lobby);
                return;
            }
        }
        S.lobby = lobby;
        logf_("steam: entered lobby %llu", (unsigned long long)lobby);
        pick_peer();
    } else if (cb == 506) {  /* LobbyChatUpdate: someone joined or left */
        pick_peer();
    } else if (cb == 1251) {  /* SteamNetworkingMessagesSessionRequest: identity at 0 */
        if (is_member(F.IdGet(param))) { F.Accept(S.nm, param); logf_("steam: accepted session from a lobby member"); }
        else logf_("steam: ignored a session request from someone not in the lobby");
    } else if (cb == 1252) {
        logf_("steam: session failed");
    } else if (cb == 333) {  /* GameLobbyJoinRequested: lobby at 0, friend at 8 */
        uint64_t lobby = *(uint64_t *)param;
        if (lobby == S.lobby) return;
        logf_("steam: invite or Join Game accepted (lobby), joining lobby %llu", (unsigned long long)lobby);
        join(lobby);
    } else if (cb == 337) {  /* GameRichPresenceJoinRequested: friend at 0, connect string (char[256]) at 8 */
        char connect[257];
        unsigned long long lobby;
        char tail;
        memcpy(connect, param + 8, 256); connect[256] = 0;
        if (sscanf(connect, "+connect_lobby %llu%c", &lobby, &tail) == 1) {
            if (lobby != S.lobby) { logf_("steam: Join Game from the friends list, joining lobby %llu", lobby); join(lobby); }
        } else logf_("steam: Join Game with an unknown connect string '%s', ignored", connect);
    }
}

static void steam_init(const char *dll, int host, const char *joinarg, const char *invite)
{
    char err[1024] = "";
    FILE *f;
    int32_t r;
    if (sizeof(void *) != 8) die("steam transport: needs a 64-bit build (steam_api64.dll)");
    if (GetFileAttributesA("steam_appid.txt") == INVALID_FILE_ATTRIBUTES && (f = fopen("steam_appid.txt", "w"))) { fputs("480", f); fclose(f); }
    steam_bind(dll);
    if (!F.IsSteamRunning()) die("steam transport: the Steam client is not running");
    r = F.InitFlat(err);
    if (r != 0) die("steam transport: SteamAPI_InitFlat failed (%d): %s", r, err);
    F.MD_Init();
    S.pipe = F.GetHSteamPipe();
    S.user = F.User(); S.mm = F.MM(); S.fr = F.Friends(); S.nm = F.NM();
    S.me = F.GetSteamID(S.user);
    logf_("steam: init ok, logged on=%s, own id ...%04llu", F.BLoggedOn(S.user) ? "True" : "False", (unsigned long long)(S.me % 10000));
    S.host = host;
    T.steam = 1;
    strcpy(T.peer_name, "(none yet)");
    if (invite) snprintf(S.invite, sizeof S.invite, "%s", invite);
    if (host) {
        F.CreateLobby(S.mm, 1, 2);  /* 1 = friends-only, 2 players */
        logf_("steam: creating friends-only lobby ...");
    } else if (strcmp(joinarg, "friend") == 0) {
        uint64_t lobby = find_friend_lobby();
        if (!lobby) die("steam transport: no friend is hosting an fc2coop lobby right now");
        join(lobby);
    } else if (strcmp(joinarg, "wait") == 0) {
        /* joined later by on_callback: an accepted invite (333) or Join Game in the friends list (337) */
        logf_("steam: waiting for a Steam invite or Join Game from the friends list ...");
    } else {
        join(_strtoui64(joinarg, NULL, 10));
        logf_("steam: joining lobby %s ...", joinarg);
    }
}

static void steam_send(int ch, const unsigned char *d, int n, int reliable)
{
    if (!S.peer) return;
    F.Send(S.nm, identity(S.peer), d, (uint32_t)n, reliable ? 8 | 1 : 0 | 1, ch);  /* Reliable 8 / Unreliable 0, + NoNagle 1 */
}

static int steam_poll(Frame *out, int max)
{
    CallbackMsg msg;
    void *msgs[32];
    int k = 0, ch, i, n;
    F.MD_RunFrame(S.pipe);
    while (F.MD_Next(S.pipe, &msg)) {
        if (msg.callback == 703) {  /* SteamAPICallCompleted: call (u64), callback (i32), size (u32) */
            unsigned char *p = msg.param;
            uint64_t call = *(uint64_t *)p;
            int32_t cb = *(int32_t *)(p + 8);
            uint32_t size = *(uint32_t *)(p + 12);
            unsigned char *buf = calloc(1, size ? size : 1);
            sbool failed = 0;
            if (buf && F.MD_CallResult(S.pipe, call, buf, (int32_t)size, cb, &failed) && !failed) on_callback(cb, buf);
            free(buf);
        } else on_callback(msg.callback, msg.param);
        F.MD_Free(S.pipe);
    }
    for (ch = 0; ch < 2; ch++) {
        n = F.Recv(S.nm, ch, msgs, 32);
        for (i = 0; i < n; i++) {
            unsigned char *m = msgs[i];
            void *data = *(void **)m;
            int32_t size = *(int32_t *)(m + 8);  /* x64: size at 8, sender identity at 16 */
            uint64_t sender = F.IdGet(m + 16);
            if (!S.logged_first) { S.logged_first = 1; logf_("steam: first message, sender identity read at offset 16 (x64)"); }
            if (sender == S.peer && size > 0 && size <= MAXPKT && k < max) { out[k].ch = ch; out[k].len = size; memcpy(out[k].d, data, size); k++; }
            F.MsgRelease(m);
        }
    }
    return k;
}

static int tr_ready(void) { return T.steam ? S.peer != 0 : 1; }
static void tr_send(int ch, const unsigned char *d, int n, int reliable) { if (T.steam) steam_send(ch, d, n, reliable); else udp_send(ch, d, n); }
static int tr_poll(Frame *out, int max) { return T.steam ? steam_poll(out, max) : udp_poll(out, max); }
static void tr_close(void)
{
    tr_send(1, (const unsigned char *)"bye", 3, 1);
    if (T.steam) {
        if (S.lobby) F.LeaveLobby(S.mm, S.lobby);
        F.ClearRichPresence(S.fr);
        F.Shutdown();
    } else closesocket(T.us);
}

/* ---------------------------------------------------------------- relay loop */
static const char *arg(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i < argc - 1; i++) if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}
static int flag(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i < argc; i++) if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *transport = arg(argc, argv, "--transport"), *gp = arg(argc, argv, "--game-port"), *rp = arg(argc, argv, "--relay-port");
    const char *dll = arg(argc, argv, "--steam-dll"), *joinarg = arg(argc, argv, "--join"), *secs = arg(argc, argv, "--seconds");
    const char *uhost = arg(argc, argv, "--udp-host"), *ul = arg(argc, argv, "--udp-local"), *up = arg(argc, argv, "--udp-peer");
    int host = flag(argc, argv, "--host"), peer_alive = 0, game_port, relay_port;
    unsigned from_game = 0, to_peer = 0, from_peer = 0, to_game = 0, dropped = 0;
    ULONGLONG t0, last_ka, last_heard, last_stat, limit;
    struct sockaddr_in gaddr;
    static Frame fr[64];
    SOCKET game;
    WSADATA w;

    if (flag(argc, argv, "--check")) {
        if (!dll) die("--check needs --steam-dll");
        steam_bind(dll);
        logf_("check: %s loaded, all functions found, Steam client %s", dll, F.IsSteamRunning() ? "running" : "NOT running");
        return 0;
    }
    if (!transport || !gp || !rp || (strcmp(transport, "udp") && strcmp(transport, "steam")))
        die("usage: fc2_relay --transport udp|steam --game-port <n> --relay-port <n> [udp: --udp-local <n> --udp-peer <n> [--udp-host <addr>]]\n"
            "       [steam: --steam-dll <steam_api64.dll> --host [--invite <name>] | --join <lobby id>|friend|wait] [--seconds <n>]");
    if (host && joinarg) die("--host and --join exclude each other");
    game_port = atoi(gp); relay_port = atoi(rp);
    limit = secs ? (ULONGLONG)(atof(secs) * 1000) : 0;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) die("WSAStartup failed");
    timeBeginPeriod(1);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (strcmp(transport, "udp") == 0) {
        if (!ul || !up) die("udp transport needs --udp-local and --udp-peer");
        udp_init(atoi(ul), uhost ? uhost : "127.0.0.1", atoi(up));
    } else {
        if (!dll || !(host || joinarg)) die("steam transport needs --steam-dll and --host or --join");
        steam_init(dll, host, joinarg, arg(argc, argv, "--invite"));
    }

    game = udp_socket("127.0.0.1", relay_port);
    memset(&gaddr, 0, sizeof gaddr);
    gaddr.sin_family = AF_INET; gaddr.sin_port = htons((u_short)game_port); gaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    logf_("relay up: probe 127.0.0.1:%d <-> relay 127.0.0.1:%d <-> %s", game_port, relay_port, transport);

    t0 = last_ka = last_heard = last_stat = GetTickCount64();
    while (!g_stop && (!limit || GetTickCount64() - t0 < limit)) {
        ULONGLONG now = GetTickCount64();
        int i, n;
        /* probe -> peer */
        for (;;) {
            unsigned char d[MAXPKT];
            struct sockaddr_in from;
            int fl = sizeof from, r = recvfrom(game, (char *)d, sizeof d, 0, (struct sockaddr *)&from, &fl);
            if (r < 0) break;
            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { dropped++; continue; }
            from_game++;
            if (tr_ready()) { tr_send(0, d, r, 0); to_peer++; }
        }
        /* peer -> probe */
        n = tr_poll(fr, 64);
        for (i = 0; i < n; i++) {
            last_heard = now;
            if (!peer_alive) { peer_alive = 1; logf_("peer is talking (%s)", T.peer_name); }
            if (fr[i].ch == 0) {
                from_peer++;
                if (sendto(game, (const char *)fr[i].d, fr[i].len, 0, (struct sockaddr *)&gaddr, sizeof gaddr) >= 0) to_game++;
            } else if (fr[i].len == 3 && memcmp(fr[i].d, "bye", 3) == 0) {
                logf_("peer said goodbye");
                peer_alive = 0;
            }
        }
        if (now - last_ka >= KEEPALIVE_MS && tr_ready()) { tr_send(1, (const unsigned char *)"ka", 2, 1); last_ka = now; }
        if (peer_alive && now - last_heard > TIMEOUT_MS) { logf_("peer silent for %d s, dropped", TIMEOUT_MS / 1000); peer_alive = 0; }
        if (now - last_stat >= 5000) {
            last_stat = now;
            logf_("stats from_game=%u to_peer=%u from_peer=%u to_game=%u dropped=%u peer=%s", from_game, to_peer, from_peer, to_game, dropped, peer_alive ? "up" : "down");
        }
        Sleep(2);
    }
    logf_("stopping; from_game=%u to_peer=%u from_peer=%u to_game=%u dropped=%u", from_game, to_peer, from_peer, to_game, dropped);
    tr_close();
    closesocket(game);
    timeEndPeriod(1);
    WSACleanup();
    return 0;
}
