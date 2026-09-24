#include "net/proud_frame.h"

namespace proud {

// WriteScalar: smallest prefix (1/2/4) that holds the length. Mirrors the
// emulador's GetPossibleScalarlength / WriteScalar.
static void WriteScalar(std::vector<uint8_t>& b, uint32_t v) {
    if (v < 128) {                     // emulador: value < 128 -> prefix 1
        b.push_back(1);
        b.push_back((uint8_t)v);
    } else if (v < 32768) {            // value < 32768 -> prefix 2
        b.push_back(2);
        b.push_back((uint8_t)(v));
        b.push_back((uint8_t)(v >> 8));
    } else {
        b.push_back(4);
        b.push_back((uint8_t)(v));
        b.push_back((uint8_t)(v >> 8));
        b.push_back((uint8_t)(v >> 16));
        b.push_back((uint8_t)(v >> 24));
    }
}

std::vector<uint8_t> EncodeFrame(const uint8_t* payload, size_t len) {
    std::vector<uint8_t> f;
    f.reserve(2 + 5 + len);
    f.push_back((uint8_t)(kNetMagic & 0xff));
    f.push_back((uint8_t)(kNetMagic >> 8));
    WriteScalar(f, (uint32_t)len);
    f.insert(f.end(), payload, payload + len);
    return f;
}

bool DecodeFrame(const uint8_t* buf, size_t avail,
                 std::vector<uint8_t>& out, size_t& consumed) {
    if (avail < 3) return false;                      // magic(2) + prefix(1)
    if (buf[0] != (kNetMagic & 0xff) || buf[1] != (kNetMagic >> 8))
        return false;                                 // bad magic (caller resyncs)
    uint8_t prefix = buf[2];
    if (prefix != 1 && prefix != 2 && prefix != 4) return false;
    if (avail < 3u + prefix) return false;

    uint32_t len = 0;
    for (uint8_t i = 0; i < prefix; ++i) len |= (uint32_t)buf[3 + i] << (8 * i);

    size_t header = 3u + prefix;
    if (avail < header + len) return false;           // wait for more
    out.assign(buf + header, buf + header + len);
    consumed = header + len;
    return true;
}

}  // namespace proud

#ifdef PROUD_FRAME_SELFTEST
#include <cassert>
// Build check: encode->decode round-trips, and short buffers wait.
int main() {
    for (size_t n : {0u, 5u, 255u, 256u, 70000u}) {
        std::vector<uint8_t> pl(n, 0xAB);
        auto f = proud::EncodeFrame(pl.data(), pl.size());
        std::vector<uint8_t> out; size_t used = 0;
        assert(proud::DecodeFrame(f.data(), f.size(), out, used));
        assert(out == pl && used == f.size());
        if (f.size() > 1) { std::vector<uint8_t> o2; size_t u2;
            assert(!proud::DecodeFrame(f.data(), f.size() - 1, o2, u2)); }
    }
    return 0;
}
#endif
