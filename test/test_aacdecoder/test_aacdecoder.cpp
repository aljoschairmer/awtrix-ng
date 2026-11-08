#include <unity.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/audio/StreamDecode.h"
#include "core/audio/StreamInputBuffer.h"
#include "core/radio/stream/AacDecoder.h"
#include "core/radio/stream/AdtsHeader.h"
#include "vectors.h"

using namespace awtrix;
using aacvectors::kAacVectors;

namespace {

struct Decoded {
  std::vector<int16_t> pcm;
  int rate = 0;
  int channels = 0;
  int frames = 0;
};

// Through the ring the radio uses, in pieces of chunk bytes, so frames cross its end.
Decoded decodeAll(const uint8_t* data, std::size_t size, std::size_t chunk) {
  radio::AacDecoder decoder;
  audio::StreamInputBuffer input(16 * 1024, audio::StreamInputBuffer::kMaxAdtsFrameBytes);
  std::vector<int16_t> pcm(radio::AacDecoder::kMaxPcmPerFrame);
  Decoded out;
  std::size_t at = 0;
  while (at < size || input.size()) {
    if (at < size) {
      const std::size_t n = std::min(chunk, std::min(size - at, input.room()));
      input.push(data + at, n);
      at += n;
    }
    const auto decode = [&](const audio::StreamInputBuffer::View& v) { return decoder.decode(v.data, v.size, pcm.data()); };
    const auto frame = [&](const mp3::DecodeResult& r) {
      if (r.samples <= 0) return true;
      out.rate = r.sampleRateHz;
      out.channels = r.channels;
      ++out.frames;
      out.pcm.insert(out.pcm.end(), pcm.begin(), pcm.begin() + r.samples * r.channels);
      return true;
    };
    const bool consumed = audio::decodeFrames(input, 64, decode, frame);
    if (!consumed && at >= size) break;
  }
  return out;
}

// The best signal-to-error ratio over the alignments a decoder delay allows, in dB.
double bestSnr(const std::vector<int16_t>& got, const int16_t* ref, int frames, int channels) {
  double best = -1e9;
  for (int offset = 0; offset < 4096; ++offset) {
    const std::size_t start = static_cast<std::size_t>(aacvectors::kSkipFrames - 2048 + offset) * channels;
    if (start + static_cast<std::size_t>(frames) * channels > got.size()) break;
    double signal = 0, error = 0;
    for (int i = 0; i < frames * channels; ++i) {
      const double r = ref[i];
      const double e = got[start + i] - r;
      signal += r * r;
      error += e * e;
    }
    const double snr = 10 * std::log10((signal + 1) / (error + 1));
    if (snr > best) best = snr;
  }
  return best;
}

void test_parses_an_adts_header() {
  const auto& v = kAacVectors[0];
  radio::AdtsHeader header;
  TEST_ASSERT_TRUE(radio::parseAdts(v.aac, v.aacBytes, header));
  TEST_ASSERT_EQUAL_INT(1, header.profile);
  TEST_ASSERT_EQUAL_INT(44100, header.sampleRateHz);
  TEST_ASSERT_EQUAL_INT(2, header.channelConfig);
  TEST_ASSERT_TRUE(header.frameBytes > 7 && header.frameBytes < 2000);
  const uint8_t mp3[] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0};
  TEST_ASSERT_FALSE(radio::parseAdts(mp3, sizeof mp3, header));
}

void test_decodes_every_vector_close_to_the_reference() {
  for (const auto& v : kAacVectors) {
    for (std::size_t chunk : {std::size_t(97), std::size_t(4096)}) {
      const Decoded d = decodeAll(v.aac, v.aacBytes, chunk);
      TEST_ASSERT_EQUAL_INT_MESSAGE(v.rate, d.rate, v.name);
      TEST_ASSERT_EQUAL_INT_MESSAGE(v.channels, d.channels, v.name);
      TEST_ASSERT_TRUE_MESSAGE(d.frames > 5, v.name);
      const double snr = bestSnr(d.pcm, v.pcm, v.frames, v.channels);
      char message[96];
      snprintf(message, sizeof message, "%s: %.1f dB", v.name, snr);
      TEST_ASSERT_TRUE_MESSAGE(snr > 40.0, message);
    }
  }
}

void test_names_the_format_and_bitrate() {
  radio::AacDecoder decoder;
  std::vector<int16_t> pcm(radio::AacDecoder::kMaxPcmPerFrame);
  const auto& v = kAacVectors[0];
  std::size_t at = 0;
  for (int i = 0; i < 20 && at < v.aacBytes; ++i) {
    const auto r = decoder.decode(v.aac + at, v.aacBytes - at, pcm.data());
    TEST_ASSERT_TRUE(r.bytesConsumed > 0);
    at += r.bytesConsumed;
  }
  TEST_ASSERT_EQUAL_STRING("aac", decoder.format());
  TEST_ASSERT_TRUE(decoder.bitrateKbps() > 30 && decoder.bitrateKbps() < 200);
}

void test_skips_bytes_before_the_first_header() {
  const auto& v = kAacVectors[1];
  std::vector<uint8_t> junk(333, 0x42);
  junk.insert(junk.end(), v.aac, v.aac + v.aacBytes);
  const Decoded d = decodeAll(junk.data(), junk.size(), 512);
  TEST_ASSERT_EQUAL_INT(v.channels, d.channels);
  TEST_ASSERT_TRUE(bestSnr(d.pcm, v.pcm, v.frames, v.channels) > 40.0);
}

void test_waits_for_a_whole_frame() {
  radio::AacDecoder decoder;
  std::vector<int16_t> pcm(radio::AacDecoder::kMaxPcmPerFrame);
  const auto& v = kAacVectors[0];
  radio::AdtsHeader header;
  TEST_ASSERT_TRUE(radio::parseAdts(v.aac, v.aacBytes, header));
  const auto r = decoder.decode(v.aac, header.frameBytes - 1, pcm.data());
  TEST_ASSERT_EQUAL(mp3::DecodeStatus::NeedMoreData, r.status);
  TEST_ASSERT_EQUAL_size_t(0, r.bytesConsumed);
}

void test_a_broken_frame_is_skipped_whole() {
  const auto& v = kAacVectors[0];
  std::vector<uint8_t> bytes(v.aac, v.aac + v.aacBytes);
  radio::AdtsHeader header;
  TEST_ASSERT_TRUE(radio::parseAdts(bytes.data(), bytes.size(), header));
  // Scramble the payload of the third frame; the header stays, so it is skipped and no more.
  std::size_t at = 0;
  for (int i = 0; i < 2; ++i) {
    radio::parseAdts(bytes.data() + at, bytes.size() - at, header);
    at += header.frameBytes;
  }
  radio::parseAdts(bytes.data() + at, bytes.size() - at, header);
  for (std::size_t i = at + header.headerBytes; i < at + header.frameBytes; ++i) bytes[i] ^= 0x5A;
  const Decoded d = decodeAll(bytes.data(), bytes.size(), 4096);
  TEST_ASSERT_TRUE(d.frames > 10);
  TEST_ASSERT_EQUAL_INT(v.channels, d.channels);
}

}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parses_an_adts_header);
  RUN_TEST(test_decodes_every_vector_close_to_the_reference);
  RUN_TEST(test_names_the_format_and_bitrate);
  RUN_TEST(test_skips_bytes_before_the_first_header);
  RUN_TEST(test_waits_for_a_whole_frame);
  RUN_TEST(test_a_broken_frame_is_skipped_whole);
  return UNITY_END();
}
