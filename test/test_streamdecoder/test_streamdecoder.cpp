#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../test_aacdecoder/vectors.h"
#include "../test_mp3pcm/vectors.h"
#include "core/audio/StreamInputBuffer.h"
#include "core/radio/StreamPolicy.h"
#include "core/radio/stream/StreamDecoder.h"

using namespace awtrix;
using Step = radio::StreamDecoder::Step;
using Block = radio::StreamDecoder::Block;

namespace {

audio::StreamInputBuffer ring() {
  return audio::StreamInputBuffer(radio::kInputBufferBytes, audio::StreamInputBuffer::kMaxAdtsFrameBytes);
}

// A station that sends a clip again and again, as much as fits.
void fill(audio::StreamInputBuffer& input, const uint8_t* data, std::size_t size, std::size_t& at) {
  while (input.room()) {
    const std::size_t n = std::min(input.room(), size - at);
    input.push(data + at, n);
    at = (at + n) % size;
  }
}

int peak(const Block& b) {
  int m = 0;
  for (std::size_t i = 0; i < b.frames * b.channels; ++i) m = std::max(m, std::abs(static_cast<int>(b.samples[i])));
  return m;
}

void test_waits_for_the_preroll() {
  radio::StreamDecoder d;
  d.restart("audio/aacp");
  auto input = ring();
  const auto& v = aacvectors::kAacVectors[0];
  input.push(v.aac, 2000);
  Block b;
  TEST_ASSERT_EQUAL(Step::Wait, d.next(input, 0, b));
  TEST_ASSERT_FALSE(d.prerolled());
}

void test_plays_aac_labelled_as_mpeg() {
  radio::StreamDecoder d;
  d.restart("audio/mpeg");
  auto input = ring();
  const auto& v = aacvectors::kAacVectors[0];
  std::size_t at = 0;
  fill(input, v.aac, v.aacBytes, at);
  Block b;
  int data = 0;
  for (int i = 0; i < 40; ++i)
    if (d.next(input, i, b) == Step::Data && !b.synthetic) ++data;
  TEST_ASSERT_TRUE(data > 10);
  TEST_ASSERT_TRUE(d.playing());
  TEST_ASSERT_EQUAL_STRING("aac", d.format());
  TEST_ASSERT_EQUAL_UINT32(44100, d.rate());
  TEST_ASSERT_EQUAL_UINT8(2, d.channels());
}

void test_plays_mp3() {
  radio::StreamDecoder d;
  d.restart("audio/mpeg");
  auto input = ring();
  std::size_t at = 0;
  fill(input, mp3vectors::ksine_stereo_128k_mp3, sizeof mp3vectors::ksine_stereo_128k_mp3, at);
  Block b;
  int data = 0;
  for (int i = 0; i < 60; ++i)
    if (d.next(input, i, b) == Step::Data) ++data;
  TEST_ASSERT_TRUE(data > 10);
  TEST_ASSERT_EQUAL_STRING("mp3", d.format());
  TEST_ASSERT_TRUE(d.bitrateKbps() > 100 && d.bitrateKbps() < 160);
}

void test_fades_a_new_station_in() {
  radio::StreamDecoder d;
  radio::StreamOptions o;
  o.fadeMs = 500;
  d.setOptions(o);
  d.restart("audio/aac");
  auto input = ring();
  const auto& v = aacvectors::kAacVectors[0];
  std::size_t at = 0;
  fill(input, v.aac, v.aacBytes, at);
  Block b;
  std::vector<int> peaks;
  for (int i = 0; i < 60 && peaks.size() < 30; ++i) {
    if (d.next(input, i, b) == Step::Data && b.frames) peaks.push_back(peak(b));
    fill(input, v.aac, v.aacBytes, at);
  }
  TEST_ASSERT_TRUE(peaks.size() >= 25);
  // Quiet at first, full after half a second (about 22 frames of 1024 at 44.1 kHz).
  TEST_ASSERT_TRUE(peaks[1] < peaks[25] / 3);
  TEST_ASSERT_TRUE(peaks[25] > 1000);
}

void test_static_fills_the_wait_and_then_gives_way() {
  radio::StreamDecoder d;
  radio::StreamOptions o;
  o.staticLevel = 60;
  d.setOptions(o);
  d.restart("audio/aac");
  auto input = ring();
  Block b;
  TEST_ASSERT_EQUAL(Step::Data, d.next(input, 0, b));
  TEST_ASSERT_TRUE(b.synthetic);
  TEST_ASSERT_TRUE(peak(b) > 500);
  d.setStatic(0);
  TEST_ASSERT_EQUAL(Step::Wait, d.next(input, 1, b));
  d.setStatic(60);
  const auto& v = aacvectors::kAacVectors[0];
  std::size_t at = 0;
  fill(input, v.aac, v.aacBytes, at);
  bool real = false;
  for (int i = 0; i < 20 && !real; ++i) real = d.next(input, 2 + i, b) == Step::Data && !b.synthetic;
  TEST_ASSERT_TRUE(real);
}

void test_static_alone() {
  radio::StreamDecoder d;
  Block b;
  TEST_ASSERT_EQUAL(Step::Wait, d.noise(b));
  d.setStatic(100);
  TEST_ASSERT_EQUAL(Step::Data, d.noise(b));
  TEST_ASSERT_EQUAL_UINT32(44100, b.rate);
  TEST_ASSERT_EQUAL_UINT8(2, b.channels);
  TEST_ASSERT_TRUE(b.synthetic);
  TEST_ASSERT_TRUE(peak(b) < 32767);
}

void test_ogg_is_refused_by_name() {
  radio::StreamDecoder d;
  d.restart("audio/mpeg");
  auto input = ring();
  std::vector<uint8_t> ogg(20000, 0x11);
  std::memcpy(ogg.data(), "OggS", 4);
  input.push(ogg.data(), ogg.size());
  Block b;
  TEST_ASSERT_EQUAL(Step::Error, d.next(input, 0, b));
  TEST_ASSERT_EQUAL(radio::Fault::Codec, d.fault());
  TEST_ASSERT_EQUAL_STRING("ogg is not supported", d.error().c_str());
}

void test_noise_is_not_playable() {
  radio::StreamDecoder d;
  d.restart("audio/mpeg");
  auto input = ring();
  std::vector<uint8_t> junk(radio::kInputBufferBytes);
  uint32_t x = 7;
  for (auto& c : junk) {
    x = x * 1664525u + 1013904223u;
    c = static_cast<uint8_t>((x >> 24) & 0x7F);
  }
  input.push(junk.data(), junk.size());
  Block b;
  Step step = Step::Wait;
  for (int i = 0; i < 200 && step != Step::Error; ++i) step = d.next(input, i, b);
  TEST_ASSERT_EQUAL(Step::Error, step);
  TEST_ASSERT_EQUAL(radio::Fault::Undecodable, d.fault());
}

void test_a_long_gap_is_no_longer_playing() {
  radio::StreamDecoder d;
  d.restart("audio/aac");
  auto input = ring();
  const auto& v = aacvectors::kAacVectors[0];
  input.push(v.aac, v.aacBytes);
  std::vector<uint8_t> more(v.aac, v.aac + v.aacBytes);
  while (input.size() < radio::kPrerollBytes) input.push(more.data(), more.size());
  Block b;
  for (int i = 0; i < 400 && input.size(); ++i) d.next(input, i, b);
  TEST_ASSERT_TRUE(d.playing());
  int64_t t = 1000;
  while (input.size()) d.next(input, t, b);
  d.next(input, t, b);
  d.next(input, t + 100, b);
  TEST_ASSERT_TRUE(d.playing());
  d.next(input, t + radio::StreamDecoder::kStarvedMs + 1, b);
  TEST_ASSERT_FALSE(d.playing());
}

}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_waits_for_the_preroll);
  RUN_TEST(test_plays_aac_labelled_as_mpeg);
  RUN_TEST(test_plays_mp3);
  RUN_TEST(test_fades_a_new_station_in);
  RUN_TEST(test_static_fills_the_wait_and_then_gives_way);
  RUN_TEST(test_static_alone);
  RUN_TEST(test_ogg_is_refused_by_name);
  RUN_TEST(test_noise_is_not_playable);
  RUN_TEST(test_a_long_gap_is_no_longer_playing);
  return UNITY_END();
}
