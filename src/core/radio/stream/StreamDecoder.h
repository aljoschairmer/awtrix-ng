#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/audio/Mp3Decoder.h"
#include "core/audio/StreamInputBuffer.h"
#include "core/radio/CodecSniff.h"
#include "core/radio/RadioStatus.h"
#include "core/radio/stream/AacDecoder.h"

namespace awtrix::radio {

// The part of a station that is the same in the TC002 runtime and in the browser: it turns the
// bytes in the ring into PCM. It waits for the preroll, finds the codec from the bytes (MP3 or
// AAC), decodes a frame per call, fades a new station in, and plays radio static while there is
// nothing to hear yet. The caller owns the ring and its lock and does the network.
class StreamDecoder {
 public:
  struct Block {
    const int16_t* samples = nullptr;
    std::size_t frames = 0;
    uint32_t rate = 0;
    uint8_t channels = 0;
    // Static the decoder made up, not the station: music analysis leaves it out.
    bool synthetic = false;
  };
  enum class Step : uint8_t { Data, Wait, Error };

  StreamDecoder();
  ~StreamDecoder();
  StreamDecoder(const StreamDecoder&) = delete;
  StreamDecoder& operator=(const StreamDecoder&) = delete;

  void setOptions(const StreamOptions& options);
  void setStatic(uint8_t level) { staticLevel_ = level > 100 ? 100 : level; }
  uint8_t staticLevel() const { return staticLevel_; }

  // A new connection to the station: the codec is found again from its bytes. contentType is
  // what it answered with, a hint only. The fade-in plays once per station, not per reconnect.
  void restart(const std::string& contentType);

  // One step: a decoded frame, or static while waiting (if any is wanted), or Wait. Error is for
  // good: the station is in a format the radio cannot play.
  Step next(audio::StreamInputBuffer& input, int64_t nowMs, Block& out);
  // Static alone, for a source with no station.
  Step noise(Block& out);

  Fault fault() const { return fault_; }
  const std::string& error() const { return error_; }
  // True from the first decoded frame of a connection until it starves for kStarvedMs.
  bool playing() const { return playing_; }
  bool prerolled() const { return prerolled_; }
  const char* format() const;
  uint32_t bitrateKbps() const { return bitrateKbps_; }
  uint32_t rate() const { return rate_; }
  uint8_t channels() const { return channels_; }
  uint32_t decodeUs() const { return decodeUs_; }
  uint32_t starvedMs() const { return starvedMs_; }

  static constexpr int64_t kStarvedMs = 300;
  static constexpr uint32_t kStaticTailMs = 250;

 private:
  bool findCodec(audio::StreamInputBuffer& input);
  bool decodeOne(audio::StreamInputBuffer& input, Block& out);
  void shape(Block& block);
  void makeStatic(int16_t* out, std::size_t frames, uint8_t channels, int32_t gainQ15, bool add);

  std::unique_ptr<mp3::Decoder> mp3_;
  std::unique_ptr<AacDecoder> aac_;
  std::vector<int16_t> pcm_;
  std::vector<int16_t> hiss_;
  Codec codec_ = Codec::Unknown;
  Codec hinted_ = Codec::Unknown;
  std::string unsupported_;
  Fault fault_ = Fault::None;
  std::string error_;
  std::size_t bytesSeen_ = 0;
  bool prerolled_ = false;
  bool decodedAnything_ = false;
  bool playing_ = false;
  int64_t lastWaitMs_ = -1;
  int64_t gapSinceMs_ = -1;
  uint32_t starvedMs_ = 0;
  uint32_t decodeUs_ = 0;
  uint32_t rate_ = 0;
  uint8_t channels_ = 0;
  uint32_t bitrateKbps_ = 0;
  uint32_t mp3BytesInWindow_ = 0;
  uint32_t mp3FramesInWindow_ = 0;
  // Fade-in of the station and the static that fades out under its first frames, in frames.
  uint32_t fadeMs_ = 0;
  bool faded_ = false;
  uint64_t fadeTotal_ = 0;
  uint64_t fadeDone_ = 0;
  uint64_t tailTotal_ = 0;
  uint64_t tailDone_ = 0;
  uint8_t staticLevel_ = 0;
  // The static's own state: a seeded noise generator, a gentle low-pass and a slow swell.
  uint32_t seed_ = 0x9E3779B9u;
  int32_t lowpass_[2] = {0, 0};
  uint32_t swell_ = 0;
};

}
