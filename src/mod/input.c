#include "input.h"

#include <windows.h>

#include "config.h"
#include "game.h"
#include "log.h"

// Declared here instead of pulling in <xinput.h>, since the DLL is loaded by
// name at runtime (the same way the game's engine does it).
typedef struct {
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT lx, ly, rx, ry;
} XGamepad;

typedef struct {
    DWORD packet;
    XGamepad pad;
} XState;

enum {
    XB_DPAD_UP = 0x0001,
    XB_DPAD_DOWN = 0x0002,
    XB_DPAD_LEFT = 0x0004,
    XB_DPAD_RIGHT = 0x0008,
    XB_LB = 0x0100,
    XB_RB = 0x0200,
    XB_A = 0x1000,
    XB_B = 0x2000,
    XB_X = 0x4000,
    XB_Y = 0x8000,
};

#define PAD_SLOTS 4
#define STICK_DEADZONE 12000
#define TRIGGER_THRESHOLD 60
// Asking XInput about an empty slot is slow, so empty slots are only
// re-checked this often (in frames).
#define RECHECK_INTERVAL 120

static DWORD(WINAPI *g_get_state)(DWORD, XState *);
static bool g_tried_load;
static bool g_connected[PAD_SLOTS];
static XGamepad g_pads[PAD_SLOTS];
static int g_recheck[PAD_SLOTS];
static bool g_focused;

static void load_xinput(void)
{
    static const wchar_t *names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    g_tried_load = true;
    for (int i = 0; i < 3 && !g_get_state; i++) {
        HMODULE m = LoadLibraryW(names[i]);
        if (m) {
            g_get_state = (void *)GetProcAddress(m, "XInputGetState");
            if (g_get_state)
                log_msg("input: using %ls", names[i]);
        }
    }
    if (!g_get_state)
        log_msg("input: XInput not available, extra players are keyboard-only");
}

void input_poll(void)
{
    if (!g_tried_load)
        load_xinput();

    // Keys are read with GetAsyncKeyState, which works even when another
    // window has focus - so only honour input while the game is in front.
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    g_focused = pid == GetCurrentProcessId();

    if (!g_get_state)
        return;
    for (int i = 0; i < PAD_SLOTS; i++) {
        if (!g_connected[i] && g_recheck[i] > 0) {
            g_recheck[i]--;
            continue;
        }
        XState st;
        bool ok = g_get_state((DWORD)i, &st) == ERROR_SUCCESS;
        if (ok != g_connected[i])
            log_msg("input: pad %d %s", i, ok ? "connected" : "disconnected");
        g_connected[i] = ok;
        if (ok)
            g_pads[i] = st.pad;
        else
            g_recheck[i] = RECHECK_INTERVAL;
    }
}

bool input_pad_connected(int slot)
{
    return slot >= 0 && slot < PAD_SLOTS && g_connected[slot];
}

uint32_t input_read_pad(int slot)
{
    if (!input_pad_connected(slot) || !g_focused)
        return 0;
    const XGamepad *p = &g_pads[slot];
    uint32_t in = 0;
    if ((p->buttons & XB_DPAD_UP) || p->ly > STICK_DEADZONE)
        in |= IN_UP;
    if ((p->buttons & XB_DPAD_DOWN) || p->ly < -STICK_DEADZONE)
        in |= IN_DOWN;
    if ((p->buttons & XB_DPAD_LEFT) || p->lx < -STICK_DEADZONE)
        in |= IN_LEFT;
    if ((p->buttons & XB_DPAD_RIGHT) || p->lx > STICK_DEADZONE)
        in |= IN_RIGHT;
    if (p->buttons & XB_A)
        in |= IN_SHOOT;
    if (p->buttons & (XB_B | XB_Y))
        in |= IN_BOMB;
    if ((p->buttons & (XB_X | XB_LB | XB_RB)) || p->left_trigger > TRIGGER_THRESHOLD ||
        p->right_trigger > TRIGGER_THRESHOLD)
        in |= IN_FOCUS;
    return in;
}

uint32_t input_read_keys(const int *keys)
{
    static const uint32_t bits[KEY_COUNT] = {IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_SHOOT, IN_BOMB, IN_FOCUS};
    if (!g_focused)
        return 0;
    uint32_t in = 0;
    for (int k = 0; k < KEY_COUNT; k++)
        if (keys[k] && (GetAsyncKeyState(keys[k]) & 0x8000))
            in |= bits[k];
    return in;
}
