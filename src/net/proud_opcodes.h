// ProudNet core opcodes. Reused verbatim from the emulador (ProudNetSrc
// Constants.cs -> ProudCoreOpCode). Wire = these ids inside the framed payload.
#pragma once
#include <cstdint>

enum class ProudCoreOp : uint16_t {
    Rmi                            = 1,   // app messages (login, etc.)
    UserMessage                    = 2,
    ConnectServerTimedout          = 3,
    NotifyServerConnectionHint     = 4,   // S->C : server cfg + RSA pubkey
    NotifyCSEncryptedSessionKey    = 5,   // C->S : session key (RSA-encrypted)
    NotifyCSSessionKeySuccess      = 6,   // S->C
    NotifyServerConnectionRequestData = 7,// C->S : version + user data
    NotifyProtocolVersionMismatch  = 8,
    NotifyServerDeniedConnection   = 9,
    NotifyServerConnectSuccess     = 10,  // S->C : assigns HostId -> connected
    UnreliablePing                 = 26,
    UnreliablePong                 = 28,
    SpeedHackDetectorPing          = 27,
    ReliableRelay1                 = 20,
    ReliableRelay2                 = 23,
    EncryptedReliable              = 36,
    Encrypted_UnReliable           = 37,
    Compressed                     = 38,
    RequestServerConnectionHint    = 46,  // C->S : first message after TCP connect
    // ... full set in emulador ProudNetSrc/Constants.cs (ProudCoreOpCode).
};
