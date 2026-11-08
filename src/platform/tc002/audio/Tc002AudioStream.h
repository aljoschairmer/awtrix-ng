#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/radio/RadioStatus.h"
#include "core/radio/StreamPolicy.h"
#include "core/radio/stream/StreamDecoder.h"
#include "platform/tc002/audio/Tc002AudioSources.h"

namespace awtrix {
namespace tc002 {

// An internet radio station. A detached network thread fills a compressed ring (it may sit in a
// connect or DNS lookup for seconds, so dropping the source never waits for it); the audio
// thread decodes from the ring on demand (radio::StreamDecoder: MP3 or AAC, the fade-in, the
// static). Reconnects with the ESP32's back-off. An empty url is a source of static alone.
class StreamSource final : public PcmSource {
 public:
  static constexpr std::size_t kRingBytes = radio::kInputBufferBytes;

  explicit StreamSource(const std::string& url, const radio::StreamOptions& options = {});
  ~StreamSource() override;
  StreamSource(const StreamSource&) = delete;
  StreamSource& operator=(const StreamSource&) = delete;

  Read next(const int16_t*& samples, std::size_t& frames) override;
  uint32_t rate() const override { return rate_; }
  uint8_t channels() const override { return channels_; }
  bool endless() const override { return true; }

  bool takeTitle(std::string& out);
  bool takeError(std::string& out);
  // The status when it changed in more than its buffer level since the last call.
  bool takeStatus(radio::Status& out);
  bool connected() const;
  // The radio static under the wait, 0..100; also for a station already playing.
  void setStatic(uint8_t level);
  // Whether the last block was static the decoder made up.
  bool synthetic() const { return synthetic_; }
  uint32_t bufferBytes() const { return buffered_; }
  uint32_t starvedMs() const { return decoder_.starvedMs(); }
  uint32_t decodeUs() const { return decoder_.decodeUs(); }

  // Network threads outlive their source until their blocking call returns; shutdown waits here.
  static bool waitForNetworkThreads(int timeoutMs);

  struct Shared;

 private:
  void updateStatus(radio::StreamDecoder::Step step, int64_t nowMs);

  std::shared_ptr<Shared> shared_;
  radio::StreamDecoder decoder_;
  bool staticOnly_ = false;
  uint32_t seenConnection_ = 0;
  uint32_t buffered_ = 0;
  bool fatal_ = false;
  bool synthetic_ = false;
  uint32_t rate_ = 0;
  uint8_t channels_ = 0;
};

}
}
