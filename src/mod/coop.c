// Extra player characters.
//
// The game has one global Player (g_Player = P1). Its update/draw callbacks
// and most of its collision routines take the Player as a pointer argument,
// so players 2-4 are plain copies of that struct owned by this DLL: every
// hook below runs the original routine for P1 and then once more per extra
// player. Lives, bombs, power and score live in GameManager and are shared.
#include "coop.h"

#include <string.h>
#include <windows.h>

#include "config.h"
#include "game.h"
#include "hook.h"
#include "input.h"
#include "log.h"
#include "net.h"

#define GAME(type, rva) ((type *)(g_base + (rva)))
#define LOG_ONCE(...)            \
    do {                         \
        static bool logged;      \
        if (!logged) {           \
            logged = true;       \
            log_msg(__VA_ARGS__); \
        }                        \
    } while (0)

static uint8_t *g_base;
static uint8_t *g_p1;
static uint8_t *g_extra[MAX_PLAYERS - 1];
// Extra players alive in the current stage; 0 outside co-op (menus, replays).
static int g_extra_count;
static uint32_t g_in_cur[MAX_PLAYERS];
static uint32_t g_in_last[MAX_PLAYERS];
static int g_pad_of[MAX_PLAYERS];
// Decided at stage start from the ini and P1's choice in the game's menu.
static uint8_t g_char_of[MAX_PLAYERS]; // 0 = Reimu, 1 = Marisa
static uint8_t g_shot_of[MAX_PLAYERS]; // 0 = A, 1 = B
static bool g_p1_keyboard_only;
static uint32_t g_frame;

// Running totals for the debug log: proof that each hook does something.
static struct {
    uint32_t extra_damage; // damage dealt to enemies by players 2-4
    uint32_t extra_hits;   // bullets/enemies that touched players 2-4
    uint32_t extra_grazes; // bullets grazed by players 2-4
    uint32_t extra_items;  // items picked up by players 2-4
    uint32_t lasers;       // laser checks run for players 2-4
    uint32_t retargets;    // enemy script runs aimed at players 2-4
} g_stats;

// Originals. bool arguments and results are declared as 32-bit and masked by
// hand: the game's compiler only defines their low byte.
static int (*orig_update)(void *player);
static int (*orig_draw[3])(void *player);
static int (*orig_added)(void *player);
static int (*orig_register_chain)(void);
static uint32_t (*orig_get_input)(void);
static int (*orig_run_ecl)(void *ecl, void *enemy);
static int (*orig_kill_box)(void *player, float *center, float *size, uint32_t circle);
static uint32_t (*orig_bomb_overlap)(void *player, float a, float b, float c, float d);
static int (*orig_damage)(void *player, float *pos, float *size, uint8_t *hit_by_bomb);
static int (*orig_laser)(void *unused, float *center, float *size, float *origin, float angle, uint32_t can_graze);
static int (*orig_load_anm)(void *unused, int slot, const char *path, int index_offset);

// --- per-player character and shot type ------------------------------------------
//
// Player code reads two GameManager globals (character, shot type) and builds
// its sprites from script indices 0x420 and up, which belong to whichever
// character sheet P1 loaded. A player with a different character or shot type
// therefore runs inside a "context": the globals are set to that player's, and
// if the character differs from P1's, the script-table range of the player
// sheet is swapped with the range holding the other character's sheet. Every
// call into Player code for players 2-4 goes through enter_player/leave_player.

static int g_alt_char = -1; // character whose sheet sits in the alt slot, -1 = none

static void swap_player_anm_tables(void)
{
    uint8_t *mgr = *GAME(uint8_t *, RVA_g_AnmManager);
    void **scripts = (void **)(mgr + MGR_SCRIPTS);
    uint32_t *offsets = (uint32_t *)(mgr + MGR_SPRITE_OFFSETS);
    for (int k = 0; k < ANM_PLAYER_RANGE; k++) {
        void *s = scripts[ANM_OFFSET_PLAYER + k];
        scripts[ANM_OFFSET_PLAYER + k] = scripts[ANM_OFFSET_ALT_PLAYER + k];
        scripts[ANM_OFFSET_ALT_PLAYER + k] = s;
        uint32_t o = offsets[ANM_OFFSET_PLAYER + k];
        offsets[ANM_OFFSET_PLAYER + k] = offsets[ANM_OFFSET_ALT_PLAYER + k];
        offsets[ANM_OFFSET_ALT_PLAYER + k] = o;
    }
}

typedef struct {
    uint8_t character, shot;
    bool swapped;
} SavedContext;

static SavedContext enter_player(int i)
{
    uint8_t *character = GAME(uint8_t, RVA_g_Character), *shot = GAME(uint8_t, RVA_g_ShotType);
    SavedContext saved = {*character, *shot, g_char_of[i] != g_char_of[0]};
    *character = g_char_of[i];
    *shot = g_shot_of[i];
    if (saved.swapped)
        swap_player_anm_tables();
    return saved;
}

static void leave_player(SavedContext saved)
{
    if (saved.swapped)
        swap_player_anm_tables();
    *GAME(uint8_t, RVA_g_Character) = saved.character;
    *GAME(uint8_t, RVA_g_ShotType) = saved.shot;
}

// Highest sprite / script id in a loaded .anm (header: sprite count, script
// count; at +0x40 a u32 offset per sprite, then {id, offset} per script).
// Both player sheets must stay below ANM_PLAYER_RANGE for the swap to cover them.
static bool anm_fits_player_range(int slot)
{
    uint8_t *mgr = *GAME(uint8_t *, RVA_g_AnmManager);
    const uint8_t *file = ((uint8_t **)(mgr + MGR_FILES))[slot];
    if (!file)
        return false;
    uint32_t sprites, scripts, max_sprite = 0, max_script = 0;
    memcpy(&sprites, file, 4);
    memcpy(&scripts, file + 4, 4);
    const uint8_t *table = file + 0x40;
    for (uint32_t i = 0; i < sprites; i++, table += 4) {
        uint32_t off, id;
        memcpy(&off, table, 4);
        memcpy(&id, file + off, 4);
        if (id > max_sprite)
            max_sprite = id;
    }
    for (uint32_t i = 0; i < scripts; i++, table += 8) {
        uint32_t id;
        memcpy(&id, table, 4);
        if (id > max_script)
            max_script = id;
    }
    log_msg("anm slot %d: %u sprites (max id %X), %u scripts (max id %X)", slot, sprites, max_sprite, scripts,
            max_script);
    return max_sprite < ANM_PLAYER_RANGE && max_script < ANM_PLAYER_RANGE;
}

// Loads `character`'s sprite sheet into the alt slot unless it is already there.
static bool load_alt_character(uint8_t character)
{
    uint8_t *mgr = *GAME(uint8_t *, RVA_g_AnmManager);
    void **files = (void **)(mgr + MGR_FILES);
    void **scripts = (void **)(mgr + MGR_SCRIPTS);
    if (files[ANM_SLOT_ALT_PLAYER] && g_alt_char == character)
        return true;
    if (!files[ANM_SLOT_ALT_PLAYER]) {
        // Nothing of ours is loaded, so the target range must be empty.
        for (int k = 0; k < ANM_PLAYER_RANGE; k++) {
            if (scripts[ANM_OFFSET_ALT_PLAYER + k]) {
                log_msg("alt character: script index %X is in use by the game", ANM_OFFSET_ALT_PLAYER + k);
                return false;
            }
        }
    }
    g_alt_char = -1;
    if (orig_load_anm(NULL, ANM_SLOT_ALT_PLAYER, character ? "data/player01.anm" : "data/player00.anm",
                      ANM_OFFSET_ALT_PLAYER) != 0) {
        log_msg("alt character: loading the sprite sheet failed");
        return false;
    }
    g_alt_char = character;
    log_msg("alt character: loaded %s into slot %d at %X", character ? "Marisa" : "Reimu", ANM_SLOT_ALT_PLAYER,
            ANM_OFFSET_ALT_PLAYER);
    if (!anm_fits_player_range(ANM_SLOT_PLAYER) || !anm_fits_player_range(ANM_SLOT_ALT_PLAYER)) {
        log_msg("alt character: a player sheet uses ids beyond %X, cannot mix characters", ANM_PLAYER_RANGE);
        g_alt_char = -1; // so later stages come back here instead of trusting the sheet
        return false;
    }
    return true;
}

// --- input -------------------------------------------------------------------

static void assign_devices(void)
{
    input_poll();
    bool taken[4] = {false};
    int connected = 0, autos = 0;
    for (int s = 0; s < 4; s++)
        connected += input_pad_connected(s);
    for (int i = 1; i < g_cfg.players; i++) {
        g_pad_of[i] = g_cfg.p[i].pad;
        if (g_pad_of[i] == PAD_AUTO)
            autos++;
        else if (g_pad_of[i] >= 0)
            taken[g_pad_of[i]] = true;
    }

    // With too few pads for everyone, P1 moves to the keyboard and the pads go
    // to the extra players.
    if (g_cfg.p1_input == P1_INPUT_AUTO)
        g_p1_keyboard_only = connected > 0 && connected < autos + 1;
    else
        g_p1_keyboard_only = g_cfg.p1_input == P1_INPUT_KEYBOARD;

    bool p1_pad_reserved = g_p1_keyboard_only;
    for (int i = 1; i < g_cfg.players; i++) {
        if (g_pad_of[i] != PAD_AUTO)
            continue;
        g_pad_of[i] = PAD_NONE;
        for (int s = 0; s < 4; s++) {
            if (!input_pad_connected(s) || taken[s])
                continue;
            if (!p1_pad_reserved) { // the game reads the first pad for P1
                p1_pad_reserved = true;
                taken[s] = true;
                continue;
            }
            g_pad_of[i] = s;
            taken[s] = true;
            break;
        }
    }

    log_msg("devices: P1 = %s", g_p1_keyboard_only ? "keyboard only" : "game default (keyboard + first pad)");
    for (int i = 1; i < g_cfg.players; i++)
        log_msg("devices: P%d = pad %d%s", i + 1, g_pad_of[i], g_cfg.p[i].keys[KEY_SHOT] ? " + keyboard" : "");
}

// The game's own keyboard reader gives P1 two layouts at once, so keys bound
// to players 2-4 would move P1 too. While co-op is active those keys are
// hidden from the game by pointing their key-table entries at index 0 of the
// key state buffer, which is never pressed.
static void hide_extra_keys_from_game(bool active)
{
    static bool extra_key[256]; // key-state indices bound to players 2-4
    static bool built;
    static uint8_t real[256];
    static bool hidden[256];

    if (!built) {
        built = true;
        for (int i = 1; i < g_cfg.players; i++) {
            for (int k = 0; k < KEY_COUNT; k++) {
                // 0xE0xx = extended key, which DirectInput numbers as xx | 0x80
                UINT sc = g_cfg.p[i].keys[k] ? MapVirtualKeyW((UINT)g_cfg.p[i].keys[k], MAPVK_VK_TO_VSC_EX) : 0;
                if (sc)
                    extra_key[(sc & 0xFF) | (sc > 0xFF ? 0x80 : 0)] = true;
            }
        }
    }

    uint8_t *table = GAME(uint8_t, RVA_g_KeyTable);
    for (int i = 1; i < 256; i++) {
        uint8_t v = table[i];
        if (hidden[i] && v == 0) // still our override; the game rebuilds the table on layout changes
            v = real[i];
        real[i] = v;
        hidden[i] = active && extra_key[v];
        table[i] = hidden[i] ? 0 : v;
    }
}

// --- network play --------------------------------------------------------------
//
// In network play each machine controls one player with the game's normal
// controls, and the input hook below hands the game the same inputs on every
// machine (see net.h). The host's full input word drives player 1 and the
// menus everywhere; guests contribute only their gameplay buttons.

static bool g_net_on;   // lockstep is running
static bool g_net_over; // it was tried and ended; stay offline until the game restarts

// A few values that must be equal on all machines at the start of a tick.
static void state_check(uint32_t check[NET_CHECKS])
{
    // Menus draw random numbers for decoration only; the stream is re-seeded at
    // every stage start, so it is compared in stages only.
    bool in_stage = *GAME(uint32_t, RVA_g_SupervisorCurState) == 2;
    check[0] = in_stage ? *GAME(uint16_t, RVA_g_RngSeed) | *GAME(uint32_t, RVA_g_RngCount) << 16 : 0;
    check[1] = (uint32_t)*GAME(uint64_t, RVA_g_Score);
    uint32_t h = (uint32_t)*GAME(int32_t, RVA_g_Stage) * 31 + (uint32_t)*GAME(int32_t, RVA_g_Difficulty) * 7 +
                 *GAME(uint16_t, RVA_g_Power) + ((uint32_t)*GAME(uint8_t, RVA_g_Lives) << 16) +
                 ((uint32_t)*GAME(uint8_t, RVA_g_Bombs) << 24);
    for (int i = 0; i < g_cfg.players; i++) {
        uint32_t pos[2];
        memcpy(pos, (i == 0 ? g_p1 : g_extra[i - 1]) + P_POS, sizeof(pos));
        h = h * 33 ^ pos[0] ^ (pos[1] << 16 | pos[1] >> 16);
    }
    check[2] = h;
}

// Called on the first tick of the main menu: a point every machine passes
// exactly once, having received no input before it.
static void net_begin(void)
{
    NetSession s = {
        .players = (uint8_t)g_cfg.players,
        .delay = (uint8_t)g_cfg.net_delay,
        .lives = *GAME(uint8_t, RVA_g_CfgLives),
        .bombs = *GAME(uint8_t, RVA_g_CfgBombs),
        .target_mode = (uint8_t)g_cfg.target_mode,
        .target_period = (uint32_t)g_cfg.target_period,
        .rng_seed = *GAME(uint16_t, RVA_g_RngSeed),
        .rng_count = *GAME(uint32_t, RVA_g_RngCount),
    };
    for (int i = 0; i < MAX_PLAYERS; i++) {
        s.character[i] = (uint8_t)g_cfg.p[i].character;
        s.shot[i] = (uint8_t)g_cfg.p[i].shot;
    }
    const uint8_t my_lives = s.lives, my_bombs = s.bombs;

    g_net_over = true; // one attempt per run, whatever happens
    log_msg("net: main menu reached, %s", g_cfg.net_mode == NET_HOST ? "waiting for guests" : "joining");
    if (!net_connect(&s))
        return;
    if (s.lives != my_lives || s.bombs != my_bombs) {
        // Different starting stock would make the two games differ at once.
        wchar_t text[256];
        wsprintfW(text,
                  L"The host's game options give %d lives and %d bombs, this game is set to %d and %d.\n\n"
                  L"Set the same values in the game's Options on both machines and start again.",
                  s.lives, s.bombs, my_lives, my_bombs);
        MessageBoxW(NULL, text, L"th06mp", MB_ICONWARNING | MB_TOPMOST);
        log_msg("net: game options differ from the host's, playing offline");
        return;
    }

    // Everything that shapes the simulation now comes from the host.
    g_cfg.players = s.players;
    g_cfg.target_mode = s.target_mode;
    g_cfg.target_period = (int)s.target_period;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_cfg.p[i].character = s.character[i];
        g_cfg.p[i].shot = s.shot[i];
    }
    *GAME(uint16_t, RVA_g_RngSeed) = s.rng_seed;
    *GAME(uint32_t, RVA_g_RngCount) = s.rng_count;
    g_frame = 0;
    g_net_on = true;
}

static uint32_t net_frame(uint32_t local)
{
    uint32_t check[NET_CHECKS], inputs[MAX_PLAYERS];
    state_check(check);
    if (!net_tick(local, check, inputs)) {
        log_msg("net: connection ended, continuing offline");
        g_net_on = false;
        memset(g_in_cur, 0, sizeof(g_in_cur));
        return local;
    }
    for (int i = 1; i < g_cfg.players; i++) {
        g_in_last[i] = g_in_cur[i];
        g_in_cur[i] = inputs[i] & IN_GAMEPLAY;
    }
    return inputs[0];
}

static uint32_t hook_get_input(void)
{
    uint32_t in = orig_get_input();
    g_frame++;
    if (g_cfg.net_mode != NET_OFF && (g_net_on || !g_net_over)) {
        if (!g_net_on) {
            // Lockstep starts on the first tick of the main menu. Until then
            // the game gets no input at all, so every machine arrives there
            // in the same state.
            if (*GAME(uint32_t, RVA_g_SupervisorCurState) != 1)
                return 0;
            net_begin();
        }
        if (g_net_on)
            return net_frame(in);
    }
    if (g_cfg.players < 2)
        return in;
    LOG_ONCE("hook: GetInput running");

    hide_extra_keys_from_game(g_extra_count > 0);
    input_poll();
    if (g_p1_keyboard_only && g_extra_count)
        in = (in & ~(uint32_t)IN_GAMEPLAY) | input_read_keys(g_cfg.p[0].keys);
    for (int i = 1; i < g_cfg.players; i++) {
        g_in_last[i] = g_in_cur[i];
        g_in_cur[i] = (input_read_pad(g_pad_of[i]) | input_read_keys(g_cfg.p[i].keys)) & IN_GAMEPLAY;
    }
    return in;
}

// --- stage start -------------------------------------------------------------

static int hook_register_chain(void)
{
    int r = orig_register_chain();
    g_extra_count = 0;
    if (r != 0 || g_cfg.players < 2)
        return r;
    if (*GAME(uint8_t, RVA_g_IsInReplay)) {
        log_msg("stage start: replay playback, extra players disabled");
        return r;
    }
    if (!g_net_on) { // over the network every player uses their own machine's controls
        assign_devices();
    } else {
        // Same stage, same random numbers everywhere, whatever the menus drew.
        *GAME(uint16_t, RVA_g_RngSeed) = (uint16_t)(g_frame * 40503u >> 8);
        *GAME(uint32_t, RVA_g_RngCount) = 0;
    }

    // Who plays what. P1's choice comes from the game's own menu.
    int extras = g_cfg.players - 1;
    uint8_t p1_char = *GAME(uint8_t, RVA_g_Character), p1_shot = *GAME(uint8_t, RVA_g_ShotType);
    bool need_alt = false;
    g_char_of[0] = p1_char;
    g_shot_of[0] = p1_shot;
    for (int i = 1; i <= extras; i++) {
        switch (g_cfg.p[i].character) {
        case CHARACTER_OTHER: g_char_of[i] = !p1_char; break;
        case CHARACTER_REIMU: g_char_of[i] = 0; break;
        case CHARACTER_MARISA: g_char_of[i] = 1; break;
        default: g_char_of[i] = p1_char; break;
        }
        switch (g_cfg.p[i].shot) {
        case SHOT_OTHER: g_shot_of[i] = !p1_shot; break;
        case SHOT_A: g_shot_of[i] = 0; break;
        case SHOT_B: g_shot_of[i] = 1; break;
        default: g_shot_of[i] = p1_shot; break;
        }
        need_alt |= g_char_of[i] != p1_char;
    }
    if (need_alt && !load_alt_character(!p1_char)) {
        log_msg("stage start: everyone plays P1's character");
        for (int i = 1; i <= extras; i++)
            g_char_of[i] = p1_char;
    }

    // Same initialisation the game just did for g_Player. Supervisor state 3
    // makes AddedCallback skip loading a sprite sheet into the player slot:
    // P1's is already there and the other character's is in the alt slot.
    uint32_t *state = GAME(uint32_t, RVA_g_SupervisorState);
    uint32_t saved_state = *state;
    bool ok = true;
    *state = 3;
    for (int i = 0; i < extras && ok; i++) {
        uint8_t *e = g_extra[i];
        memset(e, 0, PLAYER_SIZE);
        *(int32_t *)(e + P_INVULN_TIMER_PREV) = -999;
        SavedContext ctx = enter_player(i + 1);
        ok = orig_added(e) == 0;
        leave_player(ctx);
        if (!ok)
            log_msg("stage start: Player init failed for P%d", i + 2);
    }
    *state = saved_state;
    if (!ok)
        return r;

    for (int i = 0; i < g_cfg.players; i++) {
        uint8_t *p = i == 0 ? g_p1 : g_extra[i - 1];
        float x = PLAYER_SPAWN_X + ((float)i - (float)(g_cfg.players - 1) * 0.5f) * 48.0f;
        memcpy(p + P_POS, &x, sizeof(x));
    }
    memset(g_in_cur, 0, sizeof(g_in_cur));
    memset(g_in_last, 0, sizeof(g_in_last));
    g_extra_count = extras;
    for (int i = 0; i < g_cfg.players; i++)
        log_msg("stage start: P%d = %s %c", i + 1, g_char_of[i] ? "Marisa" : "Reimu", g_shot_of[i] ? 'B' : 'A');
    return r;
}

// --- per-frame player callbacks ----------------------------------------------

static uint8_t *player(int i)
{
    return i == 0 ? g_p1 : g_extra[i - 1];
}

// debug=1: a line per player state change, and everyone's position once a second.
static void debug_trace(void)
{
    static uint8_t last_state[MAX_PLAYERS];
    static uint32_t last_dump;
    int n = g_extra_count + 1;

    for (int i = 0; i < n; i++) {
        uint8_t st = player(i)[P_STATE];
        if (st != last_state[i]) {
            log_msg("P%d state %d -> %d (lives %d, bombs %d)", i + 1, last_state[i], st, *GAME(int8_t, RVA_g_Lives),
                    *GAME(int8_t, RVA_g_Bombs));
            last_state[i] = st;
        }
    }
    if (g_frame - last_dump < 60)
        return;
    last_dump = g_frame;

    char line[256];
    int len = 0;
    for (int i = 0; i < n; i++) {
        float pos[2];
        memcpy(pos, player(i) + P_POS, sizeof(pos));
        uint32_t in = i == 0 ? *GAME(uint32_t, RVA_g_CurFrameInput) : g_in_cur[i];
        len += wsprintfA(line + len, " | P%d (%d,%d) st=%d in=%02X", i + 1, (int)pos[0], (int)pos[1],
                         player(i)[P_STATE], in & 0xFF);
    }
    log_msg("frame %u power %u%s | dmg %u graze %u hit %u item %u laser %u aim %u", g_frame,
            *GAME(uint16_t, RVA_g_Power), line, g_stats.extra_damage, g_stats.extra_grazes, g_stats.extra_hits,
            g_stats.extra_items, g_stats.lasers, g_stats.retargets);
}

static bool can_collect(const uint8_t *p)
{
    return p[P_STATE] == PLAYER_STATE_ALIVE || p[P_STATE] == PLAYER_STATE_INVULNERABLE;
}

// ItemManager's pickup test and its fly-to-player logic are inlined against
// g_Player, and applying an item (power, score, lives, bombs) is a long
// switch inside that loop. Rather than copy it, items touched by another
// player are moved onto P1, where the game's own pass collects them a moment
// later in the same frame. Everything an item gives is shared anyway; the
// only visible difference is that the score popup appears at P1.
static void item_pass(void)
{
    if (!can_collect(g_p1))
        return; // the game collects nothing for P1 right now either

    const float poc_y = *GAME(float, RVA_PocY);
    float boxes[MAX_PLAYERS - 1][4]; // left, top, right, bottom
    int n = 0;
    bool above_poc = false;
    for (int i = 0; i < g_extra_count; i++) {
        const uint8_t *e = g_extra[i];
        if (!can_collect(e))
            continue;
        memcpy(&boxes[n][0], e + P_GRAB_TOP_LEFT, 2 * sizeof(float));
        memcpy(&boxes[n][2], e + P_GRAB_BOTTOM_RIGHT, 2 * sizeof(float));
        n++;
        float y;
        memcpy(&y, e + P_POS + sizeof(float), sizeof(y));
        above_poc |= y < poc_y;
    }
    if (n == 0)
        return;

    uint8_t *item = GAME(uint8_t, RVA_g_Items);
    for (int k = 0; k < ITEM_COUNT; k++, item += ITEM_STRIDE) {
        if (!item[I_IN_USE])
            continue;
        if (above_poc) // same as P1 crossing the line: everything flies in
            item[I_STATE] = ITEM_STATE_HOMING;
        float pos[2];
        memcpy(pos, item + I_POS, sizeof(pos));
        for (int i = 0; i < n; i++) {
            const float *b = boxes[i];
            if (b[0] > pos[0] + ITEM_HALF_SIZE || pos[0] - ITEM_HALF_SIZE > b[2] || b[1] > pos[1] + ITEM_HALF_SIZE ||
                pos[1] - ITEM_HALF_SIZE > b[3])
                continue;
            memcpy(item + I_POS, g_p1 + P_POS, sizeof(pos));
            g_stats.extra_items++;
            break;
        }
    }
}

// BulletManager only tests a bullet against the kill box after it has been
// grazed, and its graze test is inlined against g_Player. This repeats that
// test for the other players, so bullets that never came near P1 are still
// armed against them. It runs from the player update, which the chain calls
// before BulletManager's update in the same frame.
static void graze_pass(void)
{
    const float margin = *GAME(float, RVA_GrazeMargin);
    uint8_t *b = GAME(uint8_t, RVA_g_Bullets);
    for (int n = 0; n < BULLET_COUNT; n++, b += BULLET_STRIDE) {
        uint16_t state;
        memcpy(&state, b + B_STATE, sizeof(state));
        if (state != BULLET_STATE_FIRED || b[B_IS_GRAZED])
            continue;
        float pos[2], size[2];
        memcpy(pos, b + B_POS, sizeof(pos));
        memcpy(size, b + B_SIZE, sizeof(size));
        float reach = (size[0] < size[1] ? size[0] : size[1]) * 0.5f + margin;

        for (int i = 0; i < g_extra_count; i++) {
            uint8_t *e = g_extra[i];
            if (e[P_STATE] == PLAYER_STATE_SPAWNING || e[P_STATE] == PLAYER_STATE_DEAD)
                continue;
            float ppos[2], radius;
            memcpy(ppos, e + P_POS, sizeof(ppos));
            memcpy(&radius, e + P_HIT_RADIUS, sizeof(radius));
            float dx = ppos[0] - pos[0], dy = ppos[1] - pos[1], r = reach + radius;
            if (dx * dx + dy * dy >= r * r)
                continue;

            b[B_IS_GRAZED] = 1;
            g_stats.extra_grazes++;
            // Same rewards the game gives P1, minus the spark and rumble.
            if (!e[P_BOMB_IN_USE]) {
                int32_t *stage = GAME(int32_t, RVA_g_GrazeInStage), *total = GAME(int32_t, RVA_g_GrazeTotal);
                if (*stage < 99999)
                    ++*stage;
                if (*total < 999999)
                    ++*total;
            }
            *GAME(uint64_t, RVA_g_Score) += 500;
            uint32_t *gui = GAME(uint32_t, RVA_g_GuiFlags);
            *gui = (*gui & ~0x40u) | 0x80u;
            break;
        }
    }
}

static int hook_update(void *p)
{
    // P1's update clears its homing target, so grab it first for the others.
    float homing_target[3];
    memcpy(homing_target, g_p1 + P_LAST_ENEMY_HIT, sizeof(homing_target));

    int r = orig_update(p);
    if (p != g_p1 || !g_extra_count)
        return r;
    LOG_ONCE("hook: Player update running for %d extra player(s)", g_extra_count);

    uint32_t *cur = GAME(uint32_t, RVA_g_CurFrameInput);
    uint32_t *last = GAME(uint32_t, RVA_g_LastFrameInput);
    uint32_t saved_cur = *cur, saved_last = *last;
    for (int i = 0; i < g_extra_count; i++) {
        uint8_t *e = g_extra[i];
        memcpy(e + P_LAST_ENEMY_HIT, homing_target, sizeof(homing_target));
        *cur = g_in_cur[i + 1];
        *last = g_in_last[i + 1];
        SavedContext ctx = enter_player(i + 1);
        orig_update(e);
        leave_player(ctx);
    }
    *cur = saved_cur;
    *last = saved_last;
    graze_pass();
    item_pass();
    if (g_cfg.debug)
        debug_trace();
    return r;
}

static uint32_t tint_color(uint32_t argb, uint32_t tint)
{
    uint32_t out = argb & 0xFF000000;
    for (int shift = 0; shift < 24; shift += 8)
        out |= ((argb >> shift & 0xFF) * (tint >> shift & 0xFF) / 255) << shift;
    return out;
}

static int draw_all(int which, void *p)
{
    int r = orig_draw[which](p);
    if (p != g_p1 || !g_extra_count)
        return r;
    for (int i = 0; i < g_extra_count; i++) {
        uint8_t *e = g_extra[i];
        uint32_t *color = (uint32_t *)(e + P_SPRITE_COLOR);
        uint32_t saved = *color;
        if (g_cfg.tint)
            *color = tint_color(saved, g_cfg.p[i + 1].tint);
        SavedContext ctx = enter_player(i + 1);
        orig_draw[which](e);
        leave_player(ctx);
        *color = saved;
    }
    return r;
}

static int hook_draw_high(void *p) { return draw_all(0, p); }
static int hook_draw_low(void *p) { return draw_all(1, p); }
static int hook_draw_3(void *p) { return draw_all(2, p); }

// --- collisions --------------------------------------------------------------

// 0 = miss, 1 = hit, 2 = cancelled by a bomb; the caller removes the bullet on
// anything non-zero, so report the strongest result among all players.
static int hook_kill_box(void *p, float *center, float *size, uint32_t circle)
{
    circle &= 0xFF;
    int r = orig_kill_box(p, center, size, circle);
    for (int i = 0; i < g_extra_count; i++) {
        int k = orig_kill_box(g_extra[i], center, size, circle);
        g_stats.extra_hits += k != 0;
        if (k > r)
            r = k;
    }
    return r;
}

static uint32_t hook_bomb_overlap(void *p, float a, float b, float c, float d)
{
    uint32_t r = orig_bomb_overlap(p, a, b, c, d) & 0xFF;
    for (int i = 0; i < g_extra_count && !r; i++)
        r = orig_bomb_overlap(g_extra[i], a, b, c, d) & 0xFF;
    return r;
}

static int hook_damage(void *p, float *pos, float *size, uint8_t *hit_by_bomb)
{
    int total = orig_damage(p, pos, size, hit_by_bomb);
    for (int i = 0; i < g_extra_count; i++) {
        uint8_t flag = 0;
        // Sets hit animations on that player's bullets, so it needs their sprites.
        SavedContext ctx = enter_player(i + 1);
        int dealt = orig_damage(g_extra[i], pos, size, hit_by_bomb ? &flag : NULL);
        leave_player(ctx);
        g_stats.extra_damage += (uint32_t)dealt;
        total += dealt;
        if (hit_by_bomb)
            *hit_by_bomb |= flag;
    }
    return total;
}

// Player_CalcLaserHitbox ignores its Player argument and works on g_Player
// directly, so for the other players the fields it touches are swapped into
// g_Player around the call.
typedef struct {
    uint16_t off, len;
} Window;

static const Window g_laser_windows[] = {
    {P_POS, 0x20},           // position, respawn timer, homing target, hit radius
    {P_INVULN_TIMER_PREV, 8}, // invulnerability timer
    {P_STATE, 1},
    {0x79E8, 0x10},          // death animation timers
    {P_BOMB_IN_USE, 1},
};

static void swap_windows(uint8_t *a, uint8_t *b, const Window *w, int count)
{
    for (int i = 0; i < count; i++) {
        uint8_t tmp[0x20];
        memcpy(tmp, a + w[i].off, w[i].len);
        memcpy(a + w[i].off, b + w[i].off, w[i].len);
        memcpy(b + w[i].off, tmp, w[i].len);
    }
}

static int hook_laser(void *p, float *center, float *size, float *origin, float angle, uint32_t can_graze)
{
    const int windows = sizeof(g_laser_windows) / sizeof(g_laser_windows[0]);
    can_graze &= 0xFF;
    int r = orig_laser(p, center, size, origin, angle, can_graze);
    for (int i = 0; i < g_extra_count; i++) {
        swap_windows(g_p1, g_extra[i], g_laser_windows, windows);
        orig_laser(p, center, size, origin, angle, can_graze);
        swap_windows(g_p1, g_extra[i], g_laser_windows, windows);
        g_stats.lasers++;
    }
    return r;
}

// --- enemy aiming ------------------------------------------------------------

// Enemy scripts read g_Player.positionCenter to aim. Each enemy slot is given
// one living player to aim at, changing every target_period frames, by putting
// that player's position into g_Player while the enemy's script runs.
static int hook_run_ecl(void *ecl, void *enemy)
{
    if (!g_extra_count || g_cfg.target_mode == TARGET_P1)
        return orig_run_ecl(ecl, enemy);

    uint8_t *alive[MAX_PLAYERS];
    int n = 0;
    if (g_p1[P_STATE] != PLAYER_STATE_DEAD)
        alive[n++] = g_p1;
    for (int i = 0; i < g_extra_count; i++)
        if (g_extra[i][P_STATE] != PLAYER_STATE_DEAD)
            alive[n++] = g_extra[i];
    if (n == 0)
        return orig_run_ecl(ecl, enemy);

    // Relative to the image, so every machine in a network game picks the same target.
    uintptr_t slot = (uintptr_t)((uint8_t *)enemy - g_base) / ENEMY_STRIDE;
    uint8_t *target = alive[(slot + g_frame / (uint32_t)g_cfg.target_period) % (uintptr_t)n];
    if (target == g_p1)
        return orig_run_ecl(ecl, enemy);

    float saved[3];
    g_stats.retargets++;
    memcpy(saved, g_p1 + P_POS, sizeof(saved));
    memcpy(g_p1 + P_POS, target + P_POS, sizeof(saved));
    int r = orig_run_ecl(ecl, enemy);
    memcpy(g_p1 + P_POS, saved, sizeof(saved));
    return r;
}

// --- install -----------------------------------------------------------------

static bool check_image(uint8_t *base)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != TH06NC_TIMESTAMP || nt->OptionalHeader.SizeOfImage != TH06NC_IMAGE_SIZE) {
        log_msg("unsupported game build: timestamp %08lX size %lX (need %08X / %X)", nt->FileHeader.TimeDateStamp,
                nt->OptionalHeader.SizeOfImage, TH06NC_TIMESTAMP, TH06NC_IMAGE_SIZE);
        return false;
    }
    return true;
}

bool coop_install(uint8_t *base)
{
    g_base = base;
    if (!check_image(base))
        return false;
    // Lets host and guest run side by side on one PC. Read by the engine's
    // window setup, which has not run yet.
    if (g_cfg.net_mode != NET_OFF)
        *GAME(int32_t, RVA_g_AllowSecondInstance) = 1;
    if (g_cfg.players < 2 && g_cfg.net_mode != NET_JOIN) { // a guest gets the player count from the host
        log_msg("players=1 in th06mp.ini, leaving the game unmodified");
        return true;
    }

    g_p1 = base + RVA_g_Player;
    orig_update = (void *)(base + RVA_Player_OnUpdate);
    orig_draw[0] = (void *)(base + RVA_Player_OnDrawHighPrio);
    orig_draw[1] = (void *)(base + RVA_Player_OnDrawLowPrio);
    orig_draw[2] = (void *)(base + RVA_Player_OnDraw3);
    orig_added = (void *)(base + RVA_Player_AddedCallback);
    orig_register_chain = (void *)(base + RVA_Player_RegisterChain);
    orig_get_input = (void *)(base + RVA_GetInput);
    orig_run_ecl = (void *)(base + RVA_EclManager_RunEcl);
    orig_kill_box = (void *)(base + RVA_Player_CalcKillBox);
    orig_bomb_overlap = (void *)(base + RVA_Player_BombOverlap);
    orig_damage = (void *)(base + RVA_Player_CalcDamageToEnemy);
    orig_laser = (void *)(base + RVA_Player_CalcLaserHitbox);
    orig_load_anm = (void *)(base + RVA_AnmManager_LoadAnm);

    const size_t slot = (PLAYER_SIZE + 0xFFF) & ~(size_t)0xFFF;
    uint8_t *mem = VirtualAlloc(NULL, slot * (MAX_PLAYERS - 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        log_msg("out of memory for extra players");
        return false;
    }
    for (int i = 0; i < MAX_PLAYERS - 1; i++)
        g_extra[i] = mem + slot * i;
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_pad_of[i] = PAD_NONE;

    const Patch patches[] = {
        {PATCH_CALL, SITE_GetInput, RVA_GetInput, hook_get_input, "GetInput"},
        {PATCH_CALL, SITE_RegisterChain, RVA_Player_RegisterChain, hook_register_chain, "Player_RegisterChain"},
        {PATCH_LEA, SITE_Lea_OnUpdate, RVA_Player_OnUpdate, hook_update, "Player_OnUpdate"},
        {PATCH_LEA, SITE_Lea_OnDrawHighPrio, RVA_Player_OnDrawHighPrio, hook_draw_high, "Player_OnDrawHighPrio"},
        {PATCH_LEA, SITE_Lea_OnDrawLowPrio, RVA_Player_OnDrawLowPrio, hook_draw_low, "Player_OnDrawLowPrio"},
        {PATCH_LEA, SITE_Lea_OnDraw3, RVA_Player_OnDraw3, hook_draw_3, "Player_OnDraw3"},
        {PATCH_CALL, SITE_KillBox_Bullet, RVA_Player_CalcKillBox, hook_kill_box, "KillBox (bullets)"},
        {PATCH_CALL, SITE_KillBox_Enemy, RVA_Player_CalcKillBox, hook_kill_box, "KillBox (enemies)"},
        {PATCH_CALL, SITE_BombOverlap, RVA_Player_BombOverlap, hook_bomb_overlap, "BombOverlap"},
        {PATCH_CALL, SITE_DamageToEnemy, RVA_Player_CalcDamageToEnemy, hook_damage, "CalcDamageToEnemy"},
        {PATCH_CALL, SITE_LaserHitbox, RVA_Player_CalcLaserHitbox, hook_laser, "CalcLaserHitbox"},
        {PATCH_CALL, SITE_RunEcl, RVA_EclManager_RunEcl, hook_run_ecl, "EclManager_RunEcl"},
    };
    return hook_apply(base, patches, sizeof(patches) / sizeof(patches[0]));
}
