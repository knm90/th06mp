// Input for the extra players, read directly from XInput and the keyboard.
// (P1 keeps using the game's own input path.)
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Refreshes pad state; call once per game frame.
void input_poll(void);
bool input_pad_connected(int slot);
// Both return IN_* bits.
uint32_t input_read_pad(int slot);
uint32_t input_read_keys(const int *keys);
