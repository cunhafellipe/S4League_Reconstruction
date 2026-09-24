// ProudNet wire framing. Reused from the emulador codecs:
//   ProudFrameEncoder / ProudFrameDecoder.
// Frame = [magic:2 LE = 0x5713][scalarPrefix:1 (1|2|4)][len: prefix bytes][payload]
#pragma once
#include <cstdint>
#include <vector>

namespace proud {

constexpr uint16_t kNetMagic = 0x5713;   // Constants.NetMagic

// Prepend magic + scalar length to a payload -> full frame bytes.
std::vector<uint8_t> EncodeFrame(const uint8_t* payload, size_t len);

// Try to extract one payload from a receive buffer.
// Returns true + fills `out` and `consumed` (bytes of the whole frame) when a
// complete frame is present; false if more bytes are needed.
bool DecodeFrame(const uint8_t* buf, size_t avail,
                 std::vector<uint8_t>& out, size_t& consumed);

}  // namespace proud
