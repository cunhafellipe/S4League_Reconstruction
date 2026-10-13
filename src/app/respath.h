// Resource root: "resources" (vanilla files, config "resource_mode": "normal")
// or the extended tree (mode "full": config "full_dir" if given, else the
// sibling folder laid out like "resources" with the richest script folder).
// Both hold xml/ and resources/ inside, so every "resources/..." path of the
// game maps to "<root>/...".
#pragma once
#include <string>

namespace respath {
inline std::string& RootRef() { static std::string r = "resources"; return r; }
inline const std::string& Root() { return RootRef(); }
inline void SetRoot(const std::string& r) { RootRef() = r; }
// "resources/..." -> "<root>/..."; anything else unchanged.
inline std::string P(const std::string& p) {
    static const std::string pre = "resources";
    if (p.compare(0, pre.size(), pre) == 0 && (p.size() == pre.size() || p[pre.size()] == '/'))
        return Root() + p.substr(pre.size());
    return p;
}
// Same, as a C string that stays valid for the next 32 calls (arguments of
// APIs that take const char*).
inline const char* PC(const std::string& p) {
    static thread_local std::string ring[32];
    static thread_local unsigned i = 0;
    std::string& s = ring[i++ & 31];
    s = P(p);
    return s.c_str();
}
// Reads config.json (resource_mode / full_dir) and sets the root. Call once at start.
void InitFromConfig();
}
#define RES(x) respath::P(x)
#define RESC(x) respath::PC(x)
