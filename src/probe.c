/*
 * fc2-coop-probe: a dinput8.dll proxy for TEST COPIES of Far Cry 2 (Steam 1.03, Dunia engine).
 * Logs network, file and exit calls of Dunia.dll, runs Lua 4.0 and native commands inside the
 * game, finds the player's Havok body and exchanges its position between two copies over its own
 * UDP socket (research prototype; see README.md). MIT licence, see LICENSE.
 * Addresses (RVAs) below are for Dunia.dll SHA-256 6353936a...2765 only.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

static FILE *g_log;
static CRITICAL_SECTION g_cs;
static HMODULE g_self;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "[%8lu T%04lu] ", GetTickCount(), GetCurrentThreadId());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void addr_str(const struct sockaddr *a, char *out, size_t n)
{
    if (a && a->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)a;
        snprintf(out, n, "%s:%u", inet_ntoa(s->sin_addr), ntohs(s->sin_port));
    } else {
        snprintf(out, n, "af%d", a ? a->sa_family : -1);
    }
}

static void hex_str(const char *b, int len, char *out, size_t n)
{
    int i, m = len < 16 ? len : 16;
    size_t o = 0;
    for (i = 0; i < m && o + 3 < n; i++) o += snprintf(out + o, n - o, "%02x", (unsigned char)b[i]);
    out[o] = 0;
}

#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#endif
WINBASEAPI BOOL WINAPI GetModuleHandleExA(DWORD, LPCSTR, HMODULE *);
static const char *caller_str(void *a, char *out, size_t n)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)
        && GetModuleFileNameA(m, path, MAX_PATH)) {
        char *s = strrchr(path, '\\');
        snprintf(out, n, "%s+0x%lx", s ? s + 1 : path, (unsigned long)((BYTE *)a - (BYTE *)m));
    } else {
        snprintf(out, n, "%p", a);
    }
    return out;
}
#define CALLER(buf) caller_str(__builtin_return_address(0), buf, sizeof buf)

/* originals */
static int (WINAPI *o_WSASendTo)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static int (WINAPI *o_WSARecvFrom)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, struct sockaddr *, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static int (WINAPI *o_getaddrinfo)(const char *, const char *, const void *, void **);
static struct hostent *(WINAPI *o_gethostbyname)(const char *);
static int (WINAPI *o_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WINAPI *o_recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static int (WINAPI *o_bind)(SOCKET, const struct sockaddr *, int);
static int (WINAPI *o_connect)(SOCKET, const struct sockaddr *, int);
static VOID (WINAPI *o_ExitProcess)(UINT);
static VOID (WINAPI *o_PostQuitMessage)(int);

static int WINAPI h_sendto(SOCKET s, const char *b, int l, int f, const struct sockaddr *to, int tl)
{
    char a[40], h[40];
    int r = o_sendto(s, b, l, f, to, tl);
    addr_str(to, a, sizeof a); hex_str(b, l, h, sizeof h);
    logf_("sendto s=%u -> %s len=%d ret=%d data=%s", (unsigned)s, a, l, r, h);
    return r;
}
static int WINAPI h_recvfrom(SOCKET s, char *b, int l, int f, struct sockaddr *from, int *fl)
{
    int r = o_recvfrom(s, b, l, f, from, fl);
    if (r > 0) {
        char a[40], h[40];
        addr_str(from, a, sizeof a); hex_str(b, r, h, sizeof h);
        logf_("recvfrom s=%u <- %s len=%d data=%s", (unsigned)s, a, r, h);
    }
    return r;
}
static int WINAPI h_WSASendTo(SOCKET s, LPWSABUF b, DWORD c, LPDWORD sent, DWORD f, const struct sockaddr *to, int tl, LPWSAOVERLAPPED o, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    char a[40], h[40] = "", cs[96];
    int r = o_WSASendTo(s, b, c, sent, f, to, tl, o, cr);
    addr_str(to, a, sizeof a);
    if (c) hex_str(b[0].buf, (int)b[0].len, h, sizeof h);
    logf_("WSASendTo s=%u -> %s len=%lu ret=%d data=%s from %s", (unsigned)s, a, c ? b[0].len : 0, r, h, CALLER(cs));
    return r;
}
static int WINAPI h_WSARecvFrom(SOCKET s, LPWSABUF b, DWORD c, LPDWORD got, LPDWORD f, struct sockaddr *from, LPINT fl, LPWSAOVERLAPPED o, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = o_WSARecvFrom(s, b, c, got, f, from, fl, o, cr);
    if (r == 0 && got && *got) {
        char a[40], h[40];
        addr_str(from, a, sizeof a); hex_str(b[0].buf, (int)*got, h, sizeof h);
        logf_("WSARecvFrom s=%u <- %s len=%lu data=%s", (unsigned)s, a, *got, h);
    }
    return r;
}
static int WINAPI h_getaddrinfo(const char *n, const char *sv, const void *hints, void **res)
{
    char cs[96];
    int r = o_getaddrinfo(n, sv, hints, res);
    logf_("getaddrinfo(%s, %s) ret=%d from %s", n ? n : "(null)", sv ? sv : "(null)", r, CALLER(cs));
    return r;
}
static struct hostent *WINAPI h_gethostbyname(const char *n)
{
    char cs[96];
    struct hostent *r = o_gethostbyname(n);
    logf_("gethostbyname(%s) ret=%s from %s", n ? n : "(null)", r ? "ok" : "null", CALLER(cs));
    return r;
}
static int WINAPI h_bind(SOCKET s, const struct sockaddr *n, int l)
{
    char a[40];
    int r = o_bind(s, n, l);
    addr_str(n, a, sizeof a);
    logf_("bind s=%u %s ret=%d", (unsigned)s, a, r);
    return r;
}
static int WINAPI h_connect(SOCKET s, const struct sockaddr *n, int l)
{
    char a[40], cs[96];
    int r = o_connect(s, n, l);
    addr_str(n, a, sizeof a);
    logf_("connect s=%u %s ret=%d from %s", (unsigned)s, a, r, CALLER(cs));
    return r;
}
static VOID WINAPI h_ExitProcess(UINT c)
{
    logf_("ExitProcess(%u) caller=%p", c, __builtin_return_address(0));
    o_ExitProcess(c);
}
static VOID WINAPI h_PostQuitMessage(int c)
{
    logf_("PostQuitMessage(%d) caller=%p", c, __builtin_return_address(0));
    o_PostQuitMessage(c);
}

static BOOL (WINAPI *o_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *o_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI h_PeekMessageA(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI h_PeekMessageW(LPMSG, HWND, UINT, UINT, UINT);

static HANDLE (WINAPI *o_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *o_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI h_CreateFileA(LPCSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    HANDLE h = o_CreateFileA(n, a, sh, sa, cd, fl, t);
    if (n && (strstr(n, "av") || strstr(n, "Saved") || strstr(n, "Documents") || strstr(n, "Profile"))) logf_("CreateFileA %s acc=%lx disp=%lu -> %s", n, a, cd, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return h;
}
static HANDLE WINAPI h_CreateFileW(LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    HANDLE h = o_CreateFileW(n, a, sh, sa, cd, fl, t);
    if (n && (wcsstr(n, L"av") || wcsstr(n, L"Saved") || wcsstr(n, L"Documents") || wcsstr(n, L"Profile"))) logf_("CreateFileW %ls acc=%lx disp=%lu -> %s", n, a, cd, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return h;
}

typedef struct { const char *dll, *name; void **orig; void *hook; } Target;
static Target g_targets[] = {
    { "ws2_32.dll", "sendto", (void **)&o_sendto, h_sendto },
    { "ws2_32.dll", "recvfrom", (void **)&o_recvfrom, h_recvfrom },
    { "ws2_32.dll", "bind", (void **)&o_bind, h_bind },
    { "ws2_32.dll", "connect", (void **)&o_connect, h_connect },
    { "ws2_32.dll", "WSASendTo", (void **)&o_WSASendTo, h_WSASendTo },
    { "ws2_32.dll", "WSARecvFrom", (void **)&o_WSARecvFrom, h_WSARecvFrom },
    { "ws2_32.dll", "getaddrinfo", (void **)&o_getaddrinfo, h_getaddrinfo },
    { "ws2_32.dll", "gethostbyname", (void **)&o_gethostbyname, h_gethostbyname },
    { "user32.dll", "PeekMessageA", (void **)&o_PeekMessageA, h_PeekMessageA },
    { "user32.dll", "PeekMessageW", (void **)&o_PeekMessageW, h_PeekMessageW },
    { "kernel32.dll", "CreateFileA", (void **)&o_CreateFileA, h_CreateFileA },
    { "kernel32.dll", "CreateFileW", (void **)&o_CreateFileW, h_CreateFileW },
    { "kernel32.dll", "ExitProcess", (void **)&o_ExitProcess, h_ExitProcess },
    { "user32.dll", "PostQuitMessage", (void **)&o_PostQuitMessage, h_PostQuitMessage },
};
#define NT (sizeof g_targets / sizeof g_targets[0])

static int patch_iat(HMODULE m)
{
    BYTE *base = (BYTE *)m;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);
    void *want[NT];
    size_t i;
    int patched = 0;
    for (i = 0; i < NT; i++) {
        HMODULE h = GetModuleHandleA(g_targets[i].dll);
        if (!h) h = LoadLibraryA(g_targets[i].dll);
        want[i] = h ? (void *)GetProcAddress(h, g_targets[i].name) : NULL;
        *g_targets[i].orig = want[i];
    }
    for (; d->Name; d++) {
        void **t = (void **)(base + d->FirstThunk);
        for (; *t; t++) {
            for (i = 0; i < NT; i++) {
                if (want[i] && *t == want[i]) {
                    DWORD old;
                    VirtualProtect(t, sizeof(void *), PAGE_READWRITE, &old);
                    *t = g_targets[i].hook;
                    VirtualProtect(t, sizeof(void *), old, &old);
                    logf_("hooked %s via IAT of %s", g_targets[i].name, (char *)(base + d->Name));
                    patched++;
                }
            }
        }
    }
    return patched;
}

/* ---- Lua 4.0 access (Dunia.dll, RVAs from the Steam v1.03 build) ---- */
#define RVA_GETTOP    0x2aa9c0
#define RVA_SETTOP    0x2aa9e0
#define RVA_DOBUFFER  0x2ab600
#define RVA_CHECKLSTR 0x2d24b0
#define RVA_ALERT     0x2c3500
#define RVA_GETGLOBAL 0x2aaf90

static BYTE *g_base;
static int (__cdecl *o_gettop)(void *);
static int (__cdecl *lua_settop_)(void *, int);
static int (__cdecl *lua_dobuffer_)(void *, const char *, size_t, const char *);
static const char *(__cdecl *lua_checklstr_)(void *, int, size_t *);
static void (__cdecl *lua_getglobal_)(void *, const char *);

#define MAXL 8
static struct { void *L; unsigned long thread; unsigned long hits; } g_states[MAXL];
static int g_nstates;
static char g_cmdpath[MAX_PATH];
static volatile LONG g_busy;
static DWORD g_lastpoll;

static void *make_detour(BYTE *target, int stolen, void *hook, int keep_original)
{
    DWORD old;
    BYTE *tramp = NULL;
    if (keep_original) {
        tramp = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(tramp, target, stolen);
        tramp[stolen] = 0xE9;
        *(DWORD *)(tramp + stolen + 1) = (DWORD)(target + stolen) - (DWORD)(tramp + stolen + 5);
    }
    VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(DWORD *)(target + 1) = (DWORD)hook - (DWORD)(target + 5);
    VirtualProtect(target, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 8);
    return tramp;
}

static int __cdecl h_alert(void *L)
{
    const char *s = lua_checklstr_(L, 1, NULL);
    logf_("LUA: %s", s ? s : "(nil)");
    return 0;
}

static void run_cmd(void *L)
{
    FILE *f = fopen(g_cmdpath, "rb");
    long n;
    char *buf;
    int top, st;
    if (!f) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    buf[n] = 0;
    remove(g_cmdpath);
    top = o_gettop(L);
    lua_dobuffer_(L, "__out = ''", 10, "coop_pre");
    st = lua_dobuffer_(L, buf, (size_t)n, "coop_cmd");
    lua_getglobal_(L, "__out");
    {
        size_t ol = 0;
        const char *o = lua_checklstr_(L, o_gettop(L), &ol);
        logf_("cmd executed (%ld bytes) status=%d out(%u)=%s", n, st, (unsigned)ol, o ? o : "(null)");
    }
    lua_settop_(L, top);
    free(buf);
}


static int lua_eval(const char *code, char *out, size_t on)
{
    void *L;
    int top, st;
    size_t ol = 0;
    const char *o;
    out[0] = 0;
    if (g_nstates == 0) return -1;
    L = g_states[0].L;
    top = o_gettop(L);
    lua_dobuffer_(L, "__out = ''", 10, "coop_pre");
    st = lua_dobuffer_(L, code, strlen(code), "coop_eval");
    lua_getglobal_(L, "__out");
    o = lua_checklstr_(L, o_gettop(L), &ol);
    if (o) { strncpy(out, o, on - 1); out[on - 1] = 0; }
    lua_settop_(L, top);
    return st;
}


/* ---- native peek commands (file coop_native.txt, run on the main thread) ---- */
#define RVA_ENTMGR_PTR 0x1644e80
#define RVA_GETENTITY  0x55820
typedef void *(__attribute__((thiscall)) *getent_t)(void *mgr, void **out, unsigned lo, unsigned hi);
#define RVA_BODY_VTABLE 0xE67658
static BYTE *g_body, *g_bodymat;
static BYTE **g_av_body_p;
static int g_walk;  /* 1 = circle (walk), 2 = straight legs with pauses (walklegs) */
static float g_walk_c[3], g_walk_r, g_walk_secs, g_walk_speed;
static DWORD g_walk_t0;
#define MAXLEGS 8
static float g_legs[MAXLEGS][2], g_leg_pause;
static int g_nlegs;
static int g_net_cfg;  /* 1 = start requested, 2 = running */
static unsigned g_net_lport, g_net_pport;
static char g_net_pip[32];
static unsigned long long g_net_id;
static char g_natpath[MAX_PATH];
static int g_hold;
static unsigned long long g_hold_id;
static float g_hold_pos[3];
static unsigned long g_hold_ticks;

static void hexdump_(const BYTE *p, int len)
{
    int i, j;
    for (i = 0; i < len; i += 16) {
        char line[256];
        int o = snprintf(line, sizeof line, "%08lx:", (unsigned long)(p + i));
        for (j = 0; j < 16 && i + j + 4 <= len; j += 4) {
            unsigned v = *(const unsigned *)(p + i + j);
            float f = *(const float *)(p + i + j);
            if (f > -1e6f && f < 1e6f && (f > 1e-3f || f < -1e-3f)) o += snprintf(line + o, sizeof line - o, " %08x(%.3f)", v, f);
            else o += snprintf(line + o, sizeof line - o, " %08x", v);
        }
        logf_("%s", line);
    }
}


static int safe_read(const void *addr, void *buf, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got) && got == n;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char mod[96];
    caller_str(ep->ExceptionRecord->ExceptionAddress, mod, sizeof mod);
    logf_("CRASH: code %08lx at %s (access address %p)", (unsigned long)ep->ExceptionRecord->ExceptionCode, mod,
          ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER);
static LONG WINAPI veh_filter(EXCEPTION_POINTERS *ep)
{
    static int n;
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && n < 30) {
        char mod[96];
        n++;
        caller_str(ep->ExceptionRecord->ExceptionAddress, mod, sizeof mod);
        logf_("ACCESS VIOLATION #%d at %s, %s address %p", n, mod, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
              (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BYTE *entity_obj(unsigned long long id, void **holder_out);
static int ai_cmd(const char *ln);
static int veh_cmd(const char *ln);
static int wpn_cmd(const char *ln);
/* exclude: a body to skip (the local player's when binding the avatar: both stand on the same spot
 * after loading the same save, so the nearest body would otherwise be the player's own) */
static int find_body(unsigned long long id, BYTE **body_out, BYTE **mat_out, const BYTE *exclude)
{
    void *holder = NULL;
    BYTE *obj = entity_obj(id, &holder);
    float ex, ey, ez, best = 1.0f;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p = NULL;
    int found = 0;
    unsigned vt = (unsigned)(g_base + RVA_BODY_VTABLE);
    if (!obj) { logf_("findbody %llu: no entity", id); return 0; }
    ex = *(float *)(obj + 0x60); ey = *(float *)(obj + 0x64); ez = *(float *)(obj + 0x68);
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    *body_out = NULL; *mat_out = NULL;
    {
        static BYTE chunk[0x10000 + 0x40];
        while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & PAGE_READWRITE) && mbi.RegionSize < 0x20000000) {
                BYTE *base = (BYTE *)mbi.BaseAddress, *rend = base + mbi.RegionSize;
                BYTE *c;
                for (c = base; c < rend; c += 0x10000) {
                    size_t n = (size_t)(rend - c);
                    size_t k;
                    if (n > sizeof chunk) n = sizeof chunk;
                    if (!safe_read(c, chunk, n)) continue;
                    for (k = 0; k + 4 <= n && k < 0x10000; k += 4) {
                        if (*(unsigned *)(chunk + k) == vt && k + 0x1C <= n && c + k != exclude) {
                            BYTE *m = *(BYTE **)(chunk + k + 0x18);
                            float f[3];
                            if (safe_read(m + 0x30, f, sizeof f)) {
                                float d = fabsf(f[0] - ex) + fabsf(f[1] - ey) + fabsf(f[2] - ez);
                                if (d < 1.0f) {
                                    logf_("findbody: body %p matrix %p pos %.3f %.3f %.3f (dist %.3f)", c + k, m, f[0], f[1], f[2], d);
                                    found++;
                                    if (d < best) { best = d; *body_out = c + k; *mat_out = m; }
                                }
                            }
                        }
                    }
                }
            }
            p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
        }
    }
    logf_("findbody done: %d match(es) near entity %.3f %.3f %.3f, nearest dist %.3f", found, ex, ey, ez, *body_out ? best : -1.0f);
    return *body_out != NULL;
}
/* calls fn(address, chunk copy at that address, bytes readable from it) for every dword equal to vt in
 * committed read-write memory; reads in chunks through safe_read, like find_body */
static void scan_vtable(unsigned vt, void (*fn)(BYTE *at, const BYTE *copy, size_t avail, void *ctx), void *ctx)
{
    static BYTE chunk[0x10000 + 0x400];
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p = NULL;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & PAGE_READWRITE) && mbi.RegionSize < 0x20000000) {
            BYTE *base = (BYTE *)mbi.BaseAddress, *rend = base + mbi.RegionSize, *c;
            for (c = base; c < rend; c += 0x10000) {
                size_t n = (size_t)(rend - c), k;
                if (n > sizeof chunk) n = sizeof chunk;
                if (!safe_read(c, chunk, n)) continue;
                for (k = 0; k + 4 <= n && k < 0x10000; k += 4)
                    if (*(unsigned *)(chunk + k) == vt) fn(c + k, chunk + k, n - k, ctx);
            }
        }
        p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
}

static void bodies_cb(BYTE *at, const BYTE *copy, size_t avail, void *ctx)
{
    float f[3];
    int *n = ctx;
    if (avail < 0x1C) return;
    if (!safe_read(*(BYTE **)(copy + 0x18) + 0x30, f, sizeof f)) return;
    if (++*n <= 200) logf_("bodies: %p pos %.3f %.3f %.3f%s", at, f[0], f[1], f[2], at == g_body ? " (cached local)" : "");
}

struct ents_ctx { float c[3], r; int id_off, n; };
static void ents_cb(BYTE *at, const BYTE *copy, size_t avail, void *ctx)
{
    struct ents_ctx *e = ctx;
    float x, y, z, d;
    if (avail < 0x6C || (e->id_off >= 0 && avail < (size_t)e->id_off + 8)) return;
    x = *(const float *)(copy + 0x60); y = *(const float *)(copy + 0x64); z = *(const float *)(copy + 0x68);
    d = sqrtf((x - e->c[0]) * (x - e->c[0]) + (y - e->c[1]) * (y - e->c[1]) + (z - e->c[2]) * (z - e->c[2]));
    if (!(d <= e->r)) return;
    if (++e->n > 200) return;
    if (e->id_off >= 0) logf_("ents: %p id %llu pos %.3f %.3f %.3f dist %.2f", at, *(const unsigned long long *)(copy + e->id_off), x, y, z, d);
    else logf_("ents: %p pos %.3f %.3f %.3f dist %.2f", at, x, y, z, d);
}

static void native_cmd(const char *ln)
{
    unsigned long long id;
    unsigned long a, pv;
    float fx, fy, fz;
    int n;
    if (sscanf(ln, "peek %lx %d", &a, &n) == 2) {
        if (IsBadReadPtr((void *)a, n)) logf_("peek %lx: unreadable", a);
        else hexdump_((const BYTE *)a, n);
    } else if (sscanf(ln, "ent %llu %d", &id, &n) == 2) {
        void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
        void *holder = NULL;
        void *obj;
        if (IsBadReadPtr(pm, 4) || !*pm) { logf_("ent: entity manager null"); return; }
        ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
        if (!holder || IsBadReadPtr(holder, 16)) { logf_("ent %llu: no holder", id); return; }
        obj = *(void **)((BYTE *)holder + 0xC);
        logf_("ent %llu: holder=%p obj=%p refs=%d", id, holder, obj, *(int *)((BYTE *)holder + 8));
        if (obj && !IsBadReadPtr(obj, n)) { logf_("vtable=%p", *(void **)obj); hexdump_((const BYTE *)obj, n); }
        (*(int *)((BYTE *)holder + 8))--;  /* each lookup takes a reference */
    } else if (sscanf(ln, "pos %llu", &id) == 1) {
        void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
        void *holder = NULL;
        BYTE *obj;
        if (IsBadReadPtr(pm, 4) || !*pm) { logf_("pos: entity manager null"); return; }
        ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
        if (!holder || IsBadReadPtr(holder, 16)) { logf_("pos %llu: no entity", id); return; }
        obj = *(BYTE **)((BYTE *)holder + 0xC);
        if (!obj) logf_("pos %llu: no entity", id);
        else logf_("pos %llu: x=%.3f y=%.3f z=%.3f dir=(%.3f,%.3f)", id, *(float *)(obj + 0x60), *(float *)(obj + 0x64), *(float *)(obj + 0x68), *(float *)(obj + 0x30), *(float *)(obj + 0x34));
        (*(int *)((BYTE *)holder + 8))--;
    } else if (sscanf(ln, "setpos %llu %f %f %f", &id, &fx, &fy, &fz) == 4) {
        void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
        void *holder = NULL;
        BYTE *obj;
        if (IsBadReadPtr(pm, 4) || !*pm) { logf_("setpos: entity manager null"); return; }
        ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
        if (!holder || IsBadReadPtr(holder, 16)) { logf_("setpos %llu: no entity", id); return; }
        obj = *(BYTE **)((BYTE *)holder + 0xC);
        if (!obj) logf_("setpos %llu: no entity", id);
        else {
            *(float *)(obj + 0x60) = fx; *(float *)(obj + 0x64) = fy; *(float *)(obj + 0x68) = fz;
            logf_("setpos %llu: wrote %.3f %.3f %.3f", id, fx, fy, fz);
        }
        (*(int *)((BYTE *)holder + 8))--;
    } else if (sscanf(ln, "netstart %u %31s %u %llu", &g_net_lport, g_net_pip, &g_net_pport, &g_net_id) == 4) {
        g_net_cfg = 1;
        logf_("netstart requested: local %u peer %s:%u entity %llu", g_net_lport, g_net_pip, g_net_pport, g_net_id);
    } else if (strncmp(ln, "netstop", 7) == 0) {
        g_net_cfg = 0;
        logf_("netstop");
    } else if (sscanf(ln, "walk %f %f %f", &fx, &fy, &fz) == 3) {
        if (!g_bodymat || IsBadReadPtr(g_bodymat, 0x40)) { logf_("walk: no cached body"); return; }
        g_walk_c[0] = *(float *)(g_bodymat + 0x30); g_walk_c[1] = *(float *)(g_bodymat + 0x34); g_walk_c[2] = *(float *)(g_bodymat + 0x38);
        g_walk_r = fx; g_walk_secs = fy; g_walk_speed = fz; g_walk_t0 = GetTickCount(); g_walk = 1;
        logf_("walk: radius %.2f for %.1f s at %.2f m/s around %.2f %.2f %.2f (start on circle edge)", fx, fy, fz, g_walk_c[0], g_walk_c[1], g_walk_c[2]);
    } else if (strncmp(ln, "walklegs ", 9) == 0) {
        /* walklegs <speed> <pause s> <dx1> <dy1> [<dx2> <dy2> ...]: straight legs relative to the start, each
         * followed by a pause; a path shaped like play, unlike the circle */
        char *c = (char *)ln + 9;
        float total = 0;
        int k;
        if (!g_bodymat || IsBadReadPtr(g_bodymat, 0x40)) { logf_("walklegs: no cached body"); return; }
        g_walk_speed = strtof(c, &c); g_leg_pause = strtof(c, &c);
        for (g_nlegs = 0; g_nlegs < MAXLEGS; g_nlegs++) {
            char *e;
            g_legs[g_nlegs][0] = strtof(c, &e); if (e == c) break; c = e;
            g_legs[g_nlegs][1] = strtof(c, &e); if (e == c) break; c = e;
        }
        if (g_walk_speed <= 0 || g_nlegs == 0) { logf_("walklegs: needs a speed and at least one leg"); return; }
        for (k = 0; k < g_nlegs; k++) total += sqrtf(g_legs[k][0] * g_legs[k][0] + g_legs[k][1] * g_legs[k][1]) / g_walk_speed + g_leg_pause;
        g_walk_c[0] = *(float *)(g_bodymat + 0x30); g_walk_c[1] = *(float *)(g_bodymat + 0x34); g_walk_c[2] = *(float *)(g_bodymat + 0x38);
        g_walk_secs = total; g_walk_t0 = GetTickCount(); g_walk = 2;
        logf_("walklegs: %d legs at %.2f m/s, %.1f s pauses, %.1f s in all, from %.2f %.2f %.2f", g_nlegs, g_walk_speed, g_leg_pause, total, g_walk_c[0], g_walk_c[1], g_walk_c[2]);
    } else if (sscanf(ln, "bodyvel %f %f %f %lx", &fx, &fy, &fz, &a) == 4) {
        if (!g_body || IsBadWritePtr(g_body + a, 12)) { logf_("bodyvel: bad body/offset"); return; }
        *(float *)(g_body + a) = fx; *(float *)(g_body + a + 4) = fy; *(float *)(g_body + a + 8) = fz;
        logf_("bodyvel: wrote %.2f %.2f %.2f at body+0x%lx", fx, fy, fz, a);
    } else if (sscanf(ln, "avvel %f %f %f %lx", &fx, &fy, &fz, &a) == 4) {
        if (!g_av_body_p || !*g_av_body_p || IsBadWritePtr(*g_av_body_p + a, 12)) { logf_("avvel: no avatar body"); return; }
        *(float *)(*g_av_body_p + a) = fx; *(float *)(*g_av_body_p + a + 4) = fy; *(float *)(*g_av_body_p + a + 8) = fz;
        logf_("avvel: wrote %.2f %.2f %.2f at avatar body+0x%lx", fx, fy, fz, a);
    } else if (sscanf(ln, "findbody %llu", &id) == 1) {
        find_body(id, &g_body, &g_bodymat, NULL);
    } else if (strncmp(ln, "bodies", 6) == 0) {
        int nb = 0;
        scan_vtable((unsigned)(g_base + RVA_BODY_VTABLE), bodies_cb, &nb);
        logf_("bodies done: %d of class RVA 0x%x (first 200 logged)", nb, RVA_BODY_VTABLE);
    } else if (sscanf(ln, "ents %llu %f", &id, &fx) == 2) {
        /* objects with the same vtable as entity id's object, within fx metres of it; the offset where that
         * object stores its own id is found first, so every match is logged with its entity id */
        void *holder = NULL;
        BYTE *obj = entity_obj(id, &holder);
        struct ents_ctx e;
        int k;
        if (!obj) { logf_("ents %llu: no entity", id); return; }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
        e.c[0] = *(float *)(obj + 0x60); e.c[1] = *(float *)(obj + 0x64); e.c[2] = *(float *)(obj + 0x68);
        e.r = fx; e.n = 0; e.id_off = -1;
        for (k = 4; k + 8 <= 0x400; k += 4)
            if (!IsBadReadPtr(obj + k, 8) && *(unsigned long long *)(obj + k) == id) { e.id_off = k; break; }
        logf_("ents %llu: vtable %p, own id at +0x%x", id, *(void **)obj, e.id_off);
        scan_vtable(*(unsigned *)obj, ents_cb, &e);
        logf_("ents done: %d within %.1f m (first 200 logged)", e.n, fx);
    } else if (strncmp(ln, "posb", 4) == 0) {
        if (!g_bodymat || IsBadReadPtr(g_bodymat, 0x40) || *(unsigned *)g_body != (unsigned)(g_base + RVA_BODY_VTABLE)) logf_("posb: no valid cached body");
        else logf_("posb: %.3f %.3f %.3f", *(float *)(g_bodymat + 0x30), *(float *)(g_bodymat + 0x34), *(float *)(g_bodymat + 0x38));
    } else if (sscanf(ln, "tpb %f %f %f", &fx, &fy, &fz) == 3) {
        if (!g_bodymat || IsBadWritePtr(g_bodymat, 0x40) || *(unsigned *)g_body != (unsigned)(g_base + RVA_BODY_VTABLE)) logf_("tpb: no valid cached body");
        else {
            *(float *)(g_bodymat + 0x30) = fx; *(float *)(g_bodymat + 0x34) = fy; *(float *)(g_bodymat + 0x38) = fz;
            logf_("tpb: wrote %.3f %.3f %.3f", fx, fy, fz);
        }
    } else if (strncmp(ln, "follow ", 7) == 0 || strncmp(ln, "tp ", 3) == 0) {
        int tp = ln[0] == 't';
        char *c = (char *)ln + (tp ? 3 : 7);
        void *holder = NULL;
        BYTE *obj, *cur;
        unsigned long long eid = strtoull(c, &c, 10);
        float v[3] = { 0, 0, 0 };
        int nv = 0, ok = 1;
        unsigned long offs[12];
        int no = 0, k;
        if (tp) { for (nv = 0; nv < 3; nv++) v[nv] = strtof(c, &c); }
        while (*c && no < 12) { char *e; unsigned long o = strtoul(c, &e, 16); if (e == c) break; offs[no++] = o; c = e; }
        obj = entity_obj(eid, &holder);
        cur = obj;
        for (k = 0; k < no && cur && ok; k++) {
            if (IsBadReadPtr(cur + offs[k], 4)) { ok = 0; break; }
            cur = *(BYTE **)(cur + offs[k]);
        }
        if (!cur || !ok || IsBadReadPtr(cur, 0x40)) logf_("%s %llu: chain broken at step %d", tp ? "tp" : "follow", eid, k);
        else if (tp) {
            *(float *)(cur + 0x30) = v[0]; *(float *)(cur + 0x34) = v[1]; *(float *)(cur + 0x38) = v[2];
            logf_("tp %llu: wrote %.3f %.3f %.3f at %p+0x30", eid, v[0], v[1], v[2], cur);
        } else {
            float *f = (float *)(cur + 0x30);
            logf_("follow %llu: end %p pos %.3f %.3f %.3f (entity %.3f %.3f %.3f)", eid, cur, f[0], f[1], f[2], *(float *)(obj + 0x60), *(float *)(obj + 0x64), *(float *)(obj + 0x68));
        }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
    } else if (sscanf(ln, "scan %f %f", &fx, &fy) == 2) {
        MEMORY_BASIC_INFORMATION mbi;
        BYTE *p = NULL;
        int found = 0;
        logf_("scan for x=%.3f y=%.3f", fx, fy);
        while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi.RegionSize < 0x20000000) {
                BYTE *q = (BYTE *)mbi.BaseAddress;
                BYTE *end = q + mbi.RegionSize - 24;
                for (; q < end; q += 4) {
                    if (fabsf(*(float *)q - fx) < 0.01f) {
                        int k;
                        for (k = 4; k <= 16; k += 4) {
                            if (fabsf(*(float *)(q + k) - fy) < 0.01f) {
                                logf_("SCAN %p y@+%d z=%.3f %s", q, k, *(float *)(q + 8), (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ? "rw" : "ro");
                                found++;
                                break;
                            }
                        }
                    }
                }
            }
            p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
            if (found > 2000) break;
        }
        logf_("scan done, %d hits", found);
    } else if (sscanf(ln, "pokef %lx %f", &a, &fx) == 2) {
        if (IsBadWritePtr((void *)a, 4)) logf_("pokef %lx: not writable", a);
        else { *(float *)a = fx; logf_("pokef %lx = %.3f", a, fx); }
    } else if (sscanf(ln, "hold %llu %f %f %f", &id, &fx, &fy, &fz) == 4) {
        g_hold_id = id; g_hold_pos[0] = fx; g_hold_pos[1] = fy; g_hold_pos[2] = fz; g_hold_ticks = 0; g_hold = 1;
        logf_("hold on: %llu -> %.3f %.3f %.3f", id, fx, fy, fz);
    } else if (strncmp(ln, "unhold", 6) == 0) {
        g_hold = 0;
        logf_("hold off after %lu ticks", g_hold_ticks);
    } else if (sscanf(ln, "poke %lx %lx", &a, &pv) == 2) {
        if (IsBadWritePtr((void *)a, 4)) logf_("poke %lx: not writable", a);
        else { *(unsigned long *)a = pv; logf_("poke %lx = %08lx", a, pv); }
    } else if (!ai_cmd(ln) && !veh_cmd(ln) && !wpn_cmd(ln)) logf_("native: unknown command: %s", ln);
}

static void run_native(void)
{
    FILE *f = fopen(g_natpath, "rb");
    char ln[256];
    if (!f) return;
    while (fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        if (ln[0]) native_cmd(ln);
    }
    fclose(f);
    remove(g_natpath);
    logf_("native commands done");
}


/* ---- per-frame position hold (test) ---- */
static BYTE *entity_obj(unsigned long long id, void **holder_out)
{
    void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
    void *holder = NULL;
    if (IsBadReadPtr(pm, 4) || !*pm) return NULL;
    ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
    if (!holder || IsBadReadPtr(holder, 16)) return NULL;
    *holder_out = holder;
    return *(BYTE **)((BYTE *)holder + 0xC);
}

/* ---- moving an entity the way FC2JackalFix's noclip does (https://github.com/TGP482/FC2JackalFix, MIT) ----
 * TeleportEntity queues a remove/move/re-add chain built for pawns; on a vehicle its first step reads a null
 * holder (Dunia+0x9fcf02). Noclip instead switches the physics component off (vtable +0xB4) and calls
 * CEntity::SetEuler / SetPosition. RVAs resolved from its patterns on the Steam 1.03 build. */
#define RVA_SETPOSITION   0x4e01b0
#define RVA_SETEULER      0x4e01c0
#define RVA_GETCOMPONENT  0x4e2cc0
#define RVA_PHYSTAG_REG   0x0415d0
#define RVA_PHYSTAG_FLAG  0xfde930
#define RVA_PHYSTAG       0xfde940
#define RVA_GETVEHPHYS    0x0649f0
typedef void (__attribute__((thiscall)) *setvec_t)(void *ent, float a, float b, float c);
typedef void *(__attribute__((thiscall)) *getcomp_t)(void *ent, void *tag);
typedef void *(__attribute__((thiscall)) *getvehphys_t)(void *ent);
typedef void (__attribute__((thiscall)) *setphys_t)(void *phys, int on);
typedef void (__attribute__((fastcall)) *reg_t)(void *unused);

static void *ent_physics(BYTE *ent)
{
    if (*(int *)(g_base + RVA_PHYSTAG_FLAG) == 0) ((reg_t)(g_base + RVA_PHYSTAG_REG))(NULL);
    return ((getcomp_t)(g_base + RVA_GETCOMPONENT))(ent, g_base + RVA_PHYSTAG);
}

static void ent_set_physics(void *phys, int on)
{
    ((setphys_t)(*(void ***)phys)[0xB4 / 4])(phys, on);
}

static void ent_place(BYTE *ent, const float *pos, float yaw)
{
    ((setvec_t)(g_base + RVA_SETEULER))(ent, 0.0f, 0.0f, yaw);
    ((setvec_t)(g_base + RVA_SETPOSITION))(ent, pos[0], pos[1], pos[2]);
}

/* vinfo <id> | vphys <id> 0|1 | vset <id> <x> <y> <z> <yaw radians> | getc <id> <getter rva hex>: calls a
 * GetComponent<T> getter (thiscall on the entity, e.g. 90930 = CPawnNetworkComponent) and logs the component */
static int veh_cmd(const char *ln)
{
    unsigned long long id;
    void *holder = NULL;
    BYTE *ent;
    float v[4];
    int on;
    unsigned long rva;
    if (sscanf(ln, "getc %llu %lx", &id, &rva) == 2) {
        void *c;
        ent = entity_obj(id, &holder);
        if (!ent || IsBadReadPtr(ent, 0x70)) logf_("getc %llu: no entity", id);
        else {
            c = ((getvehphys_t)(g_base + rva))(ent);
            logf_("getc %llu %lx: entity %p vtable rva %lx -> component %p vtable rva %lx", id, rva, ent,
                  (unsigned long)(*(BYTE **)ent - g_base), c, c && !IsBadReadPtr(c, 4) ? (unsigned long)(*(BYTE **)c - g_base) : 0UL);
        }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
        return 1;
    }
    if (sscanf(ln, "vinfo %llu", &id) != 1 && sscanf(ln, "vphys %llu %d", &id, &on) != 2 &&
        sscanf(ln, "vset %llu %f %f %f %f", &id, &v[0], &v[1], &v[2], &v[3]) != 5) return 0;
    ent = entity_obj(id, &holder);
    if (!ent || IsBadReadPtr(ent, 0x70)) logf_("veh %llu: no entity", id);
    else if (strncmp(ln, "vinfo", 5) == 0) {
        float *m = (float *)(ent + 0x30);
        logf_("vinfo %llu: entity %p vtable %p physics %p vehicle physics %p pos %.3f %.3f %.3f fwd %.3f %.3f",
              id, ent, *(void **)ent, ent_physics(ent), ((getvehphys_t)(g_base + RVA_GETVEHPHYS))(ent), m[12], m[13], m[14], m[4], m[5]);
    } else if (strncmp(ln, "vphys", 5) == 0) {
        void *phys = ent_physics(ent);
        if (!phys) logf_("vphys %llu: no physics component", id);
        else { ent_set_physics(phys, on); logf_("vphys %llu: physics %p %s", id, phys, on ? "on" : "off"); }
    } else {
        ent_place(ent, v, v[3]);
        logf_("vset %llu: placed at %.3f %.3f %.3f yaw %.3f, now %.3f %.3f %.3f", id, v[0], v[1], v[2], v[3],
              *(float *)(ent + 0x60), *(float *)(ent + 0x64), *(float *)(ent + 0x68));
    }
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    return 1;
}

/* ---- weapons on any pawn ----
 * The Lua AddWeapon / SelectWeapon / DrawWeapon / HolsterWeapon bindings (registered in FUN_109fc830) look up the
 * local player and then only use its pawn component (tag 0xfde384, class checked through vtable +4), so the same
 * steps work on an AI pawn. Weapon manager = *(pawn component + 0x10) + 0x4F0; its methods load *(mgr + 4) first.
 * Inner state m = *(mgr + 4): +0x10 holder of the weapon in hand, +0x18 selected slot, +0x74 state, +0x78 mode
 * (1 weapon, 2 gadget). Draw sends signal 0x2a47e3a3 to the state driver (pawn component vtable +0x78). */
#define RVA_PAWNTAG_REG    0x040a50
#define RVA_PAWNTAG_FLAG   0xfde374
#define RVA_PAWNTAG        0xfde384
#define RVA_PAWNCLASS_REG  0x040a80
#define RVA_PAWNCLASS_FLAG 0xfde388
#define RVA_PAWNCLASS      0xfde39c
#define RVA_WPNTAG_REG     0x0987e0
#define RVA_WPNTAG_FLAG    0xfe01e0
#define RVA_WPNTAG         0xfe01f8
#define RVA_WM_SLOT        0x129de0  /* weapon component in a slot */
#define RVA_WM_REMOVE      0x12e3e0  /* (weapon, 0) */
#define RVA_WM_ADD         0x1289f0  /* (weapon component) */
#define RVA_WM_SELECT      0x12ce80  /* (&category hash) */
#define RVA_WM_SELDEFAULT  0x12b310
#define RVA_WM_SELECTED    0x129d90
#define RVA_WM_HOLSTER1    0x12b340
#define RVA_WM_HOLSTER2    0x12b350
#define RVA_STRHASH        0x229400  /* cdecl (const char *) */
#define RVA_SENDSIGNAL     0x29da10
#define SIG_DRAW           0x2a47e3a3u
typedef void *(__attribute__((thiscall)) *getp_t)(void *self);
typedef void *(__attribute__((thiscall)) *getp1_t)(void *self, void *a);
typedef void (__attribute__((thiscall)) *call2_t)(void *self, void *a, int b);
typedef char (__attribute__((thiscall)) *getc_t)(void *self);
typedef void (__attribute__((thiscall)) *signal_t)(void *driver, unsigned sig);
typedef unsigned (__cdecl *strhash_t)(const char *);

static void *pawn_comp(BYTE *ent)
{
    void *c;
    BYTE *type;
    if (*(int *)(g_base + RVA_PAWNTAG_FLAG) == 0) ((reg_t)(g_base + RVA_PAWNTAG_REG))(NULL);
    c = ((getcomp_t)(g_base + RVA_GETCOMPONENT))(ent, g_base + RVA_PAWNTAG);
    if (!c) return NULL;
    if (*(int *)(g_base + RVA_PAWNCLASS_FLAG) == 0) ((reg_t)(g_base + RVA_PAWNCLASS_REG))(NULL);
    type = ((getp_t)(*(void ***)c)[1])(c);
    if (!type || *(unsigned *)(type + 4) < 4 || *(unsigned *)(type + 0x14) != *(unsigned *)(g_base + RVA_PAWNCLASS)) return NULL;
    return c;
}

static void *weapon_comp(BYTE *ent)
{
    if (*(int *)(g_base + RVA_WPNTAG_FLAG) == 0) ((reg_t)(g_base + RVA_WPNTAG_REG))(NULL);
    return ((getcomp_t)(g_base + RVA_GETCOMPONENT))(ent, g_base + RVA_WPNTAG);
}

static BYTE *weapon_mgr(void *pc) { return *(BYTE **)((BYTE *)pc + 0x10) + 0x4F0; }

/* the entity of the weapon in the pawn's hand, or NULL */
static BYTE *weapon_held(void *pc)
{
    BYTE *m = *(BYTE **)(weapon_mgr(pc) + 4), *h;
    if (!m) return NULL;
    h = *(BYTE **)(m + 0x10);
    return h ? *(BYTE **)(h + 0xC) : NULL;
}

/* id of the weapon (mode 1, not a gadget) in the pawn entity's hand, 0 for none */
static unsigned long long held_weapon_id(BYTE *ent)
{
    void *pc = ent ? pawn_comp(ent) : NULL;
    BYTE *m, *w;
    if (!pc || !(m = *(BYTE **)(weapon_mgr(pc) + 4)) || *(int *)(m + 0x78) != 1) return 0;
    w = weapon_held(pc);
    return w ? *(unsigned long long *)(w + 8) : 0;
}

/* gives the pawn the weapon entity w (replacing what it has in that slot); returns w's weapon component or NULL */
static void *weapon_give(void *pc, BYTE *w)
{
    BYTE *mgr = weapon_mgr(pc);
    void *wc = weapon_comp(w), *old;
    unsigned slot;
    if (!wc) return NULL;
    slot = (unsigned)(size_t)((getp_t)(*(void ***)wc)[0x8C / 4])(wc);
    old = ((getp1_t)(g_base + RVA_WM_SLOT))(mgr, (void *)(size_t)slot);
    if (old) ((call2_t)(g_base + RVA_WM_REMOVE))(mgr, old, 0);
    ((getp1_t)(g_base + RVA_WM_ADD))(mgr, wc);
    return wc;
}

/* selecting a category also draws its weapon */
static void weapon_select(void *pc, const char *cat)
{
    unsigned h = ((strhash_t)(g_base + RVA_STRHASH))(cat);
    ((getp1_t)(g_base + RVA_WM_SELECT))(weapon_mgr(pc), &h);
}

static void weapon_holster(void *pc)
{
    BYTE *mgr = weapon_mgr(pc), *m = *(BYTE **)(mgr + 4);
    int st = m ? *(int *)(m + 0x74) : 0;
    if (st == 1) ((getp_t)(g_base + RVA_WM_HOLSTER1))(mgr);
    else if (st == 2) ((getp_t)(g_base + RVA_WM_HOLSTER2))(mgr);
}

/* winfo <pawn> | wadd <pawn> <weapon entity> | wsel <pawn> <primary|secondary|hand_hand> | wdraw <pawn> | wholster <pawn> */
static int wpn_cmd(const char *ln)
{
    unsigned long long id, wid;
    char cat[32];
    void *holder = NULL, *pc;
    BYTE *ent, *mgr, *m;
    int op;
    if (sscanf(ln, "winfo %llu", &id) == 1) op = 0;
    else if (sscanf(ln, "wadd %llu %llu", &id, &wid) == 2) op = 1;
    else if (sscanf(ln, "wsel %llu %31s", &id, cat) == 2) op = 2;
    else if (sscanf(ln, "wdraw %llu", &id) == 1) op = 3;
    else if (sscanf(ln, "wholster %llu", &id) == 1) op = 4;
    else return 0;
    ent = entity_obj(id, &holder);
    if (!ent || IsBadReadPtr(ent, 0x70)) { logf_("wpn %llu: no entity", id); goto done; }
    pc = pawn_comp(ent);
    if (!pc) { logf_("wpn %llu: no pawn component", id); goto done; }
    mgr = weapon_mgr(pc);
    m = *(BYTE **)(mgr + 4);
    if (!m || IsBadReadPtr(m, 0x80)) { logf_("wpn %llu: no weapon manager", id); goto done; }
    if (op == 0) {
        BYTE *held = weapon_held(pc);
        logf_("winfo %llu: pawn component %p manager %p inner %p mode %d state %d selected %d held %p id %llu",
              id, pc, mgr, m, *(int *)(m + 0x78), *(int *)(m + 0x74), *(int *)(m + 0x18), held, held ? *(unsigned long long *)(held + 8) : 0ULL);
        hexdump_(m, 0x80);
    } else if (op == 1) {
        void *wh = NULL, *wc;
        BYTE *w = entity_obj(wid, &wh);
        if (!w || !(wc = weapon_give(pc, w))) logf_("wadd %llu: %llu is not a weapon", id, wid);
        else logf_("wadd %llu: weapon %llu component %p", id, wid, wc);
        if (wh) (*(int *)((BYTE *)wh + 8))--;
    } else if (op == 2) {
        weapon_select(pc, cat);
        logf_("wsel %llu: %s, category hash now %08x", id, cat, *(unsigned *)(m + 0x18));
    } else if (op == 3) {
        if (*(int *)(m + 0x78) == 1 && weapon_held(pc)) logf_("wdraw %llu: already in hand", id);
        else if (*(int *)(m + 0x78) == 1 && !((getp_t)(g_base + RVA_WM_SELECTED))(mgr) && !((getc_t)(g_base + RVA_WM_SELDEFAULT))(mgr))
            logf_("wdraw %llu: nothing to select", id);
        else {
            ((signal_t)(g_base + RVA_SENDSIGNAL))(((getp_t)(*(void ***)pc)[0x78 / 4])(pc), SIG_DRAW);
            logf_("wdraw %llu: draw signal sent (mode %d)", id, *(int *)(m + 0x78));
        }
    } else {
        logf_("wholster %llu: state %d", id, *(int *)(m + 0x74));
        weapon_holster(pc);
    }
done:
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    return 1;
}

static void hold_tick(void)
{
    void *holder = NULL;
    BYTE *obj;
    if (!g_hold || g_nstates == 0 || GetCurrentThreadId() != g_states[0].thread) return;
    obj = entity_obj(g_hold_id, &holder);
    if (obj) {
        float *p = (float *)(obj + 0x60);
        g_hold_ticks++;
        if (g_hold_ticks <= 10 || g_hold_ticks % 120 == 0)
            logf_("hold tick %lu: engine left %.3f %.3f %.3f", g_hold_ticks, p[0], p[1], p[2]);
        p[0] = g_hold_pos[0]; p[1] = g_hold_pos[1]; p[2] = g_hold_pos[2];
    }
    if (holder) (*(int *)((BYTE *)holder + 8))--;
}

/* ---- own UDP transport: position exchange between two copies ---- */
#pragma pack(push, 1)
/* veh: the vehicle the sender is in (0 on foot), with its position and forward vector; wpn: archetype of the
 * weapon in the sender's hand ("" for none) */
typedef struct { unsigned magic, seq; float pos[3]; float dir[2]; unsigned long long veh; float vpos[3]; float vfwd[2]; char wpn[48]; } NetPkt;
#pragma pack(pop)
#define NET_MAGIC 0x33324346u
static SOCKET g_ns = INVALID_SOCKET;
static struct sockaddr_in g_peer;
static DWORD g_net_lastsend, g_net_lastlog, g_net_lastfind, g_net_lastrx;
static unsigned g_net_seq, g_net_tx, g_net_rx, g_net_drop;
static NetPkt g_remote;
/* the local player's vehicle, asked from the engine once a second (0 on foot) */
static unsigned long long g_net_veh;
static DWORD g_net_lastveh;
/* the weapon in the local player's hand; its name is asked from the engine when the entity changes */
static unsigned long long g_net_wid;
static char g_net_wpn[48];
static unsigned long long g_av_id;
static BYTE *g_av_body, *g_av_mat;
static void av_init_ptr(void) { g_av_body_p = &g_av_body; }
static float g_av_cur[3], g_av_dir[2];
static int g_av_have;
static DWORD g_av_lastspawn, g_av_lastfind;

static int av_valid(void)
{
    return g_av_mat && g_av_body != g_body && !IsBadWritePtr(g_av_mat, 0x40) && !IsBadReadPtr(g_av_body, 4) && *(unsigned *)g_av_body == (unsigned)(g_base + RVA_BODY_VTABLE);
}

static void av_remove(void)
{
    char code[128], out[64];
    if (g_av_id) {
        snprintf(code, sizeof code, "RemoveEntity('%llu') __out = 'removed'", g_av_id);
        lua_eval(code, out, sizeof out);
        logf_("avatar %llu removed: %s", g_av_id, out);
    }
    g_av_id = 0; g_av_body = g_av_mat = NULL; g_av_have = 0;
}

static void av_tick(DWORD now)
{
    av_init_ptr();
    if (g_net_rx == 0 || now - g_net_lastrx > 3000) return;
    if (!g_av_id) {
        char code[256], out[64];
        unsigned long long id;
        if (now - g_av_lastspawn < 3000) return;
        g_av_lastspawn = now;
        snprintf(code, sizeof code, "__out = tostring(SpawnEntityFromArchetype(GetEntityName(GetLocalPlayerId()), %.3f, %.3f, %.3f, 0, 0, 0))",
                 g_remote.pos[0], g_remote.pos[1], g_remote.pos[2]);
        lua_eval(code, out, sizeof out);
        id = strtoull(out, NULL, 10);
        if (id == 0 || id == 0xFFFFFFFFFFFFFFFFull) { logf_("avatar spawn failed: %s", out); return; }
        g_av_id = id;
        logf_("avatar spawned: %llu", id);
        snprintf(code, sizeof code, "CDominoManager_GetInstance():SendRegisteredEventToEntity('%llu', 'CFCXCountersComponent_SetInvincible') __out = 'ok'", id);
        lua_eval(code, out, sizeof out);
        logf_("avatar set invincible: status %s", out);
        return;
    }
    if (!av_valid()) {
        if (!g_body) return;  /* need our own body first, so it can be excluded */
        if (now - g_av_lastfind < 1000) return;
        g_av_lastfind = now;
        if (find_body(g_av_id, &g_av_body, &g_av_mat, g_body)) { g_av_have = 0; logf_("avatar body bound: %p (local body %p)", g_av_body, g_body); }
        return;
    }
    {
        float *cur = g_av_cur, *tg = g_remote.pos;
        float d = fabsf(tg[0] - cur[0]) + fabsf(tg[1] - cur[1]) + fabsf(tg[2] - cur[2]);
        if (!g_av_have || d > 6.0f) { memcpy(cur, tg, sizeof g_av_cur); g_av_have = 1; }
        else { cur[0] += (tg[0] - cur[0]) * 0.25f; cur[1] += (tg[1] - cur[1]) * 0.25f; cur[2] += (tg[2] - cur[2]) * 0.25f; }
        *(float *)(g_av_mat + 0x30) = cur[0]; *(float *)(g_av_mat + 0x34) = cur[1]; *(float *)(g_av_mat + 0x38) = cur[2];
    }
    /* heading: the body capsule never rotates and the avatar has no controller, so the entity matrix
     * keeps whatever rotation is written into it (yaw only, rows (d0,d1,0) (-d1,d0,0) (0,0,1),
     * the same layout as the player's) */
    {
        float dx = g_remote.dir[0], dy = g_remote.dir[1], n = sqrtf(dx * dx + dy * dy);
        void *holder = NULL;
        BYTE *obj;
        if (n < 0.5f) return;
        dx /= n; dy /= n;
        if (!g_av_have || g_av_dir[0] * dx + g_av_dir[1] * dy < -0.9f) { g_av_dir[0] = dx; g_av_dir[1] = dy; }
        else {
            g_av_dir[0] += (dx - g_av_dir[0]) * 0.25f; g_av_dir[1] += (dy - g_av_dir[1]) * 0.25f;
            n = sqrtf(g_av_dir[0] * g_av_dir[0] + g_av_dir[1] * g_av_dir[1]);
            if (n < 1e-3f) { g_av_dir[0] = dx; g_av_dir[1] = dy; } else { g_av_dir[0] /= n; g_av_dir[1] /= n; }
        }
        obj = entity_obj(g_av_id, &holder);
        if (obj && !IsBadWritePtr(obj + 0x30, 0x30)) {
            float *r = (float *)(obj + 0x30);
            r[0] = g_av_dir[0]; r[1] = g_av_dir[1]; r[2] = 0;
            r[4] = -g_av_dir[1]; r[5] = g_av_dir[0]; r[6] = 0;
            r[8] = 0; r[9] = 0; r[10] = 1;
        }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
    }
}

/* ---- AI body: an AI pawn walks after a target with its own locomotion (and animation) ----
 * The engine's Domino MoveTo command walks a pawn to an entity; it keeps following that entity when it
 * moves. The target entity is a pistol spawned on the ground (no faction, nothing for the AI to react
 * to) whose position is written every tick; a pawn too far behind is teleported onto it. */
enum { AI_OFF, AI_REMOTE, AI_LOCAL, AI_CIRCLE };
static int g_ai_src, g_ai_speed = 3;
static unsigned long long g_ai_id, g_ai_marker;
static float g_ai_tp = 10.0f, g_ai_c[3], g_ai_r, g_ai_v, g_ai_last[3];
/* MoveTo ends on arrival, so a stopped pawn is re-ordered once it has stood for g_ai_stall_ms while the target
 * is more than g_ai_stall_m away. Soldiers need 2-4 s to start (re-ordering sooner keeps them standing);
 * spawned buddies start within 1 s */
static DWORD g_ai_stall_ms = 6000;
static float g_ai_stall_m = 3.0f;
/* the marker leads the target by g_ai_lead seconds of its (smoothed) velocity, so a pawn following a moving
 * target keeps walking instead of arriving, stopping and waiting for the next order */
static float g_ai_lead, g_ai_vel[2], g_ai_prev[3];
static DWORD g_ai_prevt;
/* a pawn cannot walk as fast as a vehicle: above g_ai_glide_on m/s of target speed it is teleported onto the marker
 * every frame (the marker extrapolated from the last packet), back to MoveTo below g_ai_glide_off. On foot the
 * remote player measured under 4 m/s nearly always, driving 8-12 m/s */
static float g_ai_glide_on = 8.0f, g_ai_glide_off = 5.0f;
static int g_ai_glide;
static unsigned g_ai_glides;
static DWORD g_ai_lastglide;
static DWORD g_ai_t0, g_ai_lastlog, g_ai_lastmove, g_ai_lastorder;
static unsigned g_ai_orders, g_ai_tps;
/* aibody: the pawn is our own, spawned from g_ai_arch and made invincible; when it or the marker is gone (a
 * checkpoint reload or death removes both) or it ignores orders, both are spawned again at the target */
static char g_ai_arch[96];
static DWORD g_ai_lastspawn, g_ai_lastvalid;
static int g_ai_lost;
static unsigned g_ai_spawns, g_ai_stuck;
/* the peer's vehicle: our copy of it (both load the same save, so it has the same id) with its physics off,
 * placed from the packets every frame, our pawn seated in it; g_ai_noveh is one this world does not have */
static unsigned long long g_ai_veh, g_ai_noveh;
static int g_ai_seated, g_ai_seattries;
static DWORD g_ai_lastseat;
/* the peer's weapon on our body: one spawned weapon entity per category, and what the body last got */
static const char *const g_wpn_cats[4][2] = {
    { "weapons.HandToHand.", "hand_hand" }, { "weapons.Secondary.", "secondary" },
    { "weapons.Primary.", "primary" }, { "weapons.Special.", "special" } };
static unsigned long long g_ai_wid[4];
static char g_ai_warch[4][48], g_ai_wpn[48];
static DWORD g_ai_lastwpn;
static void ai_weapons_clear(void);

static void ai_vehicle_release(void)
{
    void *holder = NULL, *phys;
    BYTE *v;
    if (!g_ai_veh) return;
    v = entity_obj(g_ai_veh, &holder);
    if (v && (phys = ent_physics(v))) ent_set_physics(phys, 1);
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    logf_("ai: released vehicle %llu (physics on)", g_ai_veh);
    g_ai_veh = 0; g_ai_seated = 0;
}

static int ent_pos(unsigned long long id, float *out)
{
    void *holder = NULL;
    BYTE *obj = entity_obj(id, &holder);
    int ok = obj && !IsBadReadPtr(obj + 0x60, 12);
    if (ok) memcpy(out, obj + 0x60, 12);
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    return ok;
}

static void ai_order(const char *why)
{
    char code[192], out[32];
    snprintf(code, sizeof code, "CDominoManager_GetInstance():SendCommandEventToEntity('%llu', 'MoveTo', '%llu', %d, 0) __out = 'ok'",
             g_ai_id, g_ai_marker, g_ai_speed);
    lua_eval(code, out, sizeof out);
    g_ai_orders++; g_ai_lastorder = GetTickCount();
    logf_("ai: MoveTo sent (%s), speed %d", why, g_ai_speed);
}

static unsigned long long spawn_at(const char *arch, float x, float y, float z)
{
    char code[256], out[64];
    unsigned long long id;
    snprintf(code, sizeof code, "__out = tostring(SpawnEntityFromArchetype('%s', %.3f, %.3f, %.3f, 0, 0, 0))", arch, x, y, z);
    lua_eval(code, out, sizeof out);
    id = strtoull(out, NULL, 10);
    if (id == 0xFFFFFFFFFFFFFFFFull) id = 0;
    if (!id) logf_("ai: spawn of %s failed: %s", arch, out);
    return id;
}

static void remove_entity(unsigned long long id)
{
    char code[96], out[32];
    if (!id) return;
    snprintf(code, sizeof code, "RemoveEntity('%llu') __out = 'ok'", id);
    lua_eval(code, out, sizeof out);
}

static void ai_stop(void)
{
    char code[192], out[32];
    ai_vehicle_release();
    if (g_ai_id) {
        snprintf(code, sizeof code, "CDominoManager_GetInstance():SendCommandEventToEntity('%llu', 'MoveToStopped') __out = 'ok'", g_ai_id);
        lua_eval(code, out, sizeof out);
    }
    remove_entity(g_ai_marker);
    if (g_ai_arch[0]) { remove_entity(g_ai_id); ai_weapons_clear(); }
    logf_("ai: stopped (%u orders, %u teleports, %u spawns)", g_ai_orders, g_ai_tps, g_ai_spawns);
    g_ai_src = AI_OFF; g_ai_id = g_ai_marker = 0; g_ai_arch[0] = 0;
}

/* (re)spawns our pawn and its marker at the target, once the local player is in the world */
static void ai_spawn(const float *t, const char *why)
{
    char code[192], out[32];
    lua_eval("__out = tostring(IsEntityValid(GetLocalPlayerId()))", out, sizeof out);
    if (strcmp(out, "1") != 0) return;
    remove_entity(g_ai_marker); remove_entity(g_ai_id);
    ai_weapons_clear();
    g_ai_id = g_ai_marker = 0;
    g_ai_marker = spawn_at("weapons.Secondary.Makarov", t[0], t[1], t[2] + 0.5f);
    if (g_ai_marker) g_ai_id = spawn_at(g_ai_arch, t[0] + 1.0f, t[1], t[2] + 0.3f);
    if (!g_ai_id) return;
    g_ai_lost = 0; g_ai_lastvalid = GetTickCount();
    snprintf(code, sizeof code, "CDominoManager_GetInstance():SendRegisteredEventToEntity('%llu', 'CFCXCountersComponent_SetInvincible') __out = 'ok'", g_ai_id);
    lua_eval(code, out, sizeof out);
    g_ai_spawns++; g_ai_stuck = 0;
    g_ai_seated = 0; g_ai_seattries = 0;
    memcpy(g_ai_last, t, 12); g_ai_lastmove = GetTickCount();
    logf_("ai: body spawned %llu (%s, %s), marker %llu, invincible %s, spawns %u", g_ai_id, g_ai_arch, why, g_ai_marker, out, g_ai_spawns);
    ai_order("spawn");
}

static void ai_weapons_clear(void)
{
    int k;
    for (k = 0; k < 4; k++) { remove_entity(g_ai_wid[k]); g_ai_wid[k] = 0; g_ai_warch[k][0] = 0; }
    g_ai_wpn[0] = 0;
}

/* once a second while the body's weapon differs from the peer's: spawn the peer's weapon (once per category),
 * give it to the body and select its category, which draws it; nothing in the peer's hand holsters */
static void ai_weapon(DWORD now)
{
    void *holder = NULL, *wh = NULL, *pc;
    BYTE *ent, *w;
    const char *want = g_remote.wpn, *c;
    char code[128], out[16];
    int k;
    if (!strcmp(g_ai_wpn, want) || now - g_ai_lastwpn < 1000) return;
    g_ai_lastwpn = now;
    for (c = want; *c; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_') break;
    for (k = 0; k < 4 && strncmp(want, g_wpn_cats[k][0], strlen(g_wpn_cats[k][0])); k++) ;
    if (*want && (*c || k == 4)) { logf_("ai: peer holds %.47s, not a weapon archetype; ignored", want); strcpy(g_ai_wpn, want); return; }
    snprintf(code, sizeof code, "__out = tostring(IsEntityLoaded('%llu'))", g_ai_id);
    lua_eval(code, out, sizeof out);
    if (strcmp(out, "1") != 0) return;
    ent = entity_obj(g_ai_id, &holder);
    pc = ent ? pawn_comp(ent) : NULL;
    if (!pc) { logf_("ai: body has no pawn component, no weapon"); strcpy(g_ai_wpn, want); goto done; }
    if (!*want) { weapon_holster(pc); logf_("ai: peer holds nothing, body holsters"); strcpy(g_ai_wpn, want); goto done; }
    if (strcmp(g_ai_warch[k], want) != 0) {
        float p[3];
        unsigned long long old = g_ai_wid[k], id;
        if (!ent_pos(g_ai_id, p)) goto done;
        id = spawn_at(want, p[0], p[1], p[2] + 1.0f);
        if (!id) { strcpy(g_ai_wpn, want); goto done; }
        w = entity_obj(id, &wh);
        if (!w || !weapon_give(pc, w)) { logf_("ai: %s %llu is not a weapon", want, id); remove_entity(id); strcpy(g_ai_wpn, want); goto done; }
        remove_entity(old);
        g_ai_wid[k] = id; strcpy(g_ai_warch[k], want);
        logf_("ai: body given %s (%llu)", want, id);
    }
    weapon_select(pc, g_wpn_cats[k][1]);
    strcpy(g_ai_wpn, want);
    logf_("ai: body draws %s (%s)", want, g_wpn_cats[k][1]);
done:
    if (wh) (*(int *)((BYTE *)wh + 8))--;
    if (holder) (*(int *)((BYTE *)holder + 8))--;
}

static int ai_target(DWORD now, float *t)
{
    if (g_ai_src == AI_REMOTE) { if (g_net_rx == 0) return 0; memcpy(t, g_remote.pos, 12); return 1; }
    if (g_ai_src == AI_LOCAL) return g_bodymat && !IsBadReadPtr(g_bodymat, 0x40) ? (memcpy(t, g_bodymat + 0x30, 12), 1) : 0;
    {
        float ang = g_ai_v * ((now - g_ai_t0) / 1000.0f) / g_ai_r;
        t[0] = g_ai_c[0] + g_ai_r * (cosf(ang) - 1.0f); t[1] = g_ai_c[1] + g_ai_r * sinf(ang); t[2] = g_ai_c[2];
        return 1;
    }
}

/* the peer drives g_remote.veh. 0 when this world has no such vehicle (walk or glide instead) */
static int ai_drive(DWORD now, const float *t, const float *p)
{
    void *holder = NULL, *phys;
    BYTE *v;
    float pos[3], lead, yaw, d;
    char code[512], out[32];
    if (g_ai_veh != g_remote.veh) {
        ai_vehicle_release();
        v = entity_obj(g_remote.veh, &holder);
        phys = v && !IsBadReadPtr(v, 0x70) ? ent_physics(v) : NULL;
        if (phys) ent_set_physics(phys, 0);
        if (holder) { (*(int *)((BYTE *)holder + 8))--; holder = NULL; }
        if (!phys) { logf_("ai: peer's vehicle %llu is not in this world, walking instead", g_remote.veh); g_ai_noveh = g_remote.veh; return 0; }
        g_ai_veh = g_remote.veh; g_ai_seated = 0; g_ai_seattries = 0; g_ai_lastseat = 0;
        snprintf(code, sizeof code, "CDominoManager_GetInstance():SendCommandEventToEntity('%llu', 'MoveToStopped') __out = 'ok'", g_ai_id);
        lua_eval(code, out, sizeof out);
        logf_("ai: peer entered vehicle %llu, physics off", g_ai_veh);
    }
    v = entity_obj(g_ai_veh, &holder);
    if (!v || IsBadReadPtr(v, 0x70)) {
        if (holder) (*(int *)((BYTE *)holder + 8))--;
        logf_("ai: vehicle %llu gone", g_ai_veh);
        g_ai_noveh = g_ai_veh; g_ai_veh = 0; g_ai_seated = 0;
        return 0;
    }
    lead = fminf((now - g_net_lastrx) / 1000.0f, 0.25f);
    pos[0] = g_remote.vpos[0] + g_ai_vel[0] * lead; pos[1] = g_remote.vpos[1] + g_ai_vel[1] * lead; pos[2] = g_remote.vpos[2];
    /* the forward vector is the matrix's local +Y, hence the quarter turn (as noclip seeds its yaw) */
    yaw = atan2f(g_remote.vfwd[1], g_remote.vfwd[0]) - 1.5707963f;
    ent_place(v, pos, yaw);
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    /* seating waits for the pawn to have loaded, as the game's VehicleRide box does */
    if (!g_ai_seated && now - g_ai_lastseat >= 1000) {
        g_ai_lastseat = now;
        snprintf(code, sizeof code,
                 "local v = CVehicle_GetCurrentVehicle('%llu') if v and tostring(v:GetEntity()) == '%llu' then __out = 'in' "
                 "elseif IsEntityLoaded('%llu') == 1 then SetupVehicleFollowPath('%llu', '%llu', GetInvalidEntityId()) __out = 'seat' "
                 "else __out = 'wait' end", g_ai_id, g_ai_veh, g_ai_id, g_ai_veh, g_ai_id);
        lua_eval(code, out, sizeof out);
        if (strcmp(out, "in") == 0) { g_ai_seated = 1; logf_("ai: body seated in vehicle %llu", g_ai_veh); }
        else if (++g_ai_seattries == 6) logf_("ai: body still not seated after 6 tries (%s)", out);
    }
    if (now - g_ai_lastlog >= 1000) {
        g_ai_lastlog = now;
        d = sqrtf((t[0] - p[0]) * (t[0] - p[0]) + (t[1] - p[1]) * (t[1] - p[1]));
        logf_("ai: target (%.2f %.2f %.2f) pawn (%.2f %.2f %.2f) dist %.2f orders %u tps %u glide %u veh %llu seated %d",
              t[0], t[1], t[2], p[0], p[1], p[2], d, g_ai_orders, g_ai_tps, g_ai_glides, g_ai_veh, g_ai_seated);
    }
    return 1;
}

static void ai_tick(void)
{
    DWORD now;
    float t[3], p[3], d, moved, speed, lead;
    void *holder = NULL;
    BYTE *m;
    if (g_ai_src == AI_OFF || g_nstates == 0 || GetCurrentThreadId() != g_states[0].thread) return;
    now = GetTickCount();
    if (!ai_target(now, t)) return;
    /* a removed entity can still be found by id for a while (its object stays readable), so whether the pawn and
     * the marker still exist is asked from the engine, once a second */
    if (g_ai_arch[0] && g_ai_id && now - g_ai_lastvalid >= 1000) {
        char code[160], out[16];
        g_ai_lastvalid = now;
        snprintf(code, sizeof code, "__out = tostring(IsEntityValid('%llu')) .. tostring(IsEntityValid('%llu'))", g_ai_id, g_ai_marker);
        lua_eval(code, out, sizeof out);
        if (strcmp(out, "11") != 0 && !g_ai_lost) { logf_("ai: body or marker gone (valid %s)", out); g_ai_lost = 1; }
    }
    if (g_ai_lost || !g_ai_id || !ent_pos(g_ai_id, p) || !entity_obj(g_ai_marker, &holder)) {
        if (holder) { (*(int *)((BYTE *)holder + 8))--; holder = NULL; }
        if (g_ai_arch[0] && now - g_ai_lastspawn > 3000) { g_ai_lastspawn = now; ai_spawn(t, g_ai_spawns ? "lost, respawn" : "first"); }
        return;
    }
    if (holder) { (*(int *)((BYTE *)holder + 8))--; holder = NULL; }
    if (now - g_ai_prevt >= 100) {
        float dt = (now - g_ai_prevt) / 1000.0f;
        if (g_ai_prevt && dt < 1.0f) {
            g_ai_vel[0] += ((t[0] - g_ai_prev[0]) / dt - g_ai_vel[0]) * 0.3f;
            g_ai_vel[1] += ((t[1] - g_ai_prev[1]) / dt - g_ai_vel[1]) * 0.3f;
        }
        memcpy(g_ai_prev, t, 12); g_ai_prevt = now;
    }
    if (g_ai_src == AI_REMOTE && g_ai_arch[0]) {
        /* not a vehicle the local player sits in: both would drive it */
        if (g_remote.veh && g_remote.veh != g_ai_noveh && g_remote.veh != g_net_veh) {
            if (ai_drive(now, t, p)) return;
        } else if (g_ai_veh) {
            logf_("ai: peer left vehicle %llu", g_ai_veh);
            ai_vehicle_release();
            g_ai_lastspawn = now;
            ai_spawn(t, "peer left the vehicle");
            return;
        }
        ai_weapon(now);
    }
    speed = sqrtf(g_ai_vel[0] * g_ai_vel[0] + g_ai_vel[1] * g_ai_vel[1]);
    if (!g_ai_glide && g_ai_glide_on > 0 && speed > g_ai_glide_on) {
        g_ai_glide = 1; g_ai_glides++;
        logf_("ai: target at %.1f m/s, gliding", speed);
    } else if (g_ai_glide && (speed < g_ai_glide_off || g_ai_glide_on <= 0)) {
        g_ai_glide = 0;
        logf_("ai: target at %.1f m/s, walking again", speed);
        ai_order("glide end");
        memcpy(g_ai_last, p, 12); g_ai_lastmove = now;
    }
    lead = g_ai_lead;
    if (g_ai_glide) lead = g_ai_src == AI_REMOTE ? fminf((now - g_net_lastrx) / 1000.0f, 0.25f) : 0;
    m = entity_obj(g_ai_marker, &holder);
    if (m && !IsBadWritePtr(m + 0x60, 12)) {
        float *mp = (float *)(m + 0x60);
        mp[0] = t[0] + g_ai_vel[0] * lead; mp[1] = t[1] + g_ai_vel[1] * lead; mp[2] = t[2];
    }
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    d = sqrtf((t[0] - p[0]) * (t[0] - p[0]) + (t[1] - p[1]) * (t[1] - p[1]));
    if (g_ai_glide) {
        /* PeekMessage can run several times a frame; one teleport per frame is enough */
        if (now - g_ai_lastglide >= 15) {
            char code[192], out[32];
            g_ai_lastglide = now;
            snprintf(code, sizeof code, "TeleportEntity('%llu', '%llu') __out = 'ok'", g_ai_id, g_ai_marker);
            lua_eval(code, out, sizeof out);
        }
        if (now - g_ai_lastlog >= 1000) {
            g_ai_lastlog = now;
            logf_("ai: target (%.2f %.2f %.2f) pawn (%.2f %.2f %.2f) dist %.2f orders %u tps %u glide %u %.1f m/s", t[0], t[1], t[2], p[0], p[1], p[2], d, g_ai_orders, g_ai_tps, g_ai_glides, speed);
        }
        return;
    }
    /* the pawn's position only changes on the next frame after a teleport, hence the cooldown */
    if (d > g_ai_tp && now - g_ai_lastorder > 1000) {
        char code[192], out[32];
        snprintf(code, sizeof code, "TeleportEntity('%llu', '%llu') __out = 'ok'", g_ai_id, g_ai_marker);
        lua_eval(code, out, sizeof out);
        g_ai_tps++;
        logf_("ai: %.1f m behind, teleported", d);
        ai_order("after teleport");
        memcpy(g_ai_last, t, 12); g_ai_lastmove = now;
        return;
    }
    moved = fabsf(p[0] - g_ai_last[0]) + fabsf(p[1] - g_ai_last[1]);
    if (moved > 0.3f) { memcpy(g_ai_last, p, 12); g_ai_lastmove = now; g_ai_stuck = 0; }
    else if (d > g_ai_stall_m && now - g_ai_lastmove > g_ai_stall_ms && now - g_ai_lastorder > g_ai_stall_ms) {
        /* our own pawn that ignored five orders in a row while the target is away is replaced */
        if (g_ai_arch[0] && ++g_ai_stuck >= 5 && d > 5.0f) { logf_("ai: body ignores orders, respawning"); g_ai_lastspawn = now; ai_spawn(t, "stuck"); return; }
        ai_order("stalled");
    }
    if (now - g_ai_lastlog >= 1000) {
        g_ai_lastlog = now;
        logf_("ai: target (%.2f %.2f %.2f) pawn (%.2f %.2f %.2f) dist %.2f orders %u tps %u glide %u %.1f m/s", t[0], t[1], t[2], p[0], p[1], p[2], d, g_ai_orders, g_ai_tps, g_ai_glides, speed);
    }
}

/* aibody <archetype> <remote|local> | aifollow <pawn id> <remote|local|circle> [radius speed] | aispeed <n> |
 * aitp <metres> | aistall <ms> <metres> | ailead <seconds> | aiglide <on m/s> <off m/s> (0 = never) | aistop */
static int ai_cmd(const char *ln)
{
    unsigned long long id;
    char src[16], code[192], out[32], arch[96];
    float r = 0, v = 0, p[3];
    int n;
    if (sscanf(ln, "aibody %95s %15s", arch, src) == 2) {
        if (g_ai_src) ai_stop();
        if (strcmp(src, "remote") && strcmp(src, "local")) { logf_("ai: aibody follows remote or local"); return 1; }
        strcpy(g_ai_arch, arch);
        g_ai_src = !strcmp(src, "remote") ? AI_REMOTE : AI_LOCAL;
        ai_vehicle_release();
        g_ai_id = g_ai_marker = 0; g_ai_orders = g_ai_tps = g_ai_spawns = g_ai_glides = 0; g_ai_lastspawn = 0; g_ai_lost = 0; g_ai_glide = 0;
        g_ai_noveh = 0;
        g_ai_t0 = g_ai_lastlog = GetTickCount(); g_ai_vel[0] = g_ai_vel[1] = 0; g_ai_prevt = 0;
        logf_("ai: body %s follows %s (spawned once there is a target)", arch, src);
        return 1;
    }
    if (sscanf(ln, "aispeed %d", &n) == 1) { g_ai_speed = n; logf_("ai: speed %d", n); if (g_ai_src) ai_order("speed"); return 1; }
    if (sscanf(ln, "aitp %f", &r) == 1) { g_ai_tp = r; logf_("ai: teleport distance %.1f", r); return 1; }
    if (sscanf(ln, "ailead %f", &r) == 1) { g_ai_lead = r; logf_("ai: marker leads by %.2f s", r); return 1; }
    if (sscanf(ln, "aiglide %f %f", &r, &v) == 2) { g_ai_glide_on = r; g_ai_glide_off = v; logf_("ai: glide above %.1f m/s, walk below %.1f m/s", r, v); return 1; }
    if (sscanf(ln, "aistall %d %f", &n, &r) == 2) { g_ai_stall_ms = (DWORD)n; g_ai_stall_m = r; logf_("ai: re-order after %d ms standing, beyond %.1f m", n, r); return 1; }
    if (strncmp(ln, "aistop", 6) == 0) { ai_stop(); return 1; }
    if (sscanf(ln, "aifollow %llu %15s %f %f", &id, src, &r, &v) < 2) return 0;
    if (g_ai_src) ai_stop();
    if (!ent_pos(id, p)) { logf_("ai: no entity %llu", id); return 1; }
    snprintf(code, sizeof code, "__out = tostring(SpawnEntityFromArchetype('weapons.Secondary.Makarov', %.3f, %.3f, %.3f, 0, 0, 0))", p[0], p[1], p[2] + 0.5f);
    lua_eval(code, out, sizeof out);
    g_ai_marker = strtoull(out, NULL, 10);
    if (g_ai_marker == 0 || g_ai_marker == 0xFFFFFFFFFFFFFFFFull) { logf_("ai: marker spawn failed: %s", out); g_ai_marker = 0; return 1; }
    g_ai_id = id; g_ai_orders = g_ai_tps = g_ai_glides = 0; g_ai_glide = 0;
    g_ai_src = !strcmp(src, "remote") ? AI_REMOTE : !strcmp(src, "local") ? AI_LOCAL : AI_CIRCLE;
    if (g_ai_src == AI_CIRCLE) {
        if (r <= 0 || v <= 0) { logf_("ai: circle needs radius and speed"); ai_stop(); return 1; }
        memcpy(g_ai_c, p, 12); g_ai_r = r; g_ai_v = v;
    }
    g_ai_t0 = g_ai_lastlog = g_ai_lastmove = GetTickCount(); memcpy(g_ai_last, p, 12);
    g_ai_vel[0] = g_ai_vel[1] = 0; g_ai_prevt = 0;
    logf_("ai: following %s with pawn %llu, marker %llu", src, id, g_ai_marker);
    ai_order("start");
    return 1;
}

static void net_tick(void)
{
    DWORD now;
    if (g_net_cfg == 0) {
        if (g_av_id) av_remove();
        if (g_ns != INVALID_SOCKET) { closesocket(g_ns); g_ns = INVALID_SOCKET; logf_("net socket closed (tx=%u rx=%u)", g_net_tx, g_net_rx); }
        return;
    }
    if (g_nstates == 0 || GetCurrentThreadId() != g_states[0].thread) return;
    now = GetTickCount();
    if (g_net_cfg == 1) {
        WSADATA wd;
        struct sockaddr_in a;
        u_long nb = 1;
        WSAStartup(MAKEWORD(2, 2), &wd);
        memset(&g_peer, 0, sizeof g_peer);
        g_peer.sin_family = AF_INET; g_peer.sin_addr.s_addr = inet_addr(g_net_pip); g_peer.sin_port = htons((unsigned short)g_net_pport);
        if (g_peer.sin_addr.s_addr == INADDR_NONE) { logf_("net: bad peer address %s", g_net_pip); g_net_cfg = 0; return; }
        memset(&a, 0, sizeof a);
        /* a peer on this PC (127.x) keeps the port on loopback; any other peer needs the port on the network */
        a.sin_family = AF_INET; a.sin_port = htons((unsigned short)g_net_lport);
        a.sin_addr.s_addr = (ntohl(g_peer.sin_addr.s_addr) >> 24) == 127 ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
        g_ns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (bind(g_ns, (struct sockaddr *)&a, sizeof a) != 0) { logf_("net: bind %u failed (%d)", g_net_lport, WSAGetLastError()); closesocket(g_ns); g_ns = INVALID_SOCKET; g_net_cfg = 0; return; }
        ioctlsocket(g_ns, FIONBIO, &nb);
        g_net_seq = g_net_tx = g_net_rx = g_net_drop = 0; g_net_lastsend = g_net_lastlog = now; g_net_lastrx = 0;
        memset(&g_remote, 0, sizeof g_remote);
        g_net_cfg = 2;
        logf_("net: socket up, local %s:%u -> peer %s:%u", a.sin_addr.s_addr == htonl(INADDR_LOOPBACK) ? "127.0.0.1" : "0.0.0.0", g_net_lport, g_net_pip, g_net_pport);
    }
    if (g_walk && g_bodymat && !IsBadWritePtr(g_bodymat, 0x40)) {
        float t = (GetTickCount() - g_walk_t0) / 1000.0f;
        if (t > g_walk_secs) { g_walk = 0; logf_("walk finished"); }
        else if (g_walk == 2) {
            /* find the leg (or the pause after it) that time t falls in */
            float x = g_walk_c[0], y = g_walk_c[1];
            int k;
            for (k = 0; k < g_nlegs; k++) {
                float len = sqrtf(g_legs[k][0] * g_legs[k][0] + g_legs[k][1] * g_legs[k][1]), dur = len / g_walk_speed;
                if (t < dur) { x += g_legs[k][0] * t / dur; y += g_legs[k][1] * t / dur; break; }
                x += g_legs[k][0]; y += g_legs[k][1]; t -= dur;
                if (t < g_leg_pause) break;
                t -= g_leg_pause;
            }
            *(float *)(g_bodymat + 0x30) = x; *(float *)(g_bodymat + 0x34) = y; *(float *)(g_bodymat + 0x38) = g_walk_c[2];
        }
        else {
            float ang = g_walk_speed * t / g_walk_r;
            *(float *)(g_bodymat + 0x30) = g_walk_c[0] + g_walk_r * (cosf(ang) - 1.0f);
            *(float *)(g_bodymat + 0x34) = g_walk_c[1] + g_walk_r * sinf(ang);
            *(float *)(g_bodymat + 0x38) = g_walk_c[2];
        }
    }
    /* make sure we know our own body */
    if (!g_bodymat || IsBadReadPtr(g_body, 4) || *(unsigned *)g_body != (unsigned)(g_base + RVA_BODY_VTABLE)) {
        if (now - g_net_lastfind > 3000) {
            char c[64];
            g_net_lastfind = now;
            snprintf(c, sizeof c, "findbody %llu", g_net_id);
            native_cmd(c);
        }
    }
    /* receive everything pending; only packets from the peer's address count (the port may differ behind NAT) */
    for (;;) {
        NetPkt p;
        struct sockaddr_in from;
        int fl = sizeof from;
        int r = recvfrom(g_ns, (char *)&p, sizeof p, 0, (struct sockaddr *)&from, &fl);
        if (r == SOCKET_ERROR) { if (WSAGetLastError() == WSAEMSGSIZE) { g_net_drop++; continue; } break; }
        if (r != (int)sizeof p || p.magic != NET_MAGIC || from.sin_addr.s_addr != g_peer.sin_addr.s_addr) { g_net_drop++; continue; }
        p.wpn[sizeof p.wpn - 1] = 0;
        g_remote = p; g_net_rx++; g_net_lastrx = now;
    }
    /* with an AI body following the peer, the puppet avatar is not spawned */
    if (g_ai_src != AI_REMOTE) av_tick(now);
    if (now - g_net_lastveh >= 1000) {
        char out[32];
        unsigned long long v;
        g_net_lastveh = now;
        lua_eval("local v = CVehicle_GetCurrentVehicle(GetLocalPlayerId()) if v then __out = tostring(v:GetEntity()) else __out = '0' end", out, sizeof out);
        v = strtoull(out, NULL, 10);
        if (v == 0xFFFFFFFFFFFFFFFFull) v = 0;
        if (v != g_net_veh) logf_("net: local player %s vehicle %llu", v ? "entered" : "left", v ? v : g_net_veh);
        g_net_veh = v;
    }
    /* send at 20 Hz */
    if (g_bodymat && now - g_net_lastsend >= 50) {
        NetPkt p;
        void *holder = NULL;
        BYTE *obj = entity_obj(g_net_id, &holder);
        g_net_lastsend = now;
        memset(&p, 0, sizeof p);
        p.magic = NET_MAGIC; p.seq = ++g_net_seq;
        memcpy(p.pos, g_bodymat + 0x30, sizeof p.pos);
        p.dir[0] = obj ? *(float *)(obj + 0x30) : 0; p.dir[1] = obj ? *(float *)(obj + 0x34) : 0;
        {
            unsigned long long wid = held_weapon_id(obj);
            if (wid != g_net_wid) {
                char code[96];
                g_net_wid = wid; g_net_wpn[0] = 0;
                if (wid) {
                    snprintf(code, sizeof code, "__out = tostring(GetEntityName('%llu'))", wid);
                    lua_eval(code, g_net_wpn, sizeof g_net_wpn);
                }
                logf_("net: local player holds %s", wid ? g_net_wpn : "nothing");
            }
            memcpy(p.wpn, g_net_wpn, sizeof p.wpn);
        }
        if (holder) { (*(int *)((BYTE *)holder + 8))--; holder = NULL; }
        if (g_net_veh && (obj = entity_obj(g_net_veh, &holder)) && !IsBadReadPtr(obj, 0x70)) {
            p.veh = g_net_veh;
            memcpy(p.vpos, obj + 0x60, sizeof p.vpos);
            p.vfwd[0] = *(float *)(obj + 0x40); p.vfwd[1] = *(float *)(obj + 0x44);
        }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
        sendto(g_ns, (char *)&p, sizeof p, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
        g_net_tx++;
    }
    if (now - g_net_lastlog >= 1000) {
        g_net_lastlog = now;
        logf_("net: tx=%u rx=%u local=(%.2f %.2f %.2f) remote=(%.2f %.2f %.2f) seq=%u age=%lums drop=%u", g_net_tx, g_net_rx,
              g_bodymat ? *(float *)(g_bodymat + 0x30) : 0, g_bodymat ? *(float *)(g_bodymat + 0x34) : 0, g_bodymat ? *(float *)(g_bodymat + 0x38) : 0,
              g_remote.pos[0], g_remote.pos[1], g_remote.pos[2], g_remote.seq, g_net_lastrx ? (unsigned long)(now - g_net_lastrx) : 0UL, g_net_drop);
    }
}

static void pump_main(void)
{
    FILE *f;
    if (g_nstates == 0 || g_busy || GetCurrentThreadId() != g_states[0].thread) return;
    if (GetTickCount() - g_lastpoll < 250) return;
    g_lastpoll = GetTickCount();
    f = fopen(g_natpath, "rb");
    if (f) { fclose(f); g_busy = 1; run_native(); g_busy = 0; return; }
    f = fopen(g_cmdpath, "rb");
    if (!f) return;
    fclose(f);
    g_busy = 1;
    run_cmd(g_states[0].L);
    g_busy = 0;
}
static BOOL WINAPI h_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT c) { hold_tick(); net_tick(); ai_tick(); pump_main(); return o_PeekMessageA(m, h, a, b, c); }
static BOOL WINAPI h_PeekMessageW(LPMSG m, HWND h, UINT a, UINT b, UINT c) { hold_tick(); net_tick(); ai_tick(); pump_main(); return o_PeekMessageW(m, h, a, b, c); }

static int __cdecl h_gettop(void *L)
{
    unsigned long tid = GetCurrentThreadId();
    int i, best = -1;
    for (i = 0; i < g_nstates; i++) if (g_states[i].L == L) break;
    if (i == g_nstates && g_nstates < MAXL) {
        g_states[i].L = L; g_states[i].thread = tid; g_states[i].hits = 0; g_nstates++;
        logf_("new lua_State %p first seen on thread %lu", L, tid);
    }
    if (i < MAXL) g_states[i].hits++;
    return o_gettop(L);
}

static void install_lua(HMODULE m)
{
    char *s;
    g_base = (BYTE *)m;
    lua_settop_ = (void *)(g_base + RVA_SETTOP);
    lua_dobuffer_ = (void *)(g_base + RVA_DOBUFFER);
    lua_checklstr_ = (void *)(g_base + RVA_CHECKLSTR);
    lua_getglobal_ = (void *)(g_base + RVA_GETGLOBAL);
    GetModuleFileNameA(g_self, g_cmdpath, MAX_PATH);
    s = strrchr(g_cmdpath, '\\');
    strcpy(s + 1, "coop_cmd.lua");
    strcpy(g_natpath, g_cmdpath);
    strcpy(strrchr(g_natpath, '\\') + 1, "coop_native.txt");
    o_gettop = (void *)make_detour(g_base + RVA_GETTOP, 7, h_gettop, 1);
    make_detour(g_base + RVA_ALERT, 6, h_alert, 0);
    logf_("lua hooks installed (gettop, _ALERT); command file %s", g_cmdpath);
}

static DWORD WINAPI watcher(LPVOID p)
{
    int tries;
    (void)p;
    for (tries = 0; tries < 1200; tries++) {
        HMODULE m = GetModuleHandleA("Dunia.dll");
        if (m) {
            logf_("Dunia.dll at %p, patching IAT", (void *)m);
            logf_("patched %d IAT entries", patch_iat(m));
            install_lua(m);
            return 0;
        }
        Sleep(50);
    }
    logf_("Dunia.dll never appeared");
    return 0;
}

/* real dinput8 */
static FARPROC g_real;
__declspec(naked) HRESULT WINAPI DirectInput8Create(HINSTANCE a, DWORD b, REFIID c, LPVOID *d, LPUNKNOWN e)
{
    __asm__ __volatile__("jmp *%0" : : "m"(g_real));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], sys[MAX_PATH];
        HMODULE r;
        g_self = inst;
        SetUnhandledExceptionFilter(crash_filter);
        AddVectoredExceptionHandler(1, veh_filter);
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        GetModuleFileNameA(inst, path, MAX_PATH);
        strcpy(strrchr(path, '\\') + 1, "coop_probe.log");
        g_log = fopen(path, "a");
        logf_("probe loaded, cmdline: %s", GetCommandLineA());
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\dinput8.dll");
        r = LoadLibraryA(sys);
        g_real = r ? GetProcAddress(r, "DirectInput8Create") : NULL;
        CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    }
    return TRUE;
}
