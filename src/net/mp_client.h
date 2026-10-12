// Client for the mini multiplayer relay (server/mpserver.js, copied from
// S4Web2): WebSocket :4100 for reliable messages (hello / equip / costume /
// chat / hit -> roster / chat / hurt / kicked) and UDP :4101 for movement
// ({id, map, pos, yaw, clip} ~20/s, relayed to the other players).
// The server only relays - every client sends its own state.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace mp {

struct Peer {
    int id = 0;
    std::string nickname, map, clip;
    std::string weapon, weaponStates, equip;   // equip = JSON string, as sent by the web client
    float x = 0, y = 0, z = 0, yaw = 0;        // last received
    float rx = 0, ry = 0, rz = 0, ryaw = 0;    // rendered (smoothed toward the last received)
    bool hasPos = false;
    unsigned lastUdpMs = 0;
    float animClockMs = 0;                     // local clock for the peer's clip
    float hp = 100.f;                          // last HP the peer sent (bar over its head)
    std::string lastClip;
};

struct ChatLine { std::string nickname, text; };
struct Hurt { float dmg = 0, knock = 0, fromX = 0, fromY = 0, fromZ = 0; int fromId = 0; };
// One gun shot, as the shooter saw it: where it left, the effects of that
// weapon's attribute, and where each bullet ended (k: 0 air, 1 wall, 2 player).
struct Shot {
    int fromId = 0;
    float o[3] = { 0, 0, 0 }, yaw = 0, pitch = 0;
    std::string blast, blastSound, tracer, wall, wallSound, hit, hitSound;
    struct End { float x = 0, y = 0, z = 0; int kind = 0; };
    std::vector<End> ends;
};

class Client {
public:
    ~Client();
    // Opens the WebSocket (port) and the UDP socket (port + 1) and sends hello.
    // Blocking (TCP probe + handshake): call it through ConnectAsync from the game loop.
    bool Connect(const std::string& host, int port, int id, const std::string& nickname, const std::string& map,
                 const std::string& weapon, const std::string& weaponStates, const std::string& equip);
    // Runs Connect on a worker thread so a slow/unreachable server never freezes the frame.
    // No-op while a previous attempt is still running.
    void ConnectAsync(const std::string& host, int port, int id, const std::string& nickname, const std::string& map,
                      const std::string& weapon, const std::string& weaponStates, const std::string& equip);
    bool Connecting() const { return connecting_.load(); }
    void Disconnect();
    bool Connected() const { return wsOpen_.load() && !connecting_.load(); }

    // Main thread, every frame: applies queued WS/UDP messages to the peer table.
    void Poll(unsigned nowMs);
    // Movement over UDP (call ~20/s).
    void SendState(const std::string& map, float x, float y, float z, float yaw, const std::string& clip, float hp = 100.f);
    // Same state over the WebSocket (TCP) - fallback so players still see
    // each other when UDP :4101 is blocked by a firewall (call ~2/s).
    void SendStateWs(const std::string& map, float x, float y, float z, float yaw, const std::string& clip, float hp = 100.f);
    void SendEquip(const std::string& weapon, const std::string& weaponStates);
    void SendCostume(const std::string& equip);
    void SendChat(const std::string& text);
    void SendHit(int targetId, float dmg, float knock);
    void SendShot(const Shot& s);

    std::map<int, Peer>& Peers() { return peers_; }
    std::vector<ChatLine> TakeChat() { std::vector<ChatLine> v; v.swap(chat_); return v; }
    std::vector<Hurt> TakeHurts() { std::vector<Hurt> v; v.swap(hurts_); return v; }
    std::vector<Shot> TakeShots() { std::vector<Shot> v; v.swap(shots_); return v; }
    int MyId() const { return id_; }

private:
    void WsSend(const std::string& text);
    void WsRecvLoop();
    void HandleWs(const std::string& msg, unsigned nowMs);
    void HandleUdp(const std::string& msg, unsigned nowMs);

    std::string host_;
    int port_ = 4100;
    int id_ = 0;
    std::atomic<bool> connecting_{ false };
    std::thread connectThread_;
    void* session_ = nullptr;   // HINTERNET
    void* connect_ = nullptr;
    void* ws_ = nullptr;
    uintptr_t udp_ = ~(uintptr_t)0;   // SOCKET
    std::atomic<bool> wsOpen_{ false };
    std::thread recvThread_;
    std::mutex qMutex_;
    std::vector<std::string> wsQueue_;
    std::map<int, Peer> peers_;
    std::vector<ChatLine> chat_;
    std::vector<Hurt> hurts_;
    std::vector<Shot> shots_;
};

}  // namespace mp
