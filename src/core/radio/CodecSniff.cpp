#include "core/radio/CodecSniff.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/radio/stream/AdtsHeader.h"

namespace awtrix::radio {
namespace {

std::string lower(const std::string& text) {
  std::string out = text;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

Codec unsupported(std::string* name, const char* what) {
  if (name) *name = what;
  return Codec::Unsupported;
}

// The length of an MPEG audio frame (any layer III or II rate) whose header starts at p; 0 when
// p does not start one.
std::size_t mpegFrameBytes(const uint8_t* p) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
  const int version = (p[1] >> 3) & 0x03;  // 0 MPEG 2.5, 2 MPEG 2, 3 MPEG 1
  const int layer = (p[1] >> 1) & 0x03;    // 1 layer III, 2 layer II
  const int bitrateIndex = p[2] >> 4;
  const int rateIndex = (p[2] >> 2) & 0x03;
  if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return 0;
  static constexpr int kBitrateV1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static constexpr int kBitrateV2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  static constexpr int kRates[] = {44100, 48000, 32000};
  int rate = kRates[rateIndex];
  if (version == 2) rate /= 2;
  if (version == 0) rate /= 4;
  const int kbps = version == 3 ? kBitrateV1[bitrateIndex] : kBitrateV2[bitrateIndex];
  const int padding = (p[2] >> 1) & 0x01;
  const int coefficient = version == 3 ? 144 : 72;
  return static_cast<std::size_t>(coefficient * kbps * 1000 / rate + padding);
}

std::size_t adtsFrameBytes(const uint8_t* p, std::size_t room) {
  AdtsHeader header;
  return parseAdts(p, room, header) && header.profile == 1 ? header.frameBytes : 0;
}

// Three frames in a row of one kind, starting at offset.
template <typename Length>
bool chain(const uint8_t* data, std::size_t size, std::size_t offset, Length&& length) {
  std::size_t at = offset;
  for (int i = 0; i < 3; ++i) {
    if (at + kAdtsHeaderBytes > size) return false;
    const std::size_t bytes = length(data + at, size - at);
    if (!bytes) return false;
    at += bytes;
  }
  return true;
}

}

Codec codecFromContentType(const std::string& type, std::string* name) {
  const std::string t = lower(type);
  if (has(t, "mpegurl") || has(t, "vnd.apple")) return unsupported(name, "hls");
  if (has(t, "audio/aac") || has(t, "audio/aacp") || has(t, "audio/x-aac")) return Codec::Aac;
  if (has(t, "audio/mpeg") || has(t, "audio/mp3") || has(t, "audio/x-mp3")) return Codec::Mp3;
  if (has(t, "opus")) return unsupported(name, "opus");
  if (has(t, "ogg")) return unsupported(name, "ogg");
  if (has(t, "flac")) return unsupported(name, "flac");
  if (has(t, "mp4a-latm")) return unsupported(name, "latm");
  if (has(t, "audio/mp4") || has(t, "video/")) return unsupported(name, "mp4");
  return Codec::Unknown;
}

Codec sniff(const uint8_t* data, std::size_t size, std::string* name) {
  if (!data || size < 4) return Codec::Unknown;
  if (std::memcmp(data, "OggS", 4) == 0) return unsupported(name, "ogg");
  if (std::memcmp(data, "fLaC", 4) == 0) return unsupported(name, "flac");
  if (size >= 7 && std::memcmp(data, "#EXTM3U", 7) == 0) return unsupported(name, "hls");
  std::size_t start = 0;
  if (size >= 10 && std::memcmp(data, "ID3", 3) == 0) {
    start = 10 + ((static_cast<std::size_t>(data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) |
                  ((data[8] & 0x7F) << 7) | (data[9] & 0x7F));
    if (start >= size) return Codec::Unknown;
  }
  // A stream joined in the middle starts anywhere: try every sync until one chains.
  for (std::size_t at = start; at + 4 <= size; ++at) {
    if (data[at] != 0xFF) continue;
    if (chain(data, size, at, [](const uint8_t* p, std::size_t room) { return adtsFrameBytes(p, room); }))
      return Codec::Aac;
    if (chain(data, size, at, [](const uint8_t* p, std::size_t) { return mpegFrameBytes(p); }))
      return Codec::Mp3;
  }
  // LATM/LOAS (sync 0x56E) cannot be played; only call it so after many bytes of nothing else.
  if (size >= 8192)
    for (std::size_t at = 0; at + 2 < size; ++at)
      if (data[at] == 0x56 && (data[at + 1] & 0xE0) == 0xE0) return unsupported(name, "latm");
  return Codec::Unknown;
}

const char* codecName(Codec codec) {
  switch (codec) {
    case Codec::Mp3: return "mp3";
    case Codec::Aac: return "aac";
    case Codec::Unsupported: return "unsupported";
    case Codec::Unknown: break;
  }
  return "";
}

}
