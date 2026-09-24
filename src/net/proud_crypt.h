// ProudNet crypto - ported 1:1 from emulador Crypt.cs.
//   Secure = AES (Rijndael-128, ECB, no padding). Fast = RC4. Default key below.
//   Encrypted blob = CIPHER( [padding:1][crc32:4 LE][counter:2 LE (reliable)]
//                            [payload][fill:padding] ), block-aligned.
//   `reliable` adds the 2-byte anti-replay counter (the "Invalid decrypt counter").
#pragma once
#include <cstdint>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

struct ProudCrypt {
    ProudCrypt();                 // default key ({0x0B,0,...}) for both AES + RC4
    ~ProudCrypt();

    void SetSecureKey(const uint8_t* key, size_t len);   // session AES key
    void SetFastKey(const uint8_t* key, size_t len);     // session RC4 key

    // Raw AES-ECB block encrypt with the current secure key (used to wrap the
    // FastKey during the session-key handshake: server does AES.Decrypt(FastKey)).
    std::vector<uint8_t> AesEncryptRaw(const uint8_t* in, size_t n) { return AesEcb(in, n, true); }

    // secure=true -> AES, else RC4. reliable=true -> counter checked.
    std::vector<uint8_t> Encrypt(const uint8_t* payload, size_t n, bool secure, bool reliable);
    bool Decrypt(const uint8_t* in, size_t n, std::vector<uint8_t>& out, bool secure, bool reliable);

private:
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<uint8_t> rc4Key;
    uint16_t encCounter = 0;
    uint16_t decCounter = 0;

    std::vector<uint8_t> AesEcb(const uint8_t* in, size_t n, bool encrypt);
};

// --- session-key handshake helpers (proud namespace) --------------------------
namespace proud {
// Fill n bytes from the system RNG (BCryptGenRandom).
void RandBytes(uint8_t* out, size_t n);

// RSA-encrypt `pt` (the AES session key) with the server's public key given as a
// DER-encoded RSAPublicKey (SEQUENCE{INTEGER modulus, INTEGER exponent}), using
// OAEP-SHA1 padding. Matches the emulador's RSACryptoServiceProvider.Decrypt(x, true).
// `out` receives the ciphertext (modulus-sized). Returns false on parse/crypto error.
bool RsaOaepEncrypt(const uint8_t* der, size_t derLen,
                    const uint8_t* pt, size_t ptLen, std::vector<uint8_t>& out);
}  // namespace proud
