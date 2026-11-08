#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../test_aacdecoder/vectors.h"
#include "../test_mp3pcm/vectors.h"
#include "core/radio/CodecSniff.h"
#include "core/radio/RadioStatus.h"

using namespace awtrix;
using radio::Codec;

namespace {

std::vector<uint8_t> from(const uint8_t* data, std::size_t size, std::size_t skip = 0) {
  return std::vector<uint8_t>(data + skip, data + size);
}

void test_content_type_is_a_hint() {
  std::string name;
  TEST_ASSERT_EQUAL(Codec::Mp3, radio::codecFromContentType("audio/mpeg"));
  TEST_ASSERT_EQUAL(Codec::Aac, radio::codecFromContentType("audio/aacp; charset=x"));
  TEST_ASSERT_EQUAL(Codec::Aac, radio::codecFromContentType("AUDIO/AAC"));
  TEST_ASSERT_EQUAL(Codec::Unsupported, radio::codecFromContentType("application/ogg", &name));
  TEST_ASSERT_EQUAL_STRING("ogg", name.c_str());
  TEST_ASSERT_EQUAL(Codec::Unsupported, radio::codecFromContentType("application/vnd.apple.mpegurl", &name));
  TEST_ASSERT_EQUAL_STRING("hls", name.c_str());
  TEST_ASSERT_EQUAL(Codec::Unknown, radio::codecFromContentType("application/octet-stream"));
}

void test_finds_adts_from_the_bytes() {
  const auto& v = aacvectors::kAacVectors[0];
  TEST_ASSERT_EQUAL(Codec::Aac, radio::sniff(v.aac, v.aacBytes));
  // Joined in the middle of a frame.
  const auto mid = from(v.aac, v.aacBytes, 151);
  TEST_ASSERT_EQUAL(Codec::Aac, radio::sniff(mid.data(), mid.size()));
}

void test_finds_mp3_after_an_id3_tag() {
  TEST_ASSERT_EQUAL(Codec::Mp3, radio::sniff(mp3vectors::ksine_stereo_128k_mp3, sizeof mp3vectors::ksine_stereo_128k_mp3));
  TEST_ASSERT_EQUAL(Codec::Mp3, radio::sniff(mp3vectors::ksine_mono_8khz_mp3, sizeof mp3vectors::ksine_mono_8khz_mp3));
}

void test_names_formats_it_cannot_play() {
  std::string name;
  const uint8_t ogg[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0};
  TEST_ASSERT_EQUAL(Codec::Unsupported, radio::sniff(ogg, sizeof ogg, &name));
  TEST_ASSERT_EQUAL_STRING("ogg", name.c_str());
  const uint8_t flac[] = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
  TEST_ASSERT_EQUAL(Codec::Unsupported, radio::sniff(flac, sizeof flac, &name));
  TEST_ASSERT_EQUAL_STRING("flac", name.c_str());
}

void test_a_lone_sync_in_noise_is_not_a_codec() {
  std::vector<uint8_t> noise(4000);
  uint32_t x = 12345;
  for (auto& b : noise) {
    x = x * 1103515245u + 12345u;
    b = static_cast<uint8_t>(x >> 24);
  }
  noise[100] = 0xFF;
  noise[101] = 0xF1;
  TEST_ASSERT_EQUAL(Codec::Unknown, radio::sniff(noise.data(), noise.size()));
}

void test_status_phases_and_faults() {
  radio::Status s;
  s.enter(radio::Phase::Connecting, 100);
  TEST_ASSERT_EQUAL_INT64(100, s.sinceMs);
  s.enter(radio::Phase::Connecting, 300);
  TEST_ASSERT_EQUAL_INT64(100, s.sinceMs);
  s.fail(radio::Fault::Http, "HTTP 404", 2000, 400, 404);
  TEST_ASSERT_EQUAL(radio::Phase::Error, s.phase);
  TEST_ASSERT_EQUAL_INT(404, s.httpStatus);
  TEST_ASSERT_EQUAL_INT(2000, s.retryMs);
  TEST_ASSERT_EQUAL_STRING("http", radio::faultName(s.fault));
  TEST_ASSERT_FALSE(radio::fatal(s.fault));
  TEST_ASSERT_TRUE(radio::fatal(radio::Fault::Codec));
  s.enter(radio::Phase::Buffering, 500);
  TEST_ASSERT_EQUAL(radio::Fault::None, s.fault);
  TEST_ASSERT_TRUE(s.error.empty());
  TEST_ASSERT_EQUAL_INT(-1, s.retryMs);
  TEST_ASSERT_EQUAL_STRING("buffering", radio::phaseName(s.phase));
}

}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_content_type_is_a_hint);
  RUN_TEST(test_finds_adts_from_the_bytes);
  RUN_TEST(test_finds_mp3_after_an_id3_tag);
  RUN_TEST(test_names_formats_it_cannot_play);
  RUN_TEST(test_a_lone_sync_in_noise_is_not_a_codec);
  RUN_TEST(test_status_phases_and_faults);
  return UNITY_END();
}
