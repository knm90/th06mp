#include "hook.h"

#include <string.h>
#include <windows.h>

#include "game.h"
#include "log.h"

// rel32 operands can only reach +-2 GB, and this DLL may be loaded anywhere,
// so each patched site points at a 16-byte `jmp [rip+0]; .quad target` stub
// in a page allocated next to the game image.
#define THUNK_SIZE 16

static uint8_t *g_thunks;
static int g_thunk_count;
static int g_thunk_cap;

static uint8_t *alloc_near(uint8_t *image_base)
{
    const uintptr_t step = 0x10000;
    const uintptr_t reach = 0x70000000;
    uintptr_t base = (uintptr_t)image_base & ~(step - 1);
    for (uintptr_t off = step; off < reach; off += step) {
        if (base > off) {
            void *p = VirtualAlloc((void *)(base - off), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p)
                return p;
        }
    }
    uintptr_t end = ((uintptr_t)image_base + TH06NC_IMAGE_SIZE + step - 1) & ~(step - 1);
    for (uintptr_t off = 0; off < reach - TH06NC_IMAGE_SIZE; off += step) {
        void *p = VirtualAlloc((void *)(end + off), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p)
            return p;
    }
    return NULL;
}

static void *make_thunk(void *target)
{
    if (g_thunk_count >= g_thunk_cap)
        return NULL;
    uint8_t *t = g_thunks + g_thunk_count++ * THUNK_SIZE;
    uint64_t addr = (uint64_t)(uintptr_t)target;
    t[0] = 0xFF; // jmp qword ptr [rip+0]
    t[1] = 0x25;
    memset(t + 2, 0, 4);
    memcpy(t + 6, &addr, 8);
    return t;
}

// Offset of the rel32 operand inside the instruction and the instruction length.
static bool decode(const Patch *p, const uint8_t *insn, int *disp_off, int *len)
{
    if (p->kind == PATCH_CALL) {
        if (insn[0] != 0xE8)
            return false;
        *disp_off = 1;
        *len = 5;
        return true;
    }
    // REX.W lea r64, [rip+disp32]: 48/4C 8D, modrm with mod=00 rm=101
    if ((insn[0] != 0x48 && insn[0] != 0x4C) || insn[1] != 0x8D || (insn[2] & 0xC7) != 0x05)
        return false;
    *disp_off = 3;
    *len = 7;
    return true;
}

static bool write_code(uint8_t *dst, const void *src, size_t n)
{
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

bool hook_apply(uint8_t *image_base, const Patch *patches, int count)
{
    for (int i = 0; i < count; i++) {
        const Patch *p = &patches[i];
        uint8_t *insn = image_base + p->site;
        int disp_off, len;
        if (!decode(p, insn, &disp_off, &len)) {
            log_msg("patch %s: unexpected opcode %02X %02X %02X at %X", p->name, insn[0], insn[1], insn[2], p->site);
            return false;
        }
        int32_t disp;
        memcpy(&disp, insn + disp_off, 4);
        uint32_t points_at = (uint32_t)(p->site + len + disp);
        if (points_at != p->expect) {
            log_msg("patch %s: site %X points at %X, expected %X", p->name, p->site, points_at, p->expect);
            return false;
        }
    }

    g_thunks = alloc_near(image_base);
    if (!g_thunks) {
        log_msg("could not allocate a thunk page near the game image");
        return false;
    }
    g_thunk_cap = 0x1000 / THUNK_SIZE;

    for (int i = 0; i < count; i++) {
        const Patch *p = &patches[i];
        uint8_t *insn = image_base + p->site;
        int disp_off, len;
        decode(p, insn, &disp_off, &len);
        uint8_t *thunk = make_thunk(p->target);
        if (!thunk)
            return false;
        int64_t rel = (int64_t)(thunk - (insn + len));
        if (rel != (int32_t)rel) {
            log_msg("patch %s: thunk out of rel32 range", p->name);
            return false;
        }
        int32_t disp = (int32_t)rel;
        if (!write_code(insn + disp_off, &disp, 4)) {
            log_msg("patch %s: VirtualProtect failed (%lu)", p->name, GetLastError());
            return false;
        }
        log_msg("patched %-22s at %06X", p->name, p->site);
    }
    return true;
}
