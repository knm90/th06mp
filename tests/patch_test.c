// Offline check of the patch table: lays th06nc.exe out in memory the way the
// loader would (without running any of it), installs the hooks into that copy
// and verifies every patched site now leads into this test binary.
//
//   patch_test.exe path\to\th06nc.exe
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../src/mod/config.h"
#include "../src/mod/coop.h"
#include "../src/mod/game.h"
#include "../src/mod/log.h"

static uint8_t *map_image(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD size = GetFileSize(f, NULL), got;
    uint8_t *file = malloc(size);
    ReadFile(f, file, size, &got, NULL);
    CloseHandle(f);

    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(file + ((const IMAGE_DOS_HEADER *)file)->e_lfanew);
    uint8_t *image = VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memcpy(image, file, nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(image + sec[i].VirtualAddress, file + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    free(file);
    return image;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        return 2;
    uint8_t *image = map_image(argv[1]);
    if (!image) {
        printf("cannot read image\n");
        return 2;
    }

    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    log_init(dir);
    config_load(L"?"); // no such file: defaults, players=2

    if (!coop_install(image)) {
        printf("FAIL: coop_install refused the image (see th06mp.log in %%TEMP%%)\n");
        return 1;
    }

    static const struct {
        uint32_t site;
        int disp_off, len;
    } sites[] = {
        {SITE_GetInput, 1, 5},       {SITE_RegisterChain, 1, 5},  {SITE_RunEcl, 1, 5},
        {SITE_KillBox_Enemy, 1, 5},  {SITE_DamageToEnemy, 1, 5},  {SITE_BombOverlap, 1, 5},
        {SITE_KillBox_Bullet, 1, 5}, {SITE_LaserHitbox, 1, 5},    {SITE_Lea_OnUpdate, 3, 7},
        {SITE_Lea_OnDrawHighPrio, 3, 7}, {SITE_Lea_OnDrawLowPrio, 3, 7}, {SITE_Lea_OnDraw3, 3, 7},
        {SITE_DrawFocusVm, 1, 5},
    };
    uint8_t *self = (uint8_t *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(self + ((const IMAGE_DOS_HEADER *)self)->e_lfanew);
    int bad = 0;
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        uint8_t *insn = image + sites[i].site;
        int32_t disp;
        memcpy(&disp, insn + sites[i].disp_off, 4);
        uint8_t *thunk = insn + sites[i].len + disp;
        uint64_t target;
        memcpy(&target, thunk + 6, 8);
        int ok = thunk[0] == 0xFF && thunk[1] == 0x25 && target >= (uint64_t)(uintptr_t)self &&
                 target < (uint64_t)(uintptr_t)self + nt->OptionalHeader.SizeOfImage;
        printf("site %06X -> thunk %p -> %p  %s\n", sites[i].site, (void *)thunk, (void *)(uintptr_t)target,
               ok ? "ok" : "BAD");
        bad += !ok;
    }
    printf(bad ? "FAIL: %d bad site(s)\n" : "PASS: all %d sites patched\n", bad ? bad : (int)(sizeof(sites) / sizeof(sites[0])));
    return bad != 0;
}
