#include "net.h"

#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "log.h"

// Topology: guests talk only to the host. A guest sends its input for tick
// T+delay while it simulates tick T; the host merges everyone's inputs into
// one frame per tick and sends that frame to every guest as soon as it is
// complete. Nobody simulates tick T before holding frame T.
//
// shortcut: TCP, so one lost packet delays everything behind it; move to UDP
// with redundant inputs if internet play stutters.

#define MAGIC 0x504D3654u // "T6MP"
#define VERSION 1
#define RING 256 // ticks of history kept; far more than peers can drift apart
#define STALL_TIMEOUT_MS 10000

#pragma pack(push, 1)
typedef struct {
    uint32_t magic, version;
} Hello;

typedef struct {
    uint32_t tick, input;                   // input for `tick`
    uint32_t check_tick, check[NET_CHECKS]; // sender's state at the start of `check_tick`
} GuestMsg;

typedef struct {
    uint32_t tick, inputs[MAX_PLAYERS];
} Frame;
#pragma pack(pop)

static bool g_host;
static int g_players, g_delay;
static SOCKET g_sock[MAX_PLAYERS]; // host: one per guest slot; guest: [0] is the host
static uint32_t g_tick;
static uint32_t g_inputs[RING][MAX_PLAYERS];
// host
static uint32_t g_known[MAX_PLAYERS]; // inputs of this slot are known for ticks below this
static uint32_t g_sent;               // frames below this were sent to the guests
static uint32_t g_my_check[RING][NET_CHECKS];
static struct {
    uint32_t tick, check[NET_CHECKS];
} g_guest_check[MAX_PLAYERS][RING];
static bool g_desync_logged;
// guest
static uint32_t g_have; // frames below this have arrived

// --- waiting without freezing the game window --------------------------------

static HWND g_status;

static void status(const wchar_t *text)
{
    if (!text) {
        if (g_status)
            DestroyWindow(g_status);
        g_status = NULL;
    } else if (g_status) {
        SetWindowTextW(g_status, text);
    } else {
        const int w = 560, h = 56;
        g_status = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", text,
                                   WS_POPUP | WS_VISIBLE | WS_BORDER | SS_CENTER | SS_CENTERIMAGE,
                                   (GetSystemMetrics(SM_CXSCREEN) - w) / 2, 48, w, h, NULL, NULL, NULL, NULL);
    }
}

// Runs the thread's message loop once. False = stop waiting: the game is
// closing, or (while connecting) Esc is down.
// shortcut: Esc is read globally, so it also cancels when typed in another app.
static bool pump(bool esc_cancels)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam); // leave it for the game's own loop
            return false;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return !(esc_cancels && (GetAsyncKeyState(VK_ESCAPE) & 0x8000));
}

// --- sockets -----------------------------------------------------------------

static bool send_all(SOCKET s, const void *data, int size)
{
    return send(s, data, size, 0) == size;
}

static bool readable(SOCKET s, int ms)
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    struct timeval tv = {ms / 1000, ms % 1000 * 1000};
    return select(0, &r, NULL, NULL, &tv) == 1;
}

// Reads one whole message if it has fully arrived. Never blocks. *lost is set
// when the peer has closed the connection.
static bool read_msg(SOCKET s, void *msg, int size, bool *lost)
{
    u_long n = 0;
    if (ioctlsocket(s, FIONREAD, &n) != 0) {
        *lost = true;
        return false;
    }
    if ((int)n >= size) {
        if (recv(s, msg, size, MSG_WAITALL) == size)
            return true;
        *lost = true;
        return false;
    }
    char c;
    if (n == 0 && readable(s, 0) && recv(s, &c, 1, MSG_PEEK) <= 0)
        *lost = true; // "readable" with nothing to read means closed
    return false;
}

// Blocks for a whole message, keeping the window alive. For the handshake.
static bool read_blocking(SOCKET s, void *msg, int size)
{
    bool lost = false;
    while (!read_msg(s, msg, size, &lost)) {
        if (lost || !pump(true))
            return false;
        readable(s, 50);
    }
    return true;
}

static void no_delay(SOCKET s)
{
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
}

// --- connecting --------------------------------------------------------------

static bool host_connect(NetSession *s)
{
    wchar_t text[128];
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((u_short)g_cfg.net_port)};
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, MAX_PLAYERS) != 0) {
        log_msg("net: cannot listen on port %d (error %d)", g_cfg.net_port, WSAGetLastError());
        closesocket(ls);
        return false;
    }
    bool ok = true;
    for (int slot = 1; slot < s->players && ok; slot++) {
        wsprintfW(text, L"th06mp: waiting for player %d on port %d   (Esc: play offline)", slot + 1, g_cfg.net_port);
        status(text);
        while (ok && !readable(ls, 50))
            ok = pump(true);
        if (!ok)
            break;
        SOCKET c = accept(ls, NULL, NULL);
        Hello hello;
        NetSession theirs = *s;
        theirs.slot = (uint8_t)slot;
        no_delay(c);
        if (c == INVALID_SOCKET || !read_blocking(c, &hello, sizeof(hello)) || hello.magic != MAGIC ||
            hello.version != VERSION || !send_all(c, &theirs, sizeof(theirs))) {
            log_msg("net: rejected a connection (not th06mp version %d)", VERSION);
            closesocket(c);
            slot--; // keep waiting for this player
            continue;
        }
        g_sock[slot] = c;
        log_msg("net: player %d connected", slot + 1);
    }
    closesocket(ls);
    // Everyone is here: release the guests together.
    const char go = 1;
    for (int slot = 1; slot < s->players && ok; slot++)
        ok = send_all(g_sock[slot], &go, 1);
    return ok;
}

static bool guest_connect(NetSession *s)
{
    wchar_t text[160];
    char port[8];
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    wsprintfA(port, "%d", g_cfg.net_port);
    if (getaddrinfo(g_cfg.net_address, port, &hints, &res) != 0 || !res) {
        log_msg("net: cannot resolve '%s'", g_cfg.net_address);
        return false;
    }
    wsprintfW(text, L"th06mp: connecting to %hs:%d   (Esc: play offline)", g_cfg.net_address, g_cfg.net_port);
    status(text);

    // Retry until the host is listening. Non-blocking, so an unreachable
    // address does not freeze the window for the system's connect timeout.
    SOCKET c = INVALID_SOCKET;
    bool ok = true, connected = false;
    while (ok && !connected) {
        c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        u_long nonblocking = 1;
        ioctlsocket(c, FIONBIO, &nonblocking);
        connect(c, res->ai_addr, (int)res->ai_addrlen);
        for (int waited = 0; ok && !connected && waited < 3000; waited += 50) {
            fd_set w, e;
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(c, &w);
            FD_SET(c, &e);
            struct timeval tv = {0, 50000};
            select(0, NULL, &w, &e, &tv);
            connected = FD_ISSET(c, &w);
            if (FD_ISSET(c, &e)) { // refused: host not up yet
                Sleep(300);
                break;
            }
            ok = pump(true);
        }
        if (!connected)
            closesocket(c);
    }
    freeaddrinfo(res);
    if (!connected)
        return false;
    u_long blocking = 0;
    ioctlsocket(c, FIONBIO, &blocking);
    no_delay(c);

    const Hello hello = {MAGIC, VERSION};
    char go;
    status(L"th06mp: connected, waiting for the other players   (Esc: play offline)");
    if (!send_all(c, &hello, sizeof(hello)) || !read_blocking(c, s, sizeof(*s)) || !read_blocking(c, &go, 1)) {
        log_msg("net: lost the host during setup");
        closesocket(c);
        return false;
    }
    g_sock[0] = c;
    return true;
}

bool net_connect(NetSession *s)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_sock[i] = INVALID_SOCKET;
    g_host = g_cfg.net_mode == NET_HOST;
    bool ok = g_host ? host_connect(s) : guest_connect(s);
    status(NULL);
    if (!ok) {
        log_msg("net: not connected, playing offline");
        return false;
    }

    g_players = s->players;
    g_delay = s->delay;
    g_tick = g_sent = g_have = 0;
    memset(g_inputs, 0, sizeof(g_inputs));
    memset(g_guest_check, 0xFF, sizeof(g_guest_check));
    // Nobody has input for the first `delay` ticks; those count as known, empty.
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_known[i] = (uint32_t)g_delay;
    log_msg("net: %s, %d players, this machine is player %d, delay %d", g_host ? "hosting" : "joined", g_players,
            s->slot + 1, g_delay);
    return true;
}

// --- per tick ----------------------------------------------------------------

// Sleeps briefly for network data. False once the stall has lasted too long
// or the game is closing.
static bool stall(SOCKET any, DWORD started)
{
    DWORD waited = GetTickCount() - started;
    if (waited > STALL_TIMEOUT_MS) {
        log_msg("net: no data for %d s at tick %u", STALL_TIMEOUT_MS / 1000, g_tick);
        return false;
    }
    readable(any, 2);
    return waited < 100 || pump(false); // only bother the message loop on real stalls
}

static void compare_check(int slot, uint32_t tick)
{
    const uint32_t *theirs = g_guest_check[slot][tick % RING].check, *mine = g_my_check[tick % RING];
    if (g_desync_logged || g_guest_check[slot][tick % RING].tick != tick || memcmp(theirs, mine, sizeof(g_my_check[0])) == 0)
        return;
    g_desync_logged = true;
    log_msg("net: DESYNC with player %d at tick %u: rng %08X/%08X score %08X/%08X state %08X/%08X (host/guest)",
            slot + 1, tick, mine[0], theirs[0], mine[1], theirs[1], mine[2], theirs[2]);
}

static bool host_tick(uint32_t local, const uint32_t *check, uint32_t *inputs)
{
    const uint32_t t = g_tick;
    g_inputs[(t + g_delay) % RING][0] = local;
    g_known[0] = t + g_delay + 1;
    memcpy(g_my_check[t % RING], check, sizeof(g_my_check[0]));
    for (int slot = 1; slot < g_players; slot++)
        compare_check(slot, t); // guests may already have reported this tick

    for (DWORD started = GetTickCount();;) {
        uint32_t complete = g_known[0];
        for (int slot = 1; slot < g_players; slot++) {
            GuestMsg m;
            bool lost = false;
            while (read_msg(g_sock[slot], &m, sizeof(m), &lost)) {
                if (m.tick != g_known[slot]) {
                    log_msg("net: player %d sent tick %u, expected %u", slot + 1, m.tick, g_known[slot]);
                    return false;
                }
                g_inputs[m.tick % RING][slot] = m.input;
                g_known[slot]++;
                g_guest_check[slot][m.check_tick % RING].tick = m.check_tick;
                memcpy(g_guest_check[slot][m.check_tick % RING].check, m.check, sizeof(m.check));
                if (m.check_tick <= t)
                    compare_check(slot, m.check_tick);
            }
            if (lost) {
                log_msg("net: player %d disconnected", slot + 1);
                return false;
            }
            if (g_known[slot] < complete)
                complete = g_known[slot];
        }
        for (; g_sent < complete; g_sent++) {
            Frame f = {.tick = g_sent};
            memcpy(f.inputs, g_inputs[g_sent % RING], sizeof(f.inputs));
            for (int slot = 1; slot < g_players; slot++)
                if (!send_all(g_sock[slot], &f, sizeof(f)))
                    return false;
        }
        if (g_sent > t)
            break;
        if (!stall(g_sock[1], started))
            return false;
    }
    memcpy(inputs, g_inputs[t % RING], sizeof(g_inputs[0]));
    return true;
}

static bool guest_tick(uint32_t local, const uint32_t *check, uint32_t *inputs)
{
    const uint32_t t = g_tick;
    GuestMsg m = {.tick = t + g_delay, .input = local, .check_tick = t};
    memcpy(m.check, check, sizeof(m.check));
    if (!send_all(g_sock[0], &m, sizeof(m)))
        return false;

    for (DWORD started = GetTickCount(); g_have <= t;) {
        Frame f;
        bool lost = false;
        if (read_msg(g_sock[0], &f, sizeof(f), &lost)) {
            if (f.tick != g_have) {
                log_msg("net: host sent frame %u, expected %u", f.tick, g_have);
                return false;
            }
            memcpy(g_inputs[f.tick % RING], f.inputs, sizeof(f.inputs));
            g_have++;
        } else if (lost) {
            log_msg("net: host disconnected");
            return false;
        } else if (!stall(g_sock[0], started)) {
            return false;
        }
    }
    memcpy(inputs, g_inputs[t % RING], sizeof(g_inputs[0]));
    return true;
}

bool net_tick(uint32_t local_input, const uint32_t check[NET_CHECKS], uint32_t inputs[MAX_PLAYERS])
{
    bool ok = g_host ? host_tick(local_input, check, inputs) : guest_tick(local_input, check, inputs);
    if (ok) {
        g_tick++;
    } else {
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (g_sock[i] != INVALID_SOCKET)
                closesocket(g_sock[i]);
    }
    return ok;
}
