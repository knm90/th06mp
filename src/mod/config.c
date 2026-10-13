#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <windows.h>

#include "log.h"

Config g_cfg;

static const struct {
    const wchar_t *name;
    int vk;
} g_key_names[] = {
    {L"NONE", 0},          {L"UP", VK_UP},         {L"DOWN", VK_DOWN},       {L"LEFT", VK_LEFT},
    {L"RIGHT", VK_RIGHT},  {L"SHIFT", VK_SHIFT},   {L"LSHIFT", VK_LSHIFT},   {L"RSHIFT", VK_RSHIFT},
    {L"CTRL", VK_CONTROL}, {L"LCTRL", VK_LCONTROL}, {L"RCTRL", VK_RCONTROL}, {L"LALT", VK_LMENU},
    {L"RALT", VK_RMENU},   {L"SPACE", VK_SPACE},   {L"ENTER", VK_RETURN},    {L"TAB", VK_TAB},
    {L"COMMA", VK_OEM_COMMA}, {L"PERIOD", VK_OEM_PERIOD}, {L"SLASH", VK_OEM_2}, {L"SEMICOLON", VK_OEM_1},
    {L"NUM0", VK_NUMPAD0}, {L"NUM1", VK_NUMPAD1},  {L"NUM2", VK_NUMPAD2},    {L"NUM3", VK_NUMPAD3},
    {L"NUM4", VK_NUMPAD4}, {L"NUM5", VK_NUMPAD5},  {L"NUM6", VK_NUMPAD6},    {L"NUM7", VK_NUMPAD7},
    {L"NUM8", VK_NUMPAD8}, {L"NUM9", VK_NUMPAD9},
};

// Accepts a key name from the table, a single letter/digit, or a 0x.. virtual-key code.
static int parse_key(const wchar_t *s, int fallback)
{
    if (!s[0])
        return fallback;
    for (size_t i = 0; i < sizeof(g_key_names) / sizeof(g_key_names[0]); i++)
        if (_wcsicmp(s, g_key_names[i].name) == 0)
            return g_key_names[i].vk;
    if (!s[1] && iswalnum(s[0]))
        return towupper(s[0]);
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
        return (int)wcstol(s, NULL, 16);
    log_msg("config: unknown key name '%ls'", s);
    return fallback;
}

static void read_str(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, wchar_t *out, int cap)
{
    GetPrivateProfileStringW(sec, key, L"", out, cap, ini);
    // strip a trailing "; comment" and whitespace
    wchar_t *c = wcschr(out, L';');
    if (c)
        *c = 0;
    size_t n = wcslen(out);
    while (n && iswspace(out[n - 1]))
        out[--n] = 0;
}

static int read_int(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, int fallback)
{
    wchar_t buf[64];
    read_str(ini, sec, key, buf, 64);
    return buf[0] ? (int)wcstol(buf, NULL, 0) : fallback;
}

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.players = 2;
    g_cfg.p1_input = P1_INPUT_AUTO;
    g_cfg.tint = 1;
    g_cfg.target_mode = TARGET_ROTATE;
    g_cfg.target_period = 180;
    g_cfg.net_port = 17606;
    g_cfg.net_delay = 3;
    strcpy(g_cfg.net_address, "127.0.0.1");

    // P1's keys are only used when p1_input ends up as "keyboard".
    static const int p1_keys[KEY_COUNT] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', VK_LSHIFT};
    static const int p2_keys[KEY_COUNT] = {'W', 'S', 'A', 'D', 'J', 'K', 'L'};
    memcpy(g_cfg.p[0].keys, p1_keys, sizeof(p1_keys));
    memcpy(g_cfg.p[1].keys, p2_keys, sizeof(p2_keys));
    static const uint32_t tints[MAX_PLAYERS] = {0xFFFFFF, 0xA8D4FF, 0xFFD8A0, 0xB0FFB0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_cfg.p[i].pad = PAD_AUTO;
        g_cfg.p[i].tint = tints[i];
        // Four distinct combinations: P2 is the other character, P3 the other
        // shot type, P4 both.
        g_cfg.p[i].character = i % 2 ? CHARACTER_OTHER : CHARACTER_SAME;
        g_cfg.p[i].shot = i >= 2 ? SHOT_OTHER : SHOT_SAME;
    }
}

void config_load(const wchar_t *ini)
{
    static const wchar_t *key_names[KEY_COUNT] = {L"up", L"down", L"left", L"right", L"shot", L"bomb", L"focus"};
    wchar_t buf[64];

    defaults();
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        log_msg("config: %ls not found, using defaults", ini);
        return;
    }

    g_cfg.players = read_int(ini, L"th06mp", L"players", g_cfg.players);
    if (g_cfg.players < 1)
        g_cfg.players = 1;
    if (g_cfg.players > MAX_PLAYERS)
        g_cfg.players = MAX_PLAYERS;
    g_cfg.tint = read_int(ini, L"th06mp", L"tint", g_cfg.tint);
    g_cfg.debug = read_int(ini, L"th06mp", L"debug", g_cfg.debug);
    g_cfg.target_period = read_int(ini, L"th06mp", L"target_period", g_cfg.target_period);
    if (g_cfg.target_period < 1)
        g_cfg.target_period = 1;

    read_str(ini, L"th06mp", L"p1_input", buf, 64);
    if (_wcsicmp(buf, L"game") == 0)
        g_cfg.p1_input = P1_INPUT_GAME;
    else if (_wcsicmp(buf, L"keyboard") == 0)
        g_cfg.p1_input = P1_INPUT_KEYBOARD;

    read_str(ini, L"th06mp", L"target", buf, 64);
    if (_wcsicmp(buf, L"p1") == 0)
        g_cfg.target_mode = TARGET_P1;

    read_str(ini, L"net", L"mode", buf, 64);
    if (_wcsicmp(buf, L"host") == 0)
        g_cfg.net_mode = NET_HOST;
    else if (_wcsicmp(buf, L"join") == 0)
        g_cfg.net_mode = NET_JOIN;
    read_str(ini, L"net", L"address", buf, 64);
    if (buf[0])
        WideCharToMultiByte(CP_ACP, 0, buf, -1, g_cfg.net_address, sizeof(g_cfg.net_address), NULL, NULL);
    g_cfg.net_port = read_int(ini, L"net", L"port", g_cfg.net_port);
    g_cfg.net_delay = read_int(ini, L"net", L"delay", g_cfg.net_delay);
    if (g_cfg.net_delay < 1 || g_cfg.net_delay > 30) // the ring in net.c holds 256 ticks
        g_cfg.net_delay = 3;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        wchar_t sec[8];
        wsprintfW(sec, L"p%d", i + 1);
        PlayerConfig *pc = &g_cfg.p[i];

        read_str(ini, sec, L"pad", buf, 64);
        if (_wcsicmp(buf, L"none") == 0)
            pc->pad = PAD_NONE;
        else if (buf[0] >= L'0' && buf[0] <= L'3' && !buf[1])
            pc->pad = buf[0] - L'0';

        for (int k = 0; k < KEY_COUNT; k++) {
            read_str(ini, sec, key_names[k], buf, 64);
            pc->keys[k] = parse_key(buf, pc->keys[k]);
        }

        read_str(ini, sec, L"tint", buf, 64);
        if (buf[0])
            pc->tint = (uint32_t)wcstoul(buf, NULL, 16) & 0xFFFFFF;

        static const wchar_t *shot_names[] = {L"same", L"other", L"a", L"b"};
        read_str(ini, sec, L"shot_type", buf, 64); // "shot" is the shot button's key
        for (int s = 0; s < 4; s++)
            if (_wcsicmp(buf, shot_names[s]) == 0)
                pc->shot = s;

        static const wchar_t *character_names[] = {L"same", L"other", L"reimu", L"marisa"};
        read_str(ini, sec, L"character", buf, 64);
        for (int c = 0; c < 4; c++)
            if (_wcsicmp(buf, character_names[c]) == 0)
                pc->character = c;
    }
    log_msg("config: players=%d p1_input=%d tint=%d target=%d/%d", g_cfg.players, g_cfg.p1_input, g_cfg.tint,
            g_cfg.target_mode, g_cfg.target_period);
}
