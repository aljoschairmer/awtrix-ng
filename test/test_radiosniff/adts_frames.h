#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A station that sends AAC: ADTS frames (LC, 44.1 kHz, stereo, no CRC) of the given length with
// a silent payload. Enough for the radio to recognise AAC and refuse it; nothing decodes it.
inline std::vector<uint8_t> adtsFrames(std::size_t count, std::size_t frameBytes = 371) {
  std::vector<uint8_t> out;
  out.reserve(count * frameBytes);
  for (std::size_t i = 0; i < count; ++i) {
    const uint8_t header[7] = {
        0xFF, 0xF1, static_cast<uint8_t>((1 << 6) | (4 << 2)),
        static_cast<uint8_t>((2 << 6) | ((frameBytes >> 11) & 0x03)),
        static_cast<uint8_t>((frameBytes >> 3) & 0xFF),
        static_cast<uint8_t>(((frameBytes & 0x07) << 5) | 0x1F), 0xFC};
    out.insert(out.end(), header, header + 7);
    out.insert(out.end(), frameBytes - 7, 0x00);
  }
  return out;
}
