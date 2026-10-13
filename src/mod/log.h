#pragma once
#include <wchar.h>

// Opens (truncating) th06mp.log in `dir`.
void log_init(const wchar_t *dir);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
