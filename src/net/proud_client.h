// ProudNet client - connects to an S4 server (auth/game) reusing the emulador
// protocol (ProudNetSrc). Handshake (server pushes the Hint on TCP connect via
// SessionHandler.ChannelActive; there is no RequestServerConnectionHint handler):
//   TCP connect -> [S] NotifyServerConnectionHint(4)  = NetConfig + RSA pubkey
//       -> NotifyCSEncryptedSessionKey(5)   = RSA-OAEP(AES key) + AES(RC4 key)
//       -> [S] NotifyCSSessionKeySuccess(6)
//       -> NotifyServerConnectionRequestData(7) = userdata + Version + NetVersion
//       -> [S] NotifyServerConnectSuccess(10) assigns HostId => Connected.
//   App messages then ride EncryptedReliable(36){ Rmi(1) }.
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "net/proud_crypt.h"

namespace proud {

enum class State {
    Disconnected, Connecting, WaitHint, WaitKeyAck, WaitConnectSuccess, Connected
};

// One row of the auth ServerListAck (AuthServer.ServiceModel/ServerInfoDto).
struct ServerInfo {
    std::string name;
    uint16_t    online = 0, limit = 0;
    std::string ip;
    uint16_t    port = 0;
};

struct Client {
    State    state  = State::Disconnected;
    uint32_t hostId = 0;                 // set by NotifyServerConnectSuccess
    std::vector<ServerInfo> servers;     // filled by ServerListAck (read by ServerSelect)
    // ProudNet Version GUID sent at handshake. Each server (auth/game/chat) has
    // its own; set the matching one before Connect(). Default = auth server's.
    uint8_t version[16] = {
        0x0b,0x3c,0xe7,0x9b, 0x10,0x3b, 0x3e,0x40,
        0xbe,0x7d,0x9f,0x22, 0x27,0x02,0xa3,0x8c };
    bool isGameConn = false;             // true = connected to a game server (no ServerListReq)

    bool Connect(const char* host, uint16_t port);
    void Poll();                          // pump recv + drive handshake (call per frame)
    void SendRmi(const uint8_t* data, size_t len);
    void SendAppRmi(uint16_t opcode, const uint8_t* fields, size_t n);  // [op LE][fields]
    void Disconnect();
    bool IsConnected() const { return state == State::Connected; }

private:
    uintptr_t   sock = ~(uintptr_t)0;     // SOCKET (INVALID_SOCKET)
    std::vector<uint8_t> rx;              // receive accumulator
    ProudCrypt  crypt;                    // session AES/RC4 (keys set at handshake)

    void SendCore(uint16_t op, const uint8_t* p, size_t n);
    void OnCore(uint16_t op, const uint8_t* p, size_t n);
    void OnApp(uint16_t op, const uint8_t* p, size_t n);   // decrypted RMI app messages
};

// Process-wide auth/game connection, polled every frame by the Engine.
inline Client g_client;

}  // namespace proud
