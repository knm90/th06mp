#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static DWORD g_start;

void log_init(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\th06mp.log", dir);
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_start = GetTickCount();
}

void log_msg(const char *fmt, ...)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[%8.3f] ", (GetTickCount() - g_start) / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    if (m > (int)sizeof(buf) - n - 3)
        m = (int)sizeof(buf) - n - 3;
    n += m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    WriteFile(g_file, buf, (DWORD)n, &written, NULL);
}
