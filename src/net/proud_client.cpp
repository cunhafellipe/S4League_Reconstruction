// ProudNet client implementation. Framing = proud_frame; opcodes = proud_opcodes.
// Payload inside a frame = [op:1][body]  (ProudCoreOpCode is `: byte` -> 1 byte,
// per emulador CoreMessageEncoder.cs). Scalars = [prefix 1/2/4][value LE]
// (Extensions.WriteScalar). The RSA session-key exchange is real (proud_crypt).

// winsock2 must precede windows.h (pulled in by proud_crypt.h) or ws2tcpip.h
// conflicts with the legacy winsock.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "net/proud_client.h"
#include "net/proud_frame.h"
#include "net/proud_opcodes.h"
#include "app/dbg.h"

#include <cstring>
#include <cstdio>

namespace proud {

static bool g_wsaUp = false;
static void EnsureWSA() {
    if (!g_wsaUp) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); g_wsaUp = true; }
}

bool Client::Connect(const char* host, uint16_t port) {
    EnsureWSA();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);   // NON-BLOCKING before connect

    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, host, &a.sin_addr);
    // Non-blocking connect returns WSAEWOULDBLOCK immediately; completes async
    // (checked in Poll via select). This keeps the frame from stalling.
    ::connect(s, (sockaddr*)&a, sizeof(a));
    sock  = (uintptr_t)s;
    state = State::Connecting;
    DbgLog("[net] connect %s:%u started sock=%d", host, (unsigned)port, (int)s);
    return true;   // "started"; actual result surfaces in Poll()
}

// WriteScalar: [prefix 1/2/4][value LE] (emulador Extensions.WriteScalar).
static void PushScalar(std::vector<uint8_t>& b, uint32_t v) {
    if (v < 128)        { b.push_back(1); b.push_back((uint8_t)v); }
    else if (v < 32768) { b.push_back(2); b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
    else                { b.push_back(4); for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8*i))); }
}
// ReadScalar: inverse of PushScalar ([prefix 1/2/4][value LE]).
static uint32_t ReadScalar(const uint8_t* p, size_t n, size_t& o) {
    if (o >= n) return 0;
    uint8_t prefix = p[o++];
    uint32_t v = 0;
    for (int i = 0; i < prefix && o < n; ++i) v |= (uint32_t)p[o++] << (8*i);
    return v;
}

void Client::SendCore(uint16_t op, const uint8_t* p, size_t n) {
    std::vector<uint8_t> body;
    body.push_back((uint8_t)op);                 // ProudCoreOpCode is 1 byte
    if (p && n) body.insert(body.end(), p, p + n);
    auto frame = proud::EncodeFrame(body.data(), body.size());
    ::send((SOCKET)sock, (const char*)frame.data(), (int)frame.size(), 0);
}

void Client::OnCore(uint16_t op, const uint8_t* p, size_t n) {
    DbgLog("[net] <- core op=%u state=%d n=%zu", (unsigned)op, (int)state, n);
    switch ((ProudCoreOp)op) {
    case ProudCoreOp::NotifyServerConnectionHint: {
        // Body = NetConfigDto + ReadStruct(DER RSA pubkey). We don't need the
        // config, so locate the DER SEQUENCE (RsaOaepEncrypt scans for it) and
        // encrypt a fresh 16-byte AES session key with the server pubkey.
        uint8_t secureKey[16]; proud::RandBytes(secureKey, sizeof(secureKey));
        std::vector<uint8_t> secure;
        if (!proud::RsaOaepEncrypt(p, n, secureKey, sizeof(secureKey), secure)) {
            DbgLog("[net] RSA pubkey parse/encrypt FAILED -> disconnect");
            Disconnect(); return;                    // couldn't parse pubkey
        }
        crypt.SetSecureKey(secureKey, sizeof(secureKey));

        // FastKey = AES-ECB(RC4 key) with the just-set secure key; server does
        // Crypt.AES.Decrypt(FastKey) then InitializeFastEncryption(fastKey).
        uint8_t fastKey[16]; proud::RandBytes(fastKey, sizeof(fastKey));
        crypt.SetFastKey(fastKey, sizeof(fastKey));
        std::vector<uint8_t> fast = crypt.AesEncryptRaw(fastKey, sizeof(fastKey));

        // NotifyCSEncryptedSessionKey = SecureKey(array+scalar) + FastKey(array+scalar).
        std::vector<uint8_t> body;
        PushScalar(body, (uint32_t)secure.size()); body.insert(body.end(), secure.begin(), secure.end());
        PushScalar(body, (uint32_t)fast.size());   body.insert(body.end(), fast.begin(),   fast.end());
        SendCore((uint16_t)ProudCoreOp::NotifyCSEncryptedSessionKey, body.data(), body.size());
        state = State::WaitKeyAck;
        DbgLog("[net] Hint(4) -> sent SessionKey(5) [secure=%zu fast=%zu], WaitKeyAck", secure.size(), fast.size());
        break;
    }
    case ProudCoreOp::NotifyCSSessionKeySuccess: {
        // NotifyServerConnectionRequestData = UserData(array+scalar, empty)
        //   + Version(Guid, 16 bytes, empty) + InternalNetVersion(uint32 LE).
        const uint32_t kNetVersion = 196980;         // emulador Constants.NetVersion
        // Version GUID must match THIS server's serverVersion (auth/game differ),
        // else it replies NotifyProtocolVersionMismatch. Set `version` per Connect.
        std::vector<uint8_t> body;
        PushScalar(body, 0);                         // UserData: length 0
        body.insert(body.end(), version, version + 16);   // Version GUID (per-connection)
        for (int i = 0; i < 4; ++i) body.push_back((uint8_t)(kNetVersion >> (8*i)));
        SendCore((uint16_t)ProudCoreOp::NotifyServerConnectionRequestData, body.data(), body.size());
        state = State::WaitConnectSuccess;
        DbgLog("[net] KeySuccess(6) -> sent ConnectionRequestData(7), WaitConnectSuccess");
        break;
    }
    case ProudCoreOp::NotifyServerConnectSuccess:
        if (n >= 4) hostId = p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24);
        state = State::Connected;
        DbgLog("[net] *** CONNECTED *** hostId=%u", hostId);
        if (!isGameConn) {
            SendAppRmi(5004, nullptr, 0);        // auth: ServerListReq -> ServerListAck
            DbgLog("[net] -> ServerListReq(5004)");
        } else {
            DbgLog("[net] GAME server CONNECTED - game login TODO");
        }
        break;
    case ProudCoreOp::NotifyServerDeniedConnection:
    case ProudCoreOp::NotifyProtocolVersionMismatch:
        DbgLog("[net] server DENIED / version mismatch (op=%u) -> disconnect", (unsigned)op);
        Disconnect();
        break;
    case ProudCoreOp::EncryptedReliable: {
        // body = [EncryptMode byte][scalar len][encrypted blob]; decrypt ->
        // inner core message [core op][body]. App RMI = [Rmi=1][op LE][fields].
        size_t o = 0;
        if (n < 2) break;
        uint8_t mode = p[o++];
        uint32_t blob = ReadScalar(p, n, o);
        if (blob == 0 || o + blob > n) break;
        std::vector<uint8_t> dec;
        if (!crypt.Decrypt(p + o, blob, dec, mode == 1, true)) { DbgLog("[net] EncryptedReliable decrypt FAILED"); break; }
        if (dec.size() >= 3 && dec[0] == (uint8_t)ProudCoreOp::Rmi)
            OnApp((uint16_t)(dec[1] | (dec[2] << 8)), dec.data() + 3, dec.size() - 3);
        break;
    }
    default:
        break;   // pings handled by the app layer later.
    }
}

// Decrypted RMI app messages (auth: ServerListAck, LoginEUAck, ...).
void Client::OnApp(uint16_t op, const uint8_t* p, size_t n) {
    DbgLog("[net] <- APP rmi op=%u n=%zu", (unsigned)op, n);
    if (op == 5104) {   // AuthOpCode.ServerListAck
        { std::string hx; char hb[4]; for (size_t k=0;k<n && k<80;++k){ sprintf(hb,"%02X ",p[k]); hx+=hb; }
          DbgLog("[net] ServerListAck RAW n=%zu: %s", n, hx.c_str()); }   // ground-truth bytes for field alignment
        size_t o = 0;
        auto u8  = [&](){ return (uint8_t)(o < n ? p[o++] : 0); };
        auto u16 = [&](){ uint16_t v = (o+1 < n) ? (uint16_t)(p[o] | (p[o+1]<<8)) : 0; o += 2; return v; };
        auto i32 = [&](){ int32_t v = (o+3 < n) ? (int32_t)(p[o] | (p[o+1]<<8) | (p[o+2]<<16) | ((uint32_t)p[o+3]<<24)) : 0; o += 4; return v; };
        auto str = [&](){ uint32_t len=0; int sh=0; while (o < n) { uint8_t b=p[o++]; len |= (uint32_t)(b&0x7f)<<sh; if (!(b&0x80)) break; sh += 7; }
                          std::string s; if (o+len <= n) s.assign((const char*)(p+o), len); o += len; return s; };
        auto ep  = [&](std::string& ip, uint16_t& port){ char b[16]; if (o+6 <= n) { sprintf(b,"%u.%u.%u.%u",p[o],p[o+1],p[o+2],p[o+3]); ip=b; o+=4; port=u16(); } };

        // NeoNetsphere.Network.Data.Auth.ServerInfoDto:
        //   [0]bool IsEnabled [1]uint Id [2]ServerType(uint) [3]string Name
        //   [4]u16 Limit [5]u16 Online [6]IPEndPoint [7]u16 GroupId
        int32_t count = i32();
        servers.clear();
        for (int32_t i = 0; i < count && o < n; ++i) {
            ServerInfo s;
            u8();                           // IsEnabled (bool)
            i32();                          // Id (uint)
            uint32_t type = (uint32_t)i32();// Type (ServerType : uint; Game=3, Chat=5)
            u8(); u8();                     // 2 framing bytes (TS emulador) the source DTO doesn't show
            s.name  = str();                // Name
            s.limit = u16();                // PlayerLimit
            s.online= u16();                // PlayerOnline
            ep(s.ip, s.port);               // EndPoint
            u16();                          // GroupId
            DbgLog("[net]   server[%d] type=%u '%s' %u/%u %s:%u", i, type, s.name.c_str(), s.online, s.limit, s.ip.c_str(), s.port);
            if (type == 3) servers.push_back(s);   // Game only; Chat(5) is the paired backend, hidden like the client
        }
        DbgLog("[net] ServerListAck: %d servers", count);
    }
}

void Client::Poll() {
    if (sock == ~(uintptr_t)0) return;

    // Async connect: wait until the socket is writable (connected) or failed.
    if (state == State::Connecting) {
        fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET((SOCKET)sock, &wr); FD_SET((SOCKET)sock, &ex);
        timeval tv = { 0, 0 };
        select(0, nullptr, &wr, &ex, &tv);
        if (FD_ISSET((SOCKET)sock, &ex)) { DbgLog("[net] TCP connect FAILED"); Disconnect(); return; }
        if (!FD_ISSET((SOCKET)sock, &wr)) return;                    // still connecting
        DbgLog("[net] TCP connected -> WaitHint (awaiting server Hint)");
        state = State::WaitHint;   // server pushes NotifyServerConnectionHint on connect
    }

    uint8_t tmp[4096];
    for (;;) {
        int r = ::recv((SOCKET)sock, (char*)tmp, sizeof(tmp), 0);
        if (r > 0) { rx.insert(rx.end(), tmp, tmp + r); continue; }
        if (r == 0) { Disconnect(); return; }        // closed
        break;                                        // WSAEWOULDBLOCK
    }

    // Extract every complete frame.
    size_t off = 0;
    while (off < rx.size()) {
        std::vector<uint8_t> payload; size_t used = 0;
        if (!proud::DecodeFrame(rx.data() + off, rx.size() - off, payload, used))
            break;
        if (!payload.empty())
            OnCore(payload[0], payload.data() + 1, payload.size() - 1);   // op = 1 byte
        off += used;
    }
    if (off) rx.erase(rx.begin(), rx.begin() + off);
}

void Client::SendRmi(const uint8_t* data, size_t len) {
    // App messages ride EncryptedReliable(36){ EncryptMode.Secure, AES(Rmi{data}) }.
    // Inner plaintext core message = [Rmi op=1][data]; encrypt secure + reliable.
    std::vector<uint8_t> inner;
    inner.push_back((uint8_t)ProudCoreOp::Rmi);
    if (data && len) inner.insert(inner.end(), data, data + len);
    std::vector<uint8_t> enc = crypt.Encrypt(inner.data(), inner.size(), true, true);

    std::vector<uint8_t> body;
    body.push_back(1);                               // EncryptMode.Secure
    PushScalar(body, (uint32_t)enc.size());
    body.insert(body.end(), enc.begin(), enc.end());
    SendCore((uint16_t)ProudCoreOp::EncryptedReliable, body.data(), body.size());
}

// An app RMI = [ushort opcode LE][BlubLib fields], sent as an encrypted Rmi.
void Client::SendAppRmi(uint16_t opcode, const uint8_t* fields, size_t n) {
    std::vector<uint8_t> data;
    data.push_back((uint8_t)opcode);
    data.push_back((uint8_t)(opcode >> 8));
    if (fields && n) data.insert(data.end(), fields, fields + n);
    SendRmi(data.data(), data.size());
}

void Client::Disconnect() {
    if (sock != ~(uintptr_t)0) { ::closesocket((SOCKET)sock); sock = ~(uintptr_t)0; }
    state = State::Disconnected;
    rx.clear();
}

}  // namespace proud
