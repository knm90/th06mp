#include <wchar.h>
#include <windows.h>

#include "config.h"
#include "coop.h"
#include "log.h"

// The launcher loads this DLL while the game is parked at its entry point, so
// patching here happens before any game code has run. Returning FALSE makes
// LoadLibrary fail, which the launcher reports.
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(self);

    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(self, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;

    log_init(dir);
    log_msg("th06mp loaded");

    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%ls\\th06mp.ini", dir);
    config_load(ini);

    if (!coop_install((uint8_t *)GetModuleHandleW(NULL))) {
        log_msg("install failed, the game was not modified");
        return FALSE;
    }
    log_msg("install ok");
    return TRUE;
}
