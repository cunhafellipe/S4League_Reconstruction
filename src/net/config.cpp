#include "net/config.h"
#include <fstream>
#include <cstdlib>

namespace cfg {

static std::string Trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ServerEndpoint(std::string& ip, int& port) {
    std::ifstream f("version.ini");
    if (!f) return false;
    std::string line;
    bool inServer = false;
    while (std::getline(f, line)) {
        std::string t = Trim(line);
        if (t.empty()) continue;
        if (t[0] == '[') { inServer = (t == "[SERVER]"); continue; }
        if (!inServer) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(t.substr(0, eq));
        std::string v = Trim(t.substr(eq + 1));
        if (k == "IP1")       ip = v;
        else if (k == "PORT1") port = std::atoi(v.c_str());
    }
    return !ip.empty();
}

}  // namespace cfg
