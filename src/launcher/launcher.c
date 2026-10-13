// th06mp.exe - starts th06nc.exe with th06mp.dll loaded.
//
//   th06mp.exe [path\to\th06nc.exe]
//
// The game is created suspended and parked on a two-byte loop at its entry
// point, so the Windows loader has finished but no game code has run when the
// DLL is injected and applies its patches.
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>

#define GAME_EXE L"th06nc.exe"
#define MOD_DLL L"th06mp.dll"
#define TH06NC_TIMESTAMP 0x6A9E3A91u

typedef struct {
    PVOID reserved1;
    PVOID peb_base;
    PVOID reserved2[2];
    ULONG_PTR pid;
    PVOID reserved3;
} ProcessBasicInfo;

LONG NTAPI NtQueryInformationProcess(HANDLE process, int info_class, PVOID info, ULONG len, PULONG ret_len);

static int fail(const wchar_t *fmt, ...)
{
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    MessageBoxW(NULL, msg, L"th06mp", MB_ICONERROR);
    return 1;
}

static bool exists(const wchar_t *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool find_game(const wchar_t *self_dir, const wchar_t *arg, wchar_t *out)
{
    static const wchar_t *candidates[] = {L"%ls\\" GAME_EXE, L"%ls\\Touhou6\\" GAME_EXE, L"%ls\\..\\Touhou6\\" GAME_EXE,
                                         L"%ls\\..\\" GAME_EXE};
    wchar_t tmp[MAX_PATH];
    if (arg && arg[0]) {
        return GetFullPathNameW(arg, MAX_PATH, out, NULL) && exists(out);
    }
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        swprintf(tmp, MAX_PATH, candidates[i], self_dir);
        if (GetFullPathNameW(tmp, MAX_PATH, out, NULL) && exists(out))
            return true;
    }
    return false;
}

// Runs LoadLibraryW(dll) inside the target and reports whether it succeeded.
static bool load_dll(HANDLE process, const wchar_t *dll)
{
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll, bytes, NULL))
        return false;
    // kernel32 is mapped at the same address in every process of a session.
    LPTHREAD_START_ROUTINE load_library = (LPTHREAD_START_ROUTINE)(uintptr_t)LoadLibraryW;
    HANDLE thread = CreateRemoteThread(process, NULL, 0, load_library, remote, 0, NULL);
    if (!thread)
        return false;
    WaitForSingleObject(thread, INFINITE);
    DWORD module_low = 0; // low half of the HMODULE; 0 means LoadLibrary failed
    GetExitCodeThread(thread, &module_low);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return module_low != 0;
}

static bool write_code(HANDLE process, void *addr, const void *bytes, SIZE_T n)
{
    DWORD old;
    if (!VirtualProtectEx(process, addr, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    bool ok = WriteProcessMemory(process, addr, bytes, n, NULL);
    VirtualProtectEx(process, addr, n, old, &old);
    FlushInstructionCache(process, addr, n);
    return ok;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)inst, (void)prev, (void)show;
    wchar_t self_dir[MAX_PATH], game[MAX_PATH], game_dir[MAX_PATH], dll[MAX_PATH];

    GetModuleFileNameW(NULL, self_dir, MAX_PATH);
    *wcsrchr(self_dir, L'\\') = 0;
    swprintf(dll, MAX_PATH, L"%ls\\" MOD_DLL, self_dir);
    if (!exists(dll))
        return fail(L"%ls is missing next to the launcher.", MOD_DLL);

    // Accept the path with or without surrounding quotes.
    while (*cmdline == L' ' || *cmdline == L'"')
        cmdline++;
    size_t len = wcslen(cmdline);
    while (len && (cmdline[len - 1] == L' ' || cmdline[len - 1] == L'"'))
        cmdline[--len] = 0;
    if (!find_game(self_dir, cmdline, game))
        return fail(L"Could not find %ls.\n\nPut the launcher next to the game, or pass the game's path as an argument.",
                    GAME_EXE);
    wcscpy(game_dir, game);
    *wcsrchr(game_dir, L'\\') = 0;

    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, game_dir, &si, &pi))
        return fail(L"Could not start %ls (error %lu).", game, GetLastError());

    const wchar_t *why = NULL;
    uint8_t *base = NULL;
    uint8_t header[0x400];
    uint8_t *entry = NULL;
    uint8_t saved[2];
    static const uint8_t spin[2] = {0xEB, 0xFE}; // jmp $

    ProcessBasicInfo pbi;
    if (NtQueryInformationProcess(pi.hProcess, 0, &pbi, sizeof(pbi), NULL) < 0 ||
        !ReadProcessMemory(pi.hProcess, (uint8_t *)pbi.peb_base + 0x10, &base, sizeof(base), NULL) ||
        !ReadProcessMemory(pi.hProcess, base, header, sizeof(header), NULL)) {
        why = L"Could not read the game's process.";
        goto abort;
    }
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(header + ((const IMAGE_DOS_HEADER *)header)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != TH06NC_TIMESTAMP) {
        why = L"This build of the game is not supported by this version of th06mp.";
        goto abort;
    }
    entry = base + nt->OptionalHeader.AddressOfEntryPoint;

    if (!ReadProcessMemory(pi.hProcess, entry, saved, 2, NULL) || !write_code(pi.hProcess, entry, spin, 2)) {
        why = L"Could not patch the game's entry point.";
        goto abort;
    }
    ResumeThread(pi.hThread);

    // Wait for the main thread to arrive at the entry point.
    bool parked = false;
    for (int tries = 0; tries < 5000 && !parked; tries++) {
        Sleep(2);
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            why = L"The game exited before it finished starting.";
            goto abort;
        }
        CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL};
        SuspendThread(pi.hThread);
        if (GetThreadContext(pi.hThread, &ctx) && ctx.Rip == (DWORD64)(uintptr_t)entry)
            parked = true;
        else
            ResumeThread(pi.hThread);
    }
    if (!parked) {
        why = L"Timed out waiting for the game to start.";
        goto abort;
    }

    if (!load_dll(pi.hProcess, dll)) {
        why = L"The mod could not be loaded into the game.\n\nSee th06mp.log next to the launcher for the reason.";
        goto abort;
    }
    if (!write_code(pi.hProcess, entry, saved, 2)) {
        why = L"Could not restore the game's entry point.";
        goto abort;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;

abort:
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return fail(L"%ls", why);
}
