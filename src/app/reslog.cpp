#include "app/reslog.h"
#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "psapi.lib")

namespace reslog {
namespace {

const char* const kName[KIND_COUNT] = { "scn", "tex", "seq", "sound" };
std::recursive_mutex g_mx;
FILE* g_f = nullptr;
DWORD g_t0 = 0;
long long g_live[KIND_COUNT] = {};
int g_count[KIND_COUNT] = {};
int g_copies = 0;

FILE* File() {
    if (!g_f) {
        g_f = fopen("loaded resources.log", "w");   // clean every run
        g_t0 = GetTickCount();
        if (g_f) fprintf(g_f, "time      what   size        live totals (scn / tex / seq / sound) | process private | name\n");
    }
    return g_f;
}

double MB(long long b) { return b / (1024.0 * 1024.0); }

void Line(const char* sign, Kind k, const std::string& path, size_t bytes, const char* note) {
    FILE* f = File();
    if (!f) return;
    PROCESS_MEMORY_COUNTERS_EX pm = {}; pm.cb = sizeof(pm);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    fprintf(f, "%8.2fs %s%-5s %8.2f MB  [%.1f / %.1f / %.1f / %.1f MB] | %.0f MB | %s%s%s\n",
            (GetTickCount() - g_t0) / 1000.0, sign, kName[k], MB((long long)bytes),
            MB(g_live[SCN]), MB(g_live[TEX]), MB(g_live[SEQ]), MB(g_live[SOUND]),
            MB((long long)pm.PrivateUsage), path.c_str(), note ? "  " : "", note ? note : "");
    fflush(f);
}

}   // namespace

void Loaded(Kind k, const std::string& path, size_t bytes, const char* note) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    g_live[k] += (long long)bytes; ++g_count[k];
    Line("+", k, path, bytes, note);
}

void Released(Kind k, const std::string& path, size_t bytes, const char* note) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    g_live[k] -= (long long)bytes; --g_count[k];
    Line("-", k, path, bytes, note);
}

void Note(const char* fmt, ...) {
    std::lock_guard<std::recursive_mutex> l(g_mx);
    FILE* f = File();
    if (!f) return;
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    PROCESS_MEMORY_COUNTERS_EX pm = {}; pm.cb = sizeof(pm);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    fprintf(f, "%8.2fs ==== %s | live scn %.1f MB (%d), tex %.1f MB (%d), seq %.1f MB (%d), sound %.1f MB (%d) | process private %.0f MB\n",
            (GetTickCount() - g_t0) / 1000.0, buf,
            MB(g_live[SCN]), g_count[SCN], MB(g_live[TEX]), g_count[TEX], MB(g_live[SEQ]), g_count[SEQ],
            MB(g_live[SOUND]), g_count[SOUND], MB((long long)pm.PrivateUsage));
    fflush(f);
}

Tag::Tag(const Tag& o) : kind(o.kind), path(o.path), bytes(o.bytes) {
    if (bytes) Loaded(kind, path, bytes, "(copy)");
}
Tag::Tag(Tag&& o) noexcept : kind(o.kind), path(std::move(o.path)), bytes(o.bytes) { o.bytes = 0; }
Tag& Tag::operator=(const Tag& o) {
    if (this == &o) return *this;
    if (bytes) Released(kind, path, bytes);
    kind = o.kind; path = o.path; bytes = o.bytes;
    if (bytes) Loaded(kind, path, bytes, "(copy)");
    return *this;
}
Tag& Tag::operator=(Tag&& o) noexcept {
    if (this == &o) return *this;
    if (bytes) Released(kind, path, bytes);
    kind = o.kind; path = std::move(o.path); bytes = o.bytes; o.bytes = 0;
    return *this;
}
Tag::~Tag() { if (bytes) Released(kind, path, bytes); }
void Tag::Set(Kind k, const std::string& p, size_t b, const char* note) {
    if (bytes) Released(kind, path, bytes);
    kind = k; path = p; bytes = b;
    if (bytes) Loaded(kind, path, bytes, note);
}

}   // namespace reslog
