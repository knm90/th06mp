// Tiny patching layer: redirects existing `call rel32` / `lea reg,[rip+rel32]`
// instructions in the game to functions in this DLL. No instruction decoding
// at runtime - every site is listed in game.h and verified before patching.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { PATCH_CALL, PATCH_LEA } PatchKind;

typedef struct {
    PatchKind kind;
    uint32_t site;    // RVA of the instruction
    uint32_t expect;  // RVA it must currently point at
    void *target;     // function in this DLL
    const char *name;
} Patch;

// Verifies every patch against the image and, only if all match, applies them.
bool hook_apply(uint8_t *image_base, const Patch *patches, int count);
