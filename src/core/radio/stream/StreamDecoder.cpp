#include "core/radio/stream/StreamDecoder.h"

#include <algorithm>
#include <cmath>

#include "core/audio/StreamDecode.h"
#include "core/radio/StreamPolicy.h"
#include "system/MonotonicClock.h"

namespace awtrix::radio {
namespace {

constexpr std::size_t kStaticFrames = 1024;
constexpr uint32_t kStaticRate = 44100;
constexpr int32_t kUnity = 32768;
// Full static stays well below full scale: it is a texture under the dial, not a blast.
constexpr int32_t kStaticPeak = 9000;

int16_t clamp16(int32_t v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); }

}

StreamDecoder::StreamDecoder()
    : mp3_(new mp3::Decoder()),
      pcm_(static_cast<std::size_t>(std::max(mp3::kMaxPcmPerFrame, AacDecoder::kMaxPcmPerFrame))),
      hiss_(kStaticFrames * 2) {}

StreamDecoder::~StreamDecoder() = default;

void StreamDecoder::setOptions(const StreamOptions& options) {
  fadeMs_ = options.fadeMs > 10000 ? 10000 : options.fadeMs;
  setStatic(options.staticLevel);
}

void StreamDecoder::restart(const std::string& contentType) {
  mp3_->reset();
  if (aac_) aac_->reset();
  unsupported_.clear();
  hinted_ = codecFromContentType(contentType, &unsupported_);
  codec_ = hinted_ == Codec::Unsupported ? Codec::Unsupported : Codec::Unknown;
  bytesSeen_ = 0;
  prerolled_ = false;
  decodedAnything_ = false;
  playing_ = false;
  mp3BytesInWindow_ = mp3FramesInWindow_ = 0;
  lastWaitMs_ = gapSinceMs_ = -1;
  if (codec_ == Codec::Unsupported) {
    fault_ = Fault::Codec;
    error_ = unsupported_ + " is not supported";
  } else {
    fault_ = Fault::None;
    error_.clear();
  }
}

const char* StreamDecoder::format() const {
  if (codec_ == Codec::Aac && aac_ && aac_->format()[0]) return aac_->format();
  if (!decodedAnything_) return "";
  return codecName(codec_);
}

// The bytes decide; a header seen in time breaks no tie it does not need to. Once enough bytes
// passed without three frames of either kind, the Content-Type's guess gets its chance.
bool StreamDecoder::findCodec(audio::StreamInputBuffer& input) {
  if (codec_ == Codec::Mp3 || codec_ == Codec::Aac) return true;
  const auto view = input.decoderView();
  std::string name;
  const Codec found = sniff(view.data, view.size, &name);
  if (found == Codec::Unsupported) {
    fault_ = Fault::Codec;
    error_ = name + " is not supported";
    codec_ = Codec::Unsupported;
    return false;
  }
  if (found == Codec::Unknown) {
    if (view.size < input.maxFrameBytes()) return false;
    if (hinted_ != Codec::Mp3 && hinted_ != Codec::Aac) return false;
    codec_ = hinted_;
  } else {
    codec_ = found;
  }
  if (codec_ == Codec::Aac && !aac_) aac_.reset(new AacDecoder());
  return true;
}

bool StreamDecoder::decodeOne(audio::StreamInputBuffer& input, Block& out) {
  bool decoded = false;
  int64_t started = 0;
  const auto decode = [&](const audio::StreamInputBuffer::View& view) {
    started = monotonicUs();
    return codec_ == Codec::Aac ? aac_->decode(view.data, view.size, pcm_.data())
                                : mp3_->decode(view.data, view.size, pcm_.data());
  };
  const auto frame = [&](const mp3::DecodeResult& result) {
    if (result.samples <= 0 || result.channels < 1 || result.channels > 2) return true;
    const uint32_t took = static_cast<uint32_t>(monotonicUs() - started);
    decodeUs_ = decodeUs_ ? (decodeUs_ * 7 + took) / 8 : took;
    decoded = true;
    rate_ = static_cast<uint32_t>(result.sampleRateHz);
    channels_ = static_cast<uint8_t>(result.channels);
    if (codec_ == Codec::Aac) {
      bitrateKbps_ = static_cast<uint32_t>(aac_->bitrateKbps());
    } else {
      // An MP3 frame's own bitrate, averaged over the last frames (VBR stations vary).
      mp3BytesInWindow_ += static_cast<uint32_t>(result.bytesConsumed);
      if (++mp3FramesInWindow_ >= 38) {
        const double seconds = mp3FramesInWindow_ * (rate_ >= 32000 ? 1152.0 : 576.0) / rate_;
        bitrateKbps_ = static_cast<uint32_t>(mp3BytesInWindow_ * 8 / seconds / 1000 + 0.5);
        mp3BytesInWindow_ = mp3FramesInWindow_ = 0;
      }
    }
    out.samples = pcm_.data();
    out.frames = static_cast<std::size_t>(result.samples);
    out.rate = rate_;
    out.channels = channels_;
    out.synthetic = false;
    return false;
  };
  const auto before = input.size();
  audio::decodeFrames(input, 1, decode, frame);
  bytesSeen_ += before - input.size();
  return decoded;
}

StreamDecoder::Step StreamDecoder::next(audio::StreamInputBuffer& input, int64_t nowMs, Block& out) {
  if (codec_ == Codec::Unsupported) return Step::Error;
  if (!prerolled_ && input.size() >= kPrerollBytes) prerolled_ = true;
  bool decoded = false;
  if (prerolled_ && findCodec(input)) decoded = decodeOne(input, out);
  if (codec_ == Codec::Unsupported) return Step::Error;
  if (decoded) {
    if (!decodedAnything_ && staticLevel_ > 0) {
      tailTotal_ = static_cast<uint64_t>(rate_) * kStaticTailMs / 1000;
      tailDone_ = 0;
    }
    if (!faded_) {
      faded_ = true;
      fadeTotal_ = static_cast<uint64_t>(rate_) * fadeMs_ / 1000;
      fadeDone_ = 0;
    }
    decodedAnything_ = playing_ = true;
    lastWaitMs_ = gapSinceMs_ = -1;
    shape(out);
    return Step::Data;
  }
  // Nothing to decode in this many bytes (or a full ring of bytes no codec claims) will not
  // become playable by waiting longer.
  if (prerolled_ && !decodedAnything_ &&
      std::max<std::size_t>(bytesSeen_, input.size()) > kUndecodableAfterBytes) {
    if (codec_ == Codec::Unknown && !unsupported_.empty()) {
      fault_ = Fault::Codec;
      error_ = unsupported_ + " is not supported";
    } else {
      fault_ = Fault::Undecodable;
      error_ = codec_ == Codec::Aac ? "not playable AAC" : codec_ == Codec::Mp3 ? "not playable MP3"
                                                                                  : "not playable";
    }
    codec_ = Codec::Unsupported;
    return Step::Error;
  }
  if (prerolled_ || decodedAnything_) {
    if (lastWaitMs_ >= 0) starvedMs_ += static_cast<uint32_t>(nowMs - lastWaitMs_);
    lastWaitMs_ = nowMs;
    if (gapSinceMs_ < 0) gapSinceMs_ = nowMs;
  }
  // A short gap is the network; a long one is a station that went quiet.
  if (playing_ && gapSinceMs_ >= 0 && nowMs - gapSinceMs_ >= kStarvedMs) playing_ = false;
  // Static fills the wait for the first frame, and a starving station once it is clearly gone.
  if (staticLevel_ > 0 && (!decodedAnything_ || !playing_)) return noise(out);
  return Step::Wait;
}

StreamDecoder::Step StreamDecoder::noise(Block& out) {
  if (staticLevel_ == 0) return Step::Wait;
  const uint8_t channels = channels_ ? channels_ : 2;
  const uint32_t rate = rate_ ? rate_ : kStaticRate;
  const int32_t gain = kUnity * staticLevel_ / 100;
  makeStatic(hiss_.data(), kStaticFrames, channels, gain, false);
  out.samples = hiss_.data();
  out.frames = kStaticFrames;
  out.rate = rate;
  out.channels = channels;
  out.synthetic = true;
  return Step::Data;
}

// The station's first frames: it rises from silence, and the static under it falls away.
void StreamDecoder::shape(Block& block) {
  auto* pcm = const_cast<int16_t*>(block.samples);
  const std::size_t frames = block.frames;
  const uint8_t channels = block.channels;
  if (fadeDone_ < fadeTotal_) {
    for (std::size_t i = 0; i < frames && fadeDone_ < fadeTotal_; ++i, ++fadeDone_) {
      // An equal-power curve sounds even; a straight line sounds late.
      const double x = static_cast<double>(fadeDone_) / static_cast<double>(fadeTotal_);
      const int32_t g = static_cast<int32_t>(std::sin(x * 1.5707963) * kUnity);
      for (uint8_t c = 0; c < channels; ++c) {
        int16_t& s = pcm[i * channels + c];
        s = clamp16(static_cast<int32_t>((static_cast<int64_t>(s) * g) >> 15));
      }
    }
  }
  if (tailDone_ < tailTotal_ && staticLevel_ > 0) {
    const std::size_t n = std::min<std::size_t>(frames, static_cast<std::size_t>(tailTotal_ - tailDone_));
    const double from = static_cast<double>(tailDone_) / static_cast<double>(tailTotal_);
    const double to = static_cast<double>(tailDone_ + n) / static_cast<double>(tailTotal_);
    const int32_t g0 = static_cast<int32_t>(kUnity * staticLevel_ / 100 * (1.0 - from));
    const int32_t g1 = static_cast<int32_t>(kUnity * staticLevel_ / 100 * (1.0 - to));
    // Made in pieces so the gain can slope across the frame.
    std::size_t done = 0;
    while (done < n) {
      const std::size_t piece = std::min<std::size_t>(kStaticFrames, n - done);
      const int32_t g = g0 + static_cast<int32_t>((g1 - g0) * (static_cast<double>(done) / n));
      makeStatic(pcm + done * channels, piece, channels, g, true);
      done += piece;
    }
    tailDone_ += n;
  }
}

// White noise from xorshift32, softened by a one-pole low-pass, with a slow random swell and the
// odd crackle: closer to a dial between stations than to plain hiss.
void StreamDecoder::makeStatic(int16_t* out, std::size_t frames, uint8_t channels, int32_t gainQ15,
                               bool add) {
  for (std::size_t i = 0; i < frames; ++i) {
    ++swell_;
    // A swell of about 0.7 s, between 60 % and 100 %.
    const double phase = static_cast<double>(swell_ % 30870u) / 30870.0;
    const int32_t swell = static_cast<int32_t>(kUnity * (0.8 + 0.2 * std::sin(phase * 6.2831853)));
    for (uint8_t c = 0; c < channels; ++c) {
      seed_ ^= seed_ << 13;
      seed_ ^= seed_ >> 17;
      seed_ ^= seed_ << 5;
      int32_t white = static_cast<int32_t>(seed_ >> 16) - 32768;
      // One sample in about 3000 is a crackle.
      if ((seed_ & 0xFFF) == 0x7) white = (seed_ & 0x1000) ? 32767 : -32768;
      lowpass_[c & 1] += (white - lowpass_[c & 1]) >> 1;
      int64_t v = static_cast<int64_t>(lowpass_[c & 1]) * kStaticPeak / 32768;
      v = (v * swell) >> 15;
      v = (v * gainQ15) >> 15;
      int16_t& s = out[i * channels + c];
      s = add ? clamp16(s + static_cast<int32_t>(v)) : clamp16(static_cast<int32_t>(v));
    }
  }
}

}
