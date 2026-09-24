// ProudNet crypto implementation. AES via Windows BCrypt (ECB), RC4 + CRC32 by hand.
#include "net/proud_crypt.h"
#pragma comment(lib, "bcrypt.lib")

// Default key from emulador Crypt.cs S_defaultKey (16 bytes).
static const uint8_t kDefaultKey[16] =
    { 0x0B, 0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };

// --- CRC32 (reflected, poly 0xEDB88320). TODO verify vs BlubLib CRC32. --------
static uint32_t Crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

// --- RC4 (fresh state per message, keyed from rc4Key) -------------------------
static std::vector<uint8_t> Rc4(const std::vector<uint8_t>& key,
                                const uint8_t* in, size_t n) {
    uint8_t s[256];
    for (int i = 0; i < 256; ++i) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + s[i] + key[i % key.size()]) & 0xff;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    std::vector<uint8_t> out(n);
    int i = 0, j = 0;
    for (size_t k = 0; k < n; ++k) {
        i = (i + 1) & 0xff; j = (j + s[i]) & 0xff;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        out[k] = in[k] ^ s[(s[i] + s[j]) & 0xff];
    }
    return out;
}

ProudCrypt::ProudCrypt() {
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    SetSecureKey(kDefaultKey, sizeof(kDefaultKey));
    rc4Key.assign(kDefaultKey, kDefaultKey + sizeof(kDefaultKey));
}

ProudCrypt::~ProudCrypt() {
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
}

void ProudCrypt::SetSecureKey(const uint8_t* key, size_t len) {
    if (hKey) { BCryptDestroyKey(hKey); hKey = nullptr; }
    BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, (PUCHAR)key, (ULONG)len, 0);
    encCounter = decCounter = 0;   // new session key => reset reliable anti-replay counters
}

void ProudCrypt::SetFastKey(const uint8_t* key, size_t len) {
    rc4Key.assign(key, key + len);
}

std::vector<uint8_t> ProudCrypt::AesEcb(const uint8_t* in, size_t n, bool encrypt) {
    std::vector<uint8_t> out(n);
    ULONG res = 0;
    if (encrypt)
        BCryptEncrypt(hKey, (PUCHAR)in, (ULONG)n, nullptr, nullptr, 0,
                      out.data(), (ULONG)n, &res, 0);
    else
        BCryptDecrypt(hKey, (PUCHAR)in, (ULONG)n, nullptr, nullptr, 0,
                      out.data(), (ULONG)n, &res, 0);
    out.resize(res);
    return out;
}

std::vector<uint8_t> ProudCrypt::Encrypt(const uint8_t* payload, size_t n,
                                         bool secure, bool reliable) {
    const size_t block = 16;
    size_t extra = 1 + 4 + (reliable ? 2 : 0);
    size_t padding = block - ((n + extra) % block);   // [1..block]

    // data = [counter:2 (reliable)][payload]
    std::vector<uint8_t> data;
    if (reliable) {
        uint16_t c = encCounter++;
        data.push_back((uint8_t)c); data.push_back((uint8_t)(c >> 8));
    }
    data.insert(data.end(), payload, payload + n);
    uint32_t crc = Crc32(data.data(), data.size());

    // plain = [padding:1][crc:4 LE][data][fill:padding]
    std::vector<uint8_t> plain;
    plain.push_back((uint8_t)padding);
    plain.push_back((uint8_t)crc); plain.push_back((uint8_t)(crc >> 8));
    plain.push_back((uint8_t)(crc >> 16)); plain.push_back((uint8_t)(crc >> 24));
    plain.insert(plain.end(), data.begin(), data.end());
    plain.insert(plain.end(), padding, 0);

    return secure ? AesEcb(plain.data(), plain.size(), true)
                  : Rc4(rc4Key, plain.data(), plain.size());
}

bool ProudCrypt::Decrypt(const uint8_t* in, size_t n, std::vector<uint8_t>& out,
                         bool secure, bool reliable) {
    std::vector<uint8_t> plain = secure ? AesEcb(in, n, false)
                                        : Rc4(rc4Key, in, n);
    if (plain.size() < 5) return false;
    uint8_t padding = plain[0];
    uint32_t checksum = plain[1] | (plain[2] << 8) | (plain[3] << 16) | ((uint32_t)plain[4] << 24);
    if (5 + (size_t)padding > plain.size()) return false;
    size_t dataLen = plain.size() - 5 - padding;
    const uint8_t* data = plain.data() + 5;

    if (reliable) {
        if (dataLen < 2) return false;
        uint16_t msgCtr = (uint16_t)(data[0] | (data[1] << 8));
        uint16_t expected = decCounter++;
        if (msgCtr != expected) return false;   // Invalid decrypt counter
    }
    if (Crc32(data, dataLen) != checksum) return false;

    size_t skip = reliable ? 2 : 0;
    out.assign(data + skip, data + dataLen);
    return true;
}

// ============================ session-key handshake ==========================
namespace proud {

void RandBytes(uint8_t* out, size_t n) {
    BCryptGenRandom(nullptr, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

// DER length: short form (<0x80) or long form (0x81/0x82...). Advances p.
static bool DerLen(const uint8_t* b, size_t n, size_t& p, size_t& len) {
    if (p >= n) return false;
    uint8_t x = b[p++];
    if (x < 0x80) { len = x; return true; }
    int nb = x & 0x7f;
    if (nb == 0 || nb > 4 || p + nb > n) return false;
    len = 0;
    for (int i = 0; i < nb; ++i) len = (len << 8) | b[p++];
    return true;
}

// Find SEQUENCE{ INTEGER modulus, INTEGER exponent } anywhere in `b`; return the
// big-endian modulus/exponent spans with any leading 0x00 sign byte stripped.
static bool ParseRsaPubDer(const uint8_t* b, size_t n,
                           const uint8_t*& mod, size_t& modLen,
                           const uint8_t*& exp, size_t& expLen) {
    for (size_t i = 0; i + 1 < n; ++i) {
        if (b[i] != 0x30) continue;                       // SEQUENCE
        size_t p = i + 1, seqLen;
        if (!DerLen(b, n, p, seqLen)) continue;
        size_t seqEnd = p + seqLen;
        if (seqEnd > n) continue;
        if (p >= seqEnd || b[p] != 0x02) continue;        // INTEGER modulus
        p++; size_t mLen; if (!DerLen(b, n, p, mLen)) continue;
        const uint8_t* m = b + p; p += mLen; if (p > seqEnd) continue;
        if (p >= seqEnd || b[p] != 0x02) continue;        // INTEGER exponent
        p++; size_t eLen; if (!DerLen(b, n, p, eLen)) continue;
        const uint8_t* e = b + p; p += eLen; if (p != seqEnd) continue;
        while (mLen > 1 && m[0] == 0) { ++m; --mLen; }
        while (eLen > 1 && e[0] == 0) { ++e; --eLen; }
        mod = m; modLen = mLen; exp = e; expLen = eLen;
        return true;
    }
    return false;
}

bool RsaOaepEncrypt(const uint8_t* der, size_t derLen,
                    const uint8_t* pt, size_t ptLen, std::vector<uint8_t>& out) {
    const uint8_t *mod, *exp; size_t modLen, expLen;
    if (!ParseRsaPubDer(der, derLen, mod, modLen, exp, expLen)) return false;

    // BCRYPT_RSAKEY_BLOB header + big-endian exponent + big-endian modulus.
    std::vector<uint8_t> blob(sizeof(BCRYPT_RSAKEY_BLOB) + expLen + modLen);
    auto* h = (BCRYPT_RSAKEY_BLOB*)blob.data();
    h->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    h->BitLength = (ULONG)(modLen * 8);
    h->cbPublicExp = (ULONG)expLen;
    h->cbModulus = (ULONG)modLen;
    h->cbPrime1 = 0; h->cbPrime2 = 0;
    memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), exp, expLen);
    memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + expLen, mod, modLen);

    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) == 0 &&
        BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                            blob.data(), (ULONG)blob.size(), 0) == 0) {
        BCRYPT_OAEP_PADDING_INFO pad{ BCRYPT_SHA1_ALGORITHM, nullptr, 0 };
        ULONG outLen = 0;
        if (BCryptEncrypt(key, (PUCHAR)pt, (ULONG)ptLen, &pad, nullptr, 0,
                          nullptr, 0, &outLen, BCRYPT_PAD_OAEP) == 0) {
            out.resize(outLen);
            ok = BCryptEncrypt(key, (PUCHAR)pt, (ULONG)ptLen, &pad, nullptr, 0,
                               out.data(), outLen, &outLen, BCRYPT_PAD_OAEP) == 0;
            out.resize(outLen);
        }
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace proud

#ifdef PROUD_CRYPT_SELFTEST
#include <cassert>

// RSA round-trip: build a DER pubkey from a generated keypair, OAEP-encrypt the
// AES session key with RsaOaepEncrypt, decrypt with the private key -> match.
static void RsaSelfTest() {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    assert(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) == 0);
    assert(BCryptGenerateKeyPair(alg, &key, 1024, 0) == 0);
    assert(BCryptFinalizeKeyPair(key, 0) == 0);

    ULONG blen = 0;
    BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &blen, 0);
    std::vector<uint8_t> blob(blen);
    assert(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), blen, &blen, 0) == 0);
    auto* h = (BCRYPT_RSAKEY_BLOB*)blob.data();
    const uint8_t* exp = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    const uint8_t* mod = exp + h->cbPublicExp;

    // Build DER SEQUENCE{ INTEGER modulus(+0x00 sign), INTEGER exponent }.
    auto derInt = [](const uint8_t* v, size_t n) {
        std::vector<uint8_t> t{0x02};
        bool sign = n > 0 && (v[0] & 0x80);
        size_t len = n + (sign ? 1 : 0);
        if (len < 0x80) t.push_back((uint8_t)len);
        else { t.push_back(0x81); t.push_back((uint8_t)len); }
        if (sign) t.push_back(0x00);
        t.insert(t.end(), v, v + n);
        return t;
    };
    auto mI = derInt(mod, h->cbModulus), eI = derInt(exp, h->cbPublicExp);
    std::vector<uint8_t> seq; seq.insert(seq.end(), mI.begin(), mI.end());
    seq.insert(seq.end(), eI.begin(), eI.end());
    std::vector<uint8_t> der{0x30};
    if (seq.size() < 0x80) der.push_back((uint8_t)seq.size());
    else { der.push_back(0x81); der.push_back((uint8_t)seq.size()); }
    der.insert(der.end(), seq.begin(), seq.end());

    uint8_t aesKey[16]; proud::RandBytes(aesKey, 16);
    std::vector<uint8_t> ct;
    assert(proud::RsaOaepEncrypt(der.data(), der.size(), aesKey, 16, ct));
    assert(ct.size() == h->cbModulus);

    BCRYPT_OAEP_PADDING_INFO pad{ BCRYPT_SHA1_ALGORITHM, nullptr, 0 };
    std::vector<uint8_t> pt(256); ULONG ptLen = 0;
    assert(BCryptDecrypt(key, ct.data(), (ULONG)ct.size(), &pad, nullptr, 0,
                         pt.data(), (ULONG)pt.size(), &ptLen, BCRYPT_PAD_OAEP) == 0);
    assert(ptLen == 16 && memcmp(pt.data(), aesKey, 16) == 0);
    BCryptDestroyKey(key); BCryptCloseAlgorithmProvider(alg, 0);
}

int main() {
    RsaSelfTest();
    for (bool secure : {true, false})
      for (bool rel : {true, false}) {
        ProudCrypt a, b;   // fresh counters each side
        for (int m = 0; m < 3; ++m) {
            std::vector<uint8_t> msg;
            for (int i = 0; i < 20 + m; ++i) msg.push_back((uint8_t)(i * 7 + m));
            auto enc = a.Encrypt(msg.data(), msg.size(), secure, rel);
            std::vector<uint8_t> dec;
            assert(b.Decrypt(enc.data(), enc.size(), dec, secure, rel));
            assert(dec == msg);
        }
    }
    return 0;
}
#endif
