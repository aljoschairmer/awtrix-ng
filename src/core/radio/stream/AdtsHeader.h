#pragma once

#include <cstddef>
#include <cstdint>

namespace awtrix::radio {

// The fixed part of an ADTS frame header (ISO/IEC 13818-7, 6.2): what is needed to find a frame,
// size it and tell its format before the decoder sees a byte.
struct AdtsHeader {
  int profile = 0;          // 0 Main, 1 LC, 2 SSR, 3 LTP (the audio object type minus 1)
  int sampleRateHz = 0;     // of the core: an HE-AAC station plays at twice this
  int channelConfig = 0;    // 0 means a program config element names the channels
  std::size_t frameBytes = 0;
  std::size_t headerBytes = 0;
};

inline constexpr std::size_t kAdtsHeaderBytes = 7;
inline constexpr std::size_t kAdtsMaxFrameBytes = 8191;

inline int adtsSampleRate(int index) {
  static constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000,  7350};
  return index >= 0 && index < 13 ? kRates[index] : 0;
}

// True when data starts with a plausible ADTS header (at least kAdtsHeaderBytes of it).
inline bool parseAdts(const uint8_t* data, std::size_t size, AdtsHeader& out) {
  if (size < kAdtsHeaderBytes || data[0] != 0xFF || (data[1] & 0xF6) != 0xF0) return false;
  const int rate = adtsSampleRate((data[2] >> 2) & 0x0F);
  if (!rate) return false;
  const std::size_t length = (static_cast<std::size_t>(data[3] & 0x03) << 11) |
                             (static_cast<std::size_t>(data[4]) << 3) | (data[5] >> 5);
  const std::size_t header = (data[1] & 0x01) ? 7 : 9;
  if (length <= header) return false;
  out.profile = data[2] >> 6;
  out.sampleRateHz = rate;
  out.channelConfig = ((data[2] & 0x01) << 2) | (data[3] >> 6);
  out.frameBytes = length;
  out.headerBytes = header;
  return true;
}

}
