#pragma once

#include <cstddef>
#include <cstdint>

#include "core/audio/Mp3Decoder.h"

namespace awtrix::radio {

// AAC from an ADTS stream (AAC-LC, HE-AAC and HE-AAC v2), decoded by the FDK AAC library one frame
// at a time with the same contract as the MP3 decoder: NeedMoreData consumes nothing until the
// whole frame is in view, NoSync skips to the next header, a broken frame is skipped whole. Output
// is interleaved 16-bit PCM, at most two channels (a multichannel station is mixed down).
class AacDecoder {
 public:
  // An HE-AAC frame is 2048 samples per channel; room for the decoder's own worst case.
  static constexpr int kMaxPcmPerFrame = 2048 * 8;

  AacDecoder();
  ~AacDecoder();
  AacDecoder(const AacDecoder&) = delete;
  AacDecoder& operator=(const AacDecoder&) = delete;

  mp3::DecodeResult decode(const uint8_t* data, std::size_t bytes, int16_t* pcm);
  void reset();

  // What the last frame was: "aac", "he-aac" or "he-aac-v2"; "" before the first.
  const char* format() const { return format_; }
  // The station's bitrate in kbit/s as the decoder measures it; 0 while unknown.
  int bitrateKbps() const { return bitrateKbps_; }

 private:
  void* handle_ = nullptr;
  const char* format_ = "";
  int bitrateKbps_ = 0;
  double averageKbps_ = 0;
};

}
