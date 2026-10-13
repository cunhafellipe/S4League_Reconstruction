#include "app/respath.h"
#include "app/dbg.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace respath {

static std::string Str(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = text.find(pat);
    if (k == std::string::npos) return "";
    k = text.find(':', k + pat.size());
    if (k == std::string::npos) return "";
    const size_t a = text.find('"', k + 1);
    if (a == std::string::npos) return "";
    const size_t b = text.find('"', a + 1);
    return b == std::string::npos ? "" : text.substr(a + 1, b - a - 1);
}

// .lua files of the richest script folder (script*) under <tree>/resources.
static size_t RichestScriptFolder(const std::string& tree) {
    size_t best = 0;
    WIN32_FIND_DATAA dd;
    HANDLE h = FindFirstFileExA((tree + "/resources/script*").c_str(), FindExInfoBasic, &dd, FindExSearchLimitToDirectories, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        size_t n = 0;
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA((tree + "/resources/" + dd.cFileName + "/*.lua").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) { do ++n; while (FindNextFileA(f, &fd)); FindClose(f); }
        best = (std::max)(best, n);
    } while (FindNextFileA(h, &dd));
    FindClose(h);
    return best;
}

// The extended tree: config "full_dir" when given, else the folder next to
// "resources" that is laid out like it (xml/ + resources/) and carries the
// richest script folder - more files than the vanilla one.
static std::string FindExtendedTree(const std::string& configured) {
    if (!configured.empty()) return GetFileAttributesA((configured + "/xml").c_str()) != INVALID_FILE_ATTRIBUTES ? configured : "";
    const size_t vanilla = RichestScriptFolder("resources");
    std::string best; size_t bestN = vanilla;
    WIN32_FIND_DATAA dd;
    HANDLE h = FindFirstFileExA("*", FindExInfoBasic, &dd, FindExSearchLimitToDirectories, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return "";
    do {
        const std::string name = dd.cFileName;
        if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == "." || name == ".." || name == "resources") continue;
        if (GetFileAttributesA((name + "/xml").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (GetFileAttributesA((name + "/resources").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const size_t n = RichestScriptFolder(name);
        if (n > bestN) { bestN = n; best = name; }
    } while (FindNextFileA(h, &dd));
    FindClose(h);
    return best;
}

void InitFromConfig() {
    std::ifstream f("config.json", std::ios::binary);
    std::ostringstream ss; if (f) ss << f.rdbuf();
    const std::string text = ss.str();
    const std::string mode = Str(text, "resource_mode");
    std::string dir = Str(text, "full_dir");
    for (char& c : dir) if (c == '\\') c = '/';
    std::string root = "resources";
    if (mode == "full") {
        const std::string found = FindExtendedTree(dir);
        if (!found.empty()) root = found;
        else DbgLog("[resources] full mode: no extended tree found next to 'resources' - using vanilla");
    }
    SetRoot(root);
    DbgLog("[resources] mode=%s root='%s'", mode.empty() ? "normal" : mode.c_str(), Root().c_str());
}

}  // namespace respath
