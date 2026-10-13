// Lockstep network play over TCP.
//
// One machine hosts (it is player 1) and the others join (players 2-4, in the
// order they connect). No game state is sent: every machine runs the whole
// game and they only exchange inputs, each tick using the same inputs
// everywhere. That only works if the simulation is identical on all of them,
// so the host also checks a few state values from every guest each tick and
// logs the first tick where they differ.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

// What every machine must agree on before the first tick. The host fills it
// in; guests receive it.
#pragma pack(push, 1)
typedef struct {
    uint8_t players;
    uint8_t slot;  // the player this machine controls, 0 = host
    uint8_t delay; // frames between pressing a key and it taking effect
    uint8_t lives, bombs; // the host's game options; a guest with others would desync
    uint8_t character[MAX_PLAYERS], shot[MAX_PLAYERS]; // CHARACTER_* / SHOT_* per player
    uint8_t target_mode;
    uint32_t target_period;
    uint16_t rng_seed;
    uint32_t rng_count;
} NetSession;
#pragma pack(pop)

#define NET_CHECKS 3

// Host: waits for players-1 guests and sends each a copy of *s with its slot.
// Guest: connects and overwrites *s with the host's. A small window shows the
// progress; Esc cancels. Returns false if cancelled or failed.
bool net_connect(NetSession *s);

// One game tick. Hands in this machine's input and state check values and
// returns every player's input for this tick. False means the connection is
// gone and network play is over.
bool net_tick(uint32_t local_input, const uint32_t check[NET_CHECKS], uint32_t inputs[MAX_PLAYERS]);
