// Checks the lockstep layer without the game: run one "host" and one or more
// "join" processes; each feeds a known input pattern and verifies that every
// tick delivers every player's input from `delay` ticks earlier.
//
//   net_test.exe host <players> <port>
//   net_test.exe join <players> <port>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../src/mod/config.h"
#include "../src/mod/log.h"
#include "../src/mod/net.h"

#define TICKS 2000

static uint32_t pattern(int slot, uint32_t tick)
{
    return tick * 2654435761u + (uint32_t)slot * 40503u;
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    log_init(dir);

    g_cfg.net_mode = strcmp(argv[1], "host") == 0 ? NET_HOST : NET_JOIN;
    g_cfg.net_port = atoi(argv[3]);
    strcpy(g_cfg.net_address, "127.0.0.1");
    NetSession s = {.players = (uint8_t)atoi(argv[2]), .delay = 3, .lives = 2, .bombs = 3, .rng_seed = 0x1234};
    if (!net_connect(&s)) {
        printf("%s: connect failed\n", argv[1]);
        return 1;
    }
    if (s.rng_seed != 0x1234 || s.lives != 2) {
        printf("slot %d: session not delivered intact\n", s.slot);
        return 1;
    }

    for (uint32_t t = 0; t < TICKS; t++) {
        uint32_t check[NET_CHECKS] = {t, t * 3, 7}, in[MAX_PLAYERS];
        if (!net_tick(pattern(s.slot, t), check, in)) {
            printf("slot %d: connection lost at tick %u\n", s.slot, t);
            return 1;
        }
        for (int p = 0; p < s.players; p++) {
            uint32_t want = t < s.delay ? 0 : pattern(p, t - s.delay);
            if (in[p] != want) {
                printf("slot %d: tick %u player %d got %08X, want %08X\n", s.slot, t, p, in[p], want);
                return 1;
            }
        }
        if (t % 7 == s.slot)
            Sleep(1); // uneven pacing, so peers stall on each other
    }
    printf("slot %d: PASS, %d ticks\n", s.slot, TICKS);
    Sleep(300); // let the others drain before this socket closes
    return 0;
}
