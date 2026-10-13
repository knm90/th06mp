#pragma once
#include <stdbool.h>
#include <stdint.h>

// Verifies the game build and installs every hook. `image_base` is the base
// of th06nc.exe. Returns false (and logs why) if the game must be left alone.
bool coop_install(uint8_t *image_base);
