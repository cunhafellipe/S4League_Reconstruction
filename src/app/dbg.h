// Tiny debug log to s4_debug.log (next to the exe). ponytail: bring-up only.
#pragma once
#include <cstdio>
#include <cstdarg>

inline void DbgLog(const char* fmt, ...) {
    static FILE* f = nullptr;
    if (!f) f = fopen("s4_debug.log", "w");
    if (!f) return;
    va_list a; va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fflush(f);
}
