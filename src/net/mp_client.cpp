#include "net/mp_client.h"
#include "app/dbg.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <cmath>
#include <cstdlib>
#include <memory>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace mp {

// ---------------------------------------------------------------------------
// Minimal JSON (objects, arrays, strings, numbers, true/false/null) - enough
// for the relay's messages.
namespace {
struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    double num = 0; bool b = false; std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal* get(const char* k) const {
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string s(const char* k) const { const JVal* v = get(k); return v && v->kind == Str ? v->str : std::string(); }
    double n(const char* k, double d = 0) const { const JVal* v = get(k); return v && v->kind == Num ? v->num : d; }
};
struct JParser {
    const char* p; const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool parse(JVal& v) {
        ws(); if (p >= e) return false;
        if (*p == '{') {
            ++p; v.kind = JVal::Obj; ws();
            if (p < e && *p == '}') { ++p; return true; }
            while (p < e) {
                JVal k; if (!parse(k) || k.kind != JVal::Str) return false;
                ws(); if (p >= e || *p != ':') return false; ++p;
                JVal val; if (!parse(val)) return false;
                v.obj.push_back({ k.str, std::move(val) });
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (*p == '[') {
            ++p; v.kind = JVal::Arr; ws();
            if (p < e && *p == ']') { ++p; return true; }
            while (p < e) {
                JVal val; if (!parse(val)) return false;
                v.arr.push_back(std::move(val));
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (*p == '"') {
            ++p; v.kind = JVal::Str;
            while (p < e && *p != '"') {
                if (*p == '\\' && p + 1 < e) {
                    ++p;
                    switch (*p) {
                    case 'n': v.str += '\n'; break; case 't': v.str += '\t'; break;
                    case 'r': v.str += '\r'; break; case 'b': v.str += '\b'; break;
                    case 'f': v.str += '\f'; break;
                    case 'u': {
                        if (p + 4 < e) {
                            unsigned cp = (unsigned)strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16);
                            if (cp < 0x80) v.str += (char)cp;
                            else if (cp < 0x800) { v.str += (char)(0xC0 | (cp >> 6)); v.str += (char)(0x80 | (cp & 0x3F)); }
                            else { v.str += (char)(0xE0 | (cp >> 12)); v.str += (char)(0x80 | ((cp >> 6) & 0x3F)); v.str += (char)(0x80 | (cp & 0x3F)); }
                            p += 4;
                        }
                        break;
                    }
                    default: v.str += *p; break;
                    }
                    ++p;
                } else v.str += *p++;
            }
            if (p < e) ++p;
            return true;
        }
        if (!strncmp(p, "true", 4))  { p += 4; v.kind = JVal::Bool; v.b = true;  return true; }
        if (!strncmp(p, "false", 5)) { p += 5; v.kind = JVal::Bool; v.b = false; return true; }
        if (!strncmp(p, "null", 4))  { p += 4; v.kind = JVal::Null; return true; }
        char* end = nullptr; v.num = strtod(p, &end);
        if (end == p) return false;
        v.kind = JVal::Num; p = end; return true;
    }
};
bool ParseJson(const std::string& s, JVal& out) { JParser jp{ s.data(), s.data() + s.size() }; return jp.parse(out); }

std::string Esc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
std::string Num(double v) { char b[32]; snprintf(b, sizeof(b), "%.2f", v); return b; }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}  // namespace

// ---------------------------------------------------------------------------
Client::~Client() {
    if (connectThread_.joinable()) connectThread_.join();
    Disconnect();
}

void Client::ConnectAsync(const std::string& host, int port, int id, const std::string& nickname, const std::string& map,
                          const std::string& weapon, const std::string& weaponStates, const std::string& equip) {
    if (connecting_.load()) return;
    if (connectThread_.joinable()) connectThread_.join();
    connecting_ = true;
    connectThread_ = std::thread([=] {
        Connect(host, port, id, nickname, map, weapon, weaponStates, equip);
        connecting_ = false;
    });
}

bool Client::Connect(const std::string& host, int port, int id, const std::string& nickname, const std::string& map,
                     const std::string& weapon, const std::string& weaponStates, const std::string& equip) {
    Disconnect();
    host_ = host; port_ = port; id_ = id;
    const std::string portStr = std::to_string(port);

    // Reachability probe so a server that is not running fails fast instead of
    // waiting for WinHTTP's own timeout (runs on the connect thread).
    {
        WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
        addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) return false;
        SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        u_long nb = 1; ioctlsocket(t, FIONBIO, &nb);
        connect(t, res->ai_addr, (int)res->ai_addrlen);
        freeaddrinfo(res);
        fd_set wr; FD_ZERO(&wr); FD_SET(t, &wr);
        timeval tv{ 3, 0 };  // 3.0s timeout for VPS / WAN latency
        const bool up = select(0, nullptr, &wr, nullptr, &tv) == 1;
        closesocket(t);
        if (!up) { DbgLog("[mp] %s:%d not reachable", host.c_str(), port); return false; }
    }

    session_ = WinHttpOpen(L"projectoS10", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) { DbgLog("[mp] WinHttpOpen failed %lu", GetLastError()); return false; }
    WinHttpSetTimeouts((HINTERNET)session_, 2000, 2000, 2000, 2000);
    connect_ = WinHttpConnect((HINTERNET)session_, Widen(host).c_str(), (INTERNET_PORT)port, 0);
    if (!connect_) { DbgLog("[mp] WinHttpConnect %s:%d failed %lu", host.c_str(), port, GetLastError()); Disconnect(); return false; }
    HINTERNET req = WinHttpOpenRequest((HINTERNET)connect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) { Disconnect(); return false; }
    WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr)) {
        DbgLog("[mp] websocket handshake to %s:%d failed %lu (is server/mpserver.js running?)", host.c_str(), port, GetLastError());
        WinHttpCloseHandle(req); Disconnect(); return false;
    }
    ws_ = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!ws_) { DbgLog("[mp] websocket upgrade failed %lu", GetLastError()); Disconnect(); return false; }
    wsOpen_ = true;
    recvThread_ = std::thread([this] { WsRecvLoop(); });

    // UDP: ephemeral port; the server learns our address from the first packet.
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = 0;
        bind(s, (sockaddr*)&a, sizeof(a));
        udp_ = (uintptr_t)s;
    }

    // id 0 = let the server hand out a unique one ('welcome').
    WsSend("{\"t\":\"hello\",\"id\":" + std::to_string(id) + ",\"nickname\":\"" + Esc(nickname) +
           "\",\"map\":\"" + Esc(map) + "\",\"weapon\":\"" + Esc(weapon) + "\",\"weaponStates\":\"" + Esc(weaponStates) +
           "\",\"equip\":\"" + Esc(equip) + "\"}");
    DbgLog("[mp] connected to %s:%d (id=%d nick='%s' map='%s')", host.c_str(), port, id, nickname.c_str(), map.c_str());
    return true;
}

void Client::Disconnect() {
    wsOpen_ = false;
    if (ws_) WinHttpWebSocketClose((HINTERNET)ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    if (recvThread_.joinable()) recvThread_.join();
    if (ws_) { WinHttpCloseHandle((HINTERNET)ws_); ws_ = nullptr; }
    if (connect_) { WinHttpCloseHandle((HINTERNET)connect_); connect_ = nullptr; }
    if (session_) { WinHttpCloseHandle((HINTERNET)session_); session_ = nullptr; }
    if (udp_ != ~(uintptr_t)0) { closesocket((SOCKET)udp_); udp_ = ~(uintptr_t)0; }
    peers_.clear();
}

void Client::WsSend(const std::string& text) {
    if (!wsOpen_ || !ws_) return;
    if (WinHttpWebSocketSend((HINTERNET)ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)text.data(), (DWORD)text.size()) != NO_ERROR)
        wsOpen_ = false;
}

void Client::WsRecvLoop() {
    std::vector<char> buf(64 * 1024);
    std::string msg;
    while (wsOpen_) {
        DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive((HINTERNET)ws_, buf.data(), (DWORD)buf.size(), &got, &type);
        if (err != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { wsOpen_ = false; break; }
        msg.append(buf.data(), got);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            std::lock_guard<std::mutex> lk(qMutex_);
            wsQueue_.push_back(std::move(msg));
            msg.clear();
        }
    }
}

void Client::Poll(unsigned nowMs) {
    std::vector<std::string> q;
    { std::lock_guard<std::mutex> lk(qMutex_); q.swap(wsQueue_); }
    for (const std::string& m : q) HandleWs(m, nowMs);

    if (udp_ != ~(uintptr_t)0) {
        char buf[2048];
        for (int i = 0; i < 256; ++i) {
            int n = recvfrom((SOCKET)udp_, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) break;
            HandleUdp(std::string(buf, n), nowMs);
        }
    }
}

static void ReadPos(const JVal& o, Peer& p) {
    if (const JVal* pos = o.get("pos")) {
        p.x = (float)pos->n("x"); p.y = (float)pos->n("y"); p.z = (float)pos->n("z");
        if (!p.hasPos) { p.rx = p.x; p.ry = p.y; p.rz = p.z; p.ryaw = (float)o.n("yaw"); }
        p.hasPos = true;
    }
    if (o.get("yaw")) p.yaw = (float)o.n("yaw");
}

void Client::HandleWs(const std::string& msg, unsigned nowMs) {
    JVal m; if (!ParseJson(msg, m) || m.kind != JVal::Obj) return;
    const std::string t = m.s("t");
    if (t == "roster") {
        const JVal* list = m.get("peers");
        if (!list || list->kind != JVal::Arr) return;
        std::map<int, bool> seen;
        for (const JVal& o : list->arr) {
            int id = (int)o.n("id", -1);
            if (id < 0 || id == id_) continue;
            seen[id] = true;
            if (!peers_.count(id)) DbgLog("[mp] %s (id %d) is here - map '%s'", o.s("nickname").c_str(), id, o.s("map").c_str());
            Peer& p = peers_[id];
            p.id = id;
            p.nickname = o.s("nickname"); p.map = o.s("map");
            p.weapon = o.s("weapon"); p.weaponStates = o.s("weaponStates"); p.equip = o.s("equip");
            // Position by WS only as a fallback when UDP is not arriving.
            if (nowMs - p.lastUdpMs > 500) { ReadPos(o, p); if (!o.s("clip").empty()) p.clip = o.s("clip"); }
        }
        for (auto it = peers_.begin(); it != peers_.end(); )
            if (!seen.count(it->first)) { DbgLog("[mp] %s (id %d) left", it->second.nickname.c_str(), it->first); it = peers_.erase(it); }
            else ++it;
    } else if (t == "welcome") {
        id_ = (int)m.n("id", id_);
        DbgLog("[mp] server assigned id %d", id_);
    } else if (t == "state") {
        int id = (int)m.n("id", -1);
        if (id >= 0 && id != id_ && peers_.count(id)) {
            Peer& p = peers_[id];
            p.map = m.s("map");
            if (m.get("hp")) p.hp = (float)m.n("hp", p.hp);
            if (nowMs - p.lastUdpMs > 300) {
                ReadPos(m, p);
                if (!m.s("clip").empty()) p.clip = m.s("clip");
            }
        }
    } else if (t == "shot") {
        Shot s; s.fromId = (int)m.n("id");
        if (s.fromId == id_) return;
        if (const JVal* o = m.get("o")) if (o->kind == JVal::Arr && o->arr.size() >= 3) for (int k = 0; k < 3; ++k) s.o[k] = (float)o->arr[k].num;
        s.yaw = (float)m.n("yaw"); s.pitch = (float)m.n("pitch");
        s.blast = m.s("blast"); s.blastSound = m.s("bs"); s.tracer = m.s("tr");
        s.wall = m.s("wall"); s.wallSound = m.s("ws"); s.hit = m.s("hit"); s.hitSound = m.s("hs");
        if (const JVal* e = m.get("e")) if (e->kind == JVal::Arr)
            for (const JVal& a : e->arr) if (a.kind == JVal::Arr && a.arr.size() >= 4)
                s.ends.push_back({ (float)a.arr[0].num, (float)a.arr[1].num, (float)a.arr[2].num, (int)a.arr[3].num });
        shots_.push_back(std::move(s));
    } else if (t == "chat") {
        chat_.push_back({ m.s("nickname"), m.s("text") });
    } else if (t == "hurt") {
        Hurt h; h.dmg = (float)m.n("dmg"); h.knock = (float)m.n("knock"); h.fromId = (int)m.n("fromId");
        if (const JVal* f = m.get("from")) { h.fromX = (float)f->n("x"); h.fromY = (float)f->n("y"); h.fromZ = (float)f->n("z"); }
        hurts_.push_back(h);
    } else if (t == "kicked") {
        DbgLog("[mp] kicked: this account logged in somewhere else");
        wsOpen_ = false;
    }
}

void Client::HandleUdp(const std::string& msg, unsigned nowMs) {
    JVal m; if (!ParseJson(msg, m) || m.kind != JVal::Obj) return;
    int id = (int)m.n("id", -1);
    if (id < 0 || id == id_) return;
    auto it = peers_.find(id);
    if (it == peers_.end()) return;   // not in the roster yet
    Peer& p = it->second;
    p.map = m.s("map"); p.lastUdpMs = nowMs;
    if (m.get("hp")) p.hp = (float)m.n("hp", p.hp);
    ReadPos(m, p);
    if (!m.s("clip").empty()) p.clip = m.s("clip");
}

void Client::SendState(const std::string& map, float x, float y, float z, float yaw, const std::string& clip, float hp) {
    if (udp_ == ~(uintptr_t)0 || id_ <= 0) return;   // no id yet (waiting for 'welcome')
    const std::string s = "{\"id\":" + std::to_string(id_) + ",\"map\":\"" + Esc(map) + "\",\"pos\":{\"x\":" + Num(x) +
                          ",\"y\":" + Num(y) + ",\"z\":" + Num(z) + "},\"yaw\":" + Num(yaw) + ",\"clip\":\"" + Esc(clip) + "\",\"hp\":" + Num(hp) + "}";
    addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    static std::string cachedHost; static sockaddr_in cached{};
    const std::string key = host_ + ":" + std::to_string(port_ + 1);
    if (cachedHost != key) {
        if (getaddrinfo(host_.c_str(), std::to_string(port_ + 1).c_str(), &hints, &res) != 0 || !res) return;
        cached = *(sockaddr_in*)res->ai_addr; freeaddrinfo(res); cachedHost = key;
    }
    sendto((SOCKET)udp_, s.data(), (int)s.size(), 0, (sockaddr*)&cached, sizeof(cached));
}

void Client::SendStateWs(const std::string& map, float x, float y, float z, float yaw, const std::string& clip, float hp) {
    if (id_ <= 0) return;
    WsSend("{\"t\":\"state\",\"map\":\"" + Esc(map) + "\",\"pos\":{\"x\":" + Num(x) + ",\"y\":" + Num(y) + ",\"z\":" + Num(z) +
           "},\"yaw\":" + Num(yaw) + ",\"clip\":\"" + Esc(clip) + "\",\"hp\":" + Num(hp) + "}");
}

void Client::SendEquip(const std::string& weapon, const std::string& weaponStates) {
    WsSend("{\"t\":\"equip\",\"weapon\":\"" + Esc(weapon) + "\",\"weaponStates\":\"" + Esc(weaponStates) + "\"}");
}
void Client::SendCostume(const std::string& equip) { WsSend("{\"t\":\"costume\",\"equip\":\"" + Esc(equip) + "\"}"); }
void Client::SendChat(const std::string& text) { WsSend("{\"t\":\"chat\",\"text\":\"" + Esc(text) + "\"}"); }
void Client::SendShot(const Shot& s) {
    if (id_ <= 0) return;
    std::string e = "[";
    for (size_t i = 0; i < s.ends.size() && i < 32; ++i) {
        if (i) e += ",";
        e += "[" + Num(s.ends[i].x) + "," + Num(s.ends[i].y) + "," + Num(s.ends[i].z) + "," + std::to_string(s.ends[i].kind) + "]";
    }
    e += "]";
    WsSend("{\"t\":\"shot\",\"o\":[" + Num(s.o[0]) + "," + Num(s.o[1]) + "," + Num(s.o[2]) + "],\"yaw\":" + Num(s.yaw) + ",\"pitch\":" + Num(s.pitch) +
           ",\"blast\":\"" + Esc(s.blast) + "\",\"bs\":\"" + Esc(s.blastSound) + "\",\"tr\":\"" + Esc(s.tracer) +
           "\",\"wall\":\"" + Esc(s.wall) + "\",\"ws\":\"" + Esc(s.wallSound) + "\",\"hit\":\"" + Esc(s.hit) + "\",\"hs\":\"" + Esc(s.hitSound) +
           "\",\"e\":" + e + "}");
}

void Client::SendHit(int targetId, float dmg, float knock) {
    WsSend("{\"t\":\"hit\",\"targetId\":" + std::to_string(targetId) + ",\"dmg\":" + Num(dmg) + ",\"knock\":" + Num(knock) + "}");
}

}  // namespace mp
