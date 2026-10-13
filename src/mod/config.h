#pragma once
#include <stdint.h>
#include <wchar.h>

#define MAX_PLAYERS 4

enum { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_SHOT, KEY_BOMB, KEY_FOCUS, KEY_COUNT };

enum { PAD_AUTO = -2, PAD_NONE = -1 };
enum { P1_INPUT_AUTO, P1_INPUT_GAME, P1_INPUT_KEYBOARD };
enum { TARGET_ROTATE, TARGET_P1 };
enum { SHOT_SAME, SHOT_OTHER, SHOT_A, SHOT_B };             // relative to P1's, or fixed
enum { CHARACTER_SAME, CHARACTER_OTHER, CHARACTER_REIMU, CHARACTER_MARISA };
enum { NET_OFF, NET_HOST, NET_JOIN };

typedef struct {
    int character;       // CHARACTER_*; ignored for player 1, who picks in the game's menu
    int shot;            // SHOT_*; likewise
    int pad;             // PAD_AUTO, PAD_NONE or an XInput slot 0-3
    int keys[KEY_COUNT]; // virtual-key codes, 0 = unbound
    uint32_t tint;       // RGB multiplied into the sprite colour
} PlayerConfig;

typedef struct {
    int players;       // 1-4
    int p1_input;      // P1_INPUT_*
    int tint;          // colour the extra players
    int target_mode;   // TARGET_*
    int target_period; // frames an enemy keeps aiming at the same player
    int debug;         // log every player's position/state once a second
    int net_mode;      // NET_*
    char net_address[64]; // host to join
    int net_port;
    int net_delay;     // input delay in frames, chosen by the host
    PlayerConfig p[MAX_PLAYERS];
} Config;

extern Config g_cfg;

void config_load(const wchar_t *ini_path);
